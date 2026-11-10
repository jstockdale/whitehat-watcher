# Streaming audio — offline verification results (B1, B2, B4)

**Scope:** the offline half of the verification plan in
`docs/audio-reader-spec-2026-11-09.md` §(b)/(c) — items 1, 6, 7 of
the updated plan table. No device, no bench, no code fixes: the B3
`total: 0` guard was **not** applied (it lands separately, after the
streaming-base pin); divergences are documented, not repaired.
John approved the spec's recommendations 2026-11-09.

**Deliverables (all beside the bridge's existing tests, following
their conventions):**

| Item | Harness | Result |
|---|---|---|
| B1 bridge traces | `host-agent/mqtt-bridge/test_audio_handler.py` | 30/30 hard checks pass; 3 expected-fails (documented traps) |
| B2 console traces | `host-agent/mqtt-bridge/test_console_audio.js` | 25/25 checks pass |
| B4 byte-identity | `host-agent/mqtt-bridge/test_audio_byte_identity.py` | 12/12 checks pass |

Run them with `python3 test_audio_handler.py`,
`node test_console_audio.js`, `python3 test_audio_byte_identity.py`
from `host-agent/mqtt-bridge/`. All exit 0. Harness hygiene: B1/B4
redirect `audio_handler`'s capture dir + transcript log to a temp
dir and stub transcription (no Whisper load, nothing written into
`audio-captures/`); B2 opens the console HTML read-only
(`fs.readFileSync` only — file md5 `a22ef00d…`, mtime 2026-11-09
09:18 UTC, unchanged).

---

## B1 — bridge `AudioReassembler.feed()` (test_audio_handler.py)

Payload set: 5 deterministic chunks (4×1536 B + 700 B tail), shared
across traces so WAV comparisons are byte-exact. Spec pass bar:
exactly one completion per take per trace; WAV bytes identical
across (i)/(ii) for the same payload set.

| Trace | Expected (spec) | Observed | Verdict |
|---|---|---|---|
| (i) legacy, total on every chunk | complete once, on the final chunk; later `audio_end` a no-op (audit walkthrough ¶2–3) | exactly that; WAV = decode of the full stream | **pass** (4 checks) |
| (ii) streaming, total only on final | no completion while total unknown; complete on final chunk; `audio_end` no-op | exactly that; **WAV byte-identical to (i)** | **pass** (4 checks) |
| (iii) final chunk lost | `audio_end` adopts `chunks`; completion logs the missing seq; partial decode (walkthrough ¶4) | completion on `audio_end`; log line `missing chunks [4]`; WAV = decode of chunks 0–3 | **pass** (3 checks) |
| (iv) `audio_end` lost | 45 s sweep completes; denominator = arrived count (walkthrough ¶5) | sweep completes once; saved line reports `4/4 chunks`; no slot left pending | **pass** (3 checks) |
| (v) `total: 0` chunk | trap (R2): completes on chunk 0, remainder becomes a second fragment — fail-safe only after B3 | **exactly as predicted**, plus a filename collision the spec did not state (see D-V1) | observed-behaviour checks **pass** (4); desired-behaviour assertions **xfail ×2** (flip when B3 lands) |
| (vi) duplicates | idempotent — one completion, undisturbed WAV | duplicates while the slot is live: one completion, WAV identical to (i) | **pass** (2 checks) |
| (vi-b) final-chunk redelivery *after* completion + `audio_end` | spec silent (see D-V2) | a second, 1-chunk fragment completes (`missing chunks [0, 1, 2, 3]`) | observed-behaviour check **pass**; desired-behaviour assertion **xfail ×1** |
| (vii) mid-stream hole (seq 2) | no completion at final chunk (4 < 5); `audio_end` completes and names the hole (R4) | exactly that; `missing chunks [2]`; WAV = decode of present chunks concatenated | **pass** (4 checks) |
| (viii) interleaved takes | two takes stay separate (added per tasking; spec lists 7 traces) | both complete exactly once; with distinct payload sets, each WAV matches its own take — no cross-talk | **pass** (2 checks) |
| (ix) out-of-order, total-chunk early | completion only when the set is full; order-insensitive (added per tasking) | no completion at the early total-carrying chunk (3 < 5); completes on the last needed chunk; WAV identical to (i) | **pass** (3 checks) |

Every step of the spec's §(a) bridge walkthrough (¶1–6) is now
exercised and confirmed, including its ¶6: with one feed path,
nothing double-completes.

## B2 — console `privacyAudioChunk/End/Complete` (test_console_audio.js)

The three functions plus the ADPCM tables, `b64ToBytes`,
`adpcmDecode`, `wavBlob` are extracted **verbatim** from the shipped
console source (`~/workspace/your_files/watcher-console/watcher-console.html`,
the RUNBOOK-named copy; privacy functions at :2216–2222) and run
under Node 24 with `state` / `updateLastAudio` / `privacyAdd`
stubbed — completions are observed through the same two exits the
page uses. (The file defines `b64ToBytes` twice, :528 and :2042;
the implementations are byte-identical, and the harness uses the
one in scope before `privacyAudioChunk`.)

| Trace | Expected (spec) | Observed | Verdict |
|---|---|---|---|
| (i) legacy | complete once on final chunk (walkthrough ¶2) | once; Privacy note `5/5 chunks` | **pass** |
| (ii) streaming | slot persists without total; completes on final chunk | exactly that; **WAV blob byte-identical to (i)** | **pass** |
| (iii) final lost | `privacyAudioEnd` adopts `j.chunks`, completes partial (walkthrough ¶3) | completes on end; note `4/5 chunks` | **pass** |
| (iv) final chunk **and** `audio_end` lost | **R3: slot strands, take never surfaces** (walkthrough ¶4) | confirmed precisely: no completion, nothing added to the Privacy tab, slot still in `state.audioChunks` with `got=4` — asserted as observed behaviour so the gap stays documented until B5 | **pass** (documents R3) |
| (v) `total: 0` | console immune — it guards `j.total > 0` (:2220) | no completion on the `total: 0` chunk; the take later completes *whole* via `audio_end` (`5/5 chunks`, WAV identical to (i)) | **pass** |
| (vi) duplicates | sparse-map dedupe: `got` counts unique seqs only (audit row 14) | one completion; WAV identical to (i) | **pass** |
| (vii) hole | completes via `audio_end`; only `got/total` tells (R4) | completes; note `4/5 chunks` | **pass** |
| (viii) interleaved | (added per tasking) | both complete once; distinct payloads, no cross-talk | **pass** |
| (ix) out-of-order | seq-keyed parts make order irrelevant | completes when the set fills; WAV identical to (i) | **pass** |

25/25. The spec's console predictions hold in full — including the
two unflattering ones (R3 stranding, R4's quiet `got/total`-only
hole signal).

## B4 — byte-identity (test_audio_byte_identity.py)

**The corpus the spec assumed does not exist — divergence D-V3.**
Spec B4 says to "replay archived pre-streaming takes (the
`audio-captures/` corpus / telemetry-extracted 0.4.48-era chunk
sets)". Neither source holds chunk sets:

- `audio-captures/` contains only decoded WAVs and
  `transcripts.log` (30 files) — chunk JSON was never archived.
- `telemetry.log` does contain audio chunk lines, but the logger
  truncates each line at **290 characters** (measured: both chunk
  lines for take id 53096 are exactly 290 chars; a full chunk line
  is ~2.1 KB). All 73 `"t":"audio"` lines in the log are
  unparseable as complete chunks. The payloads are unrecoverable.

So, per the fallback, the check was **constructed** from the
firmware encoder's parameters: a host mirror of `ptt.c`'s
`adpcm_nibble` (:118–140; state reset :1071–1072; low-nibble-first
packing :1145–1146; 1536 B chunks :93) encodes a scripted 3.06 s
take (49,000 samples: decaying two-tone + noise burst + trailing
quiet → 24,500 ADPCM bytes → 16 chunks, last partial at 1,460 B),
fed through `feed()` in both shapes.

| Check | Observed | Verdict |
|---|---|---|
| encoder anchors | bridge `adpcm.py` selftest passes; silence encodes to all-zero bytes and round-trips to silence; encoder deterministic | **pass** ×3 |
| stream geometry | 2 samples/byte exactly; take has a partial final chunk (16 total) | **pass** ×2 |
| legacy shape | exactly one completion | **pass** |
| streaming shape | exactly one completion | **pass** |
| **WAV bytes, legacy vs streaming** | **byte-identical** | **pass** |
| WAV parameters | 16 kHz / mono / 16-bit | **pass** |
| WAV frames vs decoder | frames == `adpcm_decode(stream)` exactly | **pass** |
| payload integrity | fed chunk payloads concatenate to the ADPCM stream byte-for-byte — framing adds/drops nothing | **pass** |

Design item 1's host-side half is discharged: for the same ADPCM
stream, framing shape (legacy vs streaming totals) cannot change
the delivered WAV. (The device-side half — the sender's b64 routine
is unchanged — remains the audit's code-read, as the spec states.)

---

## Divergences from the spec's predictions

**D-V1 — `total: 0` fragments collide on filename (detail beyond
the spec).** Spec B3 predicts "one take, two WAVs". Observed: both
fragments are named `ptt-<stamp>-<id>.wav` with a **1-second
timestamp**, and the trap trace completes both within the same
second — so fragment 2 **overwrites fragment 1 on disk**. What
survives is only the second, incomplete fragment (chunks 1–4,
decoded with `missing chunks [0]`), while `transcripts.log` gains
two entries pointing at the same path. Evidence (B1 trace v):
fragment-1 WAV, captured at completion time, is the chunk-0-only
decode `… (0.2s, 1/0 chunks)`; the harness's path-identity probe
prints `fragment paths identical (same-second overwrite): True`.
The B3 fix moots this along with the split itself; noting it
because "two WAVs" understates the on-disk outcome.

**D-V2 — NEW: post-completion redelivery resurrects a fragment
(bridge).** Not predicted anywhere in the spec. If the final chunk
is delivered a second time *after* the take completed (at-least-once
redelivery shape), the popped id is unknown to the handler: the
stray chunk opens a fresh slot carrying `total`, and the take's
`audio_end` then completes it as a 1-chunk fragment WAV
(`missing chunks [0, 1, 2, 3]`) — a spurious extra "take" with its
own transcript attempt (B1 trace vi-b). Same class as R2 (latent:
the audit's ¶6 stands — no current configuration double-feeds a
take; console ≥0.4.186 audio rides only `watcher/audio`), and the
same shape a B3-style guard cannot fix; a real fix would need a
completed-id tombstone window, which is **not** specced and was
**not** built. Recorded for the owner; the harness asserts the
observed behaviour and carries the one-completion expectation as
an xfail so any future tombstone work flips it visibly.

**D-V3 — B4's corpus premise (above).** The spec's replay source
doesn't exist; the check was constructed from the encoder
parameters instead, as the tasking's fallback prescribed. If a
future bench session wants the gold-standard version, one take's
chunks must be captured off the wire (or out of
`subscriber.log`-grade untruncated logging) before the fact.

**Everything else matched.** All remaining spec predictions —
bridge walkthrough ¶1–6, console walkthrough ¶1–4, R2's mechanics,
R3's stranding, R4's missing-seq log line, console `total: 0`
immunity, duplicate idempotence in both readers — reproduced
exactly under the harnesses.

## What this discharges, and what remains

- Plan items **1, 6, 7** (§(c) table): **done offline**. Item 7's
  fail-safe half still waits on **B3** (approved, lands separately
  after the streaming-base pin) — B1(v)'s two xfails are its
  acceptance test.
- Items **2–5** still need the bench (B6) and John's voice/hand,
  per the runsheet — unchanged, and per D4 they were gated on this
  offline half, which is now green.
- **B5** (console sweep for the R3 stranding) remains as specced:
  rides the approved console batch; B2(iv) is its before-picture.
