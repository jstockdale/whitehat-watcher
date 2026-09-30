# SSCMA Fork — Phase 1 Implementation Log: AT+ROI Hardware Crop-Zoom

**Date:** 2026-09-24
**Status:** IMPLEMENTED & BUILT — not flashed (awaiting John's approval)
**Image:** `watcher_sscma_v0.2_roi.img` (643,072 bytes)
**SHA-256:** `7eb66951f5b19e4079125fbaba71e6a541cfc61cfe8e88ad4c4e8bffd05a40b9`

## New AT commands

```
AT+ROI=x,y,w,h    Set hardware crop window (sensor pixels, 1296x972 space)
AT+ROI?           Get current ROI  ->  {"roi": "x,y,w,h"}
AT+ROI=0,0,0,0    Disable ROI (full sensor)
```

Example: `AT+ROI=552,390,192,192` crops the center 192x192 sensor region.
With a 192x192 output preset this feeds the model native sensor pixels.

## How it works

1. `drv_ov5647_set_roi()` validates + stores the ROI in driver statics.
2. `drv_ov5647_init()` programs the stored ROI into `INP_CROP_T`
   (inclusive `last_x/last_y`, per `hx_drv_inp.h`) and computes INP binning
   factors from the ROI dimensions (crop sits before binning in the datapath).
3. `CameraHimax::setROI()` re-inits the camera when already streaming so the
   datapath (binning, HW5x5 center-crop) matches the new ROI. With
   `ENABLE_SENSOR_FAST_SWITCH` this skips sensor register reprogramming.
4. ROI persists in littlefs (`roi#x/y/w/h`); `initDefaultROI()` restores it
   before `initDefaultSensor()` at boot.

## Validation rules (drv_ov5647_set_roi)

- w/h of 0 = disable (full sensor)
- Minimum 96x96
- Must fit within the active sensor mode (1296x972 default)
- Odd x/y/w/h rounded down to even (Bayer alignment), with a warning log
- Init fails fast with `MA_EINVAL` if ROI < requested output size
  (prevents a uint16_t underflow in the HW5x5 center-crop computation)

## Files changed

- `EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/drivers/camera/drv_ov5647.c`
  - ROI state, `drv_ov5647_set_roi()`, `drv_ov5647_get_roi()`
  - `drv_ov5647_init()` applies ROI to `INP_CROP_T`, bins from ROI dims
- `.../drivers/camera/drv_ov5647.h` — new function declarations
- `EPII_CM55M_APP_S/library/sscma_micro/sscma/porting/ma_camera.h`
  - virtual `setROI()`/`getROI()` on `Camera` (default `MA_ENOTSUP`)
- `EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_camera_himax.h/.cpp`
  - overrides delegating to the OV5647 driver + re-init
- `EPII_CM55M_APP_S/library/sscma_micro/sscma/core/ma_definations.h`
  - `MA_STORAGE_KEY_ROI_X/Y/W/H`
- `EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/callback/roi.hpp` (new)
  - `setROI`, `getROI`, `initDefaultROI` callbacks
- `EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/ma_server_at.cpp`
  - registers `ROI` (argc 4) and `ROI?`; calls `initDefaultROI()` at startup

## Deviations from phase1-roi-design.md

- Callbacks live in a new `roi.hpp` instead of `sensor.hpp` (cleaner separation).
- Went through `Camera::setROI/getROI` virtuals rather than calling the driver
  directly from the AT layer (respects the sscma_micro/porting boundary).
- Did NOT use `sensordplib_inp_set_crop_area()` for runtime updates: changing
  the crop without recomputing binning/HW5x5 would corrupt the output when the
  ROI size changes. Full datapath re-init is the safe Phase 1 path; the runtime
  API remains a future optimization for same-size window moves.
- `AT+ROI?` returns `{"roi": "x,y,w,h"}` (JSON) rather than `+ROI: ...` text;
  the console speaks the JSON protocol.
- Resolved open Q1 (coordinate space): INP crop is pre-binning, sensor pixels.
- Resolved open Q2 (inclusive/exclusive): `last_x/last_y` are INCLUSIVE
  (max HSIZE-1 per `hx_drv_inp.h`), so `last = start + size - 1`.
- Resolved open Q3 (runtime reconfig): re-init via deInit/init; fast-switch
  keeps it to datapath reconfig only.

## Open / not yet validated (needs hardware)

- INP crop + binning interaction is per the documented pipeline order
  (SENSOR -> INP_CROP -> INP_BINNING -> INP_SUBSAMPLE); frame-content
  verification still needed on hardware.
- JPEG encoder input with ROI active (expected: follows output size).
- Multiple rapid ROI changes / stream stability.
- ROI change latency (target <500ms).
- `MA_STORAGE_SET_POD` requires an lvalue argument (cast rvalues fail to
  compile) — noted for future AT work.

## Build

```bash
export PATH="$HOME/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin:$PATH"
export GNU_TOOLPATH="$HOME/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin/"
cd sscma-example-we2-main/EPII_CM55M_APP_S && make -j$(nproc)
cd ../we2_image_gen_local
cp ../EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf input_case1_secboot/
./we2_local_image_gen project_case1_blp_wlcsp.json
# -> output_case1_sec_wlcsp/output.img
```

## Hardware test plan (after flash approval)

1. `AT+ROI?` -> expect `{"roi": "0,0,1296,972"}` (full sensor)
2. `AT+INVOKE=1` (or sample) full frame; note content
3. `AT+ROI=552,390,192,192` -> OK; capture; verify center crop
4. `AT+ROI=2000,2000,100,100` -> expect error code, no crash
5. `AT+ROI=0,0,0,0` -> back to full sensor
6. Reboot -> `AT+ROI?` confirms persistence
