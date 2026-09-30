# Phased plan

## Phase 0 — De-risk (an afternoon; do FIRST, in this order)

1. **Toolchain**: install ESP-IDF ≥5.2, build `vendor/watcher-firmware/examples/get_started`, flash via bottom USB-C. Confirm serial console.
2. **Focus test**: run `sscma_client_monitor`, point the Watcher at a monitor from arm's length, pull a still. **Go/no-go for precision clicking**: is UI text legible? If too soft, eyes-on-screen becomes "coarse region" only (still useful for progress bars, not small buttons).
3. **BLE HID PoC**: minimal NimBLE HIDS firmware — pair to John's laptop, move cursor, click, type. Confirms the whole input path end-to-end.
4. **Voice PoC**: build `speech_recognize`, test wake word + 2–3 custom commands in John's room (noise reality check).
5. **Still round-trip**: time SSCMA capture → Wi-Fi POST → host. Sets expectations for polling cadence.

Exit criteria: focus acceptable-or-scoped, HID moves a cursor, wake word fires reliably, still round-trip measured.

## Phase 1 — Core HID device

- ESP-IDF app skeleton on BSP components; LVGL mode UI.
- NimBLE composite HIDS (keyboard/mouse/consumer/battery), bonding, reconnect.
- Touchscreen → relative touchpad (tap = click, two-finger = right-click).
- Wheel → scroll; wheel-click cycles modes; knob button = middle-click in HID mode.
- Mode state machine; RGB LED status.

## Phase 2 — Voice + timers

- ESP-SR integration: wake word ("Hey Watcher" custom) + MultiNet command table.
- Number-word chaining grammar: "start timer" → "three" → "minutes".
- PCF8563 countdown timer → speaker chime + RGB + optional HID action.
- Speaker TTS confirmations ("Timer started, three minutes").

## Phase 3 — Eyes on screen

- `eyes/` module: on-demand SSCMA still → POST to host agent.
- Host `vision.py`: quad detect, dewarp, target grounding (template match → Muse VLM fallback).
- Normalized coords back → BLE HID move+click.
- LVGL overlay: crosshair / "looking…" status; still preview on the round screen.

## Phase 4 — Autonomous triggers

- Trigger engine: fuse voice cmds, Himax person/hand events, RTC, polling loops.
- "Watch this progress bar, click when done" (host polling loop).
- Macro sequences: "open terminal, type X when Y finishes" (HID keyboard scripting from host or device).
- Point-at-watcher + voice combo via Himax hand events.

## Stretch

- USB HID dongle cable via J2 header (GPIO19/20) for a wired-HID option.
- Custom Himax models via SenseCraft (finer gestures, e.g. pointing-finger vector).
- microSD macro library; OTA updates.

## Build discipline

- One firmware repo, ESP-IDF components; `vendor/` stays pristine (reference only).
- Every phase ends with a flash-and-demo on real hardware before the next begins.
- Keep a `docs/demo-log.md` — what was tried, what worked, measurements (still latency, focus distance, battery impact of BLE+Wi-Fi).
