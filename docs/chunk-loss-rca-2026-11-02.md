# RCA: Intermittent PNG chunk loss & truncation (2026-11-02)

Status: **Root cause identified for truncation (device log-ring lap
deletion). Whole-line loss has two candidate mechanisms, one decisive
experiment designed. A separate bridge-side splice bug was found and
proven.** Evidence from bench runs on 0.4.19 / 0.4.20, 2026-11-02.

## The four runs (bridged snaptest, 1296x972 RAW8)

| Run | FW | Device bytes | Page verdict | Bridge outcome |
|---|---|---|---|---|
| 1 (20:35) | 0.4.19 | 1,548,180 | 192 chunks, reassembled | saved, valid |
| 2 (20:53) | 0.4.20 | 1,380,749 | 172, "corrupt chunks" | save failed (chunk 167) |
| 3 (21:00) | 0.4.20 | 1,380,749* | 172, "corrupt chunks" | save failed (chunk 167) |
| 4 (21:06) | 0.4.20 | 1,373,089 | 168, "corrupt chunks" | **"saved" a spliced, corrupt PNG** |

(*Same byte count on runs 2–3 is coincidence of a static scene; fnv1a
differs per run — camera noise makes every capture's PNG distinct.)

Two distinct failure modes, plus one bridge bug:

## Failure mode 1 — TRUNCATED base64 inside valid JSON (run 2/3, seq 167)

**Evidence.** The bridge received all chunk seqs. Exactly one —
seq 167 — arrived with its `b64` string shortened: 10,598 chars on
disk, 10,597 data chars per the bridge's decoder error, vs ~10,940
expected. Shortfall: **342 chars**. The JSON envelope was intact and
parseable (the page parsed it, forwarded it, and the bridge filed it).
Every other chunk decoded cleanly; the last chunk held the exact
12-byte IEND trailer.

A shortened string inside a valid envelope means **bytes were deleted
from the middle of the serial byte stream**, inside the b64 field
(~98% of each chunk message's bytes are b64, so a random deletion
lands there ~98% of the time). MQTT cannot do this (TCP, whole
messages). The page cannot do this (it forwards the string it parsed).
The deletion is in the device→page stream.

**Mechanism (in code).** All device output — logs AND chunk JSON —
shares one 32,768-byte retained-history ring in PSRAM
(`components/link/link.c`). A priority-3 TX task is a *chasing reader*:
it tracks monotonic counters `log_written_` / `tx_read_`, and when
`lag = written − read > LOG_BUF_SIZE` it **resyncs**: it skips
`lag − 32768` oldest bytes and counts them in `log_dropped_`
(link.c, `console_tx_task`). For log lines this is harmless. For
chunk data it deletes a span from the middle of the stream.

Why the reader can fall behind despite flow control:

- `on_snaptest_chunk` (main.c) waits for `strlen(b64)+128` free bytes
  via `link_wait_for_ring_space`, then pushes ~11 KB. Post-push lag can
  therefore reach **exactly 32,768 — 100% full, zero headroom**.
- Every other producer writes to the same ring *uncontrolled*
  (`log_capture_vprintf`): ESP_LOG lines from any task — including the
  per-chunk `png_streaming_sink_cb: entry` log, heartbeats,
  `host_summary`, HID GAP events. Any of these landing while the ring
  sits at 100% pushes lag past 32,768; the TX task's next pass deletes
  the overshoot — from the *oldest unread* position, i.e. inside a
  chunk pushed ~2.8 s earlier (32 KB at 11,520 B/s) that has not
  drained yet.
- `link_ring_free_bytes()` reads the two `volatile uint64_t` counters
  lock-free on a 32-bit CPU (two 32-bit loads each). A torn read can
  transiently over-report free space. The code comment calls the
  approximation "fine for backpressure"; at 100%-full margins it is
  another path to the same lap.
- `fwrite` to USB can stall when the host (browser WebSerial) is slow
  to read; the TX task stalls with it, writers keep writing, lag
  grows. The console re-renders the chunk map's full `innerHTML` on
  every chunk — a plausible source of exactly such host-side stalls.

The observed 342-byte deletion is the expected size class: the
overshoot accumulated between TX-task checks.

This also retro-explains Oct 9: the 12,288-byte ring with a 50%
watermark *guaranteed* laps on every 11 KB chunk (reproducible
truncation); 0.4.18.88's exact-space wait made laps rare instead of
constant (intermittent single-chunk damage survived).

## Failure mode 2 — WHOLE chunks missing (run 4: seqs 4, 9, 126)

**Evidence.** Run 4's device produced 171 chunks (seq 0–170 for a
1,373,089-byte PNG). Bridge chunk files prove the page forwarded only
168: files for seqs 4, 9, 126 still carry run 3's timestamps/content —
the page never emitted them. The page's own counter agrees (got=168).
No received chunk in run 4 was truncated.

**Candidate mechanisms (not yet discriminated):**

- **(a) Same ring lap, different landing zone.** A deleted span that
  straddles a chunk boundary or eats a line's JSON framing makes the
  page's line parser drop the whole line (one boundary-crossing span
  can kill two lines). Three losses in one run implies either several
  laps or one long host stall (TX blocked in `fwrite` while writers
  lapped far past the ring).
- **(b) Page-side drop.** The console's serial line handling drops
  lines under load (its per-chunk chunk-map `innerHTML` rebuild is
  heavy). Nothing yet proves or clears the page.

**Decisive experiment (0.4.21 diagnostic):** expose `log_dropped_`
and a lap-event counter in the heartbeat/`dbg` output, and add the
expected b64 length (`blen`) to each chunk JSON. After each run:
`log_dropped_` delta > 0 during the stream convicts the ring for that
run; delta = 0 with chunks still missing clears the device and
convicts the page/USB hop.

## Bridge bug — franken-PNG splice (proven, host-side only)

`snaptest_handler.py` keeps `_pending[id]` across runs; it is deleted
only on a successful save. After run 3's failed save, run 4's chunks
merged into the stale dict: seqs 4, 9, 126 were silently filled by
run 3's chunks, and the "saved" file is a splice — 1,381,282 bytes
vs the device's 1,373,089, with a PNG chunk walk that derails at
chunk 3 (offset 16,441, absurd length field). The contiguity check
also starts at `seqs[0]` instead of requiring this capture's full
set, and no CRC/full-decode validation gates the save. The bridge
must not be trusted as an artifact source until fixed; the page's
verdict was correct in every run.

## Ruled out

- **0.4.20's timer instrumentation.** It adds two `esp_timer_get_time`
  calls around existing sink calls and touches no buffers; the failure
  modes predate it (Oct 9) and run 1 today (0.4.19, pre-split-timers)
  is the same design. The 0.4.19-clean / 0.4.20-dirty split is sample
  size (1 vs 3) on a probabilistic fault, not a regression signal.
- **MQTT / broker.** Delivers whole messages; the truncated string is
  what the page published (bridge file + decoder error agree).
- **USB serial "random drops" (as a raw medium).** The deletion has a
  precise in-device mechanism; no need to blame the wire.

## Fixes, in order

1. **Headroom margin (device, small, do first).** Chunk producer waits
  for `need + MARGIN` free bytes (suggest MARGIN = 8,192). Post-push
  lag ≤ 24,576; uncontrolled log writes (~100s of bytes between TX
  passes) can no longer lap the reader. This starves failure mode 1
  and the device-side branch of mode 2.
2. **Observability (device, same build).** Heartbeat/`dbg` exposure of
  `log_dropped_` + lap counter; `blen` field in chunk JSON so any
  receiver can pinpoint a truncated chunk deterministically.
3. **Result propagation (device, same build).** `on_snaptest_chunk`
  currently returns `true` without checking `link_send`'s result —
  close that gap so a failed enqueue aborts the encode instead of
  silently shipping a hole.
4. **Bridge reassembler hygiene (host, no flash).** Reset `_pending`
  when a new stream starts (seq 0 with an existing pending entry);
  assemble only this capture's chunks; fully validate (PNG chunk CRC
  walk + decompress) before saving; on any doubt, save nothing and
  say so.
5. **Page (batched console change, John's approval per console
  rules).** Throttle chunk-map re-rendering (it is per-chunk full
  `innerHTML`); split the verdict message into "missing" vs "corrupt"
  using `blen`. Only if mode-2 losses persist after fix 1.
6. **Strategic (already planned).** Track B / B2: chunks leave the
  serial log ring entirely on native MQTT. The ring returns to its
  proper job — logs, where a lapped byte is a shrug, not a corrupt
  image.

## Verification

0.4.21 with fixes 1–3: five consecutive bridged snaptests.
Pass = 5/5 page-reassembled + bridge-saved **and**
`log_dropped_` delta = 0 across all five streams.

## Addendum (2026-11-02, evening): contention failure proven; 0.4.22 verified

The 0.4.21 diagnostic build answered the open question from the main
report — and the answer was *neither* candidate mechanism for
whole-line loss. Run 1 on 0.4.21 (22:58Z) aborted loudly at chunk 42:

```
link: link_send: ring push failed — line dropped
main: snaptest: chunk 42 send failed, ABORTING PNG transfer
```

The ring had space (headroom worked; `log_laps` never moved). The
failure was instantaneous **lock contention**: `ring_push_nb` took
`log_lock_` with a 0-tick try-lock, and every chunk push raced every
ESP_LOG writer on the device. That is the device-side whole-chunk
loss mechanism, and it retrodicts the October single-chunk losses
(seqs 169/140/118) better than the lap-framing theory. The new abort
path converted an invisible hole into a loud, exactly-located
failure — the instrumentation paid for itself on its first run.

**0.4.22 fix:** the push gained a wait parameter (`ring_push_w`).
`link_send` (the data path) blocks up to 250 ms for `log_lock_`;
`vprintf` logging keeps the 0-tick try-lock. `log_lock_` was audited
at all take sites: it is a leaf lock, all holders are bounded
byte-copies (worst case the 32 KB logs-replay snapshot), so the
bounded wait cannot deadlock and times out only if the ring is
genuinely wedged — where failing loud is correct. SHA-256
`79b29cff11d141a315dffe859b56476f64aba5345a06c9fc1ff6239e53c10469`,
transfer `d53aa997`.

**Verification (five consecutive bridged snaptests, 0.4.22):**

| Run | Page chunks | Bytes (device = page = file) | log_laps Δ | Strict PNG |
|---|---|---|---|---|
| 1 | 214* | 1,379,730 | 0 | clean |
| 2 | 170 | 1,366,485 | 0 | clean |
| 3 | 173 | 1,391,746 | 0 | clean |
| 4 | 172 | 1,385,925 | 0 | clean |
| 5 | 170 | 1,369,427 | 0 | clean |

*Run 1's page count includes 42 stale slot counts from the aborted
0.4.21 run; freshness is proven by the byte-exact match and strict
validation (all chunk CRCs valid, IDAT decompresses to exactly
3,780,108 bytes, no trailing data). Zero aborts, zero push failures
across ~857 chunk pushes. `log_dropped` grew only +0..+136 per run
(dropped ESP_LOG lines — the benign tax the counters now make
visible and distinguishable from data loss).

**Status: RCA closed for the bridged serial path.** Remaining items
from the fix list: bridge reassembler hygiene (drafted patch,
unapplied) and Track B (native MQTT removes chunks from the ring
entirely).
