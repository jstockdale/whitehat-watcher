
#include "ma_transport_spi.h"

#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
#include <WE2_core.h>
#include <WE2_device_addr.h>
#include <console_io.h>
#include <hx_drv_gpio.h>
#include <hx_drv_scu.h>
#include <hx_drv_spi.h>
}

#include <porting/ma_osal.h>

#include <core/utils/ma_ringbuffer.hpp>

#include "ma_config_board.h"

#define SPI_PACKET_SIZE                 256
#define SPI_HEADER_LEN                  4
#define SPI_MAX_PL_LEN                  250
#define SPI_CHECKSUM_LEN                2

#define FEATURE_TRANSPORT               0x10
#define FEATURE_TRANSPORT_CMD_READ      0x01
#define FEATURE_TRANSPORT_CMD_WRITE     0x02
#define FEATURE_TRANSPORT_CMD_AVAILABLE 0x03
#define FEATURE_TRANSPORT_CMD_START     0x04
#define FEATURE_TRANSPORT_CMD_STOP      0x05
#define FEATURE_TRANSPORT_CMD_RESET     0x06

namespace ma {

static SPSCRingBuffer<char>* _spi_rb_rx  = nullptr;
static SPSCRingBuffer<char>* _spi_rb_tx  = nullptr;
static uint8_t*              _spi_rx_buf = nullptr;
static uint8_t*              _spi_tx_buf = nullptr;
static Mutex                 _spi_tx_mutex;
static DEV_SPI_PTR           _spi_dev    = nullptr;
static volatile bool         _spi_opened = false;
static volatile int32_t      _spi_rx_blocking = -1;
static volatile int32_t      _spi_tx_blocking = -1;

static void _spi_rx_callback(void* param);
static void _spi_tx_callback(void* param);

static void _spi_arm_rx() {
    if (!_spi_dev || !_spi_opened) {
        return;
    }
    std::memset(_spi_rx_buf, 0, SPI_PACKET_SIZE);
    hx_CleanDCache_by_Addr((void*)_spi_rx_buf, SPI_PACKET_SIZE);
    _spi_rx_blocking = -1;
    _spi_dev->spi_read_dma(_spi_rx_buf, SPI_PACKET_SIZE, (void*)_spi_rx_callback);
}

static void _spi_rx_callback(void* param) {
    (void)param;
    _spi_rx_blocking = 0;

    // DMA has written the received packet to _spi_rx_buf. Invalidate the DCache
    // so the CPU reads the DMA'd data, not stale cached lines.
    hx_InvalidateDCache_by_Addr((void*)_spi_rx_buf, SPI_PACKET_SIZE);

    uint8_t  feature = _spi_rx_buf[0];
    uint8_t  cmd     = _spi_rx_buf[1];
    uint16_t len     = (uint16_t)(_spi_rx_buf[2] << 8 | _spi_rx_buf[3]);

    if (feature != FEATURE_TRANSPORT || len > SPI_MAX_PL_LEN) {
        _spi_arm_rx();
        return;
    }

    switch (cmd) {
    case FEATURE_TRANSPORT_CMD_WRITE:
        // S3 sent AT command text: push to RX ringbuffer for AT server
        _spi_rb_rx->push((const char*)&_spi_rx_buf[4], len);
        _spi_arm_rx();
        break;

    case FEATURE_TRANSPORT_CMD_READ: {
        // S3 wants to read response data: pop from TX ringbuffer
        size_t avail = _spi_rb_tx->size();
        if (len > avail) {
            len = avail;
        }
        if (len > 0) {
            _spi_rb_tx->pop((char*)_spi_tx_buf, len);
        }
        hx_CleanDCache_by_Addr((void*)_spi_tx_buf, len > 0 ? len : 1);
        _spi_tx_blocking = -1;
        // Arm TX: S3 will clock out the data in its follow-up read transaction
        // We transmit len bytes; S3 may clock more (up to 4095), extra will be zeros
        // DO NOT re-arm RX here — the DW slave can only be armed for one direction.
        // RX will be re-armed in _spi_tx_callback after the TX completes.
        _spi_dev->spi_write_dma(_spi_tx_buf, len > 0 ? len : 1, (void*)_spi_tx_callback);
        break;
    }

    case FEATURE_TRANSPORT_CMD_AVAILABLE: {
        // S3 asks how many response bytes are available: return 2-byte big-endian count
        uint16_t available = (uint16_t)_spi_rb_tx->size();
        _spi_tx_buf[0] = (available >> 8) & 0xFF;
        _spi_tx_buf[1] = available & 0xFF;
        hx_CleanDCache_by_Addr((void*)_spi_tx_buf, 2);
        _spi_tx_blocking = -1;
        // DO NOT re-arm RX here — see READ handler above.
        _spi_dev->spi_write_dma(_spi_tx_buf, 2, (void*)_spi_tx_callback);
        break;
    }

    case FEATURE_TRANSPORT_CMD_RESET:
        _spi_rb_rx->clear();
        _spi_rb_tx->clear();
        _spi_arm_rx();
        break;

    case FEATURE_TRANSPORT_CMD_START:
    case FEATURE_TRANSPORT_CMD_STOP:
    default:
        _spi_arm_rx();
        break;
    }
}

static void _spi_tx_callback(void* param) {
    (void)param;
    _spi_tx_blocking = 0;
    // TX complete — now safe to re-arm RX for the next command.
    // (The DW SPI slave can only be armed for one direction at a time.)
    _spi_arm_rx();
}

SPI::SPI() : Transport(MA_TRANSPORT_SPI) {}

SPI::~SPI() { deInit(); }

ma_err_t SPI::init(const void* config) {
    if (_spi_opened || m_initialized) {
        return MA_OK;
    }

    (void)config;

    // Configure PB2-PB5 for SPI slave mode
    // PB2: MISO (DO), PB3: MOSI (DI), PB4: CLK, PB5: CS
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI_S_DI, 1);
    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI_S_CLK, 1);
    hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_S_CS, 1);

    if (hx_drv_spi_slv_init(USE_DW_SPI_SLV_0, DW_SSPI_S_RELBASE) != 0) {
        return MA_EIO;
    }

    _spi_dev = hx_drv_spi_slv_get_dev(USE_DW_SPI_SLV_0);
    if (_spi_dev == nullptr) {
        return MA_EIO;
    }

    // SPI mode 0 (CPOL=0, CPHA=0) to match S3 master
    if (_spi_dev->spi_open(DEV_SLAVE_MODE, SPI_CPOL_0_CPHA_0) != 0) {
        return MA_EIO;
    }

    if (_spi_rx_buf == nullptr) {
        _spi_rx_buf = new uint8_t[SPI_PACKET_SIZE];
    }

    if (_spi_tx_buf == nullptr) {
        _spi_tx_buf = new uint8_t[4096];
    }

    if (_spi_rb_rx == nullptr) {
        _spi_rb_rx = new SPSCRingBuffer<char>(4096);
    }

    if (_spi_rb_tx == nullptr) {
        _spi_rb_tx = new SPSCRingBuffer<char>(4096);
    }

    if (!_spi_rx_buf || !_spi_tx_buf || !_spi_rb_rx || !_spi_rb_tx) {
        return MA_ENOMEM;
    }

    std::memset(_spi_rx_buf, 0, SPI_PACKET_SIZE);
    std::memset(_spi_tx_buf, 0, 4096);

    _spi_opened = m_initialized = true;

    _spi_arm_rx();

    return MA_OK;
}

void SPI::deInit() {
    if (!_spi_opened || !m_initialized) {
        return;
    }

    if (_spi_dev) {
        _spi_dev->spi_close();
        _spi_dev = nullptr;
    }

    if (_spi_rb_rx) {
        delete _spi_rb_rx;
        _spi_rb_rx = nullptr;
    }

    if (_spi_rb_tx) {
        delete _spi_rb_tx;
        _spi_rb_tx = nullptr;
    }

    if (_spi_rx_buf) {
        delete[] _spi_rx_buf;
        _spi_rx_buf = nullptr;
    }

    if (_spi_tx_buf) {
        delete[] _spi_tx_buf;
        _spi_tx_buf = nullptr;
    }

    _spi_opened = m_initialized = false;
}

size_t SPI::available() const { return _spi_rb_rx ? _spi_rb_rx->size() : 0; }

size_t SPI::send(const char* data, size_t length) {
    if (!m_initialized || data == nullptr || length == 0 || !_spi_rb_tx) {
        return 0;
    }

    Guard guard(_spi_tx_mutex);

    return _spi_rb_tx->push(data, length);
}

size_t SPI::flush() {
    if (!m_initialized) {
        return (size_t)-1;
    }

    return 0;
}

size_t SPI::receive(char* data, size_t length) {
    if (!m_initialized || length == 0 || !_spi_rb_rx) {
        return 0;
    }

    return _spi_rb_rx->pop(data, length);
}

size_t SPI::receiveIf(char* data, size_t length, char delimiter) {
    if (!m_initialized || length == 0 || !_spi_rb_rx) {
        return 0;
    }

    return _spi_rb_rx->popIf(data, length, delimiter);
}

}  // namespace ma
