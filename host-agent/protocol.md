# Watcher host protocol — v0 (Phase 0 PoC)

Wire format: UTF-8 JSON object per line, terminated by `\n`, over USB serial
at 115200 8N1. Any received line that is not valid JSON, or not a JSON object
with a string `"t"` field, is ignored.

Source of truth for validation: `protocol.py` (`COMMANDS` / `EVENTS`).

## Host → device commands

| `t` | Fields | Description |
|---|---|---|
| `ping` | — | Liveness check → device replies `{"t":"pong"}` |
| `type` | `text` (string, required) | Type text via BLE HID as if on a keyboard |
| `key` | `code` (string, required), `mod` (string, optional) | Press a key, optionally with modifiers |
| `mouse_move` | `dx`, `dy` (int, required) | Relative mouse movement (device-defined HID units) |
| `mouse_wheel` | `delta` (int, required) | Scroll; positive = up, negative = down (notches) |
| `mouse_click` | `button` (string, optional; default `left`) | Click a mouse button |
| `consumer` | `code` (string, required) | Media / consumer-control key |
| `wifi` | `ssid`, `pass` (strings, required) | Store credentials in NVS and connect |
| `snap` | `host` (string, required), `port` (int, optional; default 8099) | Capture JPEG via Himax camera and HTTP POST raw bytes to `http://host:port/still?id=N` |

### `key` vocabulary

- `code`: `a`–`z`, `0`–`9`, `enter`, `tab`, `esc`, `space`, `backspace`,
  `delete`, `home`, `end`, `pgup`, `pgdn`, `up`, `down`, `left`, `right`,
  `f1`–`f12`
- `mod`: one or more of `ctrl`, `shift`, `alt`, `gui` joined with `+`
  (e.g. `"ctrl"`, `"ctrl+shift"`)

### `consumer` vocabulary

`vol_up`, `vol_down`, `mute`, `play_pause`, `next`, `prev`

### `mouse_click` vocabulary

`button`: `left`, `right`, `middle`

## Device → host events

| `t` | Fields | Description |
|---|---|---|
| `ready` | `fw` (string), `name` (string) | Firmware up and serial link live, e.g. `{"t":"ready","fw":"0.1.0","name":"Watcher-HID"}` |
| `pong` | — | Reply to `ping` |
| `ble` | `state`: `connected` \| `disconnected` | BLE HID link state changed |
| `still_sent` | `id` (int), `bytes` (int), `ms` (int/float) | A `snap` finished POSTing |
| `ok` | `cmd` (string) | A command completed (`cmd` = the command's `t` value) |
| `err` | `cmd` (string), `msg` (string) | A command failed |

## Examples

```
{"t":"ping"}
{"t":"type","text":"hello world"}
{"t":"key","code":"c","mod":"ctrl"}
{"t":"mouse_move","dx":10,"dy":-5}
{"t":"mouse_wheel","delta":1}
{"t":"mouse_click","button":"left"}
{"t":"consumer","code":"vol_up"}
{"t":"wifi","ssid":"MyNetwork","pass":"hunter2"}
{"t":"snap","host":"192.168.1.50","port":8099}
```

## Open questions for later phases (not in v0)

- No event for Wi-Fi connect success/failure after the `wifi` command.
- No event reporting recognized voice commands (needed for Phase 2 triggers).
- `still_sent` carries no image dimensions/format metadata.
- No sequence numbers, so `ok`/`err` can't be matched to a specific command
  instance when several are in flight.
- `mouse_move`/`mouse_wheel` units are device-defined; calibrate against firmware.
