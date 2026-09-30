# Firmware — Phase 0 PoC (`watcher_input_poc`)

ESP-IDF v5.3 application that turns the Watcher into a **BLE HID keyboard/mouse**
("Watcher-HID") with a **JSON command link** over the USB-serial port and a
**camera still** path (Himax → Wi-Fi POST → host agent).

## What it does

- `hid_ble` — NimBLE composite HIDS: keyboard (report 1), mouse (report 2),
  consumer control (report 3), battery service. Just-Works bonding, bonds in NVS.
- `link` — newline-delimited JSON over the USB-serial console (115200).
  Commands: `ping`, `type`, `key`, `mouse_move`, `mouse_wheel`, `mouse_click`,
  `consumer`, `wifi`, `snap`. Events: `ready`, `pong`, `ble`, `wifi`,
  `still_sent`, `ok`/`err`. Full schema: `../host-agent/protocol.md`.
- `net` — Wi-Fi station; credentials saved to NVS via `{"t":"wifi",...}`.
- `eyes` — SSCMA snapshot (Himax HX6538, 416×416 JPEG) → HTTP POST to the
  host agent's still receiver. Best-effort: the firmware runs fine without it.

Note: the JSON link **shares UART0 with the IDF console**. Log lines and JSON
lines interleave on TX; the host agent ignores non-JSON lines, and `idf.py
monitor` stays usable for debugging. (A dedicated link channel is Phase 1+ work.)

`vendor/` is referenced read-only (`EXTRA_COMPONENT_DIRS`); it is never modified.

## Prerequisites

- ESP-IDF **v5.3** (matches the official Watcher firmware)
- Watcher connected to the **bottom** USB-C port (data), rear port is power-only
- 5 V supply only — do not feed the board more than 5 V

### Install ESP-IDF

**Ubuntu:**
```bash
sudo apt install git wget flex bison gperf python3 python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf && ./install.sh esp32s3
```

**macOS:**
```bash
# with Homebrew: brew install cmake ninja dfu-util ccache
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf && ./install.sh esp32s3
```

Then, in every new shell:
```bash
. ~/esp-idf/export.sh
```

## Build, flash, monitor

```bash
cd firmware
. ~/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
# Linux:  port is usually /dev/ttyUSB0 (or /dev/ttyACM0); add yourself to `dialout`
# macOS:  port looks like /dev/tty.usbserial-XXXX
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
```

On first boot you should see `{"t":"ready","fw":"0.1.0",...}` among the logs.

## Try it

1. **Pair BLE:** on the host, pair with **Watcher-HID** (no PIN — Just Works).
2. **Drive it** with the host agent (recommended):
   ```bash
   cd ../host-agent
   pip install -r requirements.txt
   python3 watcherd.py console --port /dev/ttyUSB0
   # type hello world | key enter ctrl | mmove 100 -50 | snap 192.168.1.50
   ```
3. Or talk raw JSON in any serial terminal at 115200 8N1:
   ```json
   {"t":"ping"}
   {"t":"type","text":"hello from watcher"}
   {"t":"key","code":"enter"}
   {"t":"mouse_move","dx":120,"dy":-40}
   {"t":"snap","host":"192.168.1.50","port":8099,"id":1}
   ```
   Set Wi-Fi once (persisted in NVS): `{"t":"wifi","ssid":"MyNet","pass":"secret"}`.
   Run the still receiver first: `python3 watcherd.py still-server --port 8099`.

## Troubleshooting

- **Build can't find `esp_io_expander`**: it's fetched from the component
  registry on first build — make sure you're online for that build.
- **No `ready` line**: check the port, and that you're on the *bottom* USB-C.
- **BLE pairs but no keystrokes**: the host must subscribe to the HID report
  notifications (all major OSes do automatically after pairing); check
  `{"t":"ble","state":"connected"}` arrived.
- **snap fails with "wifi not connected"**: send the `wifi` command first and
  wait for the `wifi/connected` event.
- **snap fails with "no image from Himax"**: the Himax firmware/model may not
  be flashed on your unit — reflash the stock Himax firmware via the official
  `sscma_client_flasher` tooling, then retry.

## Next (Phase 1)

Touch-as-touchpad + wheel-as-scroll via the BSP touch/knob drivers, LVGL mode
UI, and the link moving off the shared console UART.
