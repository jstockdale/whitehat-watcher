# host-agent/ — Phase 0

Python daemon/CLI (`watcherd`) running on John's PC/laptop. It is the Watcher's
"brain extension": visual decisions, OS-level input injection fallback, and the
bridge a higher-level AI (e.g. Muse) drives.

## Where this fits

```
Watcher firmware (ESP32-S3)          this host agent (watcherd)              host OS
─────────────────────────          ──────────────────────────              ───────
BLE HID keyboard/mouse ──────────────► pairs directly, no agent needed ───► ◉ input
USB serial: JSON protocol ◄────────► transport_serial.py / console / type
Himax camera: snap ──HTTP POST───► still_server.py ──► vision.py ──► inject.click_at
```

- **Device = hands and eyes.** It owns BLE HID reports, the touchscreen,
  the wheel, the camera, and timers.
- **Agent = brain extension.** It receives stills, detects/dewarps screens,
  (Phase 3+) grounds button targets, and can inject input on the host itself
  as a fallback when BLE HID is unavailable.

## Setup

Requires Python 3.10+.

### Ubuntu Linux

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Serial port: the Watcher appears as `/dev/ttyUSB0` or `/dev/ttyACM0`.
Add your user to the `dialout` group (log out/in afterwards) so you can open it
without sudo:

```bash
sudo usermod -aG dialout $USER
```

Input injection needs a running X session (X11). On Wayland, pynput is
limited — prefer the BLE HID path there.

### macOS (bring-up)

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Serial port: `/dev/tty.usbserial-*` (check `ls /dev/tty.usbserial-*`).
No driver install is normally needed for the Watcher's USB-UART bridge.

**Accessibility permission:** host-side input injection (`click_at`, and the
`inject` module generally) requires the terminal/Python to be allowed under
System Settings → Privacy & Security → Accessibility, otherwise clicks/keys
are silently ignored. BLE HID from the device needs no such permission.

## Usage

### Interactive console over USB serial

```bash
python watcherd.py console --port /dev/ttyUSB0
# macOS example: python watcherd.py console --port /dev/tty.usbserial-110
```

```
> ping
< {"t":"pong"}
> type hello world
> key enter
> key c ctrl
> mmove 10 -5
> mclick left
> wheel 1
> consumer vol_up
> wifi MyNetwork hunter2
> snap 192.168.1.50
< {"t":"still_sent","id":3,"bytes":48211,"ms":640}
> quit
```

Any line starting with `{` is sent as raw JSON. Device events print as lines
starting with `<`. Serial disconnects print a clear `!` error line instead of
a traceback.

### Still receiver (device POSTs JPEGs here)

```bash
python watcherd.py still-server --port 8099 --out ./stills
# Listening for stills at http://0.0.0.0:8099/still?id=N
```

Then tell the device where to send (from the console):

```
> wifi MyNetwork hunter2
> snap 192.168.1.50
```

Each POST saves `stills/still_<id>_<timestamp>.jpg` and prints the path.
`GET /health` returns `ok` for liveness checks. The device and host must be
on the same network (the `wifi` command gets the Watcher onto your LAN).

### Host-side absolute click (automation primitive)

```bash
python watcherd.py click_at --x 0.42 --y 0.61
```

Clicks at 42% / 61% of the primary screen via pynput. In Phase 0 this is how
"click the button" executes on the host side once coordinates are known
(Phase 3+: vision grounds the target → this clicks it).

### One-shot type over serial

```bash
python watcherd.py type --text "hello" --port /dev/ttyUSB0
```

Sends `{"t":"type","text":"hello"}`; the device emits it over BLE HID.
Waits up to 3 s for the device ack (`--timeout` to change).

## Files

| File | Purpose |
|---|---|
| `watcherd.py` | CLI entry: `console`, `still-server`, `click_at`, `type` |
| `protocol.py` | Protocol v0 schema (single source of truth), encode/decode, validation |
| `protocol.md` | Human-readable schema tables |
| `transport_serial.py` | `SerialLink`: serial connect/send/background reader/reconnect |
| `still_server.py` | `StillServer`: stdlib HTTP receiver for JPEG stills |
| `inject.py` | pynput input injection: `click_at`, `move`, `click`, `type_text`, `press_key` |
| `vision.py` | OpenCV screen quad detection + perspective warp; grounding stubs for Phase 3 |

## Protocol

See [protocol.md](protocol.md) — JSON schema v0, Phase 0 PoC.

## Notes / known gaps (Phase 0)

- `ok`/`err` acks are only exemplified for `snap` in the spec; `watcherd type`
  treats them as generic and warns (doesn't fail) if no ack arrives.
- The spec defines no event reporting Wi-Fi connect results or voice-command
  recognition — the firmware will need those for Phase 2 triggers; propose
  additions in `protocol.md` when the time comes rather than inventing them here.
- `mouse_move` dx/dy units and `mouse_wheel` delta scale are device-defined;
  calibrate against the firmware's HID report values.
- No new protocol message types were added in this agent; everything here maps
  to the existing schema.
