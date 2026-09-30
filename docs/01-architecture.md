# Architecture

## Topology

```
                    +------------------ WATCHER (on-device) ------------------+
                    |  ESP32-S3 (ESP-IDF)            |  Himax HX6538          |
                    |  - hid: NimBLE HIDS           |  - person/hand detect  |
                    |    (kbd/mouse/consumer)       |  - (custom models via  |
                    |  - ui: LVGL 412x412 + touch   |    SenseCraft, later)  |
                    |  - voice: ESP-SR (wake+cmds)  |  - camera frames       |
                    |  - camera: SSCMA client       +--------> S3 over SPI   |
                    |  - triggers: RTC, wheel,      |                        |
                    |    touch, Himax events        |                        |
                    |  - link: GATT service +       |                        |
                    |    Wi-Fi HTTP client          |                        |
                    +------BLE HID-----> HOST PC <----Wi-Fi/serial----------+
                               |                ^
                    keyboard/mouse reports      | stills, events, voice cmds
                                                v
                              +---------------------------------- HOST ------+
                              | agent daemon (Python):                       |
                              |  - vision: screen quad-detect, warp,         |
                              |    button grounding (OpenCV + Muse)          |
                              |  - fallback input injection (serial path)    |
                              |  - Whisper fallback STT                      |
                              |  - Muse bridge: decisions in, coords out     |
                              +----------------------------------------------+
```

## Design decisions

1. **BLE HID is the input path.** Composite HIDS: keyboard (report 1), mouse
   (report 2), consumer control (report 3), battery service. Pairing with
   `ESP_LE_AUTH_BOND`; bonds persist in NVS.
2. **Firmware base: ESP-IDF**, reusing `vendor/watcher-firmware` BSP components
   (display/touch, codec, SSCMA client, expander, RTC). Greenfield modules are
   only: HID, host-link protocol, trigger engine.
3. **Modes, one at a time** (wheel-click or screen button cycles):
   - `HID` — touchpad + wheel-scroll + voice commands active
   - `EYES` — camera stills to host; overlay status on screen
   - `TIMER` / `MACRO` — quick actions
   Keeping one HID report source active avoids double-injection.
4. **Vision is host-assisted by design.** The S3 never does pixel CV. Himax =
   always-on eyes (person/hand events); host = visual decisions; S3 = hands.
5. **Voice on-device** (ESP-SR, PSRAM-resident models, ~6 MB flash partition).
   Host STT is fallback only — latency and privacy cost otherwise.
6. **Wired path without hardware mods:** line-based JSON protocol over the
   existing USB-UART COM port; host daemon translates to OS input events.
   Same command set as the BLE GATT service.

## Firmware module sketch (`firmware/`)

- `main/` — app entry, mode state machine, trigger engine (fuses RTC, Himax
  events, voice command IDs, wheel/touch gestures into actions)
- `components/hid_ble/` — NimBLE HIDS composite device
- `components/voice/` — ESP-SR wrapper: wake word + MultiNet command table +
  number-word chaining grammar
- `components/eyes/` — SSCMA still capture → JPEG → HTTP POST to host;
  result (coords) → HID click
- `components/link/` — GATT custom service (commands/events) + Wi-Fi provisioning
  reuse from factory firmware
- `components/ui/` — LVGL screens per mode (reuse BSP `bsp_lvgl_init`)
- Reuse BSP for: display/touch, codec/I2S, expander, RTC, SD, RGB, power.

## Host agent sketch (`host-agent/`)

- `watcherd` — single daemon:
  - transports: BLE GATT client, serial JSON, HTTP receiver for stills
  - `vision.py` — screen quad detect → `warpPerspective` → target grounding
    (template match for known UI; VLM call to Muse for "click the X button")
  - `inject.py` — OS input injection (fallback / serial-path execution)
  - `triggers.py` — polling loops ("watch this progress bar, click when done")
- Muse drives decisions: the daemon exposes "see screen" / "click at x,y" /
  "type text" tools; Muse does the reasoning.

## Protocol (v0 sketch)

Serial/GATT line protocol, JSON, newline-delimited:
- `W->H {"t":"still","id":12,"jpeg":"<base64 or chunked>"}` / `H->W {"t":"click_at","id":12,"x":0.42,"y":0.61}` (normalized coords)
- `W->H {"t":"voice","cmd":"start_timer","args":{"n":3,"unit":"min"}}`
- `W->H {"t":"event","name":"hand_present"}` / `{"t":"progress","id":7,"pct":82}`
- `H->W {"t":"type","text":"..."}`, `{"t":"key","code":"ctrl+alt+t"}`, `{"t":"say","text":"..."}`
Keep it line-delimited JSON so the serial path and a TCP socket are interchangeable.
