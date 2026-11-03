#include <WE2_device_addr.h>
#include <core/ma_compiler.h>
#include <core/ma_debug.h>
#include <hx_drv_scu.h>
#include <porting/ma_device.h>

#include <cstdint>
#include <cstring>
#include <new>

#include "drivers/npu.h"
#include "drivers/xip.h"
#include "ma_camera_himax.h"
#include "ma_config_board.h"
#include "ma_storage_lfs.h"
#include "ma_transport_console.h"
#include "ma_transport_i2c.h"
#include "ma_transport_serial.h"
#include "ma_transport_spi.h"

void* operator new(size_t size) { return pvPortMalloc(size); }

void* operator new[](size_t size) { return pvPortMalloc(size); }

void operator delete(void* ptr) { vPortFree(ptr); }

void operator delete[](void* ptr) { vPortFree(ptr); }

/* v0.4.12: route ALIGNED operator new/delete to the FreeRTOS heap too.
 * Previously only the unaligned overloads above were overridden, so any
 * aligned new — `new (std::align_val_t{32})`, or `new SPSCRingBuffer`
 * whose class is over-aligned via alignas(32) members — fell through to
 * libstdc++'s default, which routes via newlib memalign to _sbrk over a
 * 16 KiB heap. Result: std::bad_alloc at boot while 1.4 MB of FreeRTOS
 * heap sat free (v0.4.10/v0.4.11). pvPortMalloc is only 8-byte aligned,
 * so over-allocate and align manually, stashing the raw pointer in the
 * word just before the aligned block. Failure returns nullptr, matching
 * the unaligned overloads above (this TU builds -fno-exceptions). */
namespace {
void* aligned_pv_port_malloc(size_t size, size_t alignment) {
    /* align_val_t is always a power of two. */
    void* raw = pvPortMalloc(size + alignment + sizeof(void*));
    if (raw == nullptr) {
        return nullptr;
    }
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + alignment - 1) &
                        ~(alignment - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void aligned_pv_port_free(void* ptr) {
    if (ptr != nullptr) {
        vPortFree(reinterpret_cast<void**>(ptr)[-1]);
    }
}
}  // namespace

void* operator new(size_t size, std::align_val_t al) {
    return aligned_pv_port_malloc(size, static_cast<size_t>(al));
}

void* operator new[](size_t size, std::align_val_t al) {
    return aligned_pv_port_malloc(size, static_cast<size_t>(al));
}

void operator delete(void* ptr, std::align_val_t) noexcept { aligned_pv_port_free(ptr); }

void operator delete[](void* ptr, std::align_val_t) noexcept { aligned_pv_port_free(ptr); }

void operator delete(void* ptr, size_t, std::align_val_t) noexcept { aligned_pv_port_free(ptr); }

void operator delete[](void* ptr, size_t, std::align_val_t) noexcept { aligned_pv_port_free(ptr); }


namespace ma {

Device::Device() {
    MA_LOGD(MA_TAG, "Initializing device: %s", MA_BOARD_NAME);
    {
        bool success = xip_drv_init();
        MA_ASSERT(success && "Failed to initialize XIP");
        m_name = MA_BOARD_NAME;
    }

    MA_LOGD(MA_TAG, "Initializing device version");
    {
        uint32_t chipid;
        uint32_t version;
        hx_drv_scu_get_version(&chipid, &version);
        if (chipid == 0x8538000c) {
            m_version = "HX6538";
        } else {
            m_version = "HX6539";
        }
    }

    MA_LOGD(MA_TAG, "Initializing device ID");
    {
        xip_ownership_acquire();
        uint8_t id_full[16];
        if (xip_safe_enable()) {
            std::memcpy(id_full, reinterpret_cast<const void*>(BASE_ADDR_FLASH1_R_ALIAS + 0x003DF000), sizeof id_full);
        } else {
            MA_LOGE(MA_TAG, "Failed to read device ID, using default");
        }
        xip_ownership_release();
        for (size_t i = 0; i < sizeof id_full; ++i) {
            m_id += std::to_string(id_full[i]);
        }
    }

    MA_LOGD(MA_TAG, "Initializing storage");
    {
        static ma_lfs_bd_cfg_t bd_config = {
            .read_size   = 16,
            .prog_size   = 16,
            .erase_size  = 4096,
            .erase_count = 128,
            .flash_addr  = reinterpret_cast<void*>(BASE_ADDR_FLASH1_R_ALIAS + 0x00300000),
        };
        static StorageLfs storage;
        storage.init(&bd_config);
        m_storage = &storage;

        {
            MA_STORAGE_GET_POD(m_storage, "ma#bootcount", m_bootcount, 0);
            ++m_bootcount;
            MA_STORAGE_NOSTA_SET_POD(m_storage, "ma#bootcount", m_bootcount);
        }
    }

    MA_LOGD(MA_TAG, "Initializing transports");
    {
        static Console console;
        console.init(nullptr);
        m_transports.push_back(&console);

        static Serial serial;
        serial.init(nullptr);
        m_transports.push_back(&serial);

        static SPI spi;
        spi.init(nullptr);
        m_transports.push_back(&spi);

#ifdef MA_CONFIG_BOARD_I2C_SLAVE
        static I2C i2c;
        i2c.init(nullptr);
        m_transports.push_back(&i2c);
#endif

        {
            ma_mqtt_topic_config_t config;
            int ret = 0;
            using namespace std::string_literals;
            if (!MA_STORAGE_EXISTS(m_storage, MA_STORAGE_KEY_MQTT_PUB_TOPIC) || !MA_STORAGE_EXISTS(m_storage, MA_STORAGE_KEY_MQTT_SUB_TOPIC)) {
                std::string topic_prefix = "ma/"s + MA_AT_API_VERSION + "/" + PRODUCT_NAME_PREFIX + "_"s + PRODUCT_NAME_SUFFIX + "_"s + m_id;
                std::string topic_pub    = topic_prefix + "/pub";
                std::string topic_sub    = topic_prefix + "/sub";
                MA_STORAGE_SET_STR(ret, m_storage, MA_STORAGE_KEY_MQTT_PUB_TOPIC, topic_pub);
                MA_STORAGE_SET_STR(ret, m_storage, MA_STORAGE_KEY_MQTT_SUB_TOPIC, topic_sub);
                MA_STORAGE_SET_RVPOD(ret, m_storage, MA_STORAGE_KEY_MQTT_PUB_QOS, 0);
                MA_STORAGE_SET_RVPOD(ret, m_storage, MA_STORAGE_KEY_MQTT_SUB_QOS, 0);
            }
        }

    }

    MA_LOGD(MA_TAG, "Initializing sensors");
    {
        static CameraHimax camera(0);
        m_sensors.push_back(&camera);
    }

    MA_LOGD(MA_TAG, "Initializing models");
    {
        xip_ownership_acquire();
        if (xip_safe_enable()) {
            const void* address = reinterpret_cast<void*>(BASE_ADDR_FLASH1_R_ALIAS + 0x00400000);
            const size_t size   = 4096;
            size_t id           = 0;
            for (size_t i = 0; i < size; i += 4096) {
                const void* data = address + i;
                if (ma_ntohl(*(static_cast<const uint32_t*>(data) + 1)) == 0x54464C33) {  // 'TFL3'
                    ma_model_t model;
                    model.id   = ++id;
                    model.type = MA_MODEL_TYPE_UNDEFINED;
                    model.name = address;
                    model.addr = data;
                    m_models.push_back(model);
                }
            }
        } else {
            MA_LOGE(MA_TAG, "Failed to find models, using default");
        }
        xip_ownership_release();
    }

    MA_LOGD(MA_TAG, "Initializing NPU driver");
    { himax_arm_npu_init(true, true); }
}

Device* Device::getInstance() {
    static Device device{};
    return &device;
}

Device::~Device() {
    NVIC_SystemReset();
}

}  // namespace ma
