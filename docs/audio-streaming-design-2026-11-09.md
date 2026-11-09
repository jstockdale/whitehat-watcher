# Streaming audio shipping — design (2026-11-09, for fw 0.4.49)

John's brief (2026-11-08, approved 2026-11-09): replace store-then-send
with a meticulously implemented ring buffer + sender task, so sends
overlap capture instead of following it.

## What ships today (0.4.48) and why it changes

`rec_feed()` encodes PCM→IMA-ADPCM into a full-take PSRAM buffer
(240 KB at the 30 s cap). At take end, `rec_stop_and_send()` — running
in the PTT task — base64s and blasts every chunk back-to-back
(paced only by link-ring space, 3 attempts each, drops counted).
Costs: post-take latency equals the whole take's ship time; the burst
competes with telemetry/logs on the shared serial ring; and while the
burst is generally survivable at 750000 baud, the path has no slack —
the two observed chunk-loss events (22/47 delivered; take 35833's back
half) both happened around bursts/reconnect pressure.

## The wire-contract constraint (measured, not assumed)

Both consumers key on a **constant, final** `total` in every chunk:

- Bridge `audio_handler.feed()`: opportunistic completion fires when
  `len(chunks) >= total`. A running total (seq+1) completes the take
  on chunk 0 and pops the slot; `total: 0` completes instantly too.
- Console `privacyAudioChunk()`: `if (!slot || slot.total !== total)`
  **resets the slot**, discarding prior parts. A changing total
  destroys the privacy reassembly; a wrong constant (e.g. the max)
  survives but prints false "missing chunks" telemetry at the bridge
  and a false "50/157" note in the privacy log.

Rejected: running totals, `total: 0`, max-as-total. The honest fix is
a small protocol evolution, versioned across all three components in
one release — the same pattern as the `reply` command (fw 0.4.45 +
console 0.4.188):

- Chunks omit `total` while streaming. The **final** chunk carries
  the true `total` (and no other new fields — presence of `total`
  *is* the final marker for consumers that track it).
- `audio_end` is unchanged (already carries `chunks`, `ms`, `stop`,
  and the 0.4.44 VAD fields).
- Bridge: slot total updates only when a chunk carries one;
  opportunistic completion only when a total is known and reached.
  audio_end and the 45 s sweep remain the reliable terminators.
- Console: slot no longer resets when total is absent/changes; parts
  accumulate in a growable array; completion on `got >= total` only
  when total is known, else on audio_end (today's behavior there).

## Device design

**Ring.** PSRAM byte ring, capacity = the full max take
(240,000 B ADPCM) — identical memory to today's `adpcm_buf_`, so the
worst case (link wedged for a whole take) behaves exactly like today,
while the normal case streams. Single-producer (ptt task's
`rec_feed`) / single-consumer (sender task): head/tail indices, no
mutex on the data path; the consumer sleeps on a task notification.

**Producer** (`rec_feed`, unchanged metering/VAD): after encoding
nibbles, appends bytes to the ring instead of `adpcm_buf_`.
Overflow (consumer a full ring behind): the producer **stops
appending and counts unsent bytes** — overwriting oldest is
forbidden (a mid-stream hole silently corrupts ADPCM decoding for
everything after it). The take ships partial with `dropped` in
audio_end, the same honesty contract as today's drop counter.

**Commit point (discard semantics preserved).** The sender emits
nothing until the ring holds ≥ PTT_MIN_MS of audio (2,400 B) or the
take ends with ≥ MIN. A sub-300 ms tap is dropped from the ring with
zero wire traffic — byte-for-byte today's observable behavior.

**Sender task.** Created **once** in `ptt_init` (8 KB PSRAM stack) —
never per take; the 0.4.45 re-arm bug is the standing lesson on
hand-rolled task lifecycles. Per take it receives, via a small
control queue: BEGIN(take_id), then data notifications, then
END(final stats snapshot: dur_ms, peak, stop reason, VAD fields,
unsent-byte count). It slices the byte stream into the same
1536-byte chunks, base64s with the same routine today's loop uses
(identical wire bytes for the same audio), and sends with today's
discipline (`link_wait_for_ring_space` + up to 3 attempts + drop
counting). After END it drains to the captured byte count, emits the
final chunk (with `total`), then `audio_end`, then runs today's
completion sequence **in today's order**: LED working off → success
pulse → AI-thinking start → `i2s_release()` → `wakeword_start()`,
and finally gives the `take_done` semaphore.

**Take gating.** `rec_begin` waits on `take_done` before starting a
new take (encoder state and take ids must never interleave). In the
streaming steady state the drain is at most one chunk behind, so
this wait is ~always already satisfied; it binds only when the link
is wedged — exactly when starting a new take would be wrong anyway.

**Cadence.** Chunks are 1536 B ≈ 0.192 s of audio; the sender wakes
per chunk available, so the wire sees a steady ~5 msg/s trickle at
real-time rate instead of a terminal burst — gentler on the serial
ring, the console's forward queue, and the broker, and post-take
latency drops to ~one chunk's ship time.

## Verification plan

1. Byte-identity: a scripted take's chunk payloads (sans total) match
   the store-then-send encoding of the same ADPCM stream (host-side
   check against the existing decoder corpus).
2. Bench: button take + wake-word take, bridge delivers N/N chunks,
   transcript correct; `dropped` absent.
3. Discard: a <300 ms tap produces no audio traffic (telemetry).
4. Back-to-back takes: second take starts cleanly (take_done gating),
   wake word re-arms and detects again on the same boot.
5. Long take (30 s cap): streams throughout; audio_end stats sane.
