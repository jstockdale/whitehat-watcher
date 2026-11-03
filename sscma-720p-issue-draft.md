# [Feature Request] Support 1280x720 (and larger) frame capture - `MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE` silently drops high-res frames

## Summary

The OV5647 sensor modes for 1280x720 and 1920x1080 are advertised in the firmware's sensor mode list, but selecting them causes `AT+INVOKE` / `AT+SAMPLE` to time out with **no error returned**. The root cause is a compile-time static buffer size check in the AT callback layer that silently discards any JPEG frame larger than `MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE`. The OV5647 natively supports up to 2592×1944, so this is purely a firmware-side limitation.

## Hardware / firmware tested

- Device: Seeed Studio SenseCAP Watcher (Himax HX6538 + OV5647)
- Himax firmware: 2024.08.16 (stock)
- Host: ESP32-S3 communicating via SSCMA over SPI (`AT+INVOKE=1,0,0` etc.)

## Reproduction

Sensor mode list (from firmware binary, in order):

| opt | Resolution | Result                                        |
|-----|------------|-----------------------------------------------|
| 0   | 240x240    | ✅ OK                                          |
| 1   | 416x416    | ✅ OK                                          |
| 2   | 480x480    | ✅ OK                                          |
| 3   | 640x480    | ✅ OK                                          |
| 4   | 1280x720   | ❌ FAIL: AT+INVOKE times out, no image, no error |
| 5   | 1920x1080  | ❌ FAIL: AT+INVOKE times out, no image, no error |

Lower modes return valid JPEGs (verified by decoding the base64 payload — correct SOI/EOI markers, expected dimensions). Higher modes produce nothing: the invoke request simply never gets a response.

I also tested `AT+INVOKE=1,0,1` (results-only, no image) to rule out transport issues — the failure is in frame production, not delivery.

## Root cause

In `sscma/server/at/callback/invoke.hpp`:

```cpp
_ret = camera->retrieveFrame(frame, MA_PIXEL_FORMAT_JPEG);
...
buffer_size = 4 * ((frame.size + 2) / 3);
#if MA_SENSOR_ENCODE_USE_STATIC_BUFFER
    if (buffer_size > MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE) {
        MA_LOGE(MA_TAG, "buffer_size > MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE");
        goto Err;   // <-- frame silently discarded, no error to host
    }
#else
    if (buffer_size > _buffer.size()) {
        _buffer.resize(buffer_size + 1);   // <-- dynamic path handles any size
    }
#endif
```

The same pattern exists in `sscma/server/at/callback/sample.hpp` (~line 127).

A 1280x720 JPEG is typically ~130-400 KB base64-encoded, which exceeds the static buffer sized for 640x480 and smaller captures. The `goto Err` path logs to the Himax console but returns **nothing** to the AT host, so from the caller's perspective the command just hangs until timeout.

Additional notes from source review:
- There is **no raw/unencoded frame output path** — `invoke.hpp` hardcodes `MA_PIXEL_FORMAT_JPEG` for host returns (raw is captured internally for inference only). So host-side re-encoding is not a workaround.
- There is **no JPEG quality knob** exposed via AT commands to shrink the output to fit the buffer.
- Frames are returned as a **single base64 blob** in one JSON event — no chunked transfer for large frames.

## Requested changes

1. **Make the failure visible.** When the encoded frame exceeds the buffer, return an AT error response (e.g. `{ "type": 1, "name": "...", "code": <ERR_FRAME_TOO_LARGE>, ... }`) instead of going silent. A timeout with no diagnostics cost significant debugging time.
2. **Raise the default `MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE`** (or set `MA_SENSOR_ENCODE_USE_STATIC_BUFFER=0` in the Watcher / Grove Vision AI V2 board configs) so the advertised 1280x720 mode actually works. The dynamic-resize path already exists and handles arbitrary sizes — it just isn't enabled in the shipped firmware.
3. **Consider exposing the encode buffer mode / JPEG quality via AT command** so host firmware can trade quality for frame size without a Himax firmware rebuild.

## Why this matters

The 720p mode is listed in the firmware but unusable — that's a trap for anyone building on the platform. The SenseCAP Watcher's "eyes on screen" use case (and any application needing to resolve small UI text/details) genuinely needs more than 640x480. The OV5647 is a 5MP sensor; the silicon is capable, only the buffer config stands in the way.

Happy to test a preview Himax firmware image on the Watcher hardware if helpful.
