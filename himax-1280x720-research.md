# Research Report: Getting 1280x720 Working on Himax HX6538 / SenseCAP Watcher

**Date:** 2026-09-23
**Question:** Why does opt 5 (1280x720) accept the AT command but never produce a frame?

---

## 1. What the web says

**Nobody has reported 1280x720 (or higher) snapshots working on the SenseCAP Watcher.**
Searched: Seeed Studio forum, GitHub issues (seeed-studio/SenseCAP-Watcher-Firmware,
seeed-studio/SSCMA-Micro), Reddit, Hackaday, Hackster.io. Zero success stories for
high-res Watcher stills. The camera is universally treated as a low-res AI-vision
sensor, not a photography device.

**Closest precedent — reCamera (Seeed's other SSCMA device):**
A forum user wanted full-res 2592x1944 from reCamera; the Node-RED SSCMA node's
resolution dropdown maxed at 1920x1080. Fix was editing `camera.js` to pass through
the raw `option` value instead of the curated list.
([forum.seeedstudio.com/t/record-full-resolution-image-with-recamera/294672](https://forum.seeedstudio.com/t/record-full-resolution-image-with-recamera/294672))
Takeaway: SSCMA firmware *lists* higher modes than the tooling exposes — the
capability is in the firmware table, but the surrounding software doesn't use it.

**Himax firmware versions:** `himax_firmware_20240816.img` (v2024.08.16) is the
**only** Himax firmware version in circulation. Every GitHub issue, forum post,
and the OSHW repo references this exact build. There is no newer version to
upgrade to. The Himax firmware is distributed as a binary blob only — no source,
no board support package, no build instructions from Seeed.

**Relevant GitHub issue:**
[seeed-studio/SenseCAP-Watcher-Firmware#69](https://github.com/seeed-studio/SenseCAP-Watcher-Firmware/issues/69) —
"factory_firmware built from fw-v1.1.7 source fails Himax HX6538 handshake."
Confirms the Himax side is a closed binary (`himax_firmware_20240816.img`) that
the ESP32-S3 side talks to via `sscma_client`. Multiple people have hit
Himax-communication fragility; nobody has rebuilt the Himax firmware itself.

---

## 2. Root-cause analysis (from SSCMA-Micro source)

Cloned `seeed-studio/SSCMA-Micro` (the open-source SSCMA server that runs on the
Himax) and traced the exact failure path.

### The smoking gun: static JPEG encode buffer

**File:** `sscma/server/at/callback/invoke.hpp` → `eventLoopCamera()`

```cpp
_ret = camera->retrieveFrame(frame, MA_PIXEL_FORMAT_JPEG);
...
buffer_size = 4 * ((frame.size + 2) / 3);   // base64-encoded size
#if MA_SENSOR_ENCODE_USE_STATIC_BUFFER
    if (buffer_size > MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE) {
        MA_LOGE(MA_TAG, "buffer_size > MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE");
        goto Err;                            // <-- frame silently dropped
    }
```

The **same check** exists in `sscma/server/at/callback/sample.hpp`
(the `AT+SAMPLE` still-capture path).

### Why this matches our symptoms exactly

1. `AT+SENSOR=1,1,5` → `configureSensor()` in `sensor.hpp` calls
   `camera->init(5)`. Our S3 client's `sscma_client_set_sensor()` checks the
   SSCMA-level `code` in the reply — it returned `ESP_OK`, so **`camera->init(5)`
   succeeded**. The sensor IS in 1280x720 mode.
2. `AT+INVOKE` → `retrieveFrame(JPEG)` succeeds → JPEG is ~100-300KB for
   1280x720 → base64 needs ~133-400KB.
3. If `MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE` was compiled smaller than that
   (e.g., sized for 640x480-era JPEGs), the check trips → `goto Err` →
   **no image event is ever emitted**. The S3 side waits forever → timeout.
4. `AT+SENSOR?` / `AT+SENSORS?` timing out is a separate flakiness in the
   AT query path, not directly related.

### Why the buffer is small

`MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE` is a **compile-time constant** defined in
the Watcher board's `ma_config_board.h` at Himax-firmware build time. It is not
settable via any AT command. Seeed sized it for the resolutions they expected
to be used (≤640x480); the 1280x720/1920x1080 entries in the sensor mode table
are likely inherited from the generic OV5647 driver presets, not tested paths.

### Memory feasibility (not a hardware limit)

From `HX6538_datasheet.pdf` (in `vendor/oshw-watcher/Hardware/`):
- 256KB ITCM + 256KB DTCM
- **Configurable system SRAM up to 2432KB (~2.4MB)**

A 1280x720 RGB565 raw frame is 1.8MB — tight but fits. A 1280x720 JPEG is
~100-300KB, base64 ~133-400KB — fits comfortably in a right-sized static
buffer. **This is a firmware config limit, not a silicon limit.**

---

## 3. Is 1280x720 known-possible on this hardware?

**Not with the stock `himax_firmware_20240816.img`.** The mode is listed but the
encode pipeline can't deliver it. No one in the community has demonstrated it.

**It is solvable in principle** — by rebuilding the Himax firmware with a larger
`MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE`. But that requires:
- Himax WiseEye2 SDK (exists, Himax distributes it under NDA/license)
- SSCMA-Micro source (public on GitHub — have it)
- **Watcher board porting layer** (OV5647 driver config, pin map, memory layout)
  — **NOT public**. This is the blocker. Seeed has not open-sourced the Himax
  side of the Watcher.

---

## 4. Concrete next steps

### A. Confirm the diagnosis (no firmware changes, ~10 min)
Send `AT+INVOKE=1,1` while the sensor is on opt 5.
- The 2nd arg (`results_only=1`) skips JPEG encoding entirely — only the
  person-detection boxes are returned.
- **If it returns detection results:** camera works fine at 1280x720;
  the failure is 100% the encode buffer. Diagnosis confirmed.
- **If it still fails:** `camera->init(5)` is broken deeper (unlikely given
  the OK return code, but worth ruling out).
- Our S3 firmware's `snaptest` doesn't expose `results_only`; would need a
  small addition or a raw `AT+INVOKE=1,1` passthrough via the existing `at`
  dbg command (read-only allowlist — `AT+INVOKE=1,1` contains `=`, so the
  allowlist would need a carve-out, or use the UART console directly).

### B. Ask Seeed for a fixed Himax firmware (best long-term path)
File an issue on
[seeed-studio/SenseCAP-Watcher-Firmware](https://github.com/seeed-studio/SenseCAP-Watcher-Firmware/issues)
(and/or the Seeed forum) asking for a Himax firmware build with
`MA_SENSOR_ENCODE_STATIC_BUFFER_SIZE` raised to ≥512KB so the listed
1280x720/1920x1080 sensor modes actually work. Reference this analysis.
Seeed is the only party that can rebuild that binary.

### C. Don't bother with opt 6 (1920x1080)
It will hit the same (worse) buffer limit. Skip testing it.

### D. Live with 640x480 (pragmatic)
2.4x the pixels of the factory 416x416, works reliably today, no firmware
surgery needed. This is the current state of our v0.2.14 build.

### E. Nuclear option: rebuild the Himax firmware ourselves
Would need the WiseEye2 SDK + Watcher board files. Himax SDK access is the
first hurdle; the Watcher-specific OV5647/preset configuration would have to
be reverse-engineered from the binary. High effort, uncertain payoff.

---

## 5. Hard limit vs. solvable config?

**Solvable config issue** — with one caveat:

| Layer | Verdict |
|---|---|
| OV5647 sensor | ✅ Supports 1280x720 natively (it's a 5MP sensor) |
| HX6538 silicon | ✅ ~2.4MB SRAM is enough for the 1280x720 pipeline |
| Himax firmware mode table | ✅ 1280x720 listed (opt 5) |
| Himax firmware `camera->init(5)` | ✅ Returns success |
| Himax firmware JPEG encode buffer | ❌ **Too small — frame silently dropped** |
| Fixability | 🔶 Requires Himax firmware rebuild; only Seeed can do it today |

**Bottom line:** The hardware can do it. The stock Himax firmware can't, due to
an undersized compile-time encode buffer. The fix is a one-line config change
in a firmware image only Seeed can rebuild. Recommend step A (confirm) then
step B (ask Seeed), while shipping on 640x480.

---

## Key files & links

- SSCMA-Micro INVOKE handler: `/tmp/sscma-micro/sscma/server/at/callback/invoke.hpp`
- SSCMA-Micro SAMPLE handler: `/tmp/sscma-micro/sscma/server/at/callback/sample.hpp`
- SSCMA-Micro sensor config: `/tmp/sscma-micro/sscma/server/at/callback/sensor.hpp`
- AT protocol docs: `/tmp/sscma-micro/docs/protocol/at-protocol-en_US.md`
- HX6538 datasheet: `~/workspace/projects/watcher-input-assistant/vendor/oshw-watcher/Hardware/HX6538_datasheet.pdf`
- Himax firmware binary: `~/workspace/projects/watcher-input-assistant/vendor/oshw-watcher/Firmware/Himax/himax_firmware_20240816.img`
- reCamera full-res thread: https://forum.seeedstudio.com/t/record-full-resolution-image-with-recamera/294672
- Watcher firmware handshake issue: https://github.com/seeed-studio/SenseCAP-Watcher-Firmware/issues/69
- OSHW Watcher repo: https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher
