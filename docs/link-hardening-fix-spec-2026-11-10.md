# Link-hardening FIX spec — S3 fw 0.4.51 + Himax fw 0.4.20.63

**Status: SPEC ONLY.** No code in this document is authorized by its
existence. The slate below was approved by the owner 2026-11-10 00:33
PST ("Approved. Be thorough and meticulous."); this document is the
build-ready form of that slate and goes back to the owner for approval
before any build, per the standing rule that builds, staging, and
flashes are separate commissioned steps.

**Sources.** `docs/link-hardening-audit-s3-2026-11-09.md`,
`docs/link-hardening-audit-himax-2026-11-09.md`,
`docs/link-hardening-spec-2026-11-09.md` (the round this fixes),
`docs/link-hardening-soak-2026-11-09.md` (the failed soak that
motivated it). Every file:line anchor below was re-verified against
the current working trees during the writing of this spec (grep-grade
verification, not copied from the audits); §0 records the places where
verification disagreed with the audits or with this spec's own
tasking.

**Versions.** S3 firmware **0.4.51** (from live 0.4.50). Himax
firmware **0.4.20.63** (from live 0.4.20.62). No wire-format change
on either side; the pair remains wire-compatible in any combination
(§16).

**Reading order.** §0 first — it contains one correction that changes
the shape of an approved item (D2), flagged per instructions rather
than silently designed around. §§2-7 are the S3 items, §§8-13 the
Himax items, §14 the explicit exclusion, §15 the worked interaction
audit, §17 the gated acceptance sequence, §18 the numbers that remain
owner decisions.

---

## 0. Verification notes — where the audits / tasking were wrong

### 0.1 D2 as tasked is unimplementable: the Himax→S3 direction has no SSCMA frames to validate

The tasked D2 rule reads: *"the drain declares recovery only when the
captured bytes contain ≥1 CRC-valid SSCMA frame (feature byte +
CRC16-CCITT per the transport format)."* That frame format —
`[0x10][cmd][len][payload][CRC16]` — exists **only in the S3→Himax
direction**: it is constructed by the S3's send path
(`sscma_client_io_spi.c:110` `FEATURE_TRANSPORT 0x10`; header CRC at
:1063; header+payload CRC at :635/:677) and parsed by the Himax's RX
callback (`ma_transport_spi.cpp` `_spi_frame_crc_ok`, used at :715).

In the return direction — the only direction a drain capture ever
contains — the Himax stages **raw segments with no framing and, in
two of three cases, no CRC at all**:

| Staged segment | Himax staging site | Format |
|---|---|---|
| CMD_READ response | MT:851 `spi_write_dma(_spi_tx_buf, len)` | raw bytes, **no CRC** |
| CMD_READ_CRC response | MT:936 `spi_write_dma(_spi_tx_buf, len + 2)` | `[data][CRC16(data)]` — a CRC trailer, but no feature byte, no header |
| CMD_AVAILABLE response | MT:1021 `spi_write_dma(_spi_tx_buf, 2)` | raw 2-byte count, **no CRC** |

A drain whose recovery rule is "find a 0x10-framed, CRC-valid packet
in the capture" would therefore **never declare recovery on any
legitimate capture** — including the one genuine drain recovery on
record (2026-11-09 01:54:11 UTC, a stuck AVAILABLE response) — and
rung 1 would be silently deleted while appearing to be hardened.
§3 specifies the corrected D2: validation appropriate to what the
return direction actually carries (exit-cause + content + count
sanity), with the same fail-safe direction the task intended —
*when in doubt, the capture is not a recovery.*

The same looseness appears in the tasked D1 parenthetical, which
describes *"an available() exec that returns a validated frame."*
The available exec returns a **2-byte count**, not a frame
(io_spi.c:890-906; MT:1021). §2 pins the available completion as
what it is: a dual-gate exec that clocks out the 2-byte count and
returns ESP_OK. Frame-level CRC validation on the receive side
exists in exactly one place — the bulk path's per-segment CRC16
check (io_spi.c:1408-1412) — and §2's reset set accounts for it
there.

### 0.2 D3's minimal move fails its own char budget

The tasked D3 says: move `himax_link` before `stack_hwm`, object to
end within ~250 chars of the JSON start, "verify by construction."
Construction (§4) shows the literal move — himax_link after
`log_laps`, before `stack_hwm` — ends at **≈370 chars**, past both
the ~250 budget and the bridge's observed ~290-char truncation
point. The object must move into the heartbeat's identity head,
immediately after `fw`, where it ends at **≈194 chars**. §4 pins
that placement. The intent of D3 (the object survives truncation)
is unchanged; the anchor point is corrected.

### 0.3 Himax arm-site census: 22 release-reachable sites, 1 verified

The tasking says "~24 arm sites"; the Himax audit's F-2 list
enumerates 21. A grep census of the current tree
(`_spi_arm_rx()` / `_spi_arm_rx_parked()` call sites,
`ma_transport_spi.cpp`) gives **25 call sites in the file: 22
release-reachable** (18 `_spi_arm_rx` + 4 `_spi_arm_rx_parked`) plus
3 inside the compile-time-off test-hooks block (:1127, :1140,
:1142). Exactly **1 of the 22** — Guard 1's arm at MT:1688 — is
followed by the H-C verification (call at MT:1695). The audit's
list of 21 is the 22 minus Guard 1's own arm, which it (reasonably)
did not count among the *unverified*; the figures reconcile. §9's
design makes the census moot by verifying inside the arm functions
themselves.

### 0.4 Line-cite drift (substance verified in every case)

- `send_command.py`: the S3 audit's cites for `allowed()` internals
  run ~11 lines high against the current file. Verified positions:
  `allowed()` at :83, logs branch :96-100 with the bool exclusion at
  **:99**, wakeword branch :102-107, generic dbg return :108-109.
  §6 cites the verified lines.
- `link.c`: the eyes_reset rail parse is at **:2348-2349** (audit:
  :2349-2350 — the statement spans both; immaterial).
- All other anchors in both audits — io layer, ops.c, eyes.c
  supervisor, and the full `ma_transport_spi.cpp` map — verified
  exact or within one line. Where this spec cites a line, it is the
  verified one.

---

## 1. The slate

| # | Item | Side | One-line summary |
|---|---|---|---|
| D1 | Streak reset semantics | S3 0.4.51 | Streak resets only on exec-level completions; the two counterfeit resets (bare `spi_send_cmd_` pass, drain capture) are removed |
| D2 | Drain validation | S3 0.4.51 | Drain declares recovery only on a gate-LOW exit with a non-padding, count-sane segment; everything else is discarded, never stashed (§3, corrected per §0.1) |
| D3 | Heartbeat observability | S3 0.4.51 | `himax_link` moves into the heartbeat identity head (after `fw`), ending ≈194 chars in — inside the truncation budget |
| G1 | Rate dimension | S3 0.4.51 | Windowed failure measure (60 failures / trailing 60 s) opens episodes alongside the streak; failures-since-open drives rung conditions; health dwell replaces instant close |
| G2 | Summary re-key | S3 0.4.51 | WI-1 "gate still failing" summary re-keys to the monotonic failure total so it fires in flap mode (folded into §5) |
| G3 | Rail exactness | S3 0.4.51 + host | Firmware: rail=1 only for the JSON number 1; parsed value logged. Host: dedicated `eyes_reset` validation branch in `send_command.py` |
| G4 | Crediting hygiene | S3 0.4.51 | `last_rung_try_ms` resets at episode open; closes credit a rung only if it fired in-episode, else "external" |
| S-1 | Pad-state watchdog term | Himax .63 | Decimated line-state check in `spi_poll`: pad HIGH sustained 1 s while tracker-armed, unstaged, unparked ⇒ arm-failure flag + distinct DIAG |
| S-2 | H-C at every arm | Himax .63 | Arm verification moves inside `_spi_arm_rx()` / `_spi_arm_rx_parked()`; all 22 sites inherit it; Guard-1 call site removed |
| F-3 | Abstain death note | Himax .63 | First readback-API abstain per boot emits one DIAG line; later abstains counted silently; REARM format untouched |
| F-4 | POLL_GAP max payload | Himax .63 | POLL_GAP emit gains `max=`; `_poll_max_gap_us` resets on emit |
| F-5 | Guard-3 open rate limit | Himax .63 | Block-head open retry while closed limited to one attempt per 100 ms; skipped polls are not recovery attempts |
| F-6 | deInit retires Guard 3 | Himax .63 | `deInit` clears the arm-failure flag, fail count, and armed tracker before its early return |

Explicitly out of scope: **S-2b** (F3-proof staged age) — §14.

---

## 2. D1 — Streak reset semantics (S3, 0.4.51)

**Finding (S3 audit §D1, mechanism proven from the flap log).** The
gate-failure streak that arms the recovery ladder is reset in five
places, two of which do not correspond to a completed transaction:
`spi_send_cmd_` resets it on *any* successful send (io_spi.c:1100) —
and during the flap, send-episodes that ended in available-exec
failures passed through successful sends — and the available exec's
drain branch resets it on any capture (io_spi.c:873). Between them
they pinned the streak at the drain threshold (3) for ~6 hours:
the supervisor's episode threshold (25) was unreachable *by
construction*, which is why 342 MB of 0.4.50 telemetry contains
zero `episode open` lines for the storm.

**Rule.** `gate_note_success_()` (io_spi.c:480-489) may be called
**only** at an exec-level completion: a point where a whole gated
transaction — command out under a LOW gate, response clocked back,
exec returning ESP_OK — has finished. Increments are unchanged
(exactly three: io_spi.c:606 write-exec gate wait, :884
available-exec gate wait, :1052 `spi_send_cmd_` LOW-gate wait).

**The reset set, pinned per exec:**

| Exec (io_spi.c) | Today | 0.4.51 | Why |
|---|---|---|---|
| `spi_txn_write_exec` (:595) | epilogue reset on ESP_OK at :716 | **KEEP :716** | A completed write exec clocked a full packet out under gate discipline and returned ESP_OK; the link demonstrably carried a whole transaction. |
| `spi_txn_read_exec` (:723) | stash-serve reset at :769; no reset on the wire-completion path | **REMOVE :769; reset at the exec's ESP_OK exit(s)** — the `err:` epilogue (:814-816) for the wire path and the stash-serve return (:751-775) alike: every ESP_OK exit resets exactly once, no other exit does | Stash-serve bytes were put there by a previously completed drain of real staged content; serving them completes a read the poller asked for. A wire-completed read is a full round trip. A read that times out at gate-2 (:782-787) is *not* a completion and must not reset. |
| `spi_txn_available_exec` (:816) | drain-branch reset at :873; no reset on the wire path | **REMOVE :873; ADD reset on the wire path immediately after the successful 2-byte clock-out (:898)**, before the `*len` decode (:902-906), on a path the drain branch's return does not traverse | The wire path is the link's heartbeat: a dual-gate pass that clocks out the staged count is a completed transaction (per §0.1, the count is not a "frame"; the completion is the gate discipline plus the clocked response). The drain branch is a *recovery action*, not a completion — see below. |
| `spi_txn_bulk_exec` (:1629) | no exec-level reset | **ADD reset at the ESP_OK success point (:1776)** | A completed bulk segment is the strongest health evidence the link produces: kilobytes clocked under gate discipline with the per-segment CRC16 check (io_spi.c:1408-1412) passing. |
| `spi_txn_resync_exec` (:581) | no reset | **NO reset (unchanged)** | A resync proves nothing about link health — it is rung 2's *tool*. If it reset the streak, the ladder would defuse itself mid-episode. The next poll's completion does the resetting. |
| `spi_send_cmd_` (:1027), internal | reset at :1100 on any successful send | **REMOVE :1100** | A bare gate pass plus a clocked header is not a transaction. `spi_send_cmd_` is called *by* the execs above; completion belongs to them. This is the counterfeit reset that fired mid-flap. |
| Drain branch, in available exec (:854-876) | reset at :873 on any capture | **REMOVE :873** | A drain capture is rung 1 acting, not the link healing. Per §3, a *declared* drain recovery increments `rec_drain` and returns real bytes — and the streak still does not reset here. The next ordinary available completion (which the cleared gate now permits) performs the reset, one poll later. Cost: the streak lingers ≤1 poll period past a true drain recovery; no threshold in the ladder (3/25/50) is materially affected. |
| `sscma_client_gate_reset_streak()` (:503-506, header decl in `sscma_client_io.h`) | exported, **zero callers** (dead) | **REMOVE** the wrapper and its header declaration | A sixth, unaudited reset path must not exist for a future caller to reach for. D1's invariant is enforceable only if the reset sites are enumerable — after this change they are exactly the four exec sites above. |

**Preserved asymmetry (stated, not accidental).** The read exec's
gate-2 timeout (:782-787) neither increments nor resets, as today:
when a read's command never got out, the failure is accounted once
at the send side (:1052); when the command got out and the response
gate stuck, the *next* transaction's gate wait accounts it. D1 does
not change increment accounting anywhere.

**Idle-health argument.** In healthy idle the SSCMA process task
(ops.c:207ff) polls `available()` continuously at its backoff
cadence (ops.c:218, {1, 10, 50, 250} ms; any ESP_OK returns it to
1 ms, :254-258). Every such poll that passes both gates completes
at io_spi.c:898 and resets the streak. The observed healthy
transients are small and always followed by completions: the .62
soak's boot transient (2 episodes × 2 failures) and capture-start
transients (2 failures). Under the new reset set the streak
therefore sits at 0 in healthy idle and can accumulate *only* when
completions stop — which is the definition of the fault the ladder
exists for. In the flap's exact shape, available completions *do*
occur (~1 per ~300 ms cycle) and *do* reset the streak under D1;
D1 does not pretend to catch the flap. That is G1's job (§5), and
§15 walks both mechanisms against the recorded storm.

**Invariant (new, enforceable by grep).** After 0.4.51,
`gate_note_success_(` has exactly four call sites — write-exec
epilogue, read-exec ESP_OK exit, available-exec wire path,
bulk-exec success — and no others. The build review checks this by
grep, the same way the .62 review checked H-C's call sites.

---

## 3. D2 — Drain validation (S3, 0.4.51) — *corrected form, see §0.1*

**Finding (S3 audit §D2).** Rung 1's drain (`spi_drain_capture_`,
io_spi.c:522-560, invoked from the available exec's drain branch at
:868 when the streak reaches 3) declares success on *any* captured
bytes: on ESP_OK it increments `gate_drain_recoveries_` (:870),
hands the bytes to the caller as the available result (:871), and
(today) resets the streak (:873, removed by D1). Against a dead or
held-in-reset Himax, the drain clocks an idle line; whatever the
idle bytes are, the current code calls it a recovery — the ladder
defused by its own first rung. This is the failure the original
spec's dead-Himax test was written to catch and the test was never
run (it is acceptance step 2, §17).

**What the drain may declare recovery on.** All three of:

1. **Exit cause = gate LOW.** `spi_drain_capture_` loops clocking
   64-byte chunks while MISO reads HIGH (io_spi.c:533), bounded by
   its 200 ms budget and `DRAIN_STASH_CAP` 4096 (:520). In 0.4.51
   it must also report *how* it exited — gate fell LOW
   (the staged FIFO emptied and the Himax re-armed), budget
   timeout, stash full, or transmit error. **Only a gate-LOW exit
   can declare recovery.** A timeout exit means the line is still
   stuck when the drain gave up: the opposite of recovery.
2. **Content is a real segment, not padding.** Strip the trailing
   idle run — trailing bytes of a single repeated value, 0xFF or
   0x00 (the two idle patterns an un-driven or released MISO line
   produces; the 64-byte chunk over-read guarantees up to 63 such
   bytes trail any real segment). If nothing remains, the capture
   was padding-only: **not a recovery**, regardless of exit cause.
3. **Count sanity.** The drain branch serves the available exec;
   its result is consumed as a byte count. Decode the count from
   the segment's first 2 bytes exactly as the normal available
   path does (io_spi.c:902-906) and require: segment length ≥ 2
   and count ≤ `DRAIN_STASH_CAP` (4096). A segment whose prefix
   decodes past the stash cannot be a real AVAILABLE response.

**Outcomes.**

- **All three pass → declared recovery.** `rec_drain` increments
  (io_spi.c:870, now gated), the *stripped segment* — never the
  trailing idle run — is returned as the available result and is
  what may enter `drain_stash`. The streak does **not** reset
  (§2). This preserves the one proven drain recovery on record
  (01:54:11 UTC: gate-LOW exit, a real staged count, stream
  proceeded).
- **Any test fails → the capture is discarded wholesale.** No
  bytes are returned, no bytes enter `drain_stash`, `rec_drain`
  does not increment, and the available exec exits with the same
  gate failure it would have returned had the drain never run.
  The ladder sees an honest, continuing failure and escalates.
  Discards are counted in a new io-layer counter beside
  `gate_drain_recoveries_` (io_spi.c:462) with a matching accessor,
  and surfaced — rate-safely — in the §5 summary line (drains fire
  at flap cadence; a per-discard log line would be a new flood
  source, so there is none).

**Partially-valid captures.** The only partial form the return
direction produces is *real segment + trailing idle over-read*;
the rule above handles it by stripping to the segment boundary.
There is no mid-segment validation available for raw READ segments
(they carry no CRC, §0.1), and none is invented: a capture whose
non-padding prefix is a stuck *data* segment (e.g. an unread READ
response sitting in the FIFO while the S3 polls available) will
usually fail count sanity — a PNG magic prefix decodes to a count
> 4096 — and be discarded. That is the intended direction of
error. **D2 prefers a missed recovery declaration (the ladder
escalates; cost: minutes) over a false one (the ladder defused;
cost: the entire incident class).** Where a staged segment *is* a
READ_CRC segment, its trailing CRC16 would validate by
construction of the staging (MT:936), but the drain cannot
distinguish segment types from raw bytes, so D2 does not key on
it; the bulk path's own CRC check (io_spi.c:1408-1412) remains the
receive-side frame validator where the format provides one.

**Interaction with rung ordering.** None of this changes when the
drain fires (streak ≥ 3, inside the available exec, as today) or
its 200 ms budget. The change is purely in what its result *means*.

---

## 4. D3 — Heartbeat observability (S3, 0.4.51)

**Finding (S3 audit §D3).** The heartbeat's nested `himax_link`
object (link.c:955-976) is emitted *after* `stack_hwm`
(link.c:926-954) in `link_heartbeat_task` (link.c:885). In
bridge-logged form, heartbeat lines truncate at ~290 chars — the
cut lands inside `stack_hwm` — so the object the previous round
built to make the ladder observable has never once been observed:
zero `himax_link` sightings in 342 MB of 0.4.50 telemetry.

**Change.** Move the `himax_link` block to immediately after the
`fw` field (link.c:896), before `baud` (:897) — inside the
heartbeat's identity head. No field changes; the object is byte-
identical, only its position moves.

**Char budget, verified by construction.** cJSON emits compact
JSON (no spaces). Worst-case widths (state = "recovering",
streak ≤ 4 digits, gen ≤ 3, rec_drain ≤ 5, rec_reset/rec_rst/
rec_rail ≤ 3, last_rung = "external"):

| Prefix through `"fw":"0.4.51",` | 48 chars |
|---|---|
| `"himax_link":{` | 14 |
| `"state":"recovering",` | 21 |
| `"streak":1234,` | 14 |
| `"gen":123,` | 10 |
| `"rec_drain":12345,` | 18 |
| `"rec_reset":123,` | 16 |
| `"rec_rst":123,` | 14 |
| `"rec_rail":123,` | 15 |
| `"last_rung":"external"` + `}` + `,` | 24 |
| **Object ends at** | **≈194 chars** |

194 ≤ 250 budget, with ~56 chars of headroom against the budget
and ~96 against the observed ~290-char truncation. All counters
are since-boot; even implausible 10-digit values add < 30 chars,
so the placement is safe for the device's lifetime. The literal
"before stack_hwm" placement (after `log_laps`) computes to a
prefix of ≈227 chars and an end at ≈370 — over the truncation
point, which is why the head placement is specified (§0.2).

**Post-flash verification** (acceptance step 7): grep a
bridge-logged heartbeat line from live telemetry and show the
complete `himax_link` object, `"last_rung"` included, inside the
logged line. If it is not there, D3 has not landed, whatever the
version string says.

---

## 5. G1 — Rate dimension (S3, 0.4.51) — with G2 folded in

**Finding (S3 audit §G1/G2).** The ladder's only trigger is a
*consecutive*-failure streak. The flap's morphology — fail in
bursts of exactly 3, complete one transaction, repeat at ~300 ms
for ~6 hours — is invisible to it, and (after D1) remains so by
design: the completions are legitimate. The missing dimension is
*duty cycle*: how many failures per unit time, regardless of
consecutiveness. The WI-1 summary line has the same blindness
keyed the same way (ops.c:246-252 requires streak ≥ 100), plus a
second, quieter defeat: its bucket resets on every success
(ops.c:257), and flap mode delivers a success every cycle.

### 5.1 Mechanism: a slotted trailing window in the io layer

Beside the gate statics (io_spi.c:460-463), maintained inside
`gate_note_failure_` (io_spi.c:467-478) — the single increment
funnel all three failure sites already pass through:

- `gate_failures_total_` — uint32, monotonic since boot, +1 per
  noted failure.
- A ring of **6 slots × 10 s**: each slot holds a uint16 count
  and the epoch (slot start time) it belongs to; a failure
  increments the current slot (zeroing it first if its epoch is
  stale). The window value is the sum of slots whose epoch falls
  in the trailing 60 s.

New accessors beside the existing ones (io_spi.c:491-511,
declared beside `sscma_client_io.h:201-208`):
`sscma_client_gate_failures_window()` and
`sscma_client_gate_failures_total()`.

*Why a slot ring and not a token bucket:* the ring's value is
exact and bench-assertable ("the window crosses 60 at second ~8"
is a checkable prediction, §15); a bucket's level depends on a
leak-rate constant with no measured basis. State cost is 6 counts
+ 6 epochs. Slot quantization makes a failure count for 50-60 s
(it leaves when its slot rolls out) — an over-count of age by
≤ 10 s, the conservative direction for a failure measure.

### 5.2 Threshold: 60 failures in the trailing 60 s

Measured flap rate: ~150-180 gate episodes/min × 3 failures =
**450-540 failures/min (7.5-9/s)**, sustained for hours (soak §5;
S3 audit §G1). Worst healthy burst on record: the .62 boot
transient, **4 failures in a minute** (2 episodes × 2). Capture-
start transients: 2.

`GATE_RATE_THRESHOLD = 60` sits **≥ 7× below** the slowest
measured flap rate (the flap crosses it in 6.7-8.0 s) and **15×
above** the worst healthy burst ever observed. A hypothetical
healthy burst 10× worse than anything recorded (40/min) still
does not open an episode. Both margins are measured, not tuned.

### 5.3 Episode machine: two triggers, one episode

Supervisor: `eyes_link_health_task_` (eyes.c:804), 100 ms sample
(eyes.c:824). The open guard (eyes.c:873-876) becomes a
disjunction:

```
open when  streak >= LINK_STREAK_RESET (25)
        || window >= GATE_RATE_THRESHOLD (60)
```

Both triggers flow through the *same* open block (eyes.c:878-895),
which gains in its init: `ep_open_ms = now_ms`,
`ep_fail_base = sscma_client_gate_failures_total()`,
`healthy_since_ms = 0`, plus the §7 additions. One `episode` bool,
one init block, one close site — **double-open and double-credit
are impossible by construction**, not by interlock. `link_up_`
drops in the existing open path (eyes.c:890-893) for *either*
trigger. The open log line records the trigger:
`himax link episode open: trigger=streak|rate streak=%u
window60=%u total=%u`.

Define **FSEO** (failures since episode open) =
`gate_failures_total() − ep_fail_base`, evaluated each pass.
Rung conditions re-base onto *(streak OR FSEO)*, with the floor
read on episode age:

- **Rung 2** (eyes.c:900-901): `!ep_reset_tried &&
  (streak >= 25 || FSEO >= 25) &&
  (now_ms − ep_open_ms) >= LINK_FAIL_FLOOR_MS (1000)`.
- **Rungs 3/4 gate** (eyes.c:919-922):
  `(streak < 50 && FSEO < 50) ||
  (now_ms − ep_open_ms) < LINK_FAIL_FLOOR_MS` → skip.

In a pure consecutive wedge FSEO ≈ streak (every failure since
open is consecutive), so the .50 ladder behaves as before — with
one stated delta: the floor now runs from episode open rather
than streak start, so rung 2 can fire up to ~1 s later than in
0.4.50's wedge shape (the supervisor opens the episode on the
sample where the streak crosses 25, by which time the streak is
already ≥ 1.25 s old: 25 × the 50 ms gate wait). The floor's
purpose — give the Himax's own guards (≤ 300 ms, Guard 2)
room before the S3 escalates — keeps 3× headroom either way.

### 5.4 Close: health dwell replaces the instant streak==0 close

Today the episode closes on the first sample that reads
streak == 0 (eyes.c:842-871) — in flap mode that sample arrives
every cycle. New close rule: the episode closes when **streak
has read 0 continuously for `LINK_HEALTH_DWELL_MS = 2000` ms**
of supervisor samples (`healthy_since_ms` set on the first 0
sample, cleared by any failure sample). Margin: the flap's
period is ~300 ms, so a flapping link produces a failure sample
at least every ~3 samples — the dwell demands ~20 consecutive
healthy samples, a ~7× margin over the flap's longest healthy
gap. Booking at close is per §7. `link_up_` restores at close
(existing path, eyes.c:861-869).

### 5.5 The rung path must read the same health predicate

`link_wait_bus_free_` (eyes.c:787-802) aborts a rung-3/4 attempt
if it samples streak == 0 (:792-794) and returns
`sscma_client_gate_streak() > 0` (:801); the post-wait re-check
(eyes.c:949-951) does the same. Under a rate-opened episode the
streak legitimately reads 0 mid-cycle, so these reads would make
rungs 3/4 self-abort — G1 would open episodes the ladder cannot
act in (the S3 audit's G1 corollary). All three reads are
replaced by the **episode-health predicate**: abort/skip only
when no episode is active, or the §5.4 dwell has completed. The
supervisor maintains the needed state in file-scope mirrors
(`link_episode_active_`, set at open, cleared at close; the
healthy-since timestamp mirrored each pass) since the episode
locals are task-private. Behavior in the streak shape is
unchanged: there, streak == 0 sustained *is* the close.

**Rider — heartbeat state truthfulness.** `eyes_himax_link_info`
(eyes.c:1008-1009) derives `state` from the raw streak, which in
flap mode flickers "up" mid-cycle. With §5.5's mirror available,
state becomes: `"recovering"` if a rung is in progress, else
`"down"` while an episode is active, else the existing
streak-based value. One expression; it makes the object §4 moves
into view truthful in exactly the mode that motivated moving it.

### 5.6 G2 — the WI-1 summary re-keys to the monotonic total

ops.c:246-258 becomes: on an available() failure, read streak,
window, and total; when `(streak > 0 || window > 0) &&
total / 100 > poll_summary_bucket_`, set the bucket to
`total / 100` and log one line carrying all of it, plus the §3
drain counters:

```
himax gate still failing, streak=%u window60=%u total=%u drains=%u discarded=%u
```

The success path (ops.c:254-258) **no longer resets the bucket**
(the total is monotonic; the bucket self-limits — resetting it
was the second defeat). Cadence: ~5 lines/min in flap mode
(450-540 failures/min ÷ 100), ~2-3/min in a wedge — versus the
~400/min the per-attempt edge logs produced during the storm
(device `log_dropped` reached 807 by 02:03 UTC). The summary can
now fire in every failure shape the ladder itself can see.

---

## 6. G3 — Rail exactness (S3 firmware + host tooling)

**Finding (S3 audit §G3).** `eyes_reset`'s rail flag parses
loosely on both ends of the wire. Firmware: `opt_int(cmd, "rail",
…)` (link.c:2348-2349) accepts any JSON number — `"rail":2`,
`"rail":-1`, `"rail":1.5` all cycle the camera power rail. Host:
`send_command.py`'s generic dbg branch (send_command.py:108-109)
validates nothing about `rail` at all; the one live use of the
command (2026-11-09 23:52 PST, the storm-clearing reset) happened
to send the right value, which is luck, not a control.

### 6.1 Firmware (link.c)

In the `eyes_reset` handler (link.c:2315-2358), replace the
`opt_int … != 0` parse at :2348-2349 with an exact test:

- `rail` is **1 only if** the `rail` item is a cJSON number whose
  value is exactly 1 (`cJSON_IsNumber(o) && o->valuedouble == 1.0`
  — covers `1` and `1.0`; excludes `2`, `-1`, `0.5`, `1.5`,
  strings, and booleans, which cJSON does not class as numbers,
  cf. the `opt_bool` note at link.c:1119-1120).
- Absent key: the existing origin defaults stand — 0 for remote
  (MQTT) callers, 1 for serial callers.
- Anything else present: 0 (warm RST only). The failure direction
  is toward *less* hardware disruption, always.

`opt_int` itself (link.c:1110-1117) is **not** changed — other
callers rely on its loose parse; G3 is scoped to this handler.

The handler logs the parsed result before acting, so telemetry
shows what was actually requested:
`eyes_reset: rail=%d origin=mqtt|serial key=absent|number|other`.
A `"rail":true` or `"rail":2` attempt is thereafter visible as
the warm reset it safely became, not as a silent rail cycle.

*Rider (audit I3):* the handler's stale comment sentence
("Serial console only…", link.c:2319-2321) contradicts the 0.4.50
policy block directly beneath it and is corrected in the same
edit. Leaving a false statement inside code this spec touches
would be worse than the one-line scope stretch.

### 6.2 Host (send_command.py)

`allowed()` (send_command.py:83) gains a dedicated `eyes_reset`
branch immediately before the generic dbg return (:108-109),
mirroring the wakeword branch's shape (:102-107):

```python
if cmd.get("cmd") == "eyes_reset":
    rail = cmd.get("rail", 0)
    return (set(cmd.keys()) <= {"t", "cmd", "rail"}
            and isinstance(rail, int)
            and not isinstance(rail, bool)
            and rail in (0, 1))
```

The bool exclusion is load-bearing, not pedantic: in Python
`True in (0, 1)` is `True` and `isinstance(True, int)` is `True`,
so without it `"rail":true` would pass as 1. The precedent is
the logs branch (send_command.py:99:
`isinstance(n, int) and not isinstance(n, bool)`). Note the
wakeword branch (:102-107) lacks the exclusion for its `on`
field — out of this item's scope, recorded here so it is a
known gap rather than an unnoticed one.

Device and host now enforce the same predicate independently;
neither trusts the other to have checked.

---

## 7. G4 — Crediting hygiene (S3, 0.4.51)

**Finding (S3 audit §G4).** Two leaks in the supervisor's
bookkeeping (eyes.c:804ff):

1. `last_rung_try_ms` is never reset at episode open — the init
   block (eyes.c:878-884) resets `rung_fail_streak` but not the
   timestamp the backoff gate reads (eyes.c:939-944). A fresh
   episode can inherit up to 60 s of the previous episode's
   backoff wait before its first rung ≥ 3.
2. Close booking (eyes.c:842-871) credits `ep_credit` — the last
   rung *this code* executed — with no check that the recovery
   wasn't actually performed by someone else. A manual
   `eyes_reset` landing mid-episode bumps `link_gen_`
   (eyes.c:735) and heals the link; the close then credits
   whatever rung the ladder last ran, or "none", and the record
   cannot distinguish a ladder recovery from an external one.

**Changes.**

- **At episode open** (init block, eyes.c:878-884): add
  `last_rung_try_ms = 0;` and snapshot `ep_open_gen = link_gen_;`
  and `ep_credit_gen = 0;`.
- **At each rung success**: rung 2 (after the resync returns,
  eyes.c:908ff), rung 3 and rung 4 (after `eyes_recover_camera`
  returns) — when the rung returns ESP_OK and sets `ep_credit`,
  also snapshot `ep_credit_gen = link_gen_;` *after* the call, so
  a rung's own generation bump (eyes_recover_camera bumps it at
  eyes.c:735, before returning) is inside the snapshot.
- **At close** (eyes.c:842-871), book exactly one of:
  - `ep_credit` is "reset"/"rst"/"rail" **and**
    `link_gen_ == ep_credit_gen` → book as today: the matching
    `rec_reset_` / `rec_rst_` / `rec_rail_` increments,
    `last_rung_` = the rung, `last_close_rung` = the rung.
  - else, if `link_gen_ != ep_open_gen` (the generation moved
    without a credited rung explaining it — a manual
    `eyes_reset`, a relink re-handshake) → book **external**:
    `last_rung_ = "external"`, `last_close_rung = "external"`,
    **no `rec_*` counter increments**. The episode-closed log
    line shows `rung=external`.
  - else → self-healed: no counter, `last_rung_` unchanged,
    `last_close_rung = "none"` (as today's "none" booking).

**Deliberate non-change.** The fast-relapse test (eyes.c:885-888)
keys on `last_close_rung` ∈ {"rst", "rail"} and is **not**
extended to "external": relapse exists to re-escalate quickly
after the ladder's own heavy rungs; a human-issued reset carries
the human's judgment with it, and the next episode escalates on
the normal schedule. Recorded here so the asymmetry is a
decision, not an oversight.

---

## 8. S-1 — Pad-state watchdog term (Himax, 0.4.20.63)

**Finding (Himax audit §F-1, the storm's Himax-side lesson).**
Every .62 watchdog keys on the transport's own bookkeeping flags.
H-B (MT:1743-1768) watches the armed tracker; Guards 1-3 watch
`_spi_tx_done` / `_spi_tx_staged` / `_spi_rx_arm_failed`. The flap
state — line stuck HIGH in a ~300 ms rhythm with every flag
reading plausible — is visible in **none** of them:
`_spi_wd_aborts` read 0 for the entire ~6-hour storm. The one
signal that *does* describe the fault is the physical pad: while
the transport claims RX is armed, PB2 must be driven LOW. .62
measures that claim exactly once, at one arm (H-C, §9). S-1
measures it continuously.

**Mechanism.** A new block in `spi_poll`, immediately before the
H-B block (MT:1743), thread context, decimated: a file-static
counter increments per poll reaching the block; the check runs
every **64th** poll (~64 ms at the measured ~1 kHz cadence — AT
loop: `spi_poll()` + 20 µs sleep, `ma_server_at.cpp:113-114`).
The check is one read of the H-C primitive:
`hx_drv_gpio_get_in_value(GPIO13, &level)` (MT:1415).

**Predicate** — fire only when all of:

- `level == HIGH` (the pad disagrees with the arm), **and**
- `_spi_rx_armed` — the DMA-half tracker (set MT:561/:596;
  cleared MT:645 callback entry, :1386 transport cycle, :1300
  deInit). Armed means the arm muxed PB2 to GPIO13 (MT:564) and
  drove it LOW (MT:566): the pad *must* read LOW. This is H-C's
  conjunction, evaluated continuously instead of once.
- `!_spi_tx_staged` — staged (set MT:857/:939/:993/:1024, after
  each `spi_write_dma`) means PB2 belongs to SPI_S_DO and HIGH
  is the **legitimate data-ready signal** the S3 gates on.
  Checking then would fire on every long response.
- `!_spi_parked` — a parked arm muxes PB2 to SPI_S_DO and
  releases it (MT:603); the deselected slave tristates DO and
  the S3's pull-up holds the line HIGH legitimately, indefinitely.

`_spi_opened` is deliberately not a term: both arm functions
return early while closed *before* setting the tracker
(MT:536-545/:580-587 precede :561/:596), and the cycle and
deInit clear it — armed ⇒ opened, by construction.

**Sustain bound: 1 s, time-based.** The disagreement must hold
at every decimated sample across `PAD_SUSTAIN_US = 1,000,000`
µs (`_pad_high_since_us`; any predicate-false or LOW sample
resets it; a readback-API error *freezes* it — abstain, §10).
Placement against the existing bounds: Guard 2 allows a staged
response 300 ms before acting; H-B allows an unclaimed link
2 s. A line-level disagreement has no legitimate duration
anywhere near either — the only legal armed-but-not-LOW
intervals are intra-arm races of a few instructions plus one
decimation step (~64 ms). 1 s is > 15× that, gives a stuck pad
a full Guard-2 cycle to resolve as a staged-state problem
first, and still fires 2× sooner than H-B — the line is the
S3-visible truth, a stronger signal than flag bookkeeping, and
should not be the slowest watchdog on the board.

*The intra-arm race, stated so it is not rediscovered as a
bug:* `spi_poll` can sample between the tracker set (MT:561)
and the drive (MT:566) of an arm executing in the RX callback,
with the mux not yet switched. One such sample may start the
sustain timer; the next, ~64 ms later, reads the completed
arm's LOW and clears it. The time-based 1 s sustain makes
single-sample races harmless by construction — which is why
the sustain is wall time, not a sample count.

**Action on firing.** `_spi_rx_arm_failed = true;` — Guard 3
owns recovery, exactly as an H-C mismatch is handed over —
plus `_spi_pad_wd_count++` and
`diag_nb_emit("E DIAG PAD_STUCK_HIGH held_ms=%ld n=%lu", …)`,
then `_pad_high_since_us = 0` (a persistent fault re-fires at
most once per second: bounded DIAG rate, and each fire is a
fresh Guard-3 escalation input). The count is **separate** from
`_spi_wd_aborts`: the REARM line's `wd=` field (MT:336-348)
already mixes Guard 2 / Guard 3 / H-B by design (audit F-7),
and .62 storm telemetry was read against `wd=` meaning those
three — adding a fourth source silently would corrupt every
comparison with .62 logs. S-1's count lives in its own DIAG
line.

**Invariant.** Zero added latency on the re-arm path: the
check lives in `spi_poll` (like H-B), costs one GPIO register
read per 64 polls, and never runs in the RX callback, inside
an arm, or in Guard 1's timing loop.

**Composition with S-2.** S-1 detects the disagreement
*between* arms; S-2's in-arm verify detects it *at* arms. S-1
fires → Guard 3 re-arms → the verify re-checks the same
conjunction; if the mux write was truly lost, the verify
re-flags and Guard 3's fail counter advances toward teardown
(§9). Neither mechanism alone covers both timescales.

---

## 9. S-2 — H-C at every arm (Himax, 0.4.20.63)

**Finding (Himax audit §F-2).** `_spi_verify_arm_took`
(MT:1404-1422) — read back the PB2 mux and the GPIO13 level
after an arm — is called from exactly one site (Guard 1,
MT:1695). The other 21 release-reachable arm sites (§0.3)
trust the mux write blindly. The audit classed this as a
*spec* defect in the .62 round (coverage was never stated
in arm-site terms); .63 fixes the coverage, not the checker.

**Change.**

- `_spi_arm_rx()`: call the verify at the end of the success
  path, after the mux write (MT:564), the GPIO13 drive
  (MT:566), and the parked-flag clear (MT:568). The
  DMA-failure return (MT:552-559) precedes it — the .62 call
  site's "skip verify when the arm already reported failure"
  condition (MT:1694-1696) becomes structural instead of
  remembered.
- `_spi_arm_rx_parked()`: call at the end of its success
  path, after the SPI_S_DO mux (MT:603), with the **parked
  expectation**: mux == `SCU_PB2_PINMUX_SPI_S_DO`, and **no
  pad-level read** — parked tristate reads HIGH legitimately,
  so a level check would false-fire on every park. One verify
  function with a parked parameter keeps the readback and
  abstain plumbing (§10) single-sited.
- **Remove** the Guard-1 call site (MT:1694-1697). Guard 1's
  arm (MT:1688) inherits the verify like every other site.

**Ordering proof.** In `_spi_arm_rx` the two writes the
verify reads back (:564, :566) precede the verify call in
program order within the same execution context. No second
writer of the PB2 mux or of GPIO13 exists in the tree (audit
grep: GPIO13 is written LOW only, never HIGH; mux writes are
:564/:603 and init). The readback therefore measures the arm
that just executed, never a predecessor's residue.

**Safety of setting `_spi_rx_arm_failed` from inside the arm,
by caller class** (all 22 release sites):

- **RX-callback handler arms — 12 sites** (MT:729, :782,
  :807, :814, :864, :946, :1000, :1034, :1047, :1067, :1101,
  :1149), ISR context. The verify is two driver register
  reads: no blocking, no allocation, no printf; a mismatch
  writes one bool that Guard 3 consumes at the next poll; the
  verify never arms, so no reentrancy. Load-bearing
  precedent: the SCU mux *write* (:564) already executes in
  this same context on every handler arm — the verify's GET
  is its read twin, and the arm already performs a 256-byte
  memset plus DMA programming here, against which two
  register reads are noise.
- **Guard arms** (Guard 1 :1688; Guard 2 :1729; Guard 3
  :1525/:1554; H-B :1758; parked :1523/:1552/:1756), thread
  context. Intended new consequence: Guard 3's fail counter
  now advances on line-level failure of its own retry arm —
  in .62 a mux-lost Guard-3 arm read as success and reset
  the counter (MT:1566), masking exactly this fault class.
- **Init arm** (MT:1261): a verify failure leaves the flag
  set as the AT loop starts; the poll head (MT:1466) passes
  on the flag and Guard 3 owns the state from the first
  poll. In .62 this arm was the unverified residual the
  audit recorded.
- **Test-hooks arms** (MT:1127/:1140/:1142, hooks builds
  only): inherit the verify; no selector calls it directly,
  and the H-B injection (selector 2) does not arm at all, so
  hooks expectations are unchanged.

**Unchanged semantics.** Abstain-on-API-error (MT:1407,
:1415): an unreadable readback proves nothing and flags
nothing (it is now *counted*, §10). A verify failure does
**not** clear `_spi_rx_armed` — the DMA genuinely is
outstanding; the tracker remains a DMA-half claim by design,
and §8's continuous line check covers the conjunction
between arms.

---

## 10. F-3 — Abstain death note (Himax, 0.4.20.63)

**Finding (Himax audit §F-3).** H-C's abstain path is silent:
if a readback API dies, verification quietly never happens
again and nothing in telemetry says so — the same invisibility
the re-arm probes' death notes (MT:378/:390/:407/:427) were
built to prevent.

**Change.** Two statics beside the verify: `_hc_abstain_count`
(uint32) and `_hc_abstain_noted` (bool). Every readback-API
abstain increments the count: the verify's SCU read (MT:1406)
and GPIO read (MT:1414), **and** §8's pad read — one dead-API
story per boot, whichever consumer hits the error first.
The emit is deferred to `spi_poll` (the verify now runs in
ISR context, §9, where emitting is not permitted — the same
record-in-context / emit-in-poll discipline as
`_spi_diag_record`): beside the deferred-diag drain
(MT:1470ff / :1509),

```
if (_hc_abstain_count && !_hc_abstain_noted) {
    _hc_abstain_noted = true;
    diag_nb_emit("E DIAG HC readback abstain: api error n=%lu",
                 (unsigned long)_hc_abstain_count);
}
```

One line, once per boot, mirroring the probe death notes.
Subsequent abstains are silent but counted. The REARM /
REARM_LAT formats (MT:336-353) are **not** changed, and no
existing line gains a field for this; the count lives in
state, named and grep-able, if a future DIAG surface wants it.

---

## 11. F-4 — POLL_GAP max payload (Himax, 0.4.20.63)

**Finding (Himax audit §F-4).** `_poll_max_gap_us` is updated
(MT:1442) and never read — write-only state. A single 600 ms
gap and an hour of 499 ms gaps are indistinguishable in the
.62 record.

**Change.** The emit (MT:1444-1445) gains the max, and the
max resets on emit:

```
diag_nb_emit("E DIAG POLL_GAP %ld max=%ld",
             (long)(gap_us / 1000), (long)(_poll_max_gap_us / 1000));
_poll_max_gap_us = 0;
```

The max update (:1442) already runs before the threshold
test, so `max=` always includes the triggering gap.
Semantics: max = largest poll gap since the previous
POLL_GAP emit, or since boot. The change is additive — the
existing `E DIAG POLL_GAP <ms>` prefix is preserved for log
tooling that greps it.

---

## 12. F-5 — Guard-3 open rate limit (Himax, 0.4.20.63)

**Finding (Himax audit §F-5).** Guard 3's block head
(MT:1510ff) retries the open on every entry while the
transport is closed (`_spi_try_open()` at MT:1517). Guard 3
is entered at poll cadence while the flag is set, and the AT
loop polls at ~1 kHz (`ma_server_at.cpp:113-114`) — so a
failed-open state means a controller re-init attempt every
~1 ms, indefinitely: hardware churn producing no information
after the first attempts.

**Change.** A file-static `_g3_last_open_attempt_us` gates
the block-head branch: the open is attempted only when
**≥ 100 ms** (100,000 µs) have passed since the last
block-head attempt. A rate-skipped poll:

- leaves `_spi_rx_arm_failed` **set** (landing in the block's
  existing flag-restore path, MT:1524-1527, or equivalent),
- exits the Guard-3 block for this poll, and
- does **not** advance `_spi_rx_arm_fail_count` — a skipped
  poll is not a recovery attempt. (Counting it would drive
  the escalation at MT:1538-1566 to teardown-cycle a closed
  transport at ~100 Hz, recreating the churn one level down.)

When an attempt *is* made and fails, existing counting
applies unchanged. The escalation's own `_spi_transport_cycle()`
open (MT:1549) needs no second gate: reaching it requires 10
counted attempts, which under this limit are ≥ 1 s apart by
construction.

**Bound justification.** 100 ms caps the added recovery
latency at 100 ms — invisible beside the S3 ladder's 1 s
floor and 5-60 s backoffs — while cutting open-attempt churn
100×. 10 Hz still retries two orders of magnitude faster
than any human or reset timescale in the system.

**H-A(2) reachability, re-proven.** The poll-head gate
(MT:1466) is untouched; Guard 3 is still entered on every
poll while the flag is set; the limiter never clears the
flag. The sealed-state invariant — no reachable state with
all claim flags false, RX unarmed, transport closed — rests
on the flag's persistence and is unaffected.

*Hooks note:* the open-failure injection (`_test_open_fails`)
is consumed per attempt; .63 hooks-build tests must budget
~100 ms per injected failure.

---

## 13. F-6 — deInit retires Guard 3 (Himax, 0.4.20.63)

**Finding (Himax audit §F-6).** `SPI::deInit()` (MT:1266)
early-returns when `!_spi_opened || !m_initialized`
(MT:1267-1269) — precisely the failed-open state — without
clearing `_spi_rx_arm_failed`. A deInit issued mid-escalation
leaks the flag into the next session, where Guard 3 would
present a failure no arm in that session ever made. The
normal path has the same hole in miniature: it clears the
armed tracker (MT:1300) but never the flag or the fail count.

**Change.** At deInit entry, **before** the early return:

```c
_spi_rx_arm_failed = false;
_spi_rx_arm_fail_count = 0;
_spi_rx_armed = false;
```

One site, both paths: a failed-open deInit retires Guard 3,
and the tracker claim dies with the transport that made it.
`SPI::init` needs no change — statics are zero at cold boot,
and the deInit→init sequence is now clean by this rule.

---

## 14. Out of scope — S-2b, gated on FH-2

**S-2b** (make `_spi_tx_staged` F3-proof: clear it only on
TX-DMA completion plus TXAVAIL-at-capacity, instead of at
the parse-retire point, MT:746-759) is **not** in this round.
It is gated on confirming **FH-2** — the Himax audit's
parse-retire staged-age hypothesis (§8 of that audit: the
F3 lifecycle clear at MT:756-759 lets a response's *true*
staged age be sliced across commands, so Guard 2's 300 ms
clock never sees an abandoned response's full age) — with a
UART capture of the Himax console during a reproduced flap
(acceptance step 3, §17). If FH-2 confirms, S-2b becomes a
0.4.20.64 item or a .63 amendment **with the owner's separate
go**; it is not built on this spec's momentum.

The dependency is stated plainly so the round is not
oversold: if FH-2 *is* the flap's Himax-side mechanism, .63's
S-1 detects its line-level shadow and S-2 hardens every arm,
but the staging bookkeeping itself stays as .62 shipped it.
What guarantees the storm cannot again run for six hours
unobserved and un-escalated in the meantime is the S3 side
of this round — G1's rate trigger and the ladder it feeds.

---

## 15. Interaction audit — the recorded storm, replayed under the new semantics

The mandatory worked example. The recorded event (soak §5,
S3 audit §G1): soak cycle-2 crop SET returns ok at **01:58:55
UTC**; the first gate-failure streak begins the same second;
the link settles into a self-sustaining rhythm — bursts of
exactly 3 gate failures, one legitimate completion, period
~289-340 ms, ~150-180 episodes/min — and stays there,
starving multi-exchange operations, until a manual sealed
`eyes_reset` clears it at 07:54:40 UTC, **5 h 56 min** later.
The S3 never reboots; `wd=` reads 0 throughout; no ladder rung
ever fires.

Replay assumptions: 0.4.51 semantics on the S3; the Himax-side
state persists exactly as recorded (the worst case — .63's
S-1/S-2 might clear the underlying state earlier from the
other side, which only shortens what follows). Failure rate
taken at the **conservative** end of the measured band,
450 failures/min = 7.5/s; the 540/min end is noted where it
changes a time.

| Simulated time | What happens under .51 |
|---|---|
| **t+0.0 s** (01:58:55) | Crop SET ok; first gate failure. The streak climbs 0→3 in the first cycle, then the cycle's available completion resets it — on the new wire-path reset (D1, io_spi.c:898). Behavior so far is deliberately identical to .50: the completions are legitimate, and D1 says so. |
| **t+0-8 s** | Cycles repeat every ~300 ms. Per cycle: 3 failures (io total and window climb at 7.5/s); rung 1's drain fires at streak 3 in-io, as today — if its capture validates (§3) it returns the stuck count and `rec_drain` increments, but it no longer resets the streak and can no longer defuse anything. The WI-1 summary is still silent (total < 100). To every .50-era observer this still looks like nothing. It is not: the window is filling. |
| **t≈+8.0 s** | Window crosses **60** (7.5/s × 8.0 s). On the next supervisor sample (≤ 100 ms later): **episode opens**, `trigger=rate`, `link_up_` drops. The `episode open` log line exists — the line whose absence across 342 MB defined the original storm. |
| **t≈+11.4 s** | FSEO reaches **25** (3.3 s of post-open failures at 7.5/s); episode age 3.3 s ≥ the 1 s floor → **rung 2 (CMD_RESET resync) fires. This is the first rung**, at simulated second ≈ 11 (band: at 540/min the window crosses at ≈ +6.8 s and rung 2 lands at ≈ +9.6 s; across the measured band, **first rung at t+9.6 to +11.4 s**). |
| **t≈+11.4-14.8 s** | Rung 2 returns — most likely ESP_OK (the reset clocks out during a gate-LOW window; credit tentatively "reset", gen snapshot taken) — and the flap persists: a bare CMD_RESET does not re-handshake the SSCMA session, and the recorded event's cure was RST + re-handshake. FSEO keeps climbing. |
| **t≈+14.8 s** | FSEO reaches **50**. `rung_fail_streak` is 0 (rung 2 does not feed it), so backoff is 0; the bus is free; the episode-health predicate (§5.5) does not self-abort on the mid-cycle 0-samples → **rung 3 (RST, `eyes_recover_camera(false)`) fires ≈ t+14.9 s**. Its internal recovery bumps `link_gen_` (eyes.c:735); on ESP_OK, credit becomes "rst" and `ep_credit_gen` snapshots the post-return generation (G4). |
| **t≈+15-17 s** | Re-handshake completes. Polls complete continuously; the streak pins at 0; `healthy_since` starts running. |
| **t≈+18-19 s** | The 2 s dwell completes → **episode closes, booked `rung=rst`** (gen == `ep_credit_gen`), `rec_rst` increments, `last_rung = "rst"`, `link_up_` restores. Total degraded time ≈ **19 s**, against 21,401 s recorded. The re-keyed WI-1 summary (§5.6) has fired during the episode (total crossed 100 at ≈ t+13 s), carrying streak/window/total and the drain declared/discarded counters. |

**Counterfactual branches.**

- **(a) Rung 2 heals it** (the state was RESET-clearable):
  completions resume after t≈+11.4 s, dwell completes, close
  at ≈ t+14 s booked `rung=reset` — the gen never moved
  (resync does not bump it), so G4 credits the rung.
- **(b) The recorded cure, replayed mid-episode**: a manual
  sealed `eyes_reset` at, say, t+13 s bumps `link_gen_`
  outside any rung and clears the state. Dwell completes;
  close at ≈ t+15-16 s books **`rung=external`** — no `rec_*`
  increment, `last_rung = "external"`. The record now
  distinguishes the human's recovery from the ladder's, which
  is G4's entire point.
- **(c) A procedurally-successful but unhealing rung 3**: if
  `eyes_recover_camera` returns ESP_OK yet the flap persists,
  the episode stays open and rung 3 re-fires at
  recovery-procedure cadence — backoff keys only on ESP_ERR
  outcomes (`rung_fail_streak`), in .50 and .51 alike. This
  is a **pre-existing property the audits did not slate**;
  it is flagged here and in §18 for the owner's decision,
  neither silently fixed nor silently ignored.
- **(d) The 01:54:11 drain** (the genuine rung-1 recovery,
  pre-storm): under .51 the capture exits gate-LOW with a
  sane count → declared recovery, `rec_drain++`, segment
  returned, no streak reset; the next available completion
  resets the streak; the capture stream proceeds. The one
  proven good outcome of the old semantics is preserved
  exactly.
- **(e) The pure wedge** (2026-11-09 incident shape — no
  completions at all): the streak climbs uninterrupted; the
  episode opens on the streak trigger at 25 exactly as in
  .50; D1's removed resets never fired in this shape anyway
  (no successful sends occur), and drain captures of an idle
  line are now *discarded* (§3) instead of defusing the
  ladder. Rung timing as .50, except rung 2 may land ≤ 1 s
  later (§5.3 floor note). No regression in the shape .50
  was built for.
- **(f) Healthy idle and boot**: the window peaks at 4
  (boot transient) — 15× under threshold; the streak resets
  on the first completion; no episode opens, nothing logs.
  The soak's healthy cycles produce zero episode opens under
  G1, by the §5.2 margins.

The three S3 mechanisms compose as follows: **D1** removes
the counterfeit resets so the streak means what it says;
**G1** makes the streak's oscillation irrelevant by counting
what the streak cannot see; **D2** keeps rung 1's one proven
job — clearing a stuck gate mid-capture — while removing
its power to fake a recovery. No pair of them double-opens
or double-credits (§5.3); §7 arbitrates the credit when the
ladder and a human act in the same episode.

---

## 16. Rollback & pairing

No wire-format change on either side: the SSCMA packet
format, the AT surface, the heartbeat schema (field order
aside), and the seal envelope are all untouched. The pairing
matrix stays fully interoperable — {0.4.50, 0.4.51} ×
{0.4.20.62, 0.4.20.63} — and each side rolls back
independently of the other.

- **S3 → 0.4.50:** reflash the kept artifact
  `watcher-v0.4.50-merged.bin` (sha256
  `6e926729b0df30bc5de42839d31cea2fee8c4f858dad0e3a37ea68e1dd8c0776`,
  the 2026-11-09 flash record). Rollback triggers: drain
  *discards* climbing against a demonstrably healthy Himax
  (D2 false negatives blocking legitimate rung-1 recoveries);
  G1 episode opens on a healthy link; any S3 reboot
  attributable to the round — watchdogs stay armed, and a
  reboot is a defect, never a recovery mechanism.
- **Himax → 0.4.20.62:** reflash the kept kit image
  (`himax-flash-kit/v0.4.20.62/`) by the owner's XMODEM flow.
  Rollback triggers: `PAD_STUCK_HIGH` firing on a healthy
  link (its ~1/s DIAG cadence is unmistakable); Guard-3 churn
  attributable to verify false-fires; any new re-arm latency
  symptom in REARM_LAT.

---

## 17. Acceptance sequence — gated, in order

Each step is gated on the previous step's evidence and,
where it touches the device, on the owner's go at that step
(build / stage / flash remain separate commissioned steps).

1. **0.4.51 flashes first.** The mixed pair (.51 + .62) is
   wire-valid. The flash counts only on a `fw-flashed`
   record plus a heartbeat naming 0.4.51.
2. **Dead-Himax drain test** — the never-run test from the
   original spec (§4.6 of `link-hardening-spec-2026-11-09.md`).
   With the Himax held unpowered or in reset, confirm in
   telemetry: drains fire and are **discarded** (the §5.6
   summary's discarded count climbs; `rec_drain` does not);
   no recovery is declared on idle bytes; the ladder
   escalates visibly — episode open (streak trigger), rung 2,
   rung 3, rung 4 attempts in the log; ping is answered
   throughout; S3 uptime never resets. Any declared drain
   recovery on idle bytes fails the round.
3. **FH-2 UART capture, on the current pair (before .63).**
   Reproduce the flap via full-path crop sets while capturing
   the Himax UART console; look for the parse-retire /
   staged-age signature (Himax audit §8: the MT:756-759
   lifecycle clear slicing a response's true staged age so
   Guard 2's 300 ms clock never sees it whole). The result
   decides S-2b (§14) and is recorded either way — a
   non-confirmation is also a result (FH-1 then leads).
4. **Hooks-build H-C firing test.** Build .63 with
   `WATCHER_LINK_TEST_HOOKS 1` (MT:68), force the PB2 mux
   wrong via the hooks arm injection, and confirm Guard 3
   engages with the verify's flag visible in DIAG — the
   first bench proof H-C can fire at all (the .62 injection
   tests were compile-verified only, with no bench results
   on record). Exercise the F-5 open-fail injection at its
   new 100 ms cadence in the same session.
5. **0.4.20.63 flash** (owner's run, XMODEM, flash-kit
   flow). Proof of landing is the DIAG `watcher-himax` line
   naming 0.4.20.63 with the fresh build fingerprint — not
   the transfer's success message.
6. **Soak re-run.** 20 full-path cycles under the original
   pass criteria (captures verified end-to-end, no wedge,
   no S3 reboot), **plus one automatic fail**: any flap-mode
   recurrence — a 3-streak oscillation visible in telemetry —
   that does *not* produce an episode open and at least one
   rung attempt in telemetry fails the soak regardless of
   captures. A flap that opens an episode, engages the
   ladder, and closes credited (any rung — or "external"
   via a deliberate manual reset) is the mechanism working
   as designed and is recorded as such, not as a capture
   failure.
7. **D3 proof.** Grep a bridge-logged heartbeat from live
   telemetry and show the complete `himax_link` object —
   `"last_rung"` included — inside the logged line (§4). If
   it is not there, D3 has not landed, whatever the version
   string says.

---

## 18. Open decisions for the owner

Every number in this spec is either grounded in a cited
measurement or listed here. The grounded ones — G1's
60-per-60 s threshold (§5.2: 450-540/min measured flap vs 4/min
worst healthy burst), the FSEO rung thresholds (the ladder's
own 25/50), D2's count bound (4096 = `DRAIN_STASH_CAP`, a code
constant), the D3 placement (§4's construction) — are not
listed. These are:

1. **S-1 sustain = 1 s** (§8). *Recommendation: adopt.*
   Grounded only as a placement — > 15× the intra-arm race
   bound (~64 ms), 3.3× Guard 2's 300 ms, half of H-B's 2 s.
   No measured distribution of a real pad fault's duration
   exists; the fault has never been observed from the Himax
   side. Alternatives: 500 ms (faster; still 7× the race
   bound) or 2 s (matches H-B; in some shapes slower than
   the S3 ladder's first rung).
2. **S-1 decimation = every 64 polls** (§8). *Recommendation:
   adopt.* Cost arithmetic (one register read per ~64 ms;
   ≥ 15 samples per sustain window), not measurement. 32 or
   128 would serve; the sustain is time-based precisely so
   this choice is not load-bearing.
3. **`LINK_HEALTH_DWELL_MS` = 2000 ms** (§5.4).
   *Recommendation: adopt.* Semi-grounded: the flap's ~300 ms
   period bounds the fault's healthy gaps (≥ 6 cycles per
   dwell), but supervisor sampling behavior under sustained
   bulk load was not measured against this constant. Assessed
   low-risk — the streak is read by the supervisor task
   itself, independent of bulk traffic.
4. **F-5 bound = 100 ms** (§12). *Recommendation: adopt*
   (it is also the tasking's example). Grounded in cadence
   arithmetic — 1 kHz poll → 10 Hz retry, ≤ 100 ms added
   recovery latency against 1 s+ ladder timescales — not in
   a measurement of `spi_open` cost, which has never been
   profiled.
5. **Rungs ≥ 3 re-fire spacing** (§15, branch c).
   *Observation, not in the slate:* a procedurally-successful
   but unhealing rung ≥ 3 re-fires at procedure cadence
   because backoff counts only ESP_ERR outcomes — pre-existing
   .50 behavior the audits did not slate. Recommendation:
   decide whether a future round adds a minimum inter-attempt
   spacing after *any* rung ≥ 3 attempt (suggested value:
   `LINK_BACKOFF_1_MS`, 5 s). .51 as specced does not change
   this behavior.
6. **G1 slot geometry, 6 × 10 s** (§5.1). Recorded for
   completeness as an *implementation* choice, not an owner
   decision: the decision is the 60/60 s threshold; the
   geometry only quantizes it (≤ 10 s age over-count, the
   conservative direction).

---

*Prepared 2026-11-10 from the two link-hardening audits of
2026-11-09/10, against the live trees (S3 0.4.50 source,
Himax 0.4.20.62 source). SPEC ONLY — builds begin on the
owner's approval of this document, as separate gated steps
per §17.*
