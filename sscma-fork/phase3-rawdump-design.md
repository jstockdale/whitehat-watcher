# Phase 3 Design: AT+RAWDUMP — WDMA2 Raw Format Probe (v0.3 test firmware)

**Date:** 2026-09-28
**Status:** DESIGN — implementation follows this document
**Goal:** Determine the actual WDMA2 byte format (RAW8 vs packed RAW10 vs 16-bit-unpacked)
before building the full strip-capture pipeline.

## Why this test exists

The strip-capture plan is gated on one unknown: what `sensordplib_set_raw_wdma2()`
(Sensor→INP→WDMA2) actually writes per pixel. The SDK configures CSI for 10-bit but
sizes raw buffers as `w*h` (1 byte/px) — evidence, not proof. Three branches:

| Branch | Bytes for w×h | Example (480×480) |
|---|---|---|
| RAW8 (or other 1 B/px) | w×h | 230,400 |
| Packed RAW10 | w×h×1.25 | 288,000 |
| 16-bit-unpacked RAW10 | w×h×2 | 460,800 |

The byte count alone is decisive. Head-bytes hex + histogram are sanity evidence.

## Command

```
AT+RAWDUMP          → capture at current INP output size (default 480×480)
```

No arguments in v0.3: the test reuses the *current* INP crop/binning configuration,
so no INP reprogramming is needed — we only swap the datapath tail from
HW5x5→JPEG to raw WDMA2. This keeps the test minimally invasive.

Response (JSON):
```json
{"code":0,"cmd":"RAWDUMP","width":480,"height":480,"bytes":288000,
 "head_hex":"3f8a...","hist":[..16 bins..],"min":12,"max":243,"capture_ms":87}
```

## Capture sequence (drv_ov5647_raw_dump)

1. Validate: camera initialized, OV5647 driver active.
2. Use current INP output dims (`_frame.width/height`, e.g. 480×480).
3. Buffer: reuse `_wdma3_baseaddr` (600KB YUV422 buffer at 0x3416A000, SRAM_1 tail).
   Unused during raw capture; 600KB ≥ 460,800 worst case. No heap allocation.
4. Pattern-fill buffer with 0xAA.
5. `sensordplib_stop_capture()` — halt the JPEG datapath.
6. `sensordplib_set_xDMA_baseaddrbyapp(_wdma1, _wdma3_as_wdma2, _wdma3)` —
   point WDMA2 at the YUV422 buffer (WDMA1/WDMA3 addrs preserved).
7. Register one-shot DP callback; `sensordplib_set_raw_wdma2(w, h, cb)`.
8. `sensordplib_set_sensorctrl_start()` + `sensordplib_retrigger_capture()`.
9. Poll flag with timeout (2s, 5ms steps via `hx_drv_timer_cm55x_delay_ms`).
10. On frame-ready: scan from buffer end for last non-0xAA byte → `bytes_written`.
11. Copy first 64 bytes → hex; 16-bin histogram; min/max; record elapsed ms.
12. Restore: re-register `_drv_dp_event_cb`, `sensordplib_stop_capture()`.

Restore of the normal JPEG path happens one layer up (CameraHimax::rawDump
calls deInit()+init() after the driver returns, same as setROI).

## Safety properties

- No heap allocation in the driver (uses the existing 600KB frame buffer).
- Buffer overflow impossible: worst-case 16-bit (460,800 B) < 600KB buffer.
- Timeout (2s) on frame-ready; clean MA_ETIMEDOUT, datapath left stopped,
  CameraHimax still runs deInit()+init() to restore.
- Pattern-fill scan: bytes_written is measured, never assumed from format.
- If the sensor isn't streaming or init fails, returns error before touching HW.

## Interpreting results

- `bytes == w*h` → 1 byte/px (likely RAW8).
- `bytes == w*h*5/4` → packed RAW10.
- `bytes == w*h*2` → 16-bit unpacked.
- Anything else → report; investigate (stride/padding?).
- `head_hex`: packed RAW10 shows 5-byte groups (4×8 MSBs + 1 pack byte).
- `hist`: packed RAW10's every-5th-byte distribution differs visibly.

## Files

- `sscma/server/at/callback/rawdump.hpp` (new) — AT+RAWDUMP callback
- `sscma/server/at/ma_server_at.cpp` (mod) — register RAWDUMP
- `sscma/porting/ma_camera.h` (mod) — `ma_raw_dump_result_t` + `rawDump()` virtual
- `porting/himax/we2/ma_camera_himax.{h,cpp}` (mod) — override + orchestration
- `porting/himax/we2/drivers/camera/drv_ov5647.{h,c}` (mod) — capture engine

## Test procedure (on hardware, after flash)

1. `AT+RAWDUMP` → expect code 0, bytes in {230400, 288000, 460800}.
2. Record bytes + head_hex + hist → classify format.
3. `AT+INVOKE` or snapshot → confirm normal JPEG path restored (no regression).
4. `AT+ROI?` → confirm ROI fork still functional.
