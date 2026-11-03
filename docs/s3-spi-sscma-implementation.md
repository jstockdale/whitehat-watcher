# S3 SPI/SSCMA P0/P1/P2 Implementation

Implemented 2026-09-30 (subagent, John's "be meticulous and thorough").
Source-tree work only — no bench, no flash, no pushes.

Device runs **0.4.13**; tree is **0.4.14-dev** (skew noted where relevant).

## P0 — S3 robustness (built, `idf.py build` exit 0)

### P0.1 — Himax reboot detection & re-handshake (`eyes.c`)
- New state: `handshake_done_`, `link_up_at_ms_`, `relink_inflight_`,
  `RELINK_DEBOUNCE_MS=10000`.
- `on_connect` treats INIT-while-linked-up (past debounce) as a Himax
  reboot: clears link state, flushes RX, spawns `relink_worker_` (4 KiB
  stack) which settles 1 s then runs `eyes_rehandshake_()`.
- Extracted `eyes_set_model_retry_()`, `eyes_apply_sensor_opt_()`,
  `eyes_rehandshake_()`; `eyes_init` and `eyes_recover_camera` refactored
  onto them (zero behavior change).
- **Deliberately NOT flushing the pending-request list** on relink:
  use-after-free hazard vs. the process-task cursor; waiters self-drain
  via their timeouts.

### P0.2 — INIT@* always routed to reply_queue (`sscma_client_ops.c`)
- Removed the `if (client->on_connect)` guard. INIT@* is `MA_MSG_TYPE_RESP`
  (type 0, per Himax `resource.hpp::broadcastInitEvent`), so it was being
  swallowed by the response matcher when no `on_connect` was registered.
  The monitor task drops it silently when `on_connect` is NULL. S3-side
  only; no wire change.

### P0.3 — `dbg at` 4 KiB reply buffer (`link.c`)
- The worker task's 512 B stack reply buffer truncated AT+RAWREAD's
  ~2.8 KiB base64; now a 4 KiB PSRAM heap allocation.

### P2 FIFO — oldest waiter wins (`sscma_client_ops.c`)
- `request_list->pxIndex` is reset to `&xListEnd` before all three request
  scans (RESPONSE name-match, EINVAL name-match, EVENT/AT+BREAK) so the
  oldest waiter is matched first instead of the FreeRTOS round-robin
  cursor. (`vListInsertEnd` confirmed head = oldest.)

### Bulk-read plumbing (`sscma_client_ops.c`, `sscma_client_types.h`, `sscma_client_io_interface.h`)
- `sscma_client_flush_rx()` (suspends process task, resets `rx_buffer.pos`).
- `volatile bool bulk_active` on `struct sscma_client_t`; the process task
  skips AT polling while set (10 ms delay).
- Optional `read_bulk` fn pointer on the io vtable; `sscma_client_read_bulk`
  routes to it when present, else chunked AT fallback.

## P1 — CRC-protected SPI bulk transfer (both sides build)

### S3 side (`sscma_client_io_spi.c`, `eyes.c`, `eyes.h`)
- `crc16_ccitt_()` (poly 0x1021, init 0xFFFF), mirrored on Himax.
- `client_io_spi_write`: 248-byte chunks (`MAX_PL_LEN_CRC=248`) so the
  CRC16 trailer at [252],[253] survives the Himax 2-byte RX spill; CRC
  covers header+payload (replaces the 0xFF 0xFF trailer).
- New `FEATURE_TRANSPORT_CMD_READ_CRC (0x07)`; 256-byte command frames
  (Himax RX DMA needs full frames); `spi_bulk_poll_available_()` (30x100 ms
  bounded); `spi_bulk_read_chunk_()` (clocks len+2, verifies CRC);
  `client_io_spi_read_bulk()` implements START->drain->STOP holding the io
  lock for the whole transfer.
- **Whole-transfer retry via RESET+START (x3), no chunk-level retry.**
  The Himax pops the previous chunk at the *next* command parse, so a
  retried READ_CRC would return the *next* chunk's bytes — chunk retry is
  incorrect by design. Documented deviation from the plan.
- `eyes_raw_pull(buf, buf_len, out_len)`: AT+SPIDUMP (15 s timeout) ->
  parse `{len, sha256}` -> `sscma_client_read_bulk` -> mbedtls SHA-256
  verify -> `ESP_OK` / `ESP_ERR_INVALID_CRC`.
- AVAILABLE stays 2-byte legacy (corrupt counts are caught by the data
  CRC / length clamp). Backward compat: new S3 + old Himax fails bulk with
  a clear timeout diagnostic; old S3 + new Himax works via legacy paths
  (Himax accepts the 0xFF 0xFF trailer).

### Himax side (`ma_transport_spi.cpp/.h`, `spidump.hpp`, `watcher_sha256.*`)
- `watcher_sha256.h/.c`: FIPS 180-4, no heap. Host-verified against
  Python hashlib: `""`, `"abc"`, and 1M `'a'` all PASS.
- `spi_crc16_ccitt()`; CMD_WRITE verifies the CRC16 trailer when present
  and receivable (0xFF 0xFF = legacy, accepted unverified; skipped when the
  CRC bytes fall in the 2-byte spill loss region).
- `FEATURE_TRANSPORT_CMD_READ_CRC (0x07)`: stages `[data][CRC16]`, S3
  clocks len+2; peek/pop discipline matches CMD_READ (pop data bytes only,
  at next valid command). Len clamped to 4094 (4096-byte staging buffer).
- CMD_START arms the bulk feeder (re-arm restarts from offset 0);
  CMD_STOP / CMD_RESET disarm.
- Feeder runs in `ma::spi_poll()` (AT-server thread, ~1 kHz, no extra
  task): pushes the staged capture buffer into the TX ring (SPSC with the
  SPI ISR consumer); disarms when fully queued.
- `ma::spi_bulk_prepare(buf, len)` stages the buffer (declared in
  `ma_transport_spi.h`).
- `AT+SPIDUMP` (`spidump.hpp`, registered under CAPTURE_ONLY_MODE):
  fresh `rawDump` capture -> `getRawCaptureBuffer` -> SHA-256 ->
  `spi_bulk_prepare` -> replies
  `{"type":0,"name":"SPIDUMP","code":0,"data":{"len":N,"sha256":"..."}}`.
- Himax `make -j$(nproc)` exit 0; symbols confirmed in ELF
  (`ma::spi_bulk_prepare`, `spiDump`, `watcher_sha256_*`).

## P2 — PNG capture pipeline (S3, `idf.py build` exit 0)

- `eyes_capture_png()` (`eyes.c`): SPIDUMP -> 1,259,712 B PSRAM raw frame
  -> `read_bulk` (SHA-256 verified) -> `png_stream_encode`
  (1296x972, BGGR, SUB filter, 8 KiB IDAT, ROM miniz) into a 2 MiB PSRAM
  buffer -> base64 chunks (768 B -> 1024 chars) -> free both buffers.
- `eyes_png_chunk_cb_t` + `eyes_set_png_chunk_cb()` (`eyes.h`).
- `dbg png` subcommand (`link.c`): read op, not serial-gated (dbg isn't
  MQTT-gated); runs synchronously; terminal `{t:"dbg",cmd:"png",ok:bool}`.
- `main.c`: `on_png_chunk()` -> `{t:"png_chunk",seq,total,last,b64}` via
  `link_send`; registered with `eyes_set_png_chunk_cb()`.

## authZ decision (P2)

**Keep the existing `dbg at` verb blocklist** (RESET/REBOOT/OTA/UPGRADE/
ERASE/FORMAT/INVOKE/YMODEM/BOOT). Rationale: John debugs the Himax over
bridged MQTT, so serial-only would break his workflow; the blocklist was a
deliberate design choice, not a genuine fork. Documented, not gated.
AT+SPIDUMP is a benign read and is not blocklisted.

## Deferred / analytic only

- **SPI2 contention**: analytic only (1 ms CMD_AVAILABLE poll on the
  SD-shared bus, no SYNC line on this hardware). No fix without hardware
  measurement.

## P4 — Zoom crop (both sides build, v0.4.15)

One-shot native-resolution capture window. The sensor is reprogrammed at
runtime to 2592x1944 MIPI (no sensor binning); the INP captures the
`(x, y, w, h)` window 1:1 into a w*h-byte raw buffer.

### AT API design (John's directive, 2026-10-01: decided, no ruling needed)

New command `AT+CROP=x,y,w,h` / `AT+CROP?`, not an extension of `AT+ROI`.
Rationale, from the existing API conventions (`AT+ROI=X,Y,W,H` /
`AT+ROI?`; `AT+RAWDUMP`; `AT+SPIDUMP`; `AT+RAWREAD=<off>,<len>`):

- **Separate command, not ROI extended.** ROI means "persistent hardware
  zoom view": flash-stored, restored at boot, INP crop binned to standard
  output size, sensor MIPI mode untouched. CROP means "one-shot
  native-resolution capture window": never persisted, never restored at
  boot, switches the sensor 1296<->2592x1944, INP 1:1 with no binning.
  One name cannot honestly cover both mode machines — a boot-restored
  2592-mode window would break the standard 1296 pipeline.
- **Clear form matches ROI's convention** (`AT+CROP=0,0,0,0`), not a new
  `CLEAR` verb: consistency across the API beats inventing grammar.
- **Name is CROP, not ZOOM/WINDOW**: the mechanism is a sensor crop
  window (`INP_CROP_T`, `sensordplib_set_sensorctrl_inp_wi_crop_bin`
  throughout the driver); "zoom" implies magnification intent.
- **Improvement to the existing shape**: `AT+SPIDUMP` previously returned
  only `{len, sha256}`, forcing the S3 to assume 1296x972. It now also
  reports `width`/`height` (additive, backward compatible) so variable
  geometry flows end-to-end.
- Reply echoes the active window `{x,y,w,h}` so the caller sees what was
  actually programmed after driver validation.

### Implementation

- **Himax**: `drv_ov5647_init_raw_fullres()` programs the 2592x1944
  register table (in-tree `OV5647_mipi_2lane_2592x1944.i`, 92 writes),
  parameterizes the CSI RX (`set_mipi_csirx_enable(w, h)`), and configures
  the INP with subsample+binnning DISABLED and the crop window. Geometry
  must be even (Bayer phase), inside 2592x1944, w*h <= 1,259,712
  (`CROP_WINDOW_MAX_BYTES` — the proven heap budget).
  `drv_ov5647_recrop_fullres()` moves a same-size window with INP-only
  reprogramming (no sensor re-init) — the P5 fast path.
  `_sensor_fullres_mode` forces the standard init paths through a full
  sensor re-init on the way back to 1296x972 (fast-switch would otherwise
  leave the sensor in 2592 mode against a 1296 INP config).
- **AT surface**: new `AT+CROP=x,y,w,h` / `AT+CROP?` (see
  `callback/crop.hpp`). Distinct from `AT+ROI`: ROI is a persistent
  hardware zoom view (restored at boot); CROP is a one-shot capture
  window, never persisted, never restored. `AT+CROP=0,0,0,0` exits
  full-res mode. `AT+SPIDUMP` now also reports `width`/`height`.
- **S3**: `eyes_set_crop()` / `eyes_clear_crop()`; `eyes_capture_png()`
  encodes at the SPIDUMP-reported geometry; `dbg crop` subcommand
  (`{"t":"dbg","cmd":"crop","args":"x,y,w,h"|"clear"}`; no args = query).
- **Hardware risks** (bench must prove): V-1 — 2592-px lines through the
  INP line buffers / WDMA2; V-2 — runtime MIPI RX re-init at 2592x1944.

## P5 — Strip capture (both sides build, v0.4.15)

Full 2592x1944 frame as 2592x486 strips (1,259,712 B each — byte-identical
to the 1296x972 frame budget, so the proven buffer/transport is reused).

- **Flow**: `dbg strip` -> S3 loops strips: `AT+CROP=0,y0,2592,486`
  (first strip does the full sensor init; rest take the INP-recrop fast
  path) -> `AT+SPIDUMP` -> SPI bulk pull (CRC16 + SHA-256) ->
  `{t:"strip_chunk"}` base64 events. Crop window cleared on exit.
- **Geometry**: 4 strips at overlap 0 tile 1944 rows exactly
  (y0 = 0, 486, 972, 1458). Overlap is a parameter (even rows only,
  Bayer-parity-safe); `n = ceil((1944-ov)/(486-ov))` — 5 strips at ov=16
  (last strip shorter). Overlap rows are dropped from the head of each
  subsequent strip at stitch time.
- **Stitching**: host-side per WHF-CAP v0.1 §11
  (`your_files/himax-flash-kit/stitch_strips.py` assembles + renders).
  On-device stitch would fit the 8 MB PSRAM (~6.3 MB peak) but is out of
  scope for this pass.
- **S3**: `eyes_capture_strips(overlap)` + `eyes_set_strip_chunk_cb()`;
  `{"t":"dbg","cmd":"strip"[, "overlap":N]}`.

## Bench findings — 0.4.14 (2026-10-04)

### F1 — S3↔Himax link down since the Oct 1 flash; P0 REGRESSION (corrected 12:50)
**Corrected diagnosis:** the Himax is healthy. The S3's P0 handshake sends
`AT+MODEL=1`; the capture-only Himax (`CAPTURE_ONLY_MODE=1`) promptly
replies `MA_ENOTSUP` ("not available in capture-only build", `ma_types.h`
`MA_ENOTSUP=8` → S3 `error_map[8]` = `ESP_ERR_NOT_SUPPORTED`). The new P0
`eyes_set_model_retry_` treats this as fatal → "Himax not answering; eyes
staying down" → `ready_=false` forever.

Evidence it's ENOTSUP, not a timeout: 4 attempts finished in ~3.2 s total
(tick 639→3804 on Oct 4) with a 2000 ms per-attempt timeout — they failed
fast, consistent with immediate error replies. The Himax emitted
INIT@SENSOR/ALGORITHM/TRIGGER on its console (no INIT@MODEL — skipped in
capture-only mode, `ma_server_at.cpp:82`), so its AT server is alive.

**The regression:** v0.4.13's S3 had explicit capture-only tolerance —
telemetry shows `eyes: Himax is capture-only (no MODEL support); continuing
with sensor-only init` on Sep 29/30. That string is GONE from the current
tree; the P0 refactor dropped it despite the "zero behavior change" claim.

**Fix (S3, next build):** treat `ESP_ERR_NOT_SUPPORTED` from `set_model` as
"capture-only Himax" → log + continue with sensor-only init (skip to
`eyes_apply_sensor_opt_`), restoring v0.4.13 behavior. Applies to all
`eyes_set_model_retry_` callers: `eyes_init`, `eyes_recover_camera`
(`eyes_rehandshake_`), and the relink worker — no runtime recovery exists
today.

Superseded theory: the "1.5 s retry window too short for Himax cold boot"
was wrong — the Himax answered immediately every time.

**Fix implemented 2026-10-04 (v0.4.15 tree, S3 only):** `eyes_set_model_retry_`
in `firmware/components/eyes/eyes.c` now treats `ESP_ERR_NOT_SUPPORTED`
from `AT+MODEL=1` as "capture-only Himax" — logs `"<why>: Himax is
capture-only (no MODEL support); continuing with sensor-only init"`
(restoring the v0.4.13 message) and returns `ESP_OK`, breaking the retry
loop on the first ENOTSUP instead of burning all 4 attempts. Covers all
three callers (`eyes_init`, `eyes_recover_camera` via `eyes_rehandshake_`,
and the reboot re-link worker). No Himax change needed. Rebuild in
progress at time of writing.

### F1b — remaining P0.1 gaps (still real, fix separately)
- No background retry of the initial handshake; `on_connect` with the link
  previously down only logs "Himax link connected" — never handshakes.
- INIT@* not reaching the S3 over SPI on Himax boot: John saw 3 on the
  Himax console this morning; 0 arrived via SPI/MQTT all day (bridge was
  down in two windows ~pre-12:21 and ~12:32–12:39, so SPI-side arrival in
  those gaps can't be ruled out from my logs — but the S3 never acted on
  them regardless). `broadcastInitEvent` early-returns when
  `transports.size() < 2`: possible boot-ordering race, needs a Himax-side
  check.

### F2 — Bridge subscriber died silently twice (2026-10-04)
`subscriber.py` stopped twice (~before 12:20 and ~12:32 PDT) with no
traceback or error in `subscriber.log`. Manual restart via
`~/.config/watcher-mqtt.env` works. No systemd user bus in this
environment, so no auto-restart. Worth investigating.

## What needs hardware (bench checklist vs 0.4.15)

P4/P5 (after the 0.4.14 checklist above):
1. Flash Himax 0.4.15 (CROP + fullres init) and S3 0.4.15.
2. `AT+CROP=0,0,2592,486` -> expect `{code:0, data:{x:0,y:0,w:2592,h:486}}`.
3. `AT+SPIDUMP` -> expect `{len:1259712, width:2592, height:486}`.
   **This is the V-1 gate**: if the INP can't pass 2592-px lines, the
   capture will fail or return garbage here.
4. `dbg crop` with `args:"0,0,1296,972"` then `dbg png` -> PNG of the
   center... (window at native res, verify geometry in the PNG header).
5. `AT+CROP=0,0,0,0` -> back to 1296x972; `AT+SPIDUMP` ->
   `{len:1259712, width:1296, height:972}` (mode-exit path).
6. `dbg strip` -> 4 strips x ~1640 chunks; reassemble with
   `stitch_strips.py` -> 2592x1944 render, verify against the 1296x972
   frame (same scene, 2x linear resolution).
7. `dbg strip` with `overlap:16` -> 5 strips; stitch with
   `--overlap 16`, verify no seam doubling.
8. Odd geometry rejected: `AT+CROP=0,1,2592,486` -> error (Bayer phase).
9. Oversize window rejected: `AT+CROP=0,0,2592,1944` -> error
   (5,038,848 B > 1,259,712 budget).

## Files changed

S3 (`firmware/`):
- `components/sscma_client/src/sscma_client_ops.c`
- `components/sscma_client/src/sscma_client_io_spi.c`
- `components/sscma_client/include/sscma_client_types.h`
- `components/sscma_client/include/sscma_client_ops.h`
- `components/sscma_client/interface/sscma_client_io_interface.h`
- `components/eyes/eyes.c`, `components/eyes/include/eyes.h`
- `components/link/link.c`
- `main/main.c`

Himax (`sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/`):
- `library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp`
- `library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.h`
- `library/sscma_micro_porting/porting/himax/we2/watcher_sha256.h` (new)
- `library/sscma_micro_porting/porting/himax/we2/watcher_sha256.c` (new)
- `library/sscma_micro/sscma/server/at/callback/spidump.hpp` (new)
- `library/sscma_micro/sscma/server/at/callback/crop.hpp` (new, v0.4.15)
- `library/sscma_micro/sscma/server/at/ma_server_at.cpp`
- `library/sscma_micro_porting/porting/himax/we2/ma_camera_himax.h` (v0.4.15)
- `library/sscma_micro_porting/porting/himax/we2/ma_camera_himax.cpp` (v0.4.15)
- `library/sscma_micro_porting/porting/himax/we2/drivers/camera/drv_ov5647.c` (v0.4.15)
- `library/sscma_micro_porting/porting/himax/we2/drivers/camera/drv_ov5647.h` (v0.4.15)
- `library/sscma_micro_porting/porting/himax/we2/drivers/camera/drv_shared_cfg.h` (v0.4.15)
- `app/scenario_app/sscma/sscma.cpp` (v0.4.15 version bump)

Host:
- `your_files/himax-flash-kit/stitch_strips.py` (new, v0.4.15)
