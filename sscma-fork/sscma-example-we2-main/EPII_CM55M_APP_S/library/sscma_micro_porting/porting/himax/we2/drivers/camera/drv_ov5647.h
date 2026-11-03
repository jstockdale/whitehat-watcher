/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Hongtai Liu, nullptr (Seeed Technology Inc.)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef _DRV_OV5647_H_
#define _DRV_OV5647_H_

#include <core/ma_types.h>
#include <stdint.h>

#include "drv_common.h"

#ifdef __cplusplus
extern "C" {
#endif

ma_err_t drv_ov5647_probe();
ma_err_t drv_ov5647_init(uint16_t width, uint16_t height, int compression);
void     drv_ov5647_deinit();

/* Phase 1: hardware ROI (crop-zoom). Coordinates are in sensor pixels for
 * the active OV5647 mode. w/h of 0 disables the ROI (full sensor).
 * The ROI is applied on the next drv_ov5647_init(). */
ma_err_t drv_ov5647_set_roi(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
ma_err_t drv_ov5647_get_roi(uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h);

/* Phase 3: WDMA2 raw format probe (AT+RAWDUMP).
 * Captures one frame via Sensor->INP->WDMA2 at the CURRENT INP output size
 * (no INP reprogramming) into the YUV422 frame buffer (unused during raw
 * capture), then measures the actual bytes written by scanning for the
 * 0xAA fill pattern. The byte count reveals the pixel packing:
 *   w*h     -> 1 byte/px (e.g. RAW8)
 *   w*h*1.25-> packed RAW10
 *   w*h*2   -> 16-bit unpacked
 * The caller (CameraHimax) must restore the normal datapath with
 * deInit()+init() after this returns. */
#define DRV_RAW_DUMP_HEAD_BYTES 64
#define DRV_RAW_DUMP_HIST_BINS  16

typedef struct {
    uint16_t width;                              /* INP output width used */
    uint16_t height;                             /* INP output height used */
    uint32_t bytes_written;                      /* measured via 0xAA scan */
    uint8_t  head[DRV_RAW_DUMP_HEAD_BYTES];      /* first 64 captured bytes */
    uint32_t hist[DRV_RAW_DUMP_HIST_BINS];       /* 16-bin byte histogram */
    uint8_t  min_val;                            /* min byte in capture */
    uint8_t  max_val;                            /* max byte in capture */
    uint32_t capture_ms;                         /* frame-ready latency */
} drv_raw_dump_result_t;

ma_err_t drv_ov5647_raw_dump(drv_raw_dump_result_t* out);

#ifdef CAPTURE_ONLY_MODE
/* v0.4.5: full-frame raw capture.
 * drv_ov5647_set_raw_buffer() registers a caller-allocated buffer for raw
 * capture. drv_ov5647_raw_dump() uses it instead of the 600KB YUV buffer
 * when registered. drv_ov5647_init_raw() configures the sensor/INP for
 * full-resolution raw output without the HW5x5/JPEG datapath. */
void     drv_ov5647_set_raw_buffer(uint8_t* buf, size_t size, uint8_t bits);
void     drv_ov5647_clear_raw_buffer(void);
ma_err_t drv_ov5647_init_raw(uint16_t width, uint16_t height, uint8_t bits);
/* v0.4.15 (P4/P5): full-sensor-resolution raw init. Programs the OV5647
 * for 2592x1944 output and captures the (x, y, w, h) window 1:1 via the
 * INP crop. drv_ov5647_recrop_fullres() moves the window without a sensor
 * re-init (strip capture). drv_ov5647_raw_dump() is unchanged — it uses
 * _frame.width/_frame.height, which these functions set. */
ma_err_t drv_ov5647_init_raw_fullres(uint16_t crop_x, uint16_t crop_y, uint16_t crop_w, uint16_t crop_h,
                                     uint8_t bits);
ma_err_t drv_ov5647_recrop_fullres(uint16_t crop_x, uint16_t crop_y, uint16_t crop_w, uint16_t crop_h);
#endif

#ifdef __cplusplus
}
#endif

#endif
