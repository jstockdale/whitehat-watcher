# SSCMA Fork — Phase 1 Design: Hardware ROI / Crop-Zoom

**Date:** 2026-09-24  
**Status:** DESIGN — Ready for implementation after John approves  
**Prerequisite:** Phase 0 complete (build works)

## Goal

Add runtime camera ROI control via AT command, enabling the Watcher to "zoom" into screen regions for cursor detection without changing the 640×480 output resolution.

## Why This Matters

The cursor model v4 failed on real frames (0/4 detections) because 640×480 → 192×192 downscaling makes cursors too small. Hardware ROI lets us:
1. Crop a 192×192 region from the full sensor (e.g., center 192×192 of 1296×972)
2. Feed that directly to the model at native resolution
3. No software scaling, no resolution loss

## Technical Approach

### 1. AT Command Interface

**New commands** (registered in `ma_server_at.cpp`):

```
AT+ROI=x,y,w,h     Set region of interest (sensor coordinates)
AT+ROI?            Get current ROI
```

**Parameters:**
- `x, y`: Top-left corner in sensor pixels (0-based)
- `w, h`: Width and height in sensor pixels
- Constraints:
  - Must be within sensor bounds (1296×972 for current mode)
  - Minimum size: 96×96 (smaller than this, no benefit over software crop)
  - Alignment: x and w must be even (MIPI/datapath requirement — verify)
  - If `w,h` exceed output size, datapath will scale down

**Response format:**
```
AT+ROI=100,100,192,192
OK

AT+ROI?
+ROI: 100,100,192,192
OK
```

### 2. Implementation Points

**File: `sscma/server/at/callback/sensor.hpp`** (or new `roi.hpp`)
- Add `setROI()` callback function
- Add `getROI()` callback function
- Validate coordinates against current sensor mode dimensions
- Store ROI in device context or global state

**File: `ma_server_at.cpp`**
- Register `ROI` and `ROI?` services

**File: `porting/himax/we2/drivers/camera/drv_ov5647.c`**
- Modify `drv_ov5647_init()` to read stored ROI
- Populate `INP_CROP_T crop` structure:
  ```c
  crop.start_x = roi_x;
  crop.start_y = roi_y;
  crop.last_x = roi_x + roi_w - 1;  // Verify: inclusive or exclusive?
  crop.last_y = roi_y + roi_h - 1;
  ```
- **Critical:** Determine if crop coordinates are in sensor pixels or post-binning pixels
- **Critical:** Verify `last_x/last_y` semantics (inclusive vs exclusive) from sensordp library

**Camera re-initialization:**
- Changing ROI requires re-running `drv_ov5647_init()` 
- Must handle: stop stream → reconfigure → restart stream
- Should be atomic from AT command perspective (no partial state)

### 3. Coordinate System

**Sensor space:** 1296×972 (current default mode)  
**Output space:** Configured via `width, height` params to `drv_ov5647_init()` (typically 640×480 or 192×192)

**ROI semantics:**
- ROI is specified in **sensor coordinates** (before binning/scaling)
- Example: `AT+ROI=552,390,192,192` crops center 192×192 from 1296×972 sensor
- The datapath then scales/crops to the requested output size

**For cursor detection:**
- Sensor: 1296×972
- Want: 192×192 model input at native sensor resolution
- ROI: 192×192 region (no scaling needed if output is also 192×192)
- To "zoom": use smaller ROI (e.g., 96×96) with output 192×192 (2x upscale)

### 4. Validation Requirements

Before considering Phase 1 complete:

1. **Bounds checking:** ROI outside sensor → error, no crash
2. **Alignment:** Odd coordinates handled gracefully (round or reject)
3. **Stream stability:** Multiple ROI changes don't hang the datapath
4. **Image verification:** Captured frame shows expected region (compare with full-frame)
5. **Performance:** ROI change completes in <500ms (acceptable for interactive use)

### 5. Testing Plan (requires hardware flash — NOT authorized yet)

1. Flash Phase 1 firmware (after John approves)
2. `AT+ROI?` → verify defaults (full sensor)
3. `AT+ROI=0,0,1296,972` → full frame, capture, verify
4. `AT+ROI=552,390,192,192` → center crop, capture, verify content matches center of full frame
5. `AT+ROI=0,0,96,96` → top-left, verify
6. Invalid: `AT+ROI=2000,2000,100,100` → expect error, no crash
7. Rapid changes: 10 sequential ROI updates → verify stability

## Open Questions (need hardware or deeper code review)

1. **Crop coordinate space:** Are `INP_CROP_T` values in sensor pixels or after binning?  
   → Check `sensordplib_set_sensorctrl_inp_wi_crop_bin()` implementation

2. **Inclusive/exclusive:** Is `last_x` the last included pixel or one-past-end?  
   → Check sensordp library source or test on hardware

3. **Runtime reconfig:** Can we change crop without full sensor re-init?  
   → May need to stop/start stream; test stability

4. **JPEG vs raw:** Does ROI affect JPEG encoder input size?  
   → Verify output dimensions match expectations

## Phase 2 Preview (1280×720 mode)

After Phase 1 validates the ROI mechanism:

1. Create `OV5647_mipi_2lane_1280x720.i` register config
   - Base on 1296×972 config with adjusted timing
   - 1280×720 = 16:9 aspect (vs 4:3 for 1296×972)
   - Requires sensor register tuning — may need OV5647 datasheet

2. Add `OV5647_MIPI_1280X720` mode to `drv_ov5647.c`

3. Validate MIPI timing at 350 MHz (or adjust clock)

4. Use 720p as ROI source: larger sensor area → finer ROI granularity

**Note:** 1280×720 sensor config is the riskiest part — incorrect registers can hang the sensor. Test with full re-init cycle.

## Files to Modify (Phase 1)

1. `EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/callback/sensor.hpp` — add ROI callbacks
2. `EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/ma_server_at.cpp` — register ROI services  
3. `EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/drivers/camera/drv_ov5647.c` — apply ROI to crop
4. Possibly: `drv_ov5647.h` — add ROI setter API

**Do NOT modify:** `vendor/SSCMA-Micro` (read-only reference)

## Estimated Effort

- Implementation: 2-3 hours (code changes)
- Build/test cycle: 1 hour (build + image gen)
- Hardware validation: 1-2 hours (after flash approval)
- **Total:** ~1 day with hardware access
