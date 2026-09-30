# Deep dive: Watcher as AI input assistant

Researched 2026-09-22. Two research passes (hardware+examples, HID/vision/voice),
plus direct verification of the USB wiring against the OSHW schematic
(`vendor/oshw-watcher/Hardware/SenseCAP_Watcher_v1.0_SCH.pdf`).

## Hardware (verified from schematic + BSP)

| Block | Part / spec |
|---|---|
| Main MCU | ESP32-S3 @ 240 MHz, **8 MB octal PSRAM**, **32 MB flash** |
| AI co-processor | Himax HX6538 (Cortex-M55 + Ethos-U55 NPU), 16 MB own flash |
| Camera | OmniVision OV5647, 5 MP, 120° FOV, **fixed focus 3 m** — MIPI to **Himax only** |
| Display | SPD2010, 1.45" **412×412**, capacitive **multitouch**, QSPI 4-lane |
| Wheel | Quadrature encoder (GPIO41/42) + push button (IO expander) |
| Audio | ES8311 DAC → 1 W speaker; ES7243 ADC ← MEMS mic (I2S, GPIO15) |
| LED | 1× WS2812 (GPIO40) |
| IO expander | PCA9535 @ 0x21 — buttons, touch INT, all power rails |
| RTC | PCF8563 @ 0x51 — **hardware countdown timer 15 s–255 min** |
| Battery | 3.7 V 400 mAh — **5 V supply ONLY, >5 V destroys the board** |
| microSD | SPI, shares SPI2 bus with Himax link (arbitrate; don't use both at once) |
| USB-C (bottom) | **Dual USB-UART bridge only** (ch0: ESP32 UART0 console/flash w/ auto-reset, ch1: Himax UART). D+/D− go **only** to the bridge chip. Two COM ports enumerate. |
| USB-C (back) | Power only |
| Expansion | Grove I2C port + J2 2×4 header (I2C on GPIO47/48, **GPIO19/20**, 3V3, 5 V, GND) |
| IMU | **None** |

### Critical correction: native USB HID

The S3's native USB pins (GPIO19/20, labeled `USB_DM`/`USB_DP`) are **not**
connected to the USB-C port — both D+/D− pairs of the bottom port route through
22 Ω resistors straight into the dual UART bridge (the SDMM0806H mux footprint
on the schematic is marked DNI). GPIO19/20 instead break out to the **J2
expansion header** (with ESD protection). Consequences:

- **USB HID through the stock USB-C cable is impossible** without board rework.
- **USB HID via J2 is possible with a custom cable/dongle** (D−→GPIO19,
  D+→GPIO20, GND, 5 V all present on the header) — no trace cutting, just wiring.
  Stretch goal, not the primary path.
- **Zero-mod wired path:** the bottom port already gives a COM port; run a tiny
  host daemon that reads a serial command protocol from the Watcher and injects
  keystrokes/mouse via OS APIs. Same user outcome, no hardware touched.

### Camera topology (shapes everything)

The S3 has **no camera pins**. Frames and inference results reach the S3 only via
the SSCMA protocol over SPI from the Himax (frame sizes ≤ 640 px, JPEG). The
`qrcode_reader`, `bytetrack`, and `sscma_client_monitor` examples prove the path.
On-device OpenCV-style processing on the S3 is the wrong topology; the Himax
(Ethos-U55) is the vision engine, the S3 is the orchestrator, and heavy vision
belongs on the host.

## Per-feature verdicts

| Feature | Verdict | Notes |
|---|---|---|
| BLE HID keyboard+mouse+media | **PROVEN** | NimBLE-Arduino HIDS; pairs natively Win/macOS/Linux/iOS/Android; bond in NVS. Primary input path. |
| USB HID | **BLOCKED (stock port) / POSSIBLE (J2 dongle)** | See correction above. Serial+host-daemon covers wired with zero mods. |
| Screen as touchpad | **PROVEN path** | SPD2010 multitouch via `esp_lcd_touch`; map to relative HID mouse reports. |
| Wheel as scroll wheel | **TRIVIAL** | Encoder → HID scroll reports; push = middle-click or mode key. |
| Voice triggers ("timer 3 minutes") | **PROVEN** | ESP-SR: WakeNet wake word + MultiNet ≤300 custom commands; `speech_recognize` example exists. Numbers via chained mini-grammar ("start timer" + "three" + "minutes"). PCF8563 hardware timer fires it. |
| Camera "eyes on screen" → click a button | **FEASIBLE, host-assisted** | Loop: SSCMA still → HTTP POST → host (OpenCV quad-detect + warp, VLM/Muse grounding) → coords → BLE HID click. Stills at ~0.5–2 s cadence, not video. **Top risk: 3 m fixed focus** — test close-up legibility first. |
| Watch progress bar → click when done | **FEASIBLE** | Same loop, polling. Fine at still cadence. |
| Open terminal + type command after an action | **PROVEN** | BLE HID keyboard; sequencing in firmware or host agent. |
| Point-at-watcher gesture + voice | **FEASIBLE** | Hand/person detection runs on the **Himax** (that's its job; custom models via SenseCraft); S3 fuses "hand present" event with ESP-SR voice. Simple motion/blob triggers can run on the S3 directly. |
| Fine finger tracking on S3 | **RISKY** | Wrong topology — S3 never sees raw pixels. Keep fine vision on Himax or host. |
| Keep features on-device | **MOSTLY** | HID, UI, voice, timers, triggers on-device; only *visual decisions* need the host. |

## Biggest risks, in de-risk order

1. **OV5647 fixed 3 m focus vs. close-up screen legibility** — could kill precision clicking. Test with a still at arm's length before designing the vision loop.
2. **Still-image latency/quality through Himax→SPI→Wi-Fi** — measure round-trip of capture→POST→coords.
3. **MultiNet command-chaining UX** for parameterized phrases ("timer N minutes").
4. BLE+Wi-Fi coexistence throughput when streaming stills while connected as HID.

## Reference repos (cloned to `vendor/`)

- `vendor/watcher-firmware` — official ESP-IDF firmware + 15 examples. Key ones:
  `factory_firmware` (task-flow engine, LVGL, voice, OTA), `speech_recognize`
  (ESP-SR on-device), `qrcode_reader` / `bytetrack` / `sscma_client_monitor`
  (SSCMA camera path — the template for eyes-on-screen), `knob_rgb`,
  `lvgl_encoder_demo`, `openai-realtime` (needs git submodules, not fetched).
- `vendor/oshw-watcher` — schematics, HX6538 datasheet, 3D shell.
- BSP entry points: `components/sensecap-watcher/sensecap-watcher.c`
  (`bsp_lvgl_init`, `bsp_sscma_client_init`, `bsp_codec_init`,
  `bsp_i2s_read/write`, `bsp_rtc_*`, `bsp_io_expander_init`, `bsp_sdcard_init_default`).
- No Arduino/MicroPython support for the Watcher exists; ignore the XIAO-C6 demo
  in the OSHW repo (companion board, not Watcher firmware).
