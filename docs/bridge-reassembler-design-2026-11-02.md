# Bridge snaptest reassembler: design recommendation (2026-11-02)

Question (John): the reassembler should "do our best to display the data
if we get enough to be meaningful" — recover, or blank corrupted
segments? This doc examines the draft hygiene patch, the live handler,
and the actual stream format, and recommends a tiered design.

## What the draft patch does (snaptest-reassembler-hygiene.patch.DRAFT)

Four changes, all hygiene, all worth keeping:

1. Seq-0 chunk for an id with pending state resets the stream.
2. Strict base64 validation per chunk — **but a bad chunk poisons and
   drops the whole stream.**
3. Save requires seqs starting at 0, gap-free; `_pending` is cleared on
   any validation failure.
4. Full PNG chunk CRC walk before saving.

It fixes the splice bug. It is also strictly all-or-nothing: one
truncated chunk (the seq-167 case) discards a capture that is ~98%
intact. That is the behavior under review.

## What the live handler is missing (beyond the draft)

- **It never hears the end of a stream.** Grading happens only when a
  chunk happens to end with the IEND trailer. Both terminal signals
  already flow through `handle_snaptest_message` and are ignored:
  the device's `{"t":"snaptest", "bytes": N}` reply (exact expected
  byte count; `err` when the device aborted) and the page's
  `snaptest_image` verdict. A stream that loses its tail sits in
  `_pending` forever — that limbo is what made the splice possible.
- **It ignores `_mid`.** Chunks and the terminal reply share the
  command's `_mid`; that is a ready-made stream-generation key.
  Keying state by it kills the splice class structurally, not just
  by seq-0 heuristics.
- **It ignores `blen`** (in every chunk since firmware 0.4.21). A
  chunk whose received length differs from `blen` is provably the
  corruption point — no guessing.
- Seq 0 is the 8-byte PNG signature (a format constant) and seq 1 is
  IHDR (25 bytes); losing seq 0 alone should not kill a stream — the
  signature is reconstructible by definition, not invented data.

## Stream facts (measured on real captures, 2026-11-02)

- 1296×972 RGB8; one zlib/deflate stream across 167 IDATs of exactly
  8192 bytes; one MQTT chunk ≈ one IDAT (8204 B with PNG framing);
  every row uses filter 1 (SUB); scanline = 3889 B; raw total
  3,780,108 B.
- **Prefix yield is linear**: feeding the stream up to a gap recovers
  that fraction of rows, from the top, pixel-perfect. Gap at 25% →
  251/972 rows; at 50% → 496; at 96% → 927; at 99.5% → 965.
- **No resync exists.** Continuing decompression after a gap raises
  (`invalid distance code`); starting fresh mid-stream raises
  (`incorrect header check`). Bytes after a gap are unrecoverable
  with the current format. (Per-chunk full-flush + row-aligned
  chunks — plan items B4/B5 — would change this; that is a firmware
  decision for another day, not bridge behavior.)
- **Splices self-detect.** Feeding a different capture's data after
  a prefix raises immediately (`invalid block type`) and emits zero
  additional rows. Deflate is its own tripwire.
- **Partial construction is proven**: real prefix rows (original
  filter bytes + filtered data, untouched) + one red cut-line row +
  gray (128) pad rows, re-encoded, produces a strictly valid PNG
  whose recovered scanlines are byte-identical to the original.

## Recommendation: three tiers, blank-don't-invent

Keep all of the draft's hygiene; replace its all-or-nothing save
with grading. A stream is graded when it ends — on the IEND chunk,
on the device reply, on the page verdict, or when a new stream
starts (grade the stale one first). Chunks are consumed only from
the verified prefix: contiguous from seq 0, each `blen`-matching
and strictly decodable. The first gap/corruption ends the prefix;
nothing after it is ever used.

- **COMPLETE** — assembled length equals the device-reported byte
  count, CRC walk clean, full decompress exact. Saved as today
  (`snap-<ts>-<id>.png`). Nothing changes on the happy path.
- **PARTIAL** — stream ended incomplete, but the verified prefix
  yields at least the floor (recommendation: **48 rows ≈ 5%**) of
  complete scanlines. Save a constructed PNG: real rows, one red
  cut-line row, gray below — `snap-<ts>-<id>-partial.png` — plus a
  sidecar JSON (`rows_decoded`, `rows_total`, `first_gap_seq`,
  `missing_seqs`, `corrupt_seqs`, `expected_bytes`, `got_bytes`,
  grading trigger). The log line says PARTIAL with the row count.
  A partial can never be mistaken for a capture: the banding is in
  the pixels, the name, and the metadata.
- **FAILED** — below the floor, or no IHDR. No image. The per-seq
  chunk files stay on disk (they always do), the log says why, and
  state clears.

Consequences worth noting:

- The 0.4.21 chunk-42 abort would have produced a labeled partial
  with the top ~250 rows (console header, status, top of the log)
  instead of nothing.
- The October standing rule "a missing chunk aborts the image" was
  aimed at never rendering corrupt data *as if* complete. This
  design keeps that guarantee — completes are bit-exact, partials
  contain only verified bytes and are unmistakably marked — while
  no longer throwing away a 95%-good capture over one chunk. The
  rule evolves; its intent doesn't.
- The console page is untouched (its strict verdict stands). Live
  partial display in the console is a separate, batched console
  change if wanted later.
- Floor choice is the one real policy knob. 5% (~48 rows) is the
  top strip of the screen — console title and status pills; enough
  to answer "what state was the bench in". 25% (~243 rows) is a
  quarter screen. The chunk files preserve everything either way;
  the floor only decides what gets minted as an image.

## Implementation sketch

Rewrite the streaming path of `snaptest_handler.py` (~150 lines):
key state by `(id, _mid)`; ingest with `blen` + strict-b64 checks
that mark (not poison); grade on the four triggers; `_png_walk_ok`
from the draft for completes; prefix decoder + partial builder;
sidecar writer. Offline test battery before applying:

1. The five 2026-11-02 verification captures, re-chunked at the
   real emission boundaries and replayed → byte-identical completes.
2. The draft's splice repro → second stream completes; first is
   graded, never spliced.
3. Drops at IDAT 8/42/84/160 → partials at 44/251/496/927 rows (±2).
4. A `blen`-truncated chunk → boundary at that seq.
5. 42 chunks + device `err` reply → immediate ~250-row partial.
6. Missing seq 0 only → still completes (signature is a constant).

Then apply, restart the subscriber, and confirm with one live
snaptest. No firmware or console changes.


---

## Addendum — IMPLEMENTED 2026-11-02 (16:05 PST)

John's floor decision: **save a partial for any complete rows at all (≥1)** — no
minimum-row floor. Rationale (his words): "If we don't save the files we can't
inspect them to decide if we should have saved them." The floor policy gets
revisited with real partial files in hand, not before.

Implementation: `snaptest_handler.py` rewritten as designed above ((id,_mid)
keying, blen + strict-b64 mark-not-poison ingest, grading on IEND / device
terminal reply / page verdict / new-stream start, `_png_walk_ok` + exact
decompress for completes, sliced-feed prefix decode with filter-byte brake for
partials, red cut-line + gray pad, JSON sidecar). The earlier
`snaptest-reassembler-hygiene.patch.DRAFT` is superseded (old handler backed up
as `snaptest_handler.py.pre-tiered-20261102`). Console page untouched.

Offline battery (`test_snaptest_handler.py`, 17 checks, ALL PASS): five
2026-11-02 verification captures replay byte-identical; splice repro grades
stream A as a 496-row partial and stream B completes byte-identical; drops at
IDAT 8/42/84/160 → partials at 44/251/496/927 rows with pixel-identical tops;
blen-truncated chunk → corrupt boundary, 583 rows; 42-IDAT abort + err reply →
251-row partial graded on the reply; missing seq 0 → still completes; a
same-length wrong-data chunk mid-stream leaks zero garbage rows (297 = the
genuine prefix); missing only the IEND chunk → 972/972-row partial via the page
verdict trigger; a 1-IDAT prefix → 4-row partial under the any-rows policy.

Production verification: subscriber restarted via the watchdog path (pid
1719 → 15496), bridge health green, then a live bridged snaptest completed
through the new handler — `snap-20261102-160535-0.png`, 1,379,453 bytes exactly
equal to the device-reported bytes, CRC walk clean, IDAT decompresses to
exactly 3,780,108 bytes.
