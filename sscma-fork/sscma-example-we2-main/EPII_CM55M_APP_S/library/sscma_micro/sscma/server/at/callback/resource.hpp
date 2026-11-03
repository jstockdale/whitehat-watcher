#pragma once

#include <core/ma_config_internal.h>
#include <core/ma_core.h>
#include <porting/ma_porting.h>

#include <ma_config_board.h>

#include <atomic>

#ifdef CAPTURE_ONLY_MODE
/* v0.4.6: explicit firmware/build identifier for the experimental
 * capture-only build (TFLM engine not initialized). Reported via AT+VER?
 * and at boot. Defined in watcher_build_hash.h (single source of truth). */
#include "watcher_build_hash.h"
#endif

using namespace ma;

namespace ma::server::callback {

using namespace ma::engine;

struct StaticResource final {
   private:
    StaticResource() {
        device = Device::getInstance();

        MA_LOGD(MA_TAG, "Initializing engine");
#if MA_USE_ENGINE_TFLITE
        static EngineTFLite engine_default;
#elif MA_USE_ENGINE_CVI
        static EngineCVI engine_default;
#else
    #error "No engine is defined"
#endif
        engine  = &engine_default;
#ifdef CAPTURE_ONLY_MODE
        /* v0.4.4 experimental: capture-only mode skips the TFLM tensor arena
         * allocation (1,126,400 bytes), freeing ~1.1MB of heap for full-frame
         * raw capture buffers. ML inference is not available in this build. */
        MA_LOGD(MA_TAG, "Capture-only mode: skipping TFLM engine init");
#else
        int ret = engine->init();
        if (ret != MA_OK) {
            MA_LOGE(MA_TAG, "Engine init failed: %d", ret);
        }
#endif

        MA_LOGD(MA_TAG, "Initializing executor");
        static Executor executor_default;
        executor = &executor_default;

        MA_STORAGE_GET_POD(device->getStorage(), "ma#score_threshold", shared_threshold_score, shared_threshold_score);
        MA_STORAGE_GET_POD(device->getStorage(), "ma#nms_threshold", shared_threshold_nms, shared_threshold_nms);
    }

   public:
    static StaticResource* getInstance() noexcept {
        static StaticResource instance;
        return &instance;
    }

    Device*   device   = nullptr;
    Engine*   engine   = nullptr;
    Executor* executor = nullptr;

    std::atomic<std::size_t> current_task_id  = 0;
    size_t                   current_model_id = 0;
    size_t                   current_sensor_id = 0;
    size_t                   current_algorithm_id = 0;
    
    float shared_threshold_score = 0.25;
    float shared_threshold_nms   = 0.45;

    std::atomic<bool> is_ready  = false;
    std::atomic<bool> is_sample = false;
    std::atomic<bool> is_invoke = false;
};

#define static_resource StaticResource::getInstance()

/* v0.4.7: broadcast a just-encoded INIT event to every transport except the
 * front one. The initDefault*() routines configure hardware through the
 * front (console) transport and the configure*() call emits the INIT@*
 * announcement only there; the S3 link watches the SPI transport for INIT@*
 * as connection evidence, so without this rebroadcast the S3 never sees the
 * Himax come up. Call immediately after the configure*() call returns: the
 * encoder still holds the event bytes (data()/size()) until its next
 * begin(). */
inline void broadcastInitEvent(Encoder& encoder) {
    const auto& transports = static_resource->device->getTransports();
    if (transports.size() < 2) {
        return;
    }
    const char* data = reinterpret_cast<const char*>(encoder.data());
    const size_t size = encoder.size();
    for (size_t i = 1; i < transports.size(); ++i) {
        Transport* t = transports[i];
        if (t && *t) {
            t->send(data, size);
        }
    }
}

}  // namespace ma::server::callback