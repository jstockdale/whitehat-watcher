# Link hardening — S3 + host audit (fw 0.4.50 round)

**Date:** 2026-11-09 (post-incident). **Type:** read-only audit —
no code changed, no device commands sent by this audit.
**Scope:** WI-1/WI-2/WI-3 as built in the working tree
(`firmware/components/sscma_client`, `eyes`, `link`), the §3.3
origin/eyes_reset policy, the timer-probe removal, and the
round's host tooling (`seal_client.py` REPLAY-reactive patch +
tests, `send_command.py` eyes_reset allowlist entry).
**Method:** every claim below was checked against the actual
working-tree source (the tree that was built into 0.4.50) and
against the live record (`host-agent/mqtt-bridge/telemetry.log`,
342 MB, full 0.4.50 deployment). Spec references are to
`docs/link-hardening-spec-2026-11-09.md`; build-report claims to
`docs/link-hardening-s3-build-2026-11-09.md`; incident facts to
`docs/link-hardening-soak-2026-11-09.md`.

**The incident in one paragraph.** On the hardened pair
(0.4.50 + Himax .62), a full-path crop SET at 2026-11-10
01:58:55 UTC put the link into a *flap storm*: gate episodes of
exactly 3 consecutive failures then one success, period
~289–340 ms, ~150–180 episodes/min, self-sustaining at idle
for ~6 h (~62,000 episodes). No ladder rung ≥2 ever fired; the
WI-1 summary never fired; `link_up_` stayed true throughout
while multi-exchange AT operations starved. A manual sealed
`dbg eyes_reset` (rail:0) at 07:54:40 UTC cleared it instantly.
This audit explains, from the code, why every one of those
non-events was structurally guaranteed — and what else the
round got right and wrong.

---

## 1. Per-WI verification table

### WI-1 — observability (spec §2)

| Specced | As built | Verdict |
|---|---|---|
| Remove/transition-gate the per-attempt gate error at old io:821 (`spi_send_cmd_`) | Per-attempt `ESP_LOGE` gone; failure path calls `gate_note_failure_()` only (`sscma_client_io_spi.c:1052`), which logs once per streak edge (`:467-478`) | ✅ Sound — soak: 5 MISO lines vs 227k historic |
| Transition-gate the wait-level warning at old io:1031 | `spi_miso_wait_level_` warning gated on `miso_wait_failing_` (`:1264-1267`); flag cleared by *any* successful wait (`:1253`) — divergent from the gate streak (see I2) | ⚠️ Works, semantics diverge |
| Rate-limited summary from the process task: one line per 100 failed polls while a streak is ongoing | One line per +100 of *io streak* (`sscma_client_ops.c:246-252`), fires only when `streak >= 100` | ❌ Cannot fire in flap mode (G2) |
| Poll backoff 1→10→50→250 ms on consecutive available() failures; success (ESP_OK regardless of rlen) resets; bulk skip untouched | Exactly as specced (`sscma_client_ops.c:218, 225, 240-256, 232-236`) | ✅ Sound |
| Heartbeat nested `himax_link` object (fields per §10.3) | All 8 fields built as specced (`link.c:960-976`, fed by `eyes_himax_link_info`, `eyes.c:994-1009`) | ❌ Built but unobservable in the durable record (D3) |

### WI-2 — failure accounting + honest `link_up_` (spec §3)

| Specced | As built | Verdict |
|---|---|---|
| One io-owned consecutive-failure counter; accessors in the component header | `gate_streak_` + `sscma_client_gate_streak/age_ms/reset/drain_recoveries` (`io_spi.c:460-511`, `sscma_client_io.h:201-208`) | ✅ As designed |
| Hooks in worker exec paths only: send_cmd gate, write gate, available gate-2 increment; send/write/available success reset | Increments at `io_spi.c:1052` (send_cmd gate), `:606` (write gate), `:884` (available gate-2). Resets at `:1100` (**any** `spi_send_cmd_` send), `:716` (write completion), `:769` (read stash-serve), `:873` (drain capture) | ⚠️ Placement is worker-only as required — but the reset semantics are the defect (D1) |
| AT-layer error reply is a completed transaction, does not count | Holds: error replies complete via available/read successes | ✅ Sound |
| `link_up_` cleared by the supervisor at episode open (streak ≥ 25), restored on close/recovery; eyes.c remains the only writer | Exactly so (`eyes.c:873-896` open/clear, `:858-866` close/restore, `:734` recovery write) | ⚠️ Mechanism sound; threshold unreachable in flap mode, so the flag lied for 6 h (G1) |
| eyes_set_crop / eyes_set_sensor_opt failures feed the counter with retry behaviour unchanged | They reach the io layer as write/available txns; no eyes-side changes | ✅ Sound |

### WI-3 — escalation ladder (spec §4)

| Specced | As built | Verdict |
|---|---|---|
| Rung 1: capturing drain inside `spi_txn_available_exec` on gate timeout at streak ≥ 3, ≤200 ms, bytes returned via the normal success path; read serves the stash first | `io_spi.c:867-875` (branch), `:522-560` (`spi_drain_capture_`, 64 B chunks, 4096 B PSRAM stash), `:751-772` (stash-serve in read exec) | ⚠️ Works (proven live 01:54:11) but its success resets the streak (D1) and is content-blind (D2) |
| Rung 2: shared no-gate CMD_RESET helper (from unpark), dedicated worker txn, once per episode | `spi_transport_resync_` (`io_spi.c:564-578`) shared with unpark (`:1187-1194`); `SPI_TXN_RESYNC` via worker queue (`:1851`, `:1975-1980`, 2 s budget); once-per-episode flag (`eyes.c:899-916`) | ✅ Sound (never reached live) |
| Rungs 3/4: `eyes_recover_camera(false/true)` from a new supervisor task (not process/worker/monitor), bus-busy deferral in 500 ms slices bounded ~65 s | `eyes_link_health_task_` (`eyes.c:804ff`), created at end of `eyes_init` (`:645-652`, 4096 stack, idle+3, in `stack_hwm` per `link.c:939`); deferral `link_wait_bus_free_` (`eyes.c:787-802`) | ✅ As designed — and unreachable in flap mode (D1, G1) |
| Episode state machine: opens at engagement threshold, closes on successful poll/rung; backoff 5→15→60 s between failed rungs ≥3; rail only after failed RST in-episode or fast relapse; rail cap 3/rolling hour → latch to RST-only every 10 min | All present (`eyes.c:764-775` constants; `:842-896` open/close; `:917-985` rungs, backoff `:934-937`, rail gating `:922-924`, cap accounting `:830-840, 955-960`, latch `:929-932, 971-974`) | ✅ Faithful to spec; crediting fuzz (G4) |
| One log line per rung attempt and outcome | Present for rungs 2/3/4 (`eyes.c:901-915, 958-985`) | ✅ (Vacuous live: zero firings in the deployment) |
| No rung 5 — ladder never reboots the S3 | No `esp_restart`/abort anywhere in the supervisor region (`eyes.c:760-1010` checked) | ✅ Sound |
| 1 s continuous-failure floor before rung ≥2 | `sscma_client_gate_streak_age_ms() >= LINK_FAIL_FLOOR_MS` at `eyes.c:900, 918`; age measured from streak start (`io_spi.c:495-501`) | ⚠️ Correct mechanism, defeated by the same reset (age restarts every ~300 ms in flap mode) |
| `link_gen_` incremented on every successful `eyes_recover_camera` and relink re-handshake; accessor; heartbeat | `eyes.c:735` (recovery), `:385` (relink), `:151-154` accessor, heartbeat `gen` | ✅ Sound |
| §4.5/§2.5: crop callers snapshot gen, re-issue once on ESP_ERR_TIMEOUT with changed gen | All three specced sites: dbg crop clear (`link.c:2046-2058`), dbg crop set (`link.c:2064-2077`), strips loop (`eyes.c:1863-1878`) | ✅ Sound; never fired live (see §3.4) |

### Origin policy + eyes_reset (spec §10.2, build §3.3)

| Specced | As built | Verdict |
|---|---|---|
| Force-tag origin on both remote dispatch paths; host-supplied tags replaced | Bridged `sealed_fwd`: tags deleted then forced (`link.c:1294-1299`). Native: `link_sealed_dispatch(json, len, verified)` forces both (`link.c:2592-2600`); `mqtt_native.c` passes `true` only from verify-then-dispatch paths (`:304, 328, 375`), `false` for grace plain (`:386`) | ✅ Sound — no untagged remote path found |
| `_sealed` only on cryptographic verification (grace gets `_via_mqtt` only) | `sealed_fwd` sets `_sealed` iff `rc == WMS2_OK` (grace `WMS2_OK_GRACE` excluded, `link.c:1296-1299`); native iff `verified` | ✅ Sound |
| eyes_reset: remote requires the seal check; remote rail default 0, explicit `"rail":1` honored; serial unchanged (default 1) | `link.c:2339-2350`: `via_mqtt && !sealed` → refused; rail = `opt_int(...,0)` remote / `opt_int(...,1)` serial; `snap_busy` refusal intact (`:2343-2346`) | ✅ Gate sound; rail *value* validation loose (G3) |
| "Highest scope tier" (§10.2b) | Implemented as the `_sealed` verified-envelope check; build report deviation 1 discloses the substitution honestly (no per-key tiers exist in the shipped verifier) | ✅ As disclosed |

### Timer-probe removal (spec §7)

| Specced | As built | Verdict |
|---|---|---|
| Remove `timer_list_probe_` + 3 call sites from eyes.c | `grep -rn "timer_list_probe_\|timer_probe" firmware/components/` → **zero matches** | ✅ Verified removed, no remnants |

### Host — seal_client.py REPLAY-reactive resync (spec: seal-replay spec §§1-5)

| Specced | As built | Verdict |
|---|---|---|
| `_REPLAY_RE` over the same tail scan, extract `<live>` | `seal_client.py:196`, shared `_telemetry_tail()` (`:176-194`) | ✅ |
| `floor = max(advertised, latest replay)`; forward-only unchanged | `auto_resync()` (`:268-277`) via untouched `resync_counter()` | ✅ |
| Margin (+128, owner-ruled) **only** when the REPLAY candidate binds | `:269-272` — replay branch: `live + REPLAY_MARGIN`; advertised branch: floor itself | ✅ Exactly as specced |
| `resync` result logs the binding source | `{"from","floor","source"}` (`:290-291`) | ✅ |
| Unit coverage per the spec's test sketch | 24 checks in `test_seal_client_resync.py` — all spec cases incl. decreasing-pair latest-wins | ✅ With gaps (I5) |

### Host — send_command.py eyes_reset entry

| Expected | As built | Verdict |
|---|---|---|
| eyes_reset sendable in its real wire form | `"eyes_reset"` added to `DBG_ALLOWED` (`send_command.py:63-73`); live-proven 2026-11-10 07:54 (device replied ok, link recovered) | ✅ Works |
| Shape validation proportionate to a state-changing command | The generic dbg branch (`:119-121`) checks only cmd-name membership + absence of `on`/`v`/`color` keys. **No key-set restriction, no `rail` value validation at all** | ❌ Gap (G3) |

---

## 2. Flap-storm mechanism — what the code says

### 2.1 Proven by the code (and matched line-for-line by telemetry)

**The streak is capped at exactly the drain threshold.** Two
reset paths guarantee it, and both sit in the poll loop:

1. The drain branch itself: when the available exec's gate-1
   fails at streak ≥ 3, it drains; on capturing >0 bytes it
   calls `gate_note_success_()` (`io_spi.c:873`), resetting to
   0 the very streak that admitted the drain.
2. The universal send reset: `spi_send_cmd_` calls
   `gate_note_success_()` after **every** successful command
   send (`io_spi.c:1100`). When a drain captures 0 bytes (line
   already LOW at drain entry), the available exec returns the
   timeout with the streak standing at 3 — and the *next*
   poll's gate-1 pass (the line is LOW; that's why the drain
   captured nothing) resets it via this path, logging
   "recovered after 3".

There is no third outcome: every episode ends at 3, by
construction. (Episodes ending "after 2" at 01:45/01:49 are the
same send-reset firing one poll earlier, before the drain
threshold — consistent.) The full reset census is five call
sites (`io_spi.c:505` — a dead exported wrapper, no callers;
`:716, 769, 873, 1100`), and in the poll loop only `:873` and
`:1100` are reachable.

**Everything above rung 1 keys on quantities this loop
pins to ~zero.** The supervisor samples the streak every
100 ms (`eyes.c:824`) and opens an episode only at ≥25
(`eyes.c:874`); rungs 3/4 need ≥50 (`eyes.c:917`); the 1 s
floor reads `gate_streak_age_ms()` (`io_spi.c:495-501`), whose
clock restarts at every streak reset. In flap mode the streak
never exceeds 3 and never ages past ~350 ms. The thresholds
aren't merely unreached — they are *unreachable* while the
process task polls. Telemetry confirms the null result
absolutely: across the whole 0.4.50 deployment, **zero**
`episode open`, zero `ladder rung`, zero `episode closed`,
zero `gate still failing` lines.

**The ~300 ms period needs no external oscillator.** Per
episode: 3 × 50 ms gate waits (`io_spi.c:1050`'s wait is
50 ms) + the process task's WI-1 backoff delays that precede
the polls (1, then 10, then 50 ms — `sscma_client_ops.c:218,
225`; the index resets each episode because the drain branch
returns ESP_OK) + the drain itself (64 B chunks with 2 ms
pauses until MISO falls, `io_spi.c:533-556`) + queue/worker
turnaround ≈ 290-340 ms. The WI-1 backoff is a *component* of
the flap period, not its cause; the pre-0.4.50 cadence
(~51 ms polls) would have produced the same loop at ~200 ms.

**The ladder's own health signals are all instantaneous.**
`link_up_` drops only at episode open (`eyes.c:890-893`), so
it read "up" for the entire 6 h outage; `capture_png`'s
link-down recovery gate therefore never engaged while its
mode-select starved (soak §1). `eyes_himax_link_info`'s
`state` (`eyes.c:1005-1007`) is a spot sample of the same
oscillating streak. Even the rung-≥3 deferral wait would have
been defeated had an episode somehow opened:
`link_wait_bus_free_` aborts on any streak==0 sample
(`eyes.c:791-801`), and the flap crosses zero ~3×/s.

**Rung 1 is the reason the flap was survivable.** The drain
converted the old permanent wedge into a degraded-but-alive
link: short exchanges squeezed through (crop sets returned ok
at 01:58:39-55, soak §3), heartbeats never stopped, and the
owner's one manual `eyes_reset` — RST + 1 s + re-handshake
(`eyes.c:718-736`) — cleared in seconds what six hours of
ladder time could not reach. The hardening failed at
*escalation and visibility*, not at rung 1.

### 2.2 Hypothesis (not provable from the S3 tree)

What re-raises MISO after every drain remains the Himax-side
question. The code constrains it: the .62 transport reported
itself healthy throughout (REARM stats normal; zero
`POLL_GAP`/`STUCK_UNARMED`, soak §5), so its AT loop never
stalled and none of its new guards saw an unclaimed state.
The S3 evidence is consistent with RCA Addendum 2's locus —
a transport state formed at the final Guard-1 re-arm after
the crop response's last clock-out — in which the Himax
perpetually has (or believes it has) staged output: each S3
drain clocks out the residue, the Himax re-arms for exactly
one poll, and the next staged item (its log/stats stream
provides a perpetual source at idle) re-forms the pattern.
Whether the drain-capture branch (bytes >0) or the
0-capture-plus-next-pass branch dominated the 62k episodes
cannot be distinguished from the surviving log — the two
branches log identically ("recovered after 3"), and
`rec_drain` — the one counter that would have separated
them — was never observable (D3).

---

## 3. Findings

### CONFIRMED DEFECTS

**D1 — Reset-on-any-success makes the WI-2 streak unable to
represent persistent failure; rungs 2–4 and the episode
machine are unreachable in any intermittently-passing fault
mode.** The spec's own counting table (§3.1) put a reset at
"any `spi_send_cmd_` success" — but a command *send* is a
gate pass, not a transaction completion, and in a flapping
link gate passes are noise. Combined with the drain branch's
self-reset (`io_spi.c:873`), the streak caps at exactly the
drain threshold (3). Evidence: §2.1; 432/432 soak episodes
"recovered after 3"; ~62,000 more over the following 6 h;
zero supervisor output of any kind.
*Minimal fix sketch:* move the reset out of `spi_send_cmd_`
and out of the drain branch. Reset the streak only on a
*completed gated transaction* at the exec level: available =
count clocked at gate 2; read = data clocked; write =
transmitted after passing its gate. Let the drain branch
record its recovery in `gate_drain_recoveries_` *without*
zeroing the streak (at most, note the drain separately).
Under that rule a flap accumulates 3, 6, 9, … and the
existing thresholds engage unmodified: episode at 25 in
~2.5 s, rung 2 past the 1 s floor, rung 3 at 50. This is a
counting-semantics change in one file; thresholds, the
supervisor, and the drain mechanics are untouched.

**D2 — Rung-1 drain declares success on unvalidated clocked
bytes, including idle-line noise from a dead Himax.**
`spi_drain_capture_` (`io_spi.c:522-560`) is bounded only by
line level, the 200 ms budget, and the 4096 B stash; if it
clocks *anything* — including 0xFF idle bytes from an
unpowered Himax whose released PB2 reads HIGH (the RCA's
"Himax gone" case) — the available exec returns ESP_OK with
`*len = captured` (`io_spi.c:867-875`), feeds the bytes to
the AT parser as a poll reply, and credits a "recovery".
Consequences: (a) the §4.6 dead-Himax bench test (never run)
would likely show the same streak-cap as D1 plus a parser
fed garbage; (b) `rec_drain` counts non-recoveries.
*Minimal fix sketch:* take the success branch only if the
drain exited because MISO fell LOW *and* the captured bytes
are not a uniform idle pattern (all-0xFF / all-0x00 check is
a few lines); timeout/stash-full exits return the original
timeout with the streak standing.

**D3 — The `himax_link` heartbeat object has never been
observed: it is placed after `stack_hwm` in a heartbeat
whose bridge-logged form truncates at ~290 chars.**
`link.c:960-976` appends the object after the `stack_hwm`
block; the latest logged heartbeat (07:59:54) is cut off
*inside* `stack_hwm` at 293 chars total. Counts over the
full 342 MB telemetry: `himax_link` 0, `rec_drain` 0,
`last_rung` 0, `"streak"` 0. Spec §10.3's premise — "the
bridge already logs the raw heartbeat, which is all the
evidence gathering needs" — is falsified by the same
truncation the spec itself works around for png chunks
(§8.2). Downstream casualties: the §8.3 soak evidence
protocol (heartbeat `himax_link` transitions across an
event), the §9 rollback trigger (keyed on `rec_*` counters),
and any post-hoc separation of D1's two reset branches.
The object *is* delivered intact to the console page over
MQTT; it is only the durable record that loses it.
*Minimal fix sketch:* emit the 8 fields additionally as a
compact standalone telemetry line (~120 chars, survives
truncation) — e.g. alongside the heartbeat in
`link_heartbeat_task`, or as flat fields ahead of
`stack_hwm`. Standalone line is the robust option.

### DESIGN GAPS

**G1 — Link health has no rate or duty-cycle dimension.**
Every signal the round added — streak, age, episode,
`link_up_`, heartbeat `state` — is an instantaneous sample
of a consecutive-failure count. A 3 Hz flap (0% bulk
availability, 6 h) is invisible to all of them (§2.1).
*Fix sketch:* add a leaky-bucket failure counter in the io
layer (failures in the trailing 60 s, or cumulative
episode-edge count) and give the supervisor an alternative
episode-open condition on rate (e.g. >30 gate failures/min
sustained 2 min), with `link_up_` and the heartbeat `state`
reading it. Pairs naturally with D1's fix but is not
substituted by it: even with honest streaks, a fault that
passes one full transaction per N failures would still
reset-and-hide without a rate view.

**G2 — The WI-1 summary line keys on the capped counter.**
`sscma_client_ops.c:246-252` fires only at streak ≥100 —
a value flap mode never reaches — so the designed
"heartbeat of evidence" for a long failure was absent for
the entire incident (0 firings). *Fix sketch:* key the
summary on cumulative failures (or the G1 rate counter)
with the existing per-100 bucketing.

**G3 — Remote `rail` intent is validated as exactly {0,1}
on neither side of the wire.**
Host: `send_command.py:119-121` (generic dbg branch)
imposes no key-set restriction and no `rail` check —
`{"rail":true}`, `{"rail":2}`, `{"rail":"1"}` all pass
`allowed()`. Firmware: `link.c:2349-2350` treats any
nonzero `opt_int` as a rail cycle, while the §10.2(c)
decision framed remote rail as requiring explicit
`"rail":1`. The bool case fails *safe* — cJSON booleans
are not numbers (`opt_int`, `link.c:1110-1117`), so
`"rail":true` silently becomes the remote default 0: the
operator's intent is inverted without a word on either
side. (The codebase knows this trap — the `logs` branch
excludes bools explicitly, `send_command.py:105-108`; the
wakeword branch's `in (0,1)` check at `:117` does *not* —
`True in (0,1)` is `True` in Python.) *Fix sketch:* a
dedicated eyes_reset branch in `allowed()` pinning keys ⊆
{t, cmd, rail} with rail a strict non-bool int in {0,1};
firmware-side, accept rail only when the value is the
number 1.

**G4 — Episode-close crediting can misattribute recovery,
and rung backoff leaks across episodes.** `ep_credit` is
set when a rung returns ESP_OK (`eyes.c:907, 963, 982`)
but booked at episode close, whenever the streak next
reads 0 (`eyes.c:842-869`) — including closes caused by a
manual `eyes_reset`, the relink worker, or self-healing.
Since `rec_*` feeds the §9 rollback trigger, the ladder
can be credited (or blamed) for recoveries it didn't make.
Separately, `last_rung_try_ms` is not reset at episode
open (only `rung_fail_streak` is, `eyes.c:880-888`), so a
fresh episode can inherit up to 60 s of a previous
episode's backoff. *Fix sketch:* snapshot `link_gen_` at
rung success and credit at close only if the generation
hasn't moved since; reset `last_rung_try_ms` at open.

### IMPROVEMENTS

**I1 — Two per-attempt warnings survived WI-1 ungated.**
`io_spi.c:783` (read gate-2: "read: Himax never staged
data") and `io_spi.c:881` (available gate-2: "available:
Himax never staged count") log on *every* occurrence. A
gate-2 storm floods at poll cadence exactly like the old
wedge. They didn't fire in this incident (failures were
gate-1), so this is latent. Gate them transition-only or
fold them into the edge logger.

**I2 — The wait-level transition flag diverges from the
gate streak.** `miso_wait_failing_` clears on *any*
successful wait (`io_spi.c:1253`), including gate-2 and
read waits inside an ongoing gate streak; soak window:
5 MISO-flag lines vs 436 gate-edge starts. Forensically
confusing; document the distinction or unify the flags.

**I3 — Stale text around eyes_reset.** The handler's own
comment (`link.c:2319-2321`) still reads "Serial console
only: this yanks the camera power rail, which a remote
caller must never do unattended" — contradicted by the
v0.4.50 policy paragraph immediately below it. And no
round document states the command's *wire form*: it has
been a dbg subcommand since v0.4.3 (`link.c:2316`), a fact
that cost a refused-then-"unknown command" round trip
during the live recovery (first send used the retired
top-level `{"t":"eyes_reset"}`). Correct the comment;
name the envelope in spec §10.2.

**I4 — `send_command.py` refusal text is stale.**
`send_command.py:176-179` lists only a subset of dbg
subcommands (omits eyes, eyes_reset, at, png, crop, strip,
wakeword, codec, loopback, rings). Generate it from
`DBG_ALLOWED` so it can't drift again.

**I5 — seal_client: poisoned-REPLAY and coverage gaps.**
An absurd REPLAY `<live>` (over the anti-oracle bound)
makes `auto_resync()` raise on *every* send until the
line rotates out of the 16 MB tail; `seal_command`
degrades gracefully (warn + send), but the heal is
poisoned for the window's lifetime. Untested paths in
`test_seal_client_resync.py`: that refusal interplay, the
>16 MB tail truncation boundary, the replay == advertised
tie, the `--auto-resync` CLI surface, and REPLAY lines
naming a different commander (the regex doesn't anchor
the `from <id>` suffix — currently impossible here: one
pinned key). *Fix sketch:* clamp the replay candidate at
the anti-oracle bound inside `auto_resync` (treat
over-cap as absent, warn once) instead of raising per
send; add the five tests.

**I6 — REPLAY heal can cost two drops under log flood.**
The 16 MB tail window rotates: during the incident
investigation the 22:31 REPLAY line was found *outside*
the window after the wedge flood. If the binding line has
rotated out before the next send, that send drops and
regenerates the line; the send after heals. Spec's "one
drop per desync" holds only within the window. Worth one
sentence in the spec/runbook; no code change implied.

**I7 — Dead exported API.** `sscma_client_gate_streak_reset()`
(`io_spi.c:503-506`, `sscma_client_io.h:204`) has no
callers anywhere in the tree. Remove it, or document why
it's kept — a future caller resetting the streak from
outside the io layer would silently re-arm D1.

**I8 — Task-create headroom got tighter and it showed.**
The soak recorded `re-link worker create failed`
(`eyes.c:411-417`) during the .62 boot re-link on
0.4.50 — the link recovered anyway via INIT@TRIGGER
(01:49:39), and causation is *not* established. But the
round added a permanent internal-RAM task (supervisor,
4096 stack, `eyes.c:645`) to a device whose heartbeats
show `internal_free` under 10 KB, and by 07:59
`internal_largest` had fallen to 1088 B. Worth a
headroom review before any further permanent tasks.

### VERIFIED SOUND (checked, no action)

- **WI-1 flood suppression for gate-1 failures** works as
  designed (soak §5: 5 MISO lines vs the 227,344-line
  historic baseline). Note the reduction is per-*episode*,
  not per-*failure* — at the flap's unbounded episode rate
  the pair still emitted ~345 lines/min and the device
  dropped 52,185 log lines over the boot (heartbeat
  `log_dropped`, 07:59:54). The discipline holds; the
  *rate* is G1's problem, not WI-1's.
- **WI-1 backoff** exact table, reset rule, and bulk-skip
  preservation (`sscma_client_ops.c:214-256`).
- **WI-2 hook placement** — worker exec paths only, never
  the queue wrappers (census in §1); bulk-internal sends
  flow through the same hooks as the spec intended.
- **WI-3 mechanics** — shared resync helper with unpark;
  resync as a queued worker txn (serialization per
  invariants 4/5); rung ≥3 bus gating with bounded
  deferral; rail cap counted per execution inside a
  pruned rolling hour; latch pacing; per-rung attempt and
  outcome lines; supervisor created once, failure-logged,
  present in `stack_hwm`.
- **§2.5 crop re-issue** — exactly-once, gen-checked, at
  all three specced sites; commands re-issued (set/clear)
  are idempotent. Telemetry shows **zero** firings, and
  the code explains why: the flap-triggering set returned
  `ok:true` (no timeout → no re-issue), and the flap-era
  clear timed out *without* a generation change (gen moves
  only on recovery). **The re-issue did not cause, worsen,
  or mask the flap.** Its one theoretical race — a set
  applied just before a session loss, reply lost, then
  re-issued into the new session — is the designed case
  and harmless for these idempotent commands.
- **Origin tagging** — both remote paths force both tags
  and strip host-supplied ones; `_sealed` tracks
  cryptographic verification exactly, including the grace
  exclusions on both transports. The only untagged entry
  to `handle_command` is the serial RX queue producer
  (`link.c:2847`) — the project's documented
  physical-access trust boundary, unchanged by this round.
- **eyes_reset device gate** — refusal, defaults, and
  `snap_busy` check as specced; live-proven at 07:54:41
  UTC (ok reply, re-handshake, flap ended; the final
  `gate failing` line in the log is timestamped 07:54:40).
- **Timer-probe removal** — zero remnants tree-wide.
- **seal_client patch** — faithful to its spec on every
  point (§1 table). The estimator asymmetry (floors =
  max-over-tail, replay = last-in-tail) was stress-tested
  on paper: because the device high-water is monotonic
  (NVS-persisted, boot floor = stored + stride, never
  decreases), REPLAY `<live>` values are non-decreasing in
  log order, so last-in-tail *is* the max for any
  same-epoch window. The misleading case requires an epoch
  break (NVS erase / re-provision / a second device on the
  same log): a stale high `<live>` then pushes the CLI
  counter forward — bounded by the 2^20 anti-oracle cap,
  costing counter space, never a desync (replay protection
  only rejects ctr ≤ high-water; overshoot is safe).
  Forward-only + cap do make the asymmetry harmless for
  correctness; I5 covers the residual noise case.

---

## 4. Recommended order of work (fix sketches only — nothing implemented)

1. **D1** (counting semantics) — the one change that makes
   the rest of the ladder real. Retest with the §4.6
   dead-Himax procedure *and* a flap-injection (the soak
   loop now produces one on demand).
2. **D3** (himax_link visibility) — before any further
   ladder tuning; without it, no fix can be evidenced.
3. **D2** (drain content sanity) — same io file, same
   bench pass as D1.
4. **G1/G2** (rate dimension + summary re-keying) — the
   visibility layer for the next morphology, whatever it
   is.
5. **G3, G4, I1–I8** — small, independent, land with the
   next build on each side.

*End of audit.*
