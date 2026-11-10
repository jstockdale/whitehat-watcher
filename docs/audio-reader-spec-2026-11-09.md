# Streaming audio — reader audit + remaining build spec (2026-11-09)

**Scope:** audit of the streaming-shipper design
(`docs/audio-streaming-design-2026-11-09.md`) against the actual tree,
and a build spec for everything still missing. Analysis only — no code
was changed, nothing built, no bench commands run.

**Headline verdict:** the premise that "the reader-side halves may not
have shipped" does **not** survive the audit. All three components
implement the design's wire contract in the tree as it stands today:

- **Device:** the full streaming shipper is in
  `firmware/components/ptt/ptt.c` (tree FW_VERSION is `0.4.49.1`,
  `firmware/main/main.c:35`; .1 touched the PNG path only, audio
  untouched per `docs/0.4.49.1-streaming-proof-runsheet-2026-11-09.md`).
- **Bridge:** `audio_handler.feed()` already treats `total` as
  optional exactly as the design prescribes.
- **Console:** `privacyAudioChunk()` was rewritten for streaming in
  console **0.4.189** (its changelog says so, in so many words); the
  on-disk console source is 0.4.190, which did not touch audio.

What is actually missing is not reader code. It is: (1) **all**
verification — the three implementations of the new contract have
never met on the wire; (2) two small robustness fixes the audit
surfaced; (3) provenance — the entire streaming release is
uncommitted working-tree state (device + bridge) or lives outside
git entirely (console).

**Provenance caveats for every "shipped" claim below.** `git status`
shows `firmware/components/ptt/ptt.c` and
`host-agent/mqtt-bridge/audio_handler.py` as modified-uncommitted
(last commit touching either: `984fc9f`, the 2026-11-03 A0 commit).
The running bridge imports `audio_handler` from this tree
(`host-agent/mqtt-bridge/subscriber.py:31`), so tree state is live
bridge behavior. The console source of truth is **not** in the
project repo tree at all — no `watcher-console.html` exists anywhere
under `~/workspace/projects/watcher-input-assistant/`; the shipped
copy is `~/workspace/your_files/watcher-console/watcher-console.html`
(named as the console location by `docs/RUNBOOK.md:497` and as "the
copy on disk" by `docs/console-manifest-spec-2026-11-09.md:11`).
Line numbers below for the console refer to that file.

---

## (a) Audit findings

### Findings table

| # | Design element | Device (ptt.c) | Bridge (audio_handler.py) | Console (watcher-console.html) |
|---|---|---|---|---|
| 1 | Chunks omit `total` while streaming | ✅ `ship_send_chunk` adds `total` only `if (final)` — ptt.c:1268-1270 | ✅ `total = int(raw_total) if raw_total is not None else None` — :117-118 | ✅ `total=(typeof j.total==="number"&&j.total>0)?j.total:0`, applied only `if(total)` — :2220 |
| 2 | Final chunk carries the true `total`; presence of `total` is the final marker | ✅ `final = ending && (ring_cons_ + n >= end_target)` — :1342; total = ⌈produced/1536⌉ — :1356-1357 | ✅ adopts any carried total — :128-129 | ✅ adopts any carried total — :2220 |
| 3 | `audio_end` unchanged (id, chunks, ms, stop, 0.4.44 VAD fields) | ✅ built at :1379-1395 (thr/amb/spk only when `vad_ext`) | ✅ consumes `chunks` as fallback total — :143-146 | ✅ consumes `chunks` as fallback total — :2221 |
| 4 | PSRAM byte ring, capacity = full max take (240,000 B), no new memory | ✅ `SHIP_RING_CAP = (PTT_MAX_MS × 16000/1000)/2` = 240,000 — :225 (PTT_MAX_MS 30000 :63, rate 16000 :49); allocated once :1573 | n/a | n/a |
| 5 | SPSC ring, no mutex on data path, consumer notified | ✅ volatile monotonic counters :229-230, discipline comment :212-214; producer `xTaskNotifyGive` per append :285-287 | n/a | n/a |
| 6 | Overflow: producer stops appending, counts unsent, **never overwrites** | ✅ truncate-for-rest-of-take in `ring_append` — :261-283; unsent folded into `audio_end.dropped` — :1376-1378 | n/a (any chunk count decodes) | n/a |
| 7 | Commit point at PTT_MIN_MS (2,400 B); sub-300 ms tap = zero wire traffic | ✅ `SHIP_MIN_BYTES` = 2,400 — :226; sender commit gate :1330-1340; discard decided in `rec_stop` (`ship = !(dur < PTT_MIN_MS \|\| produced == 0)`) — :1216; discarded take drained silently, no chunks/audio_end/LEDs — :1417-1421 | n/a | n/a |
| 8 | Sender task created **once** in `ptt_init`, 8 KB PSRAM stack, never per take | ✅ `task_psram_create(ship_task, "ptt_ship", 8192, …)` — :1583-1587 | n/a | n/a |
| 9 | Control queue: BEGIN at take start, END with final-stats snapshot | ✅ `ship_msg_t` (kind/id/ship/dur/produced/unsent/peak/stop/VAD) :236-252; queue :1574; BEGIN sent before first `rec_feed` :1115-1119; END snapshot :1213-1228 | n/a | n/a |
| 10 | `take_done` gating: next take waits for sender completion | ✅ with a deviation — bounded 3 s wait, then the take is **refused loudly** — :1061-1066; semaphore created + given at init :1575-1578, given after completion sequence :1430 | n/a | n/a |
| 11 | Send discipline: `link_wait_for_ring_space` + ≤3 attempts + drop counting; identical b64 routine (byte-identical payloads) | ✅ :1237-1277 (padding dance comment :1232-1236) | n/a | n/a |
| 12 | Completion sequence in the old order: audio_end → LED working off → success pulse → AI-thinking → `i2s_release()` → `wakeword_start()` → give `take_done` | ✅ :1379-1430, in that order | n/a | n/a |
| 13 | Bridge: slot total updates only when a chunk carries one; opportunistic completion only on a **known, reached** total; audio_end + 45 s sweep remain terminators | n/a | ✅ completion guard `slot["total"] is not None and len(chunks) >= total` — :134-136; audio_end adoption :138-149; inactivity sweep :152-172 (called from subscriber.py:307) | n/a |
| 14 | Console: slot no longer resets on absent/changing total; parts accumulate; completion on known total, else on audio_end | n/a | n/a | ✅ :2220-2222 — sparse `parts{}` map + `got` counter instead of a growable array (equivalent, and dedupes by seq); `privacyAudioEnd` adopts `j.chunks` when total unknown :2221 |
| 15 | Cadence: 1536 B chunks ≈ 0.192 s → ~5 msg/s trickle during the take | ✅ by construction: full chunks ship as they accumulate once committed — :1338-1367; chunk size :93 | observes only | observes only |
| 16 | Verification plan items 1–5 | ❌ **none run.** Item 1 (byte-identity) "not recorded as done" (runsheet); items 2–5 written up as a gated bench sheet (`docs/0.4.49.1-streaming-proof-runsheet-2026-11-09.md`) but never executed — no streamed take has ever flowed end-to-end | ❌ same | ❌ same |

### How each reader behaves on a streamed take TODAY (concrete trace)

**Bridge** (`audio_handler.py` `feed()`, reached via
`handle_device_message` from subscriber.py:192 on `watcher/audio`,
or :174 for telemetry-embedded lines):

1. Chunks seq 0…N−2 (no `total`): slot created with `total=None`
   (:122-125), chunks stored by seq, **no completion** (:134 guard).
2. Final chunk (carries `total=N`): slot total set (:128-129),
   `len(chunks) >= total` → `_complete()` fires immediately, slot
   popped, WAV written + transcribed (:134-136, :174-203).
3. `audio_end` then arrives, finds no slot, returns `None` (:139-150)
   — no double completion.
4. Final chunk lost, `audio_end` arrives: slot total still `None` →
   adopts `audio_end.chunks` (:143-146) → `_complete` logs the missing
   seq list and decodes the partial take (:185-188).
5. `audio_end` also lost: the 45 s inactivity sweep completes the
   slot with the denominator reported as what actually arrived
   (:152-172, :182-184). A live stream is never swept mid-take —
   expiry keys on last activity, and a 30 s-capped take trickling at
   ~5 msg/s refreshes it constantly.
6. Duplicate chunks are idempotent (seq-keyed dict, :126). With
   console ≥0.4.186 audio rides only `watcher/audio`, so the
   telemetry-embedded path (:174) is vestigial for audio; no current
   configuration double-feeds a take.

**Console** (`privacyAudioChunk`/`privacyAudioEnd`/
`privacyAudioComplete`, watcher-console.html:2220-2222):

1. Total-less chunks: slot persists (no reset), parts accumulate in
   the sparse map, `got` counts unique seqs only.
2. Final chunk: `slot.total` set; `got >= total` →
   `privacyAudioComplete` — parts concatenated in seq order, ADPCM
   decoded in-page, WAV offered in the Privacy tab, log line shows
   an honest `got/total`.
3. Final chunk lost: `privacyAudioEnd` adopts `j.chunks` and
   completes; the log line shows e.g. `156/157 chunks`.
4. Final chunk **and** `audio_end` both lost: the slot strands in
   `state.audioChunks` — the console reassembler has **no sweep**.
   The take never appears in the Privacy tab. The bridge path is
   unaffected (its own sweep delivers the WAV + transcript), so the
   loss is cosmetic and page-local, but it is silent. See risk R3.

### Design-vs-code disagreements and deviations

Flagged per instructions — none are wire-breaking:

- **D-1 (wording, console):** the design says console parts accumulate
  in a "growable array"; the code uses a sparse object map + `got`
  counter (:2220). Functionally equivalent and strictly better
  (seq dedupe). No action.
- **D-2 (device, gating bound):** the design says "`rec_begin` waits
  on `take_done`" without a bound; the code waits ≤3 s and then
  **refuses the take** with a log line (`previous take still
  shipping — cannot record`, :1061-1066). Arguably safer than the
  design as written; flagged so the bench proof doesn't mistake a
  refusal under a wedged link for a dead button (see R5).
- **D-3 (device, `dropped` units):** the design says overflow ships
  "with `dropped` in audio_end" as the honesty contract. The code
  folds unsent bytes into `dropped` **converted to chunk units**
  (⌈unsent/1536⌉) and adds send-failure drops (also chunk units) —
  :1376-1378. One number, chunk-denominated, two causes. No reader
  consumes `dropped` today (bridge `feed` ignores it; console
  `privacyAudioEnd` ignores it), so this is telemetry-only — but
  nobody should build alerting on it without this footnote.
- **D-4 (comment accuracy, device):** ptt.c:223-224 claims "Bridge
  and console were updated in the same release to match." Verified
  **true** (bridge in-tree; console 0.4.189 changelog,
  watcher-console.html:414) — recorded here because the claim was
  previously unverified, and because both halves are uncommitted /
  outside git (see R6), so the comment is currently the *only*
  place the pairing is written down in the firmware tree.
- **D-5 (version drift, benign):** the design targets "fw 0.4.49";
  the tree is 0.4.49.1. The .1 delta is the `dbg png` PSRAM fix +
  temporary timer probes; the audio path is byte-identical to the
  0.4.49 streaming build (per the proof runsheet's state section).
  Console moved 0.4.189 → 0.4.190 with a UI-only changelog (:413).
  Neither drift touches the contract.

---

## (b) Build spec for what is missing (dependency order)

The remaining work is verification infrastructure, two one-line-class
fixes, and release hygiene — in this order, so bench time (the scarce
resource, and the only part needing John) is spent exclusively on
device behavior.

**B1. Bridge reader regression harness (no device, no John).**
A test file beside the handler (the bridge already keeps
`test_subscriber_seal.py` as the local pattern) driving
`AudioReassembler.feed()` through seven synthetic traces:
(i) legacy stream, constant total on every chunk (regression —
behavior must be byte-identical to pre-streaming);
(ii) streaming stream, total only on the final chunk;
(iii) final chunk lost, `audio_end` adoption;
(iv) `audio_end` lost, sweep completion;
(v) `total: 0` chunk (documents risk R2; expected to fail-safe only
after B3);
(vi) duplicate-chunk idempotence;
(vii) mid-stream hole → missing-seq list in the completion log.
Pass bar: exactly one completion per take in every trace, WAV bytes
identical across (i)/(ii) for the same payload set.

**B2. Console reassembler harness (no device, no John).**
The same seven traces against the privacy reassembler. Two routes:
extract `privacyAudioChunk`/`privacyAudioEnd`/`privacyAudioComplete`
(+ `b64ToBytes`, `adpcmDecode`, `wavBlob` dependencies) into a small
Node/jsdom shim — recommended, it's re-runnable — or drive the real
page via the existing `tools/console-capture` Playwright harness and
inject device lines. Do the shim first; the page-level check happens
for free during B6 (the Privacy tab either shows the take or not).

**B3. Bridge `total: 0` guard (depends on B1 trace (v)).**
`audio_handler.py:117-118` accepts `total=0` as a *known* total;
:134-136 then completes the take on chunk 0 (`len >= 0` always) and
pops the slot; the rest of the take rebuilds a fresh slot and
`audio_end` completes a second fragment — one take, two WAVs, split
transcripts. No real sender emits 0 (device final total is
⌈produced/1536⌉ ≥ 1 for any shipped take; legacy firmware sent true
counts), so this is a latent trap, not a live bug — but the console
already guards (`j.total > 0`, watcher-console.html:2220) and the
bridge should match: treat `total <= 0` as absent. One line plus the
B1(v) assertion flipping to pass.

**B4. Byte-identity corpus check — design verification item 1
(depends on B1; no device, no John).**
Replay archived pre-streaming takes (the `audio-captures/` corpus /
telemetry-extracted 0.4.48-era chunk sets) through `feed()` twice —
once in legacy shape, once re-shaped to streaming shape (totals
stripped except the final chunk) — and require byte-identical WAV
output. This is the host-side half of the design's item 1; the
device-side half (the sender's b64 routine is unchanged, ptt.c:1237)
is a code-read, already done in this audit.

**B5. Console stale-slot sweep (optional; owner decision D1).**
Give `state.audioChunks` the bridge's courtesy: a periodic sweep
completing slots idle >45 s (mirrors the bridge's proven constant),
so a take that loses both its final chunk and its `audio_end` still
surfaces in the Privacy tab as a partial. Small, page-local, touches
no wire behavior. Per console discipline this rides a batched,
approved console version (see versioning below), never a drive-by
edit.

**B6. Bench proof (needs John — see (c)).**
Execute `docs/0.4.49.1-streaming-proof-runsheet-2026-11-09.md` as
written (preflight order, per-step go, wedge watch item). Depends on
B1–B4 passing, so that any failure at the bench is attributable to
the device half alone.

**B7. Release pin / close-out (depends on D3).**
Commit the firmware + bridge tree state; record the console file's
sha256 alongside the commit (or bring the console under version
control, per D3). The runsheet already makes the 0.4.49.1 source
eligible for the clean-copy push once the proof passes; B7 is that
step, extended to name the audio pairing explicitly.

### Versioning proposal (one release, per the `reply` precedent)

The precedent: the `reply` command shipped as fw 0.4.45 + console
0.4.188, each changelog naming the other half.

- **Retroactive naming.** The streaming release already paired
  organically: **fw 0.4.49 + console 0.4.189**, bridge riding along
  unversioned. Console 0.4.189's changelog entry
  (watcher-console.html:414) is the model — it names the firmware
  and the contract change. Record this pairing in the close-out
  commit message (B7) so it exists somewhere besides a firmware
  comment (D-4).
- **The rule going forward.** Any change to the audio wire contract
  (fields, final-marker semantics, chunking) ships as **one
  release**: fw and console bump together in the same step, the
  bridge change lands in the same commit, and both changelogs name
  the pairing. Device-only or reader-only *contract* changes are
  forbidden — today's readers degrade gracefully (partial decode +
  log line), so a one-sided contract change fails *silently*, not
  loudly (see R1). Next available pair if a contract change is ever
  needed: fw 0.4.50 + console 0.4.192 or later (0.4.191 is already
  spoken for by the queued console gate fix).
- **Reader-only fixes are exempt — explicitly.** B3 and B5 change no
  wire behavior for any real sender; they may ship independently
  (B3 with B1, B5 in the next approved console batch) and should be
  labeled "no contract change" in their changelogs so the exemption
  stays auditable.
- **Bridge versioning gap.** The bridge has no version surface at
  all; "which reader is running" is currently answerable only via
  git. Recommendation (decision D2): log a contract/version line at
  subscriber start (e.g. in the existing startup banner), so bench
  telemetry forever records which reassembler produced a transcript.

---

## (c) Verification plan (design items 1–5, updated by the audit)

| Design item | Status after audit | Updated form | Needs John? |
|---|---|---|---|
| 1. Byte-identity of streamed payloads vs store-then-send | Not done; device-side half discharged by code-read (b64 routine unchanged, ptt.c:1237-1277) | B4 corpus replay through both readers' logic (B1/B2 harnesses) | **No** — offline, any time |
| 2. Button take + wake-word take; N/N chunks; correct transcript; `dropped` absent | Not done | Runsheet step 1, unchanged. Add one streaming-specific pass check: chunk arrivals in the subscriber log are *spread across the take's duration* (~5 msg/s), not a terminal burst — that timing signature, not the transcript, is what proves the shipper streams | **Yes** — his **voice** for the wake-word take, his hand for the button take |
| 3. <300 ms tap → no audio traffic | Not done | Runsheet step 2, unchanged (telemetry silence is the pass bar) | **Yes** — his hand at the device |
| 4. Back-to-back takes; re-arm on same boot; no interleave | Not done | Runsheet step 3, unchanged. Audit addition: also confirm no `previous take still shipping` refusal (ptt.c:1063) appears at normal cadence — that log line is the take_done gate's tell | **Yes** — his **voice** (two wake-word takes) |
| 5. Long take (30 s cap) streams throughout; audio_end stats sane | Not done | Runsheet step 4, unchanged. Explicitly **not** extended to a forced-overflow take: manufacturing a wedged link to fill the ring is out of scope for this proof, doubly so while the link-hardening work is open | **Yes** — his presence (voice or held button to the cap) |
| 6. **New (audit):** synthetic reader traces | Did not exist in the design plan | B1 (bridge, 7 traces) + B2 (console shim, same 7) | **No** |
| 7. **New (audit):** `total: 0` fail-safe | Did not exist | B1 trace (v) before/after B3 | **No** |

Sequencing: items 1, 6, 7 are free and go first (B1–B4). Items 2–5
are exactly the existing runsheet and should run in one bench
session, in its order, under its preflight (bridge health → lock →
seal resync → heartbeat fw check) and its wedge watch item.

---

## (d) Risks and open decisions

### Risks

- **R1 — The contract has never been exercised; graceful degradation
  hides mismatches.** Both readers complete partial takes and log a
  line; neither alarms. A one-sided slip (hypothetically: a future
  fw emitting `total` on a non-final chunk) would make the bridge
  complete early, pop the slot, and complete the remainder as a
  second fragment — two plausible-looking WAVs, garbled transcripts,
  no error anywhere. This is why (b) front-loads synthetic traces
  and byte-identity: they convert silent corruption into a failing
  test, offline, before the bench ever sees a streamed take.
- **R2 — Bridge `total: 0` trap.** Detailed at B3. Latent (no sender
  emits 0), one-line fix, console already immune. Until B3 lands,
  any experimental sender must never emit `total: 0` — the design
  doc already rejects that encoding; this is the concrete reason.
- **R3 — Console slot has no expiry.** Final-chunk + `audio_end`
  both lost ⇒ the take silently never appears in the Privacy tab
  (bridge still delivers it). Page-local, cosmetic, silent. Fix
  specced at B5.
- **R4 — Mid-stream holes silently time-compress.** Both readers
  concatenate *present* chunks in seq order; a lost middle chunk
  removes 0.192 s and desyncs ADPCM state for everything after it.
  The bridge names the missing seqs at completion
  (audio_handler.py:185-188); the console shows only `got/total`.
  Pre-existing, design-accepted — but streaming removes the old
  burst-loss obviousness, so the bridge's missing-chunk log line is
  now the only tell. Accept; do not "fix" by zero-filling without a
  separate design pass (zero-fill preserves timing but still desyncs
  ADPCM; neither is clearly righter).
- **R5 — take_done refusal looks like a dead button.** Under a
  wedged link the previous take's drain can exceed the 3 s gate; the
  next press is refused with only a log line (ptt.c:1061-1066) — no
  user-visible cue was found on that path in this audit. During the
  bench proof (and the open link-wedge work), treat a silent press
  as a gating event first, not a hardware fault.
- **R6 — Provenance.** The streaming release exists as: a flashed
  binary, uncommitted tree state (fw + bridge), and a console file
  outside git. A tree accident or VM replacement loses the only
  source copy of fw 0.4.49+'s shipper. B7 closes this; until then
  the audit's line citations are also the map for reconstruction.
- **R7 — Overflow path is itself unproven.** The ring-full branch
  (ptt.c:267-273) and the `dropped` fold (:1376-1378) have never run
  live; plan item 5 deliberately doesn't force them. First live
  encounter will likely be accidental (a long take during a link
  outage) — the readers handle any chunk count, so the blast radius
  is one truncated take, honestly reported.

### Open decisions for the owner (with recommendations)

- **D1 — Ship B3/B5 now, or hold for the next paired release?**
  Recommendation: **B3 now** (with B1; it is bridge-only, touches no
  wire behavior, and removes a trap before any experimental sender
  exists). **B5 rides the next approved console batch** (console
  changes are batched per standing discipline; the queued 0.4.191
  gate-fix batch is the natural vehicle, subject to its own
  approval).
- **D2 — Give the bridge a version surface?** Recommendation:
  **yes** — one startup log line naming the audio contract it
  implements ("audio reader: streaming-total contract, 0.4.49+").
  Cheap, and it makes every future transcript attributable to a
  reader build from telemetry alone.
- **D3 — Bring `watcher-console.html` under version control?**
  Recommendation: **yes** — "one release" is unprovable while one of
  the three components has no history. Minimum viable form: commit
  the file (and its changelog) in the clean-copy pin at B7; fuller
  form (repo home + export step) can follow the console-manifest
  work. Owner's call on the form; the pin itself shouldn't wait.
- **D4 — Does host-side verification (items 1/6/7) gate the bench
  proof, or run in parallel?** Recommendation: **gate it.** The
  bench session needs John's voice and attention; spending it
  discovering a reader bug that a free offline test would have
  caught is the exact waste the sequencing in (b) exists to avoid.
