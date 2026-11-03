#ifndef _MA_CAMERA_HIMAX_H_
#define _MA_CAMERA_HIMAX_H_

#include <porting/ma_camera.h>

#include "ma_config_board.h"

namespace ma {

class CameraHimax final : public Camera {
   public:
    CameraHimax(size_t id);
    ~CameraHimax();

    ma_err_t init(size_t preset_idx) noexcept override;
    void     deInit() noexcept override;

    ma_err_t startStream(StreamMode mode) noexcept override;
    void     stopStream() noexcept override;

    ma_err_t commandCtrl(CtrlType ctrl, CtrlMode mode, CtrlValue& value) noexcept override;

    /* Phase 1 (SSCMA fork): hardware ROI via OV5647 INP crop */
    ma_err_t setROI(uint16_t x, uint16_t y, uint16_t w, uint16_t h) noexcept override;
    ma_err_t getROI(uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h) noexcept override;

    /* Phase 3 (SSCMA fork): WDMA2 raw format probe (AT+RAWDUMP) */
    ma_err_t rawDump(raw_dump_result_t& out) noexcept override;

#ifdef CAPTURE_ONLY_MODE
    /* v0.4.5: get the full-frame raw capture buffer.
     * Returns the buffer pointer, size in bytes, and bit depth (8).
     * The buffer is valid after a successful raw preset init until deInit. */
    ma_err_t getRawCaptureBuffer(const uint8_t** buf, size_t* size, uint8_t* bits) noexcept;

    /* v0.4.15 (P4/P5): one-shot native-resolution crop window.
     * setCropWindow() enters full-res mode: the sensor is programmed for
     * 2592x1944 and the INP captures the (x, y, w, h) window 1:1 into a
     * w*h-byte raw buffer (32-byte aligned, allocated here and reused by
     * every capture until the window changes or clears). Geometry must be
     * even (Bayer phase), inside 2592x1944, and w*h <= CROP_WINDOW_MAX_BYTES.
     * Unlike setROI(), the window is NOT persisted and does NOT survive a
     * reboot. Moving to a same-size window (P5 strips) uses the cheap INP
     * recrop path (no sensor re-init); other changes do a full re-init.
     * clearCropWindow() exits full-res mode and restores the raw preset.
     * getCropWindow() reports the active window (all zero = none). */
    ma_err_t setCropWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) noexcept;
    ma_err_t clearCropWindow() noexcept;
    ma_err_t getCropWindow(uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h) noexcept;
#endif

    ma_err_t retrieveFrame(ma_img_t& frame, ma_pixel_format_t format) noexcept override;
    void     returnFrame(ma_img_t& frame) noexcept override;
};

}  // namespace ma

#endif