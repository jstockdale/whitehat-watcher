# S3 SPI link + sscma_client — assessment and next steps

**Date:** 2026-09-30 · **Tree:** `firmware/` at 0.4.14-dev (`FW_VERSION "0.4.14"` in `main.c`; device runs 0.4.13 — note the skew) · **Read-only review, no bench touched.**

## How it works today

**Transport** — `components/sscma_client/src/sscma_client_io_spi.c`
- Seeed `FEATURE_TRANSPORT` packet protocol over SPI2 (bus shared with the SD card; SD CS parked high), 12 MHz, mode 0, `wait_delay` 2 ms, **no SYNC GPIO** (`sync_gpio_num = -1`, "use pure SPI polling").
- Packet: `[0x10][CMD][LEN_HI][LEN_LO][payload…][0xFF][0xFF]`. The 2-byte trailer is a **placeholder, not a checksum** — no integrity check on the wire in either direction.
- Reads: `CMD_AVAILABLE` → `CMD_READ` in ≤4095-byte requests. Writes: ≤250-byte payload chunks.
- Hardening already landed: persistent internal DMA staging buffer (kills the per-transaction PSRAM bounce alloc that caused the 0.4.0/0.4.1 `ESP_ERR_NO_MEM` camera wedge); mutex + `spi_device_acquire_bus` around every op; failed reads flush the partial frame instead of desyncing the parser.

**Client core** — `components/sscma_client/src/sscma_client_ops.c`
- FreeRTOS task polls `available()` every **1 ms** (was 10 ms; comment tags it "Phase 1 (v0.4.14)"), reads into a 64 KB PSRAM rx buffer, frames on `\r{` … `}\n`, cJSON-parses.
- Dispatch: `type:0` (RESPONSE) → matched against the pending-request list **by command name** (`AT+RAWREAD=0,2048` → key `RAWREAD`); `type:1` (EVENT) → `on_event`; `type:2` (LOG) → `on_log`. Any `name` starting `INIT@` → `on_connect` (when registered).
- `sscma_client_request(cmd, reply, wait, timeout)`: registers request → writes AT over SPI → blocks on a per-request queue.

**App layer** — `components/eyes/eyes.c`
- Registers `on_connect` (sets `link_up_`, logs event name), `on_event` (extracts snapshot JPEGs from invoke events), `on_log` (no-op).
- Init: PCA9535 power rails → SPI2 → `AT+MODEL=1` retried 4× (RTC-WDT fed between attempts) → sensor forced to 640×480 preset.
- `eyes_at(cmd, out, out_len)`: generic synchronous AT passthrough, 3 s timeout. `eyes_snap()`: `AT+INVOKE` snapshot with generation-guarded single slot and **auto-recovery after 3 straight invoke failures** (RST pulse ± rail power-cycle + re-handshake, ~2–4 s).
- `link.c` exposes `{"t":"dbg","cmd":"at","at":"AT+…"}` → `eyes_at()` → reply over the serial link. Verb blocklist: RESET/REBOOT/OTA/UPGRADE/ERASE/FORMAT/INVOKE/YMODEM/BOOT. The `at` subcommand is **not** gated on `via_mqtt` (unlike rail/power commands), so it will be reachable over native MQTT too once that lands.

## What works
- Basic AT command/response over SPI (ID, INFO, SENSOR, MODEL).
- `INIT@*` handshake → `on_connect` → `link_up_` gating; snapshots refuse cleanly before link-up instead of hanging.
- Snapshot path (`AT+INVOKE` → JPEG event → base64 → link) and the recovery path.
- DMA-heap and parser-desync hardening from the 0.4.0/0.4.1 incidents.

## Gaps and fragilities
1. **Himax reboots are invisible to the S3.** `link_up_` is set once by `on_connect` and never cleared except during explicit recovery. If the Himax reboots (as it did twice today), the S3 keeps `link_up_ = true` against a dead/rebooted peer; there is no re-handshake. The re-broadcast `INIT@*`s *could* serve as a reboot detector — today `on_connect` just re-sets an already-true flag.
2. **`"request not found"` warnings are benign noise with a real root cause.** The Himax sends INIT broadcasts as `type:0` (RESPONSE), not `type:1` (EVENT). Whenever `on_connect` is NULL (pre-registration window, or any client without it), they fall into request-name matching and warn. Functionally harmless — the SPI AT path is demonstrably alive — but it masks real mismatches in the same log line.
3. **No bulk frame path over SPI.** `sscma_client_read_bulk()` is declared in the header but has no caller; there is no `AT+SPIDUMP`/`AT+RAWDUMP` support and no raw-frame API in `eyes.c`. Frame pulls go through the page's USB serial. Worse, the `dbg at` path uses a **512-byte `out` buffer** — a single `AT+RAWREAD` response (~2.8 KB) is silently truncated, so even one chunk can't come back through the S3 today.
4. **No transport checksum** (0xFF 0xFF placeholder both ways). At 12 MHz over short traces this has held, but bulk transfers will stress it.
5. **Same-name concurrent requests aren't disambiguated.** Two in-flight `RAWREAD`s can receive each other's replies (name-only matching, round-robin list cursor). Harmless for offset-identified chunk pulls; wrong in general.
6. **1 ms `CMD_AVAILABLE` polling** is constant SPI chatter on a bus shared with the SD card. Works, but contention is unmeasured.
7. **`png_stream.c`** (streaming Bayer→RGB→PNG via ROM miniz) exists but is not wired to any SPI frame receiver — the "SPI frame receiver → png_stream_encode → MQTT chunk transport" chain is still open.

## Prioritized next steps

**P0 — correctness, small, no protocol changes**
1. **Detect Himax reboot on the S3.** On a re-broadcast `INIT@*` arriving while already linked, treat as peer reboot: clear `link_up_`, flush the request list, re-run the handshake (`AT+ID?`/`AT+MODEL=1`/sensor preset) before setting `link_up_` again. Eliminates the stale-link window (today: S3 happily reports link_up against a rebooted Himax).
2. **Reclassify INIT broadcasts in the client core.** Route `type:0` + `name=INIT@*` to the event path (or a dedicated `on_himax_boot` callback) instead of request matching — kills the warning noise and gives (1) a clean hook.
3. **Fix the `dbg at` truncation**: grow `out` to 4 KB (or make it dynamic) so single-chunk `AT+RAWREAD` works through the S3; document that multi-chunk bulk still isn't supported there.

**P1 — bulk frame transfer over SPI (the `SPI_PHASE_PLAN.md` work, still awaiting link-up verification)**
4. Phase 1: implement `client_io_spi_read_bulk()` (known-length, ≤65535-byte `CMD_READ`s, backpressure via `CMD_AVAILABLE`, no fixed delays), Himax-side `AT+SPIDUMP` frame feeder, and an `eyes_raw_pull()` API. Verify: 1,259,712 bytes, SHA-256 match, ≤2.5 s, no AT/JSON corruption during bulk.
5. Phase 2: real CRC16-CCITT replacing the `0xFF 0xFF` placeholder (both directions, 3× bounded retry), then stepwise `wait_delay` reduction with error-rate measurement.

**P2 — robustness / native-MQTT readiness**
6. Disambiguate in-flight same-name requests (monotonic sequence tags, or strict FIFO reply matching).
7. Measure SPI2 contention (SD + 1 ms AT polling + future bulk); keep polling but quantify, since there is no SYNC line on this HW.
8. **Decide AT-passthrough authZ before native MQTT goes live.** `dbg at` is currently reachable `via_mqtt` with only the verb blocklist as guard — deliberate choice needed (seal-gated? serial-console-only like the rail commands?) before the S3 speaks native MQTT.
9. Wire `png_stream.c` to the bulk receiver (step 4) for the S3-side debayer→PNG→MQTT chunk path.

## What was deliberately not touched
No firmware edits, no builds, no flashes, no bench lock, no device commands — a frame pull is running. All findings are from source reading (`sscma_client/*`, `eyes/*`, `link.c`, `SPI_PHASE_PLAN.md`) cross-checked against today's telemetry (`request not found: INIT@*` lines).
