# MQTT-over-WiFi Transport Reliability — Research Deep Dive
**Date:** 2026-10-09 · **For:** Track B implementation (unified plan)
**Sources:** `firmware/components/seal/mqtt_native.c` (838 lines, read in full),
`firmware/components/link/link.c` (`link_send`, ring), `firmware/main/main.c`
(`on_snaptest_chunk`), ESP-IDF `esp-mqtt` source (`mqtt_client.c`, `mqtt_outbox`),
sdkconfig (`CONFIG_LWIP_TCP_*`), lwIP TCP option docs.

---

## 1. Current architecture: why the mirror queue drops

### The TX path today (0.4.18.88, bridged mode)

```
eyes.c PNG sink → on_snaptest_chunk() [main.c]
  → link_wait_for_ring_space(60s, len+128)   // backpressure vs USB drain
  → link_send(cJSON)                         // holds tx_lock_
      → ring_push_nb(JSON + "\n")             // 32KB PSRAM log ring (USB drain)
      → tx_mirror_cb_(s, len)                // mqtt_native_mirror_tx, still holding tx_lock_
          → malloc copy (PSRAM)
          → xQueueSend(tx_q_, &m, 0)         // ZERO WAIT — drops on full
  → native_task (1 Hz poll)
      → drain_tx_queue()
          → esp_mqtt_client_publish(topic, line, len, qos=0, retain=0)
```

### The three drop points

| # | Location | Mechanism | Trigger |
|---|----------|-----------|---------|
| 1 | `mqtt_native_mirror_tx` | `xQueueSend(tx_q_, &m, 0)` — `free(m.line)` on full | queue depth 16, producer faster than 1 Hz drain |
| 2 | `drain_tx_queue` | 1-second poll loop; publishes QoS 0 | any burst >16 msgs/sec; QoS 0 = no broker ack |
| 3 | esp-mqtt | QoS 0 publish while disconnected → returns -1, message gone | WiFi flap |

In bridged mode today this *happens to work* because the encoder is paced to the
115200-baud USB drain (~1 chunk/sec ≪ 16-deep queue @ 1 Hz drain). **In native
WiFi mode the USB pacing disappears** (that's the point of B2) and the encoder
firehoses ~163 chunks as fast as it can encode them. The 16-deep queue overflows
in the first second → catastrophic loss. This is exactly what B1 must fix before
anything else in Track B.

### Why B1's "bounded-blocking enqueue (2 s)" is the right fix

- The producer (`on_snaptest_chunk` → `link_send`) already tolerates blocking:
  it does `link_wait_for_ring_space(60000, …)` today. A 2 s block on the MQTT
  mirror queue is consistent with John's "blocking is correct for PNG transport
  backpressure" ruling.
- Contract constraint: `mqtt_native_mirror_tx` is called **with `tx_lock_` held**
  from `link_send`. It must never call back into link, never touch NVS, never
  do unbounded work. `xQueueSend(tx_q_, &m, pdMS_TO_TICKS(2000))` satisfies this.
- **Deadlock audit (required before shipping B1):** the drain side
  (`drain_tx_queue` in `native_task`) must never take `tx_lock_`. It doesn't
  today — it only calls `esp_mqtt_client_publish` + `free`. Keep it that way.
  Also: the 2 s timeout must be < the 60 s `link_wait_for_ring_space` timeout so
  the ring wait remains the outer backpressure bound, and the abort path
  (`return false` → abort PNG, never corrupt) stays reachable.
- Event-driven drain: replace the `vTaskDelay(1000)` poll with a direct
  `xTaskNotifyGive` from `mqtt_native_mirror_tx` after a successful enqueue
  (or a queue-set). `native_task` then blocks on `ulTaskNotifyTake` with a
  modest timeout (keep the 1 s cadence as a fallback for heartbeats/status).
  This cuts per-chunk latency from ~500 ms average to ~ms.
- `chunk_dropped` counter: increment in the mirror on `xQueueSend` timeout
  (should now only happen on the 2 s timeout, i.e. broker genuinely stuck),
  expose in heartbeat/status. Target: **0 forever**. Note the existing
  `log_dropped_` precedent in `ring_push_nb` — mirror that pattern
  (monotonic `uint64_t`, never reset).

---

## 2. ESP-IDF esp-mqtt: QoS 1 guarantees and sharp edges

### What QoS 1 actually gives you (verified in `mqtt_client.c`)

- `esp_mqtt_client_publish(client, topic, data, len, qos=1, retain)` enqueues
  into the **outbox** and returns a msg_id. The outbox entry persists until the
  broker's PUBACK arrives.
- **Reconnect resend:** `mqtt_resend_queued()` re-sends unacked QoS 1/2 messages
  with DUP=1 when the transport reconnects (MQTT-4.4.0-1 behavior). The client
  uses clean session by default (`disable_clean_session` is false → clean
  session true), but the *local* outbox survives reconnect regardless — the
  resend happens from the client's outbox, not the broker session.
- **Not-connected publish:** if `client->state != MQTT_STATE_CONNECTED`,
  a QoS 1 publish still enqueues (returns msg_id) and is sent on reconnect.
  QoS 0 in the same situation returns -1 and the message is lost. **This is the
  single biggest reliability win of moving chunks to QoS 1.**
- **Outbox full:** `esp_mqtt_client_publish` returns **-2** if
  `config->outbox.limit > 0` and the limit would be exceeded. It **never
  blocks**. Current code does not set `outbox.limit` → default 0 = unlimited
  (memory risk, no drops from this path).

### Sharp edges

1. **`OUTBOX_EXPIRED_TIMEOUT_MS` (default 30 s).** Messages that stay in the
   outbox *longer than 30 s before being published* are silently discarded
   (`outbox_delete_expired`). Scenario: WiFi drops for 35 s mid-frame with
   QoS 1 chunks queued → chunks evaporate from the outbox even though QoS 1
   "guarantees" delivery. **For a 1.3 MB / ~3 min transfer this is a real
   hazard.** Mitigations: raise to e.g. 300 000 ms via `MQTT_USE_CUSTOM_CONFIG`,
   or (better) make the producer's 2 s bounded block the backpressure point so
   the outbox never holds more than a few seconds of data.
2. **Outbox memory is internal by default.** Kconfig `MQTT_USE_CUSTOM_OUTBOX`
   / "use external memory for outbox data" exists. With ~11 KB chunk payloads
   × dozens in flight, put the outbox in PSRAM
   (`config->outbox` + the Kconfig external-memory option) — internal SRAM has
   ~700 bytes free per the project notes; this is non-negotiable.
3. **`cfg.buffer.out_size = 8192` currently.** The out buffer must hold the
   largest single PUBLISH packet (topic + ~11 KB chunk JSON + MQTT header).
   **8192 is too small for 11 KB chunks** — `mqtt_client.c` fragments only if
   configured; otherwise the publish fails or truncates. Raise `out_size` to
   ≥ 16384 (chunk JSON + headroom). `buffer.size` (inbound) can stay 8192 —
   inbound commands are small.
4. **One in-flight PUBLISH at a time.** esp-mqtt serializes publishes through
   a single transport write path (`pending_publish_qos` / `pending_msg_id`
   state). Throughput is therefore bounded by (PUBLISH bytes + PUBACK RTT),
   not by TCP windowing alone. With QoS 1 on a LAN (RTT ~5 ms), per-chunk
   overhead is ~2 RTT ≈ 10 ms → 163 chunks ≈ 1.6 s of pure protocol overhead.
   Acceptable within the ≤ 6 s target, but it means QoS 1 is *not free*.
5. **No `MQTT_EVENT_PUBLISHED` handler currently.** For flow control or
   per-chunk confirmation, register it — but note it fires per PUBACK, and at
   163 chunks the event-queue churn is fine (it's the same mechanism as the
   existing event handler).

### Reconnection

- `cfg.network.reconnect_timeout_ms = 5000` — esp-mqtt's own reconnect; the
  file header says "do not fight it." Correct: don't add a second reconnect
  layer.
- On `MQTT_EVENT_DISCONNECTED`, `native_up_ = false`; `drain_tx_queue`
  **drops the backlog while offline** ("stale lines would confuse the host").
  For telemetry that's right; **for image chunks it must change** — B1's
  bounded queue + QoS 1 outbox is what makes chunk survival across a flap
  possible. The `drain_tx_queue` drop-while-offline path needs a chunk-aware
  exception: chunks stay queued (they're in the outbox anyway once published),
  telemetry may drop.

---

## 3. LWIP TCP tuning for ~1.3 MB transfers

### Current vs proposed

| Knob | Current (sdkconfig) | B3 target | Notes |
|------|--------------------|-----------|-------|
| `TCP_MSS` | 1440 | 1440 (keep) | WiFi MTU 1500 − 40 |
| `TCP_SND_BUF_DEFAULT` | 5744 (~4×MSS) | ~16384 (~11×MSS) | **the** throughput knob |
| `TCP_WND_DEFAULT` | 5760 | 16384 (match) | advertised receive window; keep ≥ SND_BUF |
| `TCP_SND_QUEUELEN` | default (derived) | verify ≥ 2×SND_BUF/MSS in pbufs | lwIP requires this ratio or things break |

### The math

TCP throughput ≤ min(SND_BUF, peer WND) / RTT.

- Today: 5744 B / 10 ms RTT ≈ **574 KB/s** → 1.3 MB in ~2.3 s *at the TCP
  layer*. Sounds fine — but this is the ceiling before TLS record overhead,
  MQTT framing, esp-mqtt's serialized publish path, and PUBACK latency.
- B3: 16384 B / 10 ms ≈ **1.6 MB/s** → comfortable headroom for the ≤ 6 s
  (and later ≤ 3 s) wall-time targets.

### Costs and constraints

- SND_BUF is **per-PCB, from the LWIP heap**. One MQTT TLS connection +
  maybe a second socket: 2 × 16 KB = 32 KB. The plan notes the
  "Wi-Fi/LWIP-PSRAM option per SRAM doc §5.6" — with ~700 B internal free,
  the LWIP heap for these buffers should come from PSRAM if that option is
  enabled; otherwise consciously spend internal and re-verify the
  `size-components` SRAM attribution (carry-over board item).
- `TCP_SND_QUEUELEN` **must** be ≥ (2 × TCP_SND_BUF / TCP_MSS) in pbufs
  (≈ 23 pbufs at 16384/1440); ESP-IDF derives it from SND_BUF by default —
  verify after the change, don't assume.
- Raising WND increases the *receive* buffer symmetrically; harmless here
  (device mostly sends).
- TLS (mbedTLS) has its own record layer (16 KB max records); the SND_BUF
  increase composes fine — TLS just makes more `send()` calls.

### What B3 does NOT need

- Window scaling (`LWIP_WND_SCALE`): 16384 < 65535, no scaling needed.
- MSS changes: 1440 is already optimal for 1500-MTU WiFi.
- `TCP_OVERSIZE`: leave at default.

---

## 4. Chunk envelope design (B5)

### Current envelope (0.4.18.88, streaming)

```json
{"t":"snaptest_chunk","id":0,"seq":157,"total":-1,"b64":"..."}
```

Gap detection today: receiver sorts by `seq`, requires contiguous 0..N, finds
IEND by decoding the tail. Works (proven 163/163) but has no per-chunk
integrity and no row geometry — a corrupt chunk is undetectable until the
whole frame fails IEND validation.

### Precedent in-tree: `strip_chunk` already carries geometry

```json
{"t":"strip_chunk","strip_idx":..,"strip_count":..,"y0":..,"h":..,
 "seq":..,"total":..,"last":..,"b64":"..."}
```

`y0`/`h` are exactly B5's `row0`/`nrows` idea, already proven in the strip
path. B5 should mirror this field naming where possible.

### Proposed B5 envelope (per chunk, QoS 1)

```json
{"t":"snaptest_chunk","id":0,"seq":157,"total":163,
 "row0":1408,"nrows":2,"crc32":"a1b2c3d4","b64":"..."}
```

| Field | Purpose |
|-------|---------|
| `seq` | ordering + gap detection (existing) |
| `total` | **known up front** (B5) — replaces `total:-1` streaming; receiver knows the full set |
| `row0`, `nrows` | row-aligned chunk geometry → **gap rendering** (gray bands) without decoding |
| `crc32` | per-chunk integrity of the *decoded* bytes; corrupt chunk → mark missing, don't poison the frame |
| `b64` | chunk payload, independently padded (existing convention — keep) |

Design notes:
- `total` known up front requires the encoder to know the chunk count before
  streaming. With row-aligned chunks of fixed `nrows`, total =
  ceil(972 / nrows) — computable before encode starts. (With full-flush per
  chunk, each chunk's compressed size varies but the *count* is deterministic.)
- `crc32` should cover the decoded PNG bytes of that chunk (not the base64
  text) — the receiver already base64-decodes per chunk.
- Keep `id` (frame/session id) — the bridge keys reassembly on it.
- 2 rows ≈ 7.8 KB vs 4 rows ≈ 15.6 KB per chunk (plan's open decision):
  smaller chunks = finer-grained retransmit + smoother progress, but more
  MQTT packets (≈486 vs ≈243 for 972 rows) and more PUBACK overhead at QoS 1
  (~10 ms/chunk → 4.9 s vs 2.4 s protocol overhead). **Recommend 4 rows** as
  the starting point — fewer packets, still fine-grained enough for gray-band
  rendering. Measure before/after per the plan's instrumentation rule.

### B4 gate (ROM-miniz full-flush test)

The decision B5 depends on: can the ROM miniz compressor emit
`TDEFL_FULL_FLUSH` boundaries such that each chunk raw-inflates standalone?
Test: compress with per-chunk full flush, then raw-inflate chunk *k* alone for
every *k* and byte-compare against the corresponding slice. If any chunk fails
→ vendored-miniz fallback (plan's words). This is a 30-minute experiment that
must land before B5's format change.

---

## 5. QoS 1 manifest pattern

### The problem with per-chunk QoS 1 alone

QoS 1 guarantees *each message* arrives ≥ once, but the receiver still needs
to know **when the frame is complete** and **which chunks form the set**.
Options:

**Option A — manifest-first (recommended):**
1. Publisher sends `{"t":"png_manifest","id":0,"total":163,"rows":972,
   "row0":[0,4,8,…],"crc32":"<frame crc>","chunk_crc":["a1..","b2..",…]}`
   with **QoS 1, retained = false** *before* the first chunk.
2. Chunks follow, QoS 1, any order tolerated.
3. Receiver: on manifest, allocates the frame state; on each chunk, marks
   `received[seq]`; when `received.count == total` → validate frame CRC →
   save. No IEND-sniffing needed (though keep IEND as a belt-and-suspenders
   check on the reassembled bytes).
4. Timeout: if `count < total` after N seconds past the last chunk, the
   receiver publishes `{"t":"png_fetch","id":0,"missing":[12,57]}` (→ B6 ARQ).

**Option B — manifest-last:** chunks first, manifest with `total` last.
Simpler for the encoder (doesn't need the count up front) but the receiver
can't distinguish "frame complete, 2 chunks lost" from "2 chunks still in
flight" — it must always wait for a timeout. Worse UX, slower completion
detection.

**Option C — no manifest, `total` in every chunk:** what B5's envelope
already does (`total:163` per chunk). The *first* chunk acts as an implicit
manifest. This is the minimal change and composes with Option A later:
ship C now, add the explicit manifest when B6 (ARQ) needs the authoritative
chunk-CRC list.

### Recommended sequencing (matches the plan)

- **B1+B2+B5:** per-chunk QoS 1 + `total` in every chunk (Option C). Gap
  detection = `received.count < total` after the encoder signals done
  (the existing `snaptest` result JSON with `soi/eoi` already serves as the
  "done" signal — reuse it, don't invent a new one).
- **B5 gray-band rendering:** with `row0/nrows` per chunk, the page/bridge can
  render exact gray bands for missing rows instead of failing the frame.
- **B6:** explicit manifest (Option A) + `png_fetch` retransmit, ≤ 2 rounds.
  The manifest's `chunk_crc[]` lets the receiver distinguish *missing* from
  *corrupt* (CRC mismatch → treat as missing → fetch).

### Deduplication

QoS 1 means the receiver **will** see duplicates (DUP=1 on resend). The
receiver must be idempotent per `(id, seq)`: "first write wins," count each
seq once. The bridge's `pending["chunks"][seq] = b64` dict assignment is
already naturally idempotent. The page's `slot.b64BySeq[seq]` likewise.
No extra work needed — just don't `push()` duplicates into a list.

---

## 6. Implementation checklist for Track B (concrete)

**B1 — mirror path (mqtt_native.c):**
- [ ] `tx_q_` depth 16 → 64 (covers ~2 s of chunks at full encoder speed as a
      shock absorber; the 2 s bounded block is the real backpressure)
- [ ] `xQueueSend(tx_q_, &m, 0)` → `xQueueSend(tx_q_, &m, pdMS_TO_TICKS(2000))`;
      on timeout: `chunk_dropped_++`, `ESP_LOGE` (never silent), free
- [ ] `xTaskNotifyGive` on enqueue; `native_task` waits on notify (keep 1 s
      fallback timeout for heartbeat cadence)
- [ ] `chunk_dropped_` (`uint64_t`, monotonic) → heartbeat/status JSON
- [ ] Chunks publish QoS 1; telemetry stays QoS 0 (split by message type in
      `drain_tx_queue` — needs an `is_chunk` flag on `tx_msg_t`)
- [ ] `drain_tx_queue` drop-while-offline: exempt chunks (they're QoS 1 in
      the outbox; the local queue is just a staging area)
- [ ] `cfg.buffer.out_size` 8192 → 16384; outbox data → PSRAM (Kconfig)

**B2 — skip USB ring (link.c `link_send`):**
- [ ] Detect chunk: the cJSON object has `"t":"snaptest_chunk"` (or
      `"png_chunk"`). `link_send` receives the object *before* serialization —
      check `cJSON_GetObjectItem(obj,"t")` string compare. Cheap.
- [ ] `if (is_chunk && mqtt_native_is_up())` → skip the `ring_push_nb` block
      entirely, still call `tx_mirror_cb_`. (Mirror is the only path in native
      mode.)
- [ ] `mqtt_native.h` needs `mqtt_native_is_up()` — **already exists**
      (line 800-ish: `bool mqtt_native_is_up(void)`). No header change needed.
- [ ] Edge: `native_up_` flaps mid-frame → chunks split across USB ring and
      MQTT. The receiver keys on `(id, seq)` per transport; document that a
      mid-frame flap produces two partial receivers and the frame is
      abandoned (encoder aborts on flap — simplest correct behavior).

**B3 — LWIP (sdkconfig.defaults):**
- [ ] `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`,
      `CONFIG_LWIP_TCP_WND_DEFAULT=16384`; verify `TCP_SND_QUEUELEN` derives
      ≥ 23; `idf.py reconfigure` (sdkconfig exists → defaults don't apply
      without it — AGENTS.md lesson)
- [ ] Confirm LWIP heap placement (PSRAM option per SRAM doc §5.6) or
      re-run `size-components` for the internal-RAM cost

**B4 — ROM-miniz gate:** 30-min experiment, standalone from firmware.

**B5 — envelope (main.c `on_snaptest_chunk` + eyes.c chunker):**
- [ ] Fixed `nrows` (recommend 4), `total = ceil(972/nrows)` up front
- [ ] Per-chunk `TDEFL_FULL_FLUSH`, `row0/nrows/crc32` fields
- [ ] Receiver (page + bridge): gap → gray bands via row geometry

**B6 — ARQ:** manifest + `png_fetch`, stretch goal.

---

## 7. Risks and open questions

1. **esp-mqtt serialized publish + QoS 1 PUBACK latency** is the throughput
   ceiling, not TCP. If measurement shows chunk pacing ≫ 10 ms/chunk, look at
   the mqtt task priority (currently 5) vs the WiFi/LWIP task priorities.
2. **30 s outbox expiry** vs multi-minute transfers: raise
   `OUTBOX_EXPIRED_TIMEOUT_MS` or keep the outbox shallow via B1's 2 s block.
   Do one of these; doing neither risks silent chunk evaporation on a
   >30 s WiFi flap.
3. **TLS handshake on reconnect** (~1–2 s, PSRAM ok) — fine for telemetry,
   but a mid-frame reconnect + 2 s B1 block + 5 s esp-mqtt reconnect timeout
   can exceed the encoder's patience. The abort path must stay loud.
4. **`mqtt_native_mirror_tx` malloc per chunk** (~11 KB PSRAM each, 64-deep
   queue = 704 KB worst case). PSRAM has room, but consider a free-list or
   passing the already-serialized buffer ownership instead of copying.
5. **QoS 1 duplicates vs `total` counting:** duplicates are idempotent by
   `(id,seq)` dict — no count inflation. Verified in both receivers.

---

*End of research. All code citations are from the 0.4.18.88 tree unless noted.*
