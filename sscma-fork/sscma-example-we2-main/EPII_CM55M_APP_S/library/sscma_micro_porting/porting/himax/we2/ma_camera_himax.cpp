#include "ma_camera_himax.h"

#include <core/ma_debug.h>

#include <new>

#include "drivers/camera/drv_common.h"
#include "drivers/camera/drv_imx219.h"
#include "drivers/camera/drv_imx708.h"
#include "drivers/camera/drv_ov5647.h"
#include "drivers/camera/drv_shared_cfg.h"

namespace ma {

static ma_err_t (*_drv_cam_init)(uint16_t, uint16_t, int) = nullptr;
static void (*_drv_cam_deinit)()                          = nullptr;

struct presets_wrapper_t {
    const char* description;
    uint16_t    width;
    uint16_t    height;
    int         compression; /* 0 = raw (no JPEG), else JPEG QTABLE (10 or 4) */
#ifdef CAPTURE_ONLY_MODE
    uint8_t     raw_bits; /* 0 = not raw, 8 = RAW8, 10 = packed RAW10 */
#endif
};

static const presets_wrapper_t _presets[] = {
#ifdef CAPTURE_ONLY_MODE
  { "240x240 Small", 240, 240, 10, 0},
  { "416x416 Small", 416, 416, 10, 0},
  { "480x480 Small", 480, 480, 10, 0},
  { "640x480 Small", 640, 480, 10, 0},
  {"240x240 Medium", 240, 240,  4, 0},
  {"416x416 Medium", 416, 416,  4, 0},
  {"480x480 Medium", 480, 480,  4, 0},
  /* v0.4.4 experimental: full-frame raw capture at sensor native resolution.
   * Requires CAPTURE_ONLY_MODE (TFLM arena freed for the large buffer). */
  {"1296x972 RAW8", 1296, 972, 0, 8},
  {"1296x972 RAW10", 1296, 972, 0, 10},
#else
  { "240x240 Small", 240, 240, 10},
  { "416x416 Small", 416, 416, 10},
  { "480x480 Small", 480, 480, 10},
  { "640x480 Small", 640, 480, 10},
  {"240x240 Medium", 240, 240,  4},
  {"416x416 Medium", 416, 416,  4},
  {"480x480 Medium", 480, 480,  4},
#endif
};

static ma_pixel_rotate_t _rotation_override      = MA_PIXEL_ROTATE_0;
static volatile int      _frame_shared_ref_count = 0;
// ^ shared pointer or unique pointer would be better but currently is not allowed

#ifdef CAPTURE_ONLY_MODE
/* v0.4.4: full-frame raw buffer, allocated from heap at init time.
 * In capture-only mode the TFLM arena (~1.1MB) is not allocated, so the
 * heap has room for the 1.26MB (RAW8) or 1.57MB (RAW10) buffer. */
static uint8_t* _raw_capture_buf      = nullptr;
static size_t   _raw_capture_buf_size = 0;
#endif

CameraHimax::CameraHimax(size_t id) : Camera(id) {
    for (const auto& preset : _presets) {
        m_presets.push_back({.description = preset.description});
    }
    m_presets.shrink_to_fit();
}

CameraHimax::~CameraHimax() { deInit(); }

ma_err_t CameraHimax::init(size_t preset_idx) noexcept {
    if (m_initialized) [[unlikely]] {
        return MA_OK;
    }

    preset_idx = preset_idx ? preset_idx : 3;
    if (preset_idx >= m_presets.size()) [[unlikely]] {
        return MA_EINVAL;
    }
    m_preset_idx = preset_idx;

    if (drv_ov5647_probe() == MA_OK) {
        _drv_cam_init   = drv_ov5647_init;
        _drv_cam_deinit = drv_ov5647_deinit;
    } else if (drv_imx708_probe() == MA_OK) {
        _drv_cam_init   = drv_imx708_init;
        _drv_cam_deinit = drv_imx708_deinit;
    } else {
        return MA_ENOTSUP;
    }

    const auto& preset = _presets[m_preset_idx - 1];
#ifdef CAPTURE_ONLY_MODE
    ma_err_t ec;
    if (preset.compression == 0 && preset.raw_bits != 0) {
        /* Raw preset: allocate the full-frame buffer from the heap.
         * TFLM is not initialized in capture-only mode, so ~1.1MB is free. */
        size_t need = (preset.raw_bits == 8)
                          ? RAW8_FRAME_SIZE(preset.width, preset.height)
                          : RAW10_FRAME_SIZE(preset.width, preset.height);
        /* 32-byte align for DMA. */
        need = (need + 31) & ~31u;
        MA_LOGD(MA_TAG, "Raw preset: allocating %lu bytes for %ux%u %u-bit",
                (unsigned long)need, preset.width, preset.height, preset.raw_bits);
        _raw_capture_buf = new (std::nothrow) uint8_t[need];
        if (_raw_capture_buf == nullptr) {
            MA_LOGE(MA_TAG, "Raw preset: buffer allocation failed (%lu bytes)", (unsigned long)need);
            return MA_ENOMEM;
        }
        _raw_capture_buf_size = need;
        drv_ov5647_set_raw_buffer(_raw_capture_buf, need, preset.raw_bits);
        ec = drv_ov5647_init_raw(preset.width, preset.height, preset.raw_bits);
        if (ec != MA_OK) {
            delete[] _raw_capture_buf;
            _raw_capture_buf      = nullptr;
            _raw_capture_buf_size = 0;
            drv_ov5647_clear_raw_buffer();
            return ec;
        }
    } else {
        ec = _drv_cam_init(preset.width, preset.height, preset.compression);
        if (ec != MA_OK) {
            return ec;
        }
    }
#else
    auto ec = _drv_cam_init(preset.width, preset.height, preset.compression);
    if (ec != MA_OK) {
        return ec;
    }
#endif

    m_initialized = true;

    return MA_OK;
}

void CameraHimax::deInit() noexcept {
    if (!m_initialized) [[unlikely]] {
        return;
    }

    if (_drv_cam_deinit) [[likely]] {
        _drv_cam_deinit();
    }

#ifdef CAPTURE_ONLY_MODE
    /* Free the raw capture buffer if allocated. */
    if (_raw_capture_buf != nullptr) {
        drv_ov5647_clear_raw_buffer();
        delete[] _raw_capture_buf;
        _raw_capture_buf      = nullptr;
        _raw_capture_buf_size = 0;
    }
#endif

    m_initialized = false;
}

ma_err_t CameraHimax::startStream(StreamMode mode) noexcept {
    if (!m_initialized) [[unlikely]] {
        return MA_EPERM;
    }

    if (m_streaming) [[unlikely]] {
        return MA_OK;
    }

    m_stream_mode = mode;

    m_streaming = true;

    return MA_OK;
}

void CameraHimax::stopStream() noexcept {
    if (!m_initialized) [[unlikely]] {
        return;
    }

    if (!m_streaming) [[unlikely]] {
        return;
    }

    if (_frame_shared_ref_count > 0) {
        MA_LOGE(MA_TAG, "Streaming stopped with %d frames unreturned", _frame_shared_ref_count);
        _frame_shared_ref_count = 0;
    }

    m_streaming = false;
}

ma_err_t CameraHimax::commandCtrl(CtrlType ctrl, CtrlMode mode, CtrlValue& value) noexcept {
    if (!m_initialized) [[unlikely]] {
        return MA_EPERM;
    }

    switch (ctrl) {
    case kRotate: {
        switch (mode) {
        case kWrite:
            _rotation_override = static_cast<ma_pixel_rotate_t>(value.i32);
            break;
        case kRead:
            value.i32 = _rotation_override;
            break;
        default:
            return MA_EINVAL;
        }
    } break;
    case kRegister: {
        switch (mode) {
        case kWrite:
            return drv_set_reg(value.u16s[0], value.bytes[2]);
        case kRead:
            return drv_get_reg(value.u16s[0], &value.bytes[2]);
        default:
            return MA_EINVAL;
        }
    } break;
    default:
        return MA_ENOTSUP;
    }

    return MA_OK;
}

ma_err_t CameraHimax::setROI(uint16_t x, uint16_t y, uint16_t w, uint16_t h) noexcept {
    // Phase 1: ROI is implemented for the OV5647 driver only.
    if (m_initialized && _drv_cam_init != drv_ov5647_init) {
        return MA_ENOTSUP;
    }

    ma_err_t ret = drv_ov5647_set_roi(x, y, w, h);
    if (ret != MA_OK) {
        return ret;
    }

    if (m_initialized) {
        // Re-run datapath configuration so binning/HW5x5 match the new ROI.
        // With ENABLE_SENSOR_FAST_SWITCH this skips sensor register
        // programming and only reconfigures the datapath.
        deInit();
        ret = init(m_preset_idx ? m_preset_idx : 3);
    }

    return ret;
}

ma_err_t CameraHimax::getROI(uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h) noexcept {
    // Phase 1: ROI is implemented for the OV5647 driver only.
    if (m_initialized && _drv_cam_init != drv_ov5647_init) {
        return MA_ENOTSUP;
    }

    return drv_ov5647_get_roi(&x, &y, &w, &h);
}

ma_err_t CameraHimax::rawDump(raw_dump_result_t& out) noexcept {
    // Phase 3: raw WDMA2 probe is implemented for the OV5647 driver only.
    if (m_initialized && _drv_cam_init != drv_ov5647_init) {
        return MA_ENOTSUP;
    }
    if (!m_initialized) {
        return MA_EPERM;
    }

    drv_raw_dump_result_t drv_out = {};
    ma_err_t ret = drv_ov5647_raw_dump(&drv_out);

#ifdef CAPTURE_ONLY_MODE
    /* v0.4.4: for raw presets, the INP is already in raw mode — no JPEG
     * datapath to restore. Skip the deInit/init cycle so the capture
     * buffer persists for AT+RAWREAD. For JPEG presets, restore as before. */
    bool is_raw_preset = false;
    if (m_preset_idx > 0 && m_preset_idx <= sizeof(_presets) / sizeof(_presets[0])) {
        is_raw_preset = (_presets[m_preset_idx - 1].compression == 0);
    }
    if (!is_raw_preset) {
#endif
    // Restore the normal JPEG datapath (same as setROI). Runs even if the
    // capture failed, so the device never wedges in the raw path.
    deInit();
    ma_err_t ec = init(m_preset_idx ? m_preset_idx : 3);
    if (ec != MA_OK) {
        MA_LOGE(MA_TAG, "RawDump: datapath restore failed %d", ec);
        return ec;
    }
#ifdef CAPTURE_ONLY_MODE
    }
#endif

    if (ret != MA_OK) {
        return ret;
    }

    static_assert(sizeof(out.head) == sizeof(drv_out.head), "rawdump head size mismatch");
    static_assert(sizeof(out.hist) == sizeof(drv_out.hist), "rawdump hist size mismatch");
    out.width         = drv_out.width;
    out.height        = drv_out.height;
    out.bytes_written = drv_out.bytes_written;
    out.min_val       = drv_out.min_val;
    out.max_val       = drv_out.max_val;
    out.capture_ms    = drv_out.capture_ms;
    memcpy(out.head, drv_out.head, sizeof(out.head));
    memcpy(out.hist, drv_out.hist, sizeof(out.hist));
    return MA_OK;
}

#ifdef CAPTURE_ONLY_MODE
ma_err_t CameraHimax::getRawCaptureBuffer(const uint8_t** buf, size_t* size, uint8_t* bits) noexcept {
    if (!m_initialized) {
        return MA_EPERM;
    }
    if (_raw_capture_buf == nullptr || _raw_capture_buf_size == 0) {
        return MA_ENOENT;
    }
    if (m_preset_idx == 0 || m_preset_idx > sizeof(_presets) / sizeof(_presets[0])) {
        return MA_EINVAL;
    }
    const auto& preset = _presets[m_preset_idx - 1];
    if (preset.compression != 0 || preset.raw_bits == 0) {
        return MA_ENOTSUP;
    }
    if (buf) *buf = _raw_capture_buf;
    if (size) *size = _raw_capture_buf_size;
    if (bits) *bits = preset.raw_bits;
    return MA_OK;
}
#endif

ma_err_t CameraHimax::retrieveFrame(ma_img_t& frame, ma_pixel_format_t format) noexcept {
    if (!m_initialized) [[unlikely]] {
        return MA_EPERM;
    }

    if (!m_streaming) [[unlikely]] {
        return MA_EPERM;
    }

    if (frame.data || frame.size) {
        MA_LOGE(MA_TAG, "Frame ownership violation");
        return MA_EINVAL;
    }

    if (_frame_shared_ref_count == 0) {
        switch (m_stream_mode) {
        case kRefreshOnRetrieve:
            drv_capture_next();
            break;
        default:
            break;
        }
        auto ret = drv_capture(1000);
        if (ret != MA_OK) {
            return ret;
        }
    }

    switch (format) {
    case MA_PIXEL_FORMAT_AUTO:
    case MA_PIXEL_FORMAT_YUV422:
        frame = drv_get_frame();
        break;
    case MA_PIXEL_FORMAT_JPEG:
        frame = drv_get_jpeg();
        break;
    default:
        return MA_ENOTSUP;
    }

    frame.rotate = _rotation_override;
    ++_frame_shared_ref_count;

    return MA_OK;
}

void CameraHimax::returnFrame(ma_img_t& frame) noexcept {
    if (!m_initialized) [[unlikely]] {
        return;
    }

    if (!m_streaming) [[unlikely]] {
        return;
    }

    if (!frame.data) [[unlikely]] {
        MA_LOGW(MA_TAG, "Return empty frame");
        return;
    }

    frame.data = nullptr;
    frame.size = 0;
    --_frame_shared_ref_count;
    // ^ ref count is not thread safe and we don't have a simple way to check if the frame is really borrowed from here

    if (_frame_shared_ref_count == 0) {
        switch (m_stream_mode) {
        case kRefreshOnReturn:
            drv_capture_next();
            break;
        default:
            break;
        }
    } else if (_frame_shared_ref_count < 0) {
        MA_LOGE(MA_TAG, "Unowned return frame detected");
        _frame_shared_ref_count = 0;
    }
}

}  // namespace ma