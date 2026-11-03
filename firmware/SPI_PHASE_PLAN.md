# S3↔Himax SPI: Phase 1/2 + PNG Integration Plan

**Status**: Design — awaiting link-up verification on hardware (v0.4.7 Himax + 0.4.13 S3).
**Prerequisite**: John flashes both; `link_up:true` confirmed via bench.

## Link fix (done, in v0.4.7 / 0.4.13)

- **Himax**: `broadcastInitEvent()` in `resource.hpp` re-sends the just-encoded
  INIT@* event to every transport except the front (console). Hooked into
  `initDefaultSensor/Algorighm/Trigger/Model`.
- **S3**: `on_connect` now fires on any `INIT@*` event name prefix
  (`EVENT_INIT_PREFIX`), not just `INIT@STAT`. Log names the event.

## Phase 1: Known-length bulk read (target ~600 KB/s, ~2.1 s/frame)

### Problem
`client_io_spi_read()` issues one CMD_READ per 4095 bytes with a fixed 2 ms
`vTaskDelay` before AND after each command packet. For a 1,259,712-byte
1296×972 RAW8 frame: 308 round trips × 4 ms = ~1.2 s in fixed delays alone,
plus AVAILABLE polling overhead per chunk.

### S3 changes (`sscma_client_io_spi.c`)
1. New `client_io_spi_read_bulk(io, data, total_len)`:
   - Takes the known total length up front (from capture metadata).
   - Issues CMD_READ with up to 65535-byte requests (uint16_t protocol limit),
     not 4095.
   - **No fixed delays.** Before each CMD_READ, polls CMD_AVAILABLE with a
     bounded timeout (100 ms); issues READ for `min(available, remaining)`.
     This is the backpressure: the S3 never outruns the Himax TX ring.
   - Keeps the DMA staging buffer path (no per-transaction PSRAM bounce alloc).
2. Keep existing `client_io_spi_read()` untouched for AT/JSON traffic.

### Himax changes (`ma_transport_spi.cpp`)
1. Frame feeder: after a capture completes, a task pushes frame bytes into
   `_spi_rb_tx` in chunks, blocking when the ring is full (natural
   backpressure against the S3's drain rate).
2. New AT command `AT+SPIDUMP` (or equivalent): captures a frame and arms the
   feeder, replying with the byte count so the S3 knows `total_len` up front.
3. Consider growing the TX ring for bulk (4 KB → 16 KB) if SRAM allows —
   measure first.

### Verification
- Transfer 1,259,712 bytes, SHA-256 match against Himax-side hash.
- Measure wall time; target ≤ 2.5 s.
- Confirm no AT/JSON traffic corruption during bulk (interleave a ping).

## Phase 2: Real CRC16 + delay tuning (target ~950 KB/s, ~1.3 s/frame)

### Changes (both sides, protocol bump)
1. Replace placeholder `0xFF, 0xFF` trailer with CRC16-CCITT (poly 0x1021,
   init 0xFFFF) over `[FEATURE|CMD|LEN_HI|LEN_LO|PAYLOAD]`.
   - S3 `client_io_spi_write`: compute and append.
   - S3 `client_io_spi_read_bulk`: verify per chunk; Himax `CMD_READ` handler:
     compute over peeked bytes.
   - Himax `CMD_WRITE` handler: verify incoming.
2. On CRC mismatch: bounded retry (3× per chunk), then abort with diagnostic.
3. Reduce `wait_delay` 2 ms → 1 ms → 0.5 ms stepwise, measuring CRC error
   rate at each step. Stop when errors appear; back off one step.
4. Only then consider 24 MHz (Phase 3, separate approval).

### Verification
- Bit-error injection test (if feasible) or long soak: 50 frames, zero CRC
  failures at the chosen delay.
- Throughput measurement at each delay step.

## PNG integration (after Phase 2 verified)

### Flow
1. S3: `eyes_capture_spi_frame()`:
   - Send `AT+SPIDUMP` over SPI, parse byte count.
   - `malloc(1,259,712, MALLOC_CAP_SPIRAM)` frame buffer.
   - `client_io_spi_read_bulk()` into the buffer.
   - `png_stream_encode()` with `PNG_CFA_BGGR` (default; runtime-tunable),
     `PNG_FILTER_SUB`, 8 KiB IDAT chunks.
   - Sink callback publishes each PNG chunk over MQTT.
   - Free frame buffer.
2. MQTT transport: PNG chunks via the sealed telemetry path
   (`{t:"png_chunk", idx, total, data}`), mirroring the `himax_chunk`
   pattern the console already handles. Native MQTT when provisioned.
3. Host: reassemble chunks → `frame.png`.

### Memory budget (S3)
- Frame buffer: 1,259,712 B PSRAM.
- tdefl_compressor: ~175 KiB PSRAM.
- Row/IDAT buffers: ~12 KiB internal.
- Total PSRAM: ~1.45 MB — within the 8 MB PSRAM budget.

### Verification
- PNG decodes (PIL) with exact flat-region recovery.
- CFA pattern check via `debayer_check.py` against the captured frame.
- End-to-end: MQTT → host PNG → viewable image.

## Explicitly out of scope
- Phase 3 (24 MHz) — separate approval after Phase 2 soak.
- 40 MHz or zero-wait operation.
- Touching the UART/console AT path behavior.
