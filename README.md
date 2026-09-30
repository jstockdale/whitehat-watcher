# Watcher Input Assistant

Turn a Seeed Studio SenseCAP Watcher (ESP32-S3) into an AI-driven input assistant:
BLE/USB HID keyboard+mouse, touchscreen-as-touchpad, wheel-as-scroll-wheel,
on-device voice triggers, and camera "eyes on the screen" with a host AI agent
(Muse) doing the visual decisions.

## Docs

- `docs/00-deep-dive.md` — hardware findings + per-feature feasibility verdicts
- `docs/01-architecture.md` — system architecture (firmware, host agent, protocol)
- `docs/02-phased-plan.md` — phased build plan with de-risking order
- `docs/03-feature-backlog.md` — John's feature ideas mapped to phases

## Layout

- `vendor/` — read-only reference clones (never build from here directly)
  - `vendor/oshw-watcher/` — schematics, HX6538 datasheet, 3D shell
  - `vendor/watcher-firmware/` — official ESP-IDF firmware + examples (our BSP source)
- `firmware/` — our ESP-IDF app (planned; see `firmware/README.md`)
- `host-agent/` — Python host daemon: vision, OS input injection, Muse bridge (planned)

## Ground rules

- Firmware base: **ESP-IDF** (≥5.2), forked from `vendor/watcher-firmware` BSP components.
  No Arduino port of the Watcher exists; all peripheral drivers live in the ESP-IDF BSP.
- **Never power the board above 5V.** Back USB-C is power-only; bottom USB-C is the
  data/flash port (dual USB-UART bridge: ch0 = ESP32 console+flash, ch1 = Himax).
- Don't touch Himax firmware unless a custom AI model is needed — flash ESP32 only.
- The camera belongs to the Himax HX6538; the S3 only receives frames via SSCMA/SPI.
  Design vision features host-assisted from the start.
