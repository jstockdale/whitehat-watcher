# SSCMA Fork — Phase 0 Report: Reproducible Build

**Date:** 2026-09-24  
**Status:** ✅ COMPLETE — Unmodified firmware builds reproducibly, image generation works

## What Was Built

Two firmware variants compiled and linked successfully:

1. **Grove Vision AI V2** (default target, `MA_BOARD_GROVE_VISION_AI_V2`)
   - ELF: 12MB, 458KB text, 1.9MB total
   - Image: 624KB with `ckBS` magic matching factory format

2. **SENSECAP Watcher** (`MA_BOARD_WATCHER`)
   - ELF: 12MB (rebuilt with Watcher board define)
   - Image: `watcher_sscma_v0.1.img` (624KB)
   - SHA256: `0a4bb76f85b241e0fd7a53775556dda32935c5932021bc86d005fba2bf57cfb4`

## Toolchain

- **ARM GNU Toolchain 13.2.rel1** (arm-none-eabi-gcc 13.2.1)
- Location: `~/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/`
- User-space install, no sudo required

## Critical: Submodule Version Pinning

The tarball lacked git history. Correct versions recovered via GitHub API:

| Submodule | Pinned Commit | Source |
|-----------|---------------|--------|
| SSCMA-Micro | `43332b4297548c049609f0bea03590bb80745843` | GitHub API submodule metadata |
| cJSON | `12c4bf1986c288950a3d06da757109a6aa1ece38` | GitHub API submodule metadata |
| littlefs | `d01280e64934a09ba16cac60cf9d3a37e228bb66` | GitHub API submodule metadata |

**Lesson:** Using SSCMA-Micro `main` fails with FreeRTOS API incompatibility (`uxQueueGetQueueLength`, `uxQueueSpacesAvailableFromISR` not in bundled FreeRTOS 10.5.1). Always use pinned commits.

## Build Commands

```bash
# Set toolchain
export PATH="$HOME/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin:$PATH"
export GNU_TOOLPATH="$HOME/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin/"

# Build
cd sscma-example-we2-main/EPII_CM55M_APP_S
make -j$(nproc)

# Generate image
cd ../we2_image_gen_local
cp ../EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf input_case1_secboot/
./we2_local_image_gen project_case1_blp_wlcsp.json
# Output: output_case1_sec_wlcsp/output.img
```

## Watcher Board Selection

The build system hardcodes `MA_BOARD_GROVE_VISION_AI_V2` in:
- `EPII_CM55M_APP_S/library/sscma_micro_porting/sscma_micro_porting.mk`

Changed to `MA_BOARD_WATCHER` for Watcher builds. This selects:
- `porting/himax/we2/boards/ma_board_watcher.h`
- Watcher-specific pin mappings, sensor config

## Image Format Verification

Generated images match factory format:
- Magic: `ckBS` (0x63 0x6b 0x42 0x53)
- Header: `00 00 01 00 52 00 00 00...`
- Factory image: 536KB, Generated: 624KB (size difference expected — different app content)

## Secure Boot / eFuse Findings

**No eFuse-burning code found** in the repository build system.

Evidence:
- `grep -ri "efuse"` found zero matches in build scripts, Makefiles, Python tools
- Image generation (`we2_local_image_gen`) signs images but does not program eFuses
- The `secureboot_tool/` directory contains certificate generation, not eFuse programming

**Interpretation:** The build signs images in the secure-boot format, but nothing in the provided tooling burns eFuses. This supports (but does not prove) John's hypothesis that Seeed did not burn secure-boot eFuses on the Watcher.

**Definitive test:** Flash a self-built image and observe boot behavior. NOT authorized in Phase 0.

## Camera Findings (for Phase 1/2)

### Available Sensor Modes
Driver: `drv_ov5647.c` supports three compile-time modes:
- `OV5647_MIPI_640X480` (220 MHz MIPI clock)
- `OV5647_MIPI_1296X972` (350 MHz) — **currently selected default**
- `OV5647_MIPI_2592X1944` (350 MHz)

**No 1280x720 mode exists** — Phase 2 will create one.

### Hardware Crop (Phase 1 target)
The datapath configuration documents:
```
SENSOR --> INP_CROP --> INP_BINNING --> INP_SUBSAMPLE
```

Driver uses `INP_CROP_T crop` structure passed to `sensordplib_set_sensorctrl_inp_wi_crop_bin()`. Currently zeroed (no crop). **This is the Phase 1 hook point.**

### AT Command Structure
Commands registered in `ma_server_at.cpp` via `addService()`. Sensor commands in `callback/sensor.hpp`:
- `AT+SENSORS?` — list sensors
- `AT+SENSOR?` — current sensor status  
- `AT+SENSOR=id,enable,opt_id` — configure sensor

**Phase 1 will add:** `AT+ROI=x,y,w,h` and `AT+ROI?`

## Reproducibility Notes

1. Tarball (not git clone) used due to network timeouts — no submodule history
2. Submodule commits recovered from GitHub API — document these in build script
3. Toolchain must be 13.2.rel1 (newer may work, untested)
4. Build takes ~7 minutes on multi-core (318 object files)
5. `make` doesn't track `.mk` changes — manual clean needed after board define change

## Files Produced

- `watcher_sscma_v0.1.img` — Watcher firmware image (624KB)
- Build tree: `sscma-example-we2-main/EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/`
- Generated image: `sscma-example-we2-main/we2_image_gen_local/output_case1_sec_wlcsp/output.img`

## Next: Phase 1

See `phase1-roi-design.md` for the AT+ROI implementation plan.
