# SSCMA-Micro Fork — Scoping Investigation

**Date:** 2026-09-24
**Goal:** Add (1) ROI/crop-zoom AT command and (2) 1280x720 frame support to the SenseCAP Watcher's Himax HX6538 firmware.
**Status:** Scoping only. No code modified. Vendor repos untouched.

---

## 0. TL;DR — the single most important finding

**Do NOT fork `vendor/SSCMA-Micro`.** That repo is only the portable SDK *library* (no build system, no board code, no sensor drivers). The actual buildable Watcher Himax firmware is a separate project:

**`https://github.com/seeed-studio/sscma-example-we2`** — "SSCMA Deployment on Himax WE-II"

It contains:
- `EPII_CM55M_APP_S/` — the full Himax WE2 SDK firmware tree (GNU Make build)
- `app/scenario_app/sscma/` — the SSCMA AT-server app (what the Watcher runs)
- `app/scenario_app/sscma/linker/watcher.ld` — **a dedicated `SENSECAP_WATCHER` build target** (`sscma.mk` line 71-73)
- `we2_image_gen_local/` — the firmware image generator (produces `output.img`, max 1MB)
- `xmodem/` — `xmodem_send.py` flash script
- OV5647 listed as a supported camera (`CIS_SUPPORT_INAPP_MODEL = cis_ov5647`)

**The fork target is `sscma-example-we2`, not `SSCMA-Micro`.** The `SSCMA-Micro` SDK is pulled in as `library/sscma_micro` (a submodule/copy) inside it.

---

## 1. Build system

### How it builds
- **Build system:** GNU Make with hierarchical `.mk` configuration (no CMake).
- **Entry:** `cd EPII_CM55M_APP_S && make` (select app via `APP_TYPE` in `makefile`; the SSCMA app is `APP_TYPE = sscma`).
- **Toolchain:** ARM GNU Toolchain (`arm-none-eabi-gcc`), tested versions `12.3.rel1` and `13.2.rel1` — downloadable from developer.arm.com. `TOOLCHAIN = gnu` is the default (`arm` compiler also supported with `.sct` linker scripts).
- **Output:** `obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf`
- **Image gen:** copy the `.elf` to `we2_image_gen_local/input_case1_secboot/`, run `./we2_local_image_gen project_case1_blp_wlcsp.json` → `output_case1_sec_wlcsp/output.img` (max 1MB). macOS/Windows variants exist.
- **RTOS:** FreeRTOS 10.5.1 (`OS_SEL := freertos_10_5_1`), TrustZone enabled (`TRUSTZONE := y`), single security domain.
- **Target selection:** `TARGET = SENSECAP_WATCHER` picks `app/scenario_app/sscma/linker/watcher.ld`; alternatives are `GROVE_VISION_AI_V2` (`grove.ld`) and `SENSECAP_A1102` (`a1102.ld`).

### Can we build it on this Linux machine?
**Yes.** Requirements: GNU Make, ARM GNU toolchain 12.3+/13.2+ (`arm-none-eabi-gcc`), Python 3 (for image gen + xmodem scripts), `git submodule update --init` (pulls CMSIS-CV and other submodules). No Windows-only tools in the loop. The earlier `git clone` of the repo timed out on network, but the repo is public and shallow-cloneable; retry with `--depth 1` on a better connection.

### Watcher-specific differences to verify after cloning
- Which `cis_sensor` config the Watcher target uses (expect `cis_ov5647`; the Grove V2 default may be `cis_imx219`).
- The `watcher.ld` memory layout vs `grove.ld` (SRAM partitioning for frame buffers).
- Whether the Watcher's SSCMA `porting/himax/we2` camera driver exposes `commandCtrl(kWindow)`.

---

## 2. Sensor configuration (OV5647)

### Where resolution is configured
Board-level sensor init lives in the app tree, per sensor:
```
EPII_CM55M_APP_S/app/scenario_app/<app>/cis_sensor/cis_ov5647/
├── OV5647_mipi_2lane_2592x1944.i   # full 5MP register init sequence
├── OV5647_mipi_2lane_640x480.i     # VGA register init sequence (what Watcher uses today)
├── cisdp_cfg.h                      # datapath pipeline config (crop/binning/subsample/JPEG)
├── cisdp_sensor.c / .h              # sensor bring-up code
```
(Paths from `HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2`, same tree layout as `sscma-example-we2`.)

**There is no 1280x720 (720p) sensor config today.** Only 2592x1944 and 640x480 `.i` register files exist. Adding 720p means writing a new `OV5647_mipi_2lane_1280x720.i` register sequence (the OV5647 datasheet documents the 720p mode registers; the existing `.i` files are the template).

### The datapath pipeline (`cisdp_cfg.h`) — this is where crop-zoom lives
```
SENSOR --> INP_CROP --> INP_BINNING --> INP_SUBSAMPLE --> HW2x2 --> CDM --> HW5x5 (demosaic) --> JPEG encoder
```
Relevant defines in `cisdp_cfg.h` (current Watcher values):
- `DP_INP_CROP_START_X/Y` = 0,0 and `DP_INP_CROP_WIDTH/HEIGHT` = 640x480 — **hardware crop stage, currently full-frame**. Setting these to an ROI (e.g. 320x240 at some offset) gives **zero-cost hardware crop-zoom**.
- `OV5647_SUPPORT_BINNING` is defined → sensor runs binned 640x480 mode.
- Non-binning alternative in the same file: sensor 2592x1944 → INP crop 2592x1944 → INP out 648x486 → HW2x2 crop 640x480.

### Where the SSCMA SDK sees resolution
- `AT+SENSOR=1,1,<OPT_ID>` → `configureSensor()` in `sscma/server/at/callback/sensor.hpp` → `camera->init(opt_id)`. The OPT_ID → resolution mapping is implemented in the **board-level** camera driver (`library/sscma_micro/porting/himax/we2/...`), not in the portable SDK.
- The SDK's `Camera::CtrlType` enum (`sscma/porting/ma_camera.h`) already defines `kWindow`, `kZoom`, `kPan`, `kTilt`, and `kRegister` — the abstraction for ROI control exists, but **no AT command binds to `commandCtrl()`** in the current SDK. Our fork adds that binding.

---

## 3. Frame pipeline (SDK side — `vendor/SSCMA-Micro`)

### AT+SAMPLE (JPEG snapshot)
`sscma/server/at/callback/sample.hpp`:
1. `camera->startStream(kRefreshOnReturn)`
2. `camera->retrieveFrame(frame, MA_PIXEL_FORMAT_JPEG)` — the **hardware JPEG encoder** produces the JPEG (this is why we're capped — see §4)
3. base64-encode into `_buffer`; if `MA_SENSOR_ENCODE_USE_STATIC_BUFFER` is set, the JPEG's base64 size must fit `MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE` or the sample **fails** (`"buffer_size > MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE"`)
4. `camera->returnFrame(frame)`

### AT+INVOKE (model inference)
`sscma/server/at/callback/invoke.hpp`, `eventLoopCamera()`:
1. `camera->retrieveFrame(raw_frame, MA_PIXEL_FORMAT_AUTO)` — raw frame (RGB565/YUV)
2. Optionally also grabs a JPEG for the event reply (same static-buffer limit applies)
3. `_algorithm->run(&raw_frame)` → `Detector::preprocess()` (`sscma/core/model/ma_model_detector.cpp:34`) → `ma::cv::convert(input_img_, &img_)` (`sscma/core/cv/ma_cv.cpp:1067`)
4. `convert` → `rgb_to_rgb` → e.g. `rgb888_to_rgb888` (`ma_cv.cpp:92`) which does a **nearest-neighbor full-frame stretch** from sensor frame to model input size (e.g. 640x480 → 192x192). No letterbox, no crop — the whole frame is squashed.

### Where to insert ROI crop (ranked)
1. **Best — hardware INP_CROP via new AT command** (`AT+ROI=x,y,w,h` or `AT+WINDOW=...`): binds to `Camera::commandCtrl(kWindow, kWrite, ...)` → board driver reprograms `DP_INP_CROP_*` (or calls the Himax `sensordp` INP crop API at runtime). Zero CPU cost, zero extra memory, applies to both SAMPLE and INVOKE paths. Requires the sensor to be in a higher-res mode than the crop (e.g. 2592x1944 or 1280x720) for the crop to actually gain detail.
2. **Good — SDK preprocess crop:** in `Detector::preprocess()` (`ma_model_detector.cpp:34`), crop an ROI from `input_img_` into a temp buffer before `convert()`. Portable, applies to all detector models, small CPU cost (one extra crop pass on M55 with Helium). Affects INVOKE only.
3. **Fallback — host-side crop (no Himax changes):** crop the 640x480 JPEG on our side and re-run. Already possible today; documented as Option A in the parent conversation.

---

## 4. Memory layout — can it hold 1280x720?

From the HX6538 datasheet (`vendor/oshw-watcher/Hardware/HX6538_datasheet.pdf`, §1-2):
- **2.5 MB internal ultra-low-leakage SRAM** ("Configurable system memory, up to 2432KB" + 64KB boot ROM)
- Dual Cortex-M55 (Big 400MHz w/ 256KB ITCM + 256KB DTCM; Little 150MHz), Ethos-U55 @ 400MHz, 64 MACs
- **Hardware accelerators: "Image crop, sub-sampling, and binning" + hardware JPEG codec** — crop-zoom is a hardware feature
- External QSPI Flash 1–16MB (Watcher has 16MB for Himax per user guide)
- **Hardware JPEG encoder limit: MAX 640x640** (from `DP_JPEG_CFG` in `cisdp_cfg.h`: "MAX:640X640, MIN:16X16, WIDTH/16, HEIGHT/16")

### 1280x720 buffer math vs 2.5MB SRAM
| Format | 1280x720 size | Fits? |
|---|---|---|
| RGB565 | 1.76 MB | Yes, but tight alongside TFLM arena + firmware |
| RGB888 | 2.76 MB | **No** — exceeds total SRAM |
| YUV420 (HW5x5 output) | 1.38 MB | Yes |
| JPEG (typical) | ~100–200 KB | Yes |

### Consequences for the 720p plan
1. **720p JPEG via hardware encoder is impossible** — the HW JPEG block caps at 640x640. 720p snapshots need either:
   - **Software JPEG encode** — `JPEGENC` is already vendored in `SSCMA-Micro/3rdparty/JPEGENC` and `ma_cv.cpp` has `rgb_to_jpeg()` gated on `MA_USE_LIB_JPEGENC`. Feasible on the M55, slower than HW.
   - Or stream YUV/RGB565 raw (1.38–1.76MB per frame over UART/SPI — slow, but workable for stills).
2. **720p for INVOKE (model input):** don't buffer full 720p RGB888. Better pipeline: sensor 1280x720 (new `.i` file) → **INP_CROP to ROI** (hardware) → HW5x5 → model. The model never sees the full frame; SRAM only holds the crop + tensor arena.
3. **`MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE`** (the JPEG base64 staging buffer) must be sized for the largest JPEG we emit. It's a board-level define — check/raise it in the fork when enabling larger frames. (Not present in the portable SDK; defined in the we2 port.)
4. The 1MB `output.img` limit is for firmware flash image size, not RAM — unrelated to frame buffers.

---

## 5. Flashing — how the Himax gets new firmware

### Tools (from `sscma-example-we2`/CLAUDE.md)
1. **XMODEM over UART:** `python3 xmodem/xmodem_send.py --port=/dev/ttyACM0 --baudrate=921600 --protocol=xmodem --file=output.img [--model="model.tflite 0xB7B000 0x00000"]`
2. **`himax-flash-tool -d WiseEye2 -f <img>`** (alternative)
3. `we2_local_image_gen` produces the flashable `output.img` from the `.elf` (secure-boot image format)

### Watcher hardware path (from `vendor/oshw-watcher/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf` → `sch.txt`)
- ESP32-S3 ↔ Himax links: `ESP_GPIO5/SPI2_MOSI`, `ESP_GPIO4/SPI2_SCK`, `ESP_GPIO21/Himax_SPI_CS`, **`ESP_GPIO17/UART1_TX`**, **`ESP_GPIO18/UART1_RX`** (Himax side: `PB6/UART1_RX`, `PB7/UART1_TX`)
- **Dedicated "Himax download" header J1** (2.54mm pitch) on the PCB with a **UART SWITCH** — the Himax UART can be switched between the ESP32-S3 and the external download header
- `HIMAX_RST` (reset), `BOOTOPT0` test point, `EXP_Himax_EN` / `EXP_Himax_Rst` (expander-controlled power/reset)
- The SSCMA AT protocol itself supports XMODEM (`xmodem_send.py --model=...` flashes models to e.g. `0xB7B000`/`0x00400000`); firmware images go through the ROM bootloader via UART.

### Bricking risk: MEDIUM-HIGH, but recoverable
- **Good news:** the HX6538 has a ROM bootloader (`64KB boot ROM` per datasheet) and a physical download header + BOOTOPT strapping. A bad app image should be recoverable via UART XMODEM as long as the ROM bootloader is intact.
- **Bad news:** secure boot is enabled in the SSCMA build (`TRUSTZONE := y`, `TRUSTZONE_FW_TYPE := 1`, `we2_local_image_gen` makes `*_secboot` images). If the Watcher's eFuses enforce secure boot with Seeed's key, **our self-signed image may not boot**. This is the #1 risk to verify before investing heavily: check whether `we2_image_gen_local` in the repo can produce an image the Watcher's bootloader accepts, or whether we need the non-secure `project_case1_blp` (non-secboot) config.
- **Safety net:** the factory `himax_firmware_20240816.img` is published in `vendor/oshw-watcher/Firmware/Himax/` — we have a known-good image to flash back.
- **Recommended procedure:** first flash a no-op rebuild of the unmodified fork and confirm it boots + responds to AT commands before changing anything functional.

---

## 6. Proposed fork work breakdown

### Phase 0 — Reproduce the build (1–2 days)
1. Shallow-clone `seeed-studio/sscma-example-we2`, `git submodule update --init`
2. Install ARM GNU toolchain 13.2.rel1
3. `cd EPII_CM55M_APP_S && APP_TYPE=sscma TARGET=SENSECAP_WATCHER make` (confirm exact var names in `makefile`)
4. Generate `output.img` via `we2_image_gen_local`, compare size/structure with factory `himax_firmware_20240816.img`
5. **Secure-boot question:** determine if self-built image boots on the Watcher

### Phase 1 — ROI/crop-zoom AT command (2–4 days)
1. Add `addService("ROI", ...)` (or `WINDOW`) in the we2 port's AT server (mirrors `ma_server_at.cpp` pattern in the SDK)
2. Bind to `Camera::commandCtrl(kWindow, kWrite, value)`; implement the board-level handler in `library/sscma_micro/porting/himax/we2/` to reprogram the INP crop registers at runtime (`sensordp` INP APIs)
3. Verify: `AT+ROI=100,100,320,240` → subsequent `AT+SAMPLE` returns the cropped region
4. S3-side: expose ROI in our `sscma_client` fork

### Phase 2 — 1280x720 sensor mode (3–5 days)
1. Write `OV5647_mipi_2lane_1280x720.i` register init (template from the 640x480 `.i` + OV5647 datasheet 720p mode)
2. New `cisdp_cfg.h` variant: sensor 1280x720 → INP crop 1280x720 → HW5x5 → **software JPEG** (`MA_USE_LIB_JPEGENC`) since HW JPEG caps at 640x640
3. Raise `MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE` for ~200KB JPEGs
4. Validate SRAM budget: 720p YUV420 (1.38MB) + TFLM arena + JPEG working buffers < 2432KB

### Phase 3 — Integration & hardening
1. New `AT+SENSOR` OPT_ID for the 720p preset (so ESP32-S3 can switch 640x480 ↔ 720p at runtime)
2. Combine: 720p sensor mode + hardware ROI crop → effective zoom up to ~4x with full sensor detail on the model
3. Flash procedure doc for John (download header vs ESP32-S3 UART passthrough)

---

## 7. Open questions (need the actual repo / hardware to answer)
1. Does the Watcher's ROM bootloader accept self-signed (non-Seeed-key) secure-boot images? **Go/no-go for the whole project.**
2. Does the we2 port's camera driver implement `commandCtrl(kWindow)`, or is it a stub?
3. Exact SRAM partition in `watcher.ld` — how much is actually free for a 720p frame + TFLM arena?
4. Can the ESP32-S3 put the Himax into UART download mode via GPIO (HIMAX_RST + BOOTOPT), or is the physical J1 header required?
5. Which `cis_sensor` dir + `cisdp_cfg.h` the `SENSECAP_WATCHER` target actually compiles (vs Grove V2 defaults).

## Key file paths (for the fork)
- Repo: `https://github.com/seeed-studio/sscma-example-we2`
- App: `EPII_CM55M_APP_S/app/scenario_app/sscma/` (`sscma.mk`, `linker/watcher.ld`)
- SSCMA lib: `EPII_CM55M_APP_S/library/sscma_micro/` (= `SSCMA-Micro` SDK + `porting/himax/we2/`)
- Sensor: `EPII_CM55M_APP_S/app/scenario_app/<app>/cis_sensor/cis_ov5647/` (`.i` files, `cisdp_cfg.h`)
- AT services: SDK `sscma/server/at/ma_server_at.cpp` (`addService("SAMPLE"...`, line 234; `addService("INVOKE"...`, line 242); callbacks in `sscma/server/at/callback/{sample,invoke,sensor}.hpp`
- Preprocess: `sscma/core/model/ma_model_detector.cpp:34` → `sscma/core/cv/ma_cv.cpp:1067` (`convert`), `:92` (`rgb888_to_rgb888` full-frame stretch)
- Camera abstraction: `sscma/porting/ma_camera.h` (`CtrlType::kWindow/kZoom/kRegister`, `commandCtrl`)
- Image gen: `we2_image_gen_local/`; flash: `xmodem/xmodem_send.py`
- Factory recovery image: `vendor/oshw-watcher/Firmware/Himax/himax_firmware_20240816.img`
- HX6538 datasheet: `vendor/oshw-watcher/Hardware/HX6538_datasheet.pdf` (2.5MB SRAM, HW crop/binnning/JPEG, 64KB boot ROM)
- Watcher schematic: `vendor/oshw-watcher/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf` (Himax download header J1, UART switch, ESP32-S3↔Himax pin mapping)
