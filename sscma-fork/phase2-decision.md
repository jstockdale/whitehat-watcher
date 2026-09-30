# SSCMA Fork — Phase 2 Decision Record

**Date:** 2026-09-24
**Decision:** Keep 1296×972 sensor mode. Do NOT implement native 1280×720.

## Reasoning (John, 2026-09-24)

1296×972 (1,259,712 px) is strictly better resolution than 1280×720
(921,600 px) — 37% more pixels. The 16:9 aspect ratio benefit of 720p
does not justify losing over a third of the pixels, especially when
Phase 1's `AT+ROI` hardware crop can produce any 16:9 window on demand
(e.g. `AT+ROI=8,126,1280,720` for centered 720p from the 1296×972 frame).

Cropping or downsampling can always be done later in software or via
the WE2's INP — no sensor register risk required.

## Consequences

- **Phase 2 (native 720p sensor mode) is CANCELLED.** The
  `OV5647_mipi_2lane_1280x720.i` register file will not be created.
- The risky sensor-register work (PLL, timing, windowing for a new
  mode) is avoided entirely.
- Current sensor configuration remains:
  - Mode: `OV5647_MIPI_1296X972`
  - MIPI clock: 350 MHz, 2 lanes
  - `OV5647_SENSOR_WIDTH=1296`, `OV5647_SENSOR_HEIGHT=972`
- For 16:9 output, use `AT+ROI` to crop (Phase 1 mechanism).

## What's next

Bring-up plan for flashing the Himax WE2 (research in progress):
1. Flashing procedure (tool, protocol, steps)
2. Recovery / unbrick procedure
3. Factory image backup
4. Validation sequence (v0.1 baseline → v0.2_roi)

**Status of images:**
- `watcher_sscma_v0.1.img` (624 KB) — baseline, unflashed
- `watcher_sscma_v0.2_roi.img` (643 KB) — Phase 1 ROI, unflashed
- No Phase 2 image will be built.
