# Link hardening — build-ready implementation spec

**Date:** 2026-11-09. **Status:** spec only — no code changed,
nothing built or flashed. Awaiting owner approval per work item
group (§8 lists the open decisions).
**Sources:** `docs/link-wedge-rca-2026-11-09.md` (§§1–4 + the two
2026-11-09 addenda), `docs/link-wedge-regression-2026-11-09.md`.
**Scope:** the RCA §4 plan, in its order — S3 observability →
S3 link-health accounting → S3 escalation ladder → Himax
uncovered-state fixes → Himax stuck-unarmed watchdog + poll-gap
instrumentation → verification. RCA §5 (power-off) is **out of
scope**. H-F is addressed only as a decision (§8.4).

**The defect, in one paragraph.** The S3↔Himax SPI link's
handshake is one wire (MISO/PB2): LOW = Himax RX armed, HIGH =
everything else (response staged, parked, or Himax gone). Every
S3 command path hard-gates on LOW with a 50 ms fail-fast timeout
(`sscma_client_io_spi.c:820-823`), and the task that polls the
link counts none of the failures, so when the Himax's transport
reaches a state no re-arm guard covers, the S3 retries the gate
every ~51 ms forever — ~24k log lines in 21 minutes, no recovery,
device otherwise healthy. Twice on 2026-11-09 this state formed
≤179 ms after a *successful* AT+CROP exchange completed on the
wire; the stuck state forms at the final Guard-1 re-arm after
the CROP response's last clock-out (RCA Addendum 2). The fix is
two-sided: an S3 escalation ladder that recovers the link for
any Himax-side cause, and Himax changes that close the uncovered
re-arm states. The S3 ladder is the load-bearing half.

---

## 0. Verification notes — RCA claims checked against the tree

Every load-bearing RCA citation was re-checked against the
working tree on 2026-11-09. The RCA's code reading holds up
well. Discrepancies and refinements, in descending importance:

1. **`dbg eyes_reset` "serial-only" is not a device-side
   property of the bench's actual path (material).** The
   rejection at `link.c:2259-2262` fires only when the command
   object carries `_via_mqtt`. That tag is force-set in exactly
   one place: `link_sealed_dispatch` (`link.c:2507-2508`), the
   **native-MQTT** dispatch. The **bridged** path the bench uses
   — `sealed_fwd`, `link.c:1181-1261` — verifies the envelope,
   then dispatches the inner command by recursion
   (`handle_command(inner)`, `link.c:1259`) **without** setting
   `_via_mqtt`. At the device, then, an `eyes_reset` arriving
   over `sealed_fwd` is *not* rejected by the check the RCA
   cites; RCA §2.2's "policy-blocked during the incident" is
   true for native MQTT, and may have been true in the event
   via the console page's command registry, but it is not a
   device-side property of the bridged path. §8.2 reframes the
   decision accordingly: the question is not whether to *open*
   a path but whether to make an existing, inconsistent one
   explicit, uniform, and scope-gated.
2. **The AT write gate lives in the worker executor, not the
   vtable wrapper.** RCA S5 points at `client_io_spi_write`
   (~:425). In the tree, `client_io_spi_write` is the vtable
   wrapper at `sscma_client_io_spi.c:1707-1711`; it submits a
   queue txn, and the 50 ms LOW gate itself is in
   `spi_txn_write_exec` at `:431-434`. Substance unchanged —
   but all WI-2/WI-3 hooks must be placed in the worker-context
   exec functions, never in the wrappers.
3. **RCA §4.1's baseline loop under-tests the trigger it
   discovered.** Its sequence (crop set → png → strip set →
   png → clear) alternates geometries of *identical* byte count
   (2592×486 = 1296×972 = 1,259,712). With crop already set,
   `setCropWindow` takes its fast path (same byte count,
   `ma_camera_himax.cpp:425-435`), so half the loop's sets never
   run the full path both wedges followed. §6 of this spec
   corrects it: a crop **clear precedes every set**, forcing
   the full path every cycle.
4. **Recovery timings.** `sscma_client_reset` asserts RST for
   100 ms with a 200 ms settle after release
   (`sscma_client_ops.c:843-846`), not a bare "100 ms pulse";
   `eyes_recover_camera` adds a 300 ms rail drop + 100 ms
   settle on the rail rung (`eyes.c:661-671`), a 1 s
   post-reset wait and the re-handshake (`eyes.c:684-686`) —
   2–4 s per rung-≥3 recovery, as its own comment states
   (`eyes.c:648`). Ladder budgets in WI-3 use these numbers.
5. **Cosmetic drift.** `PIN_SSCMA_RST` is at `eyes.c:57`
   (RCA says :58). `PIN_HIMAX_RAIL` is at `eyes.c:62` as cited,
   wired at `:545`. Nothing else cited in the RCA was found
   stale; all other line numbers in this spec were re-derived
   from the tree.

There is no host-side unit-test harness for the `sscma_client`,
`eyes`, `link`, or Himax transport components in the tree. Every
"test" below is therefore a bench procedure with explicit
log/telemetry assertions, matching how this project verifies
firmware today.

---

## 1. Invariants (apply to every work item)

1. **The ladder never reboots the S3.** Highest rung is a Himax
   rail cycle. No rung 5 exists (RCA §2.3). Any design that
   reaches for `esp_restart` is a spec violation.
2. **Guard-1 re-arm timing contract (RCA §3-H-E; AGENTS.md).**
   Worst-case re-arm after a completed TX ≤ ~30 ms (current
   Guard 1: signal-driven arm at ≥2 ms after DMA-done with
   drain agreement, 25/30 ms fallback ceiling,
   `ma_transport_spi.cpp:1455-1488`), and re-arm is never gated
   on the master doing something first. Any change that reverts
   to a pure timer must honour the AGENTS.md floor:
   transfer_time + 1500 µs for small transfers, and the timeout
   must stay *shorter* than the S3's minimum inter-command
   time. WI-4/WI-5 changes must add no latency to the re-arm
   path itself (flag writes and post-arm readbacks only).
3. **`eyes_recover_camera` caller invariants** (`eyes.c:644-649`):
   the caller must guarantee no snapshot is in flight.
   `eyes_reset` refuses while `snap_busy_` (checked at
   `link.c:2264-2267`); the ladder must check the same
   conditions and **defer**, never preempt: `eyes_snap_busy()`
   (`eyes.c:697`), the client's `bulk_active`, and the io
   layer's `test_active`. Deferral is bounded in practice: the
   snap slot is stealable after 60 s (`eyes.c:137`).
4. **Bulk exclusion.** While `bulk_active` is set, the process
   task skips AVAILABLE polling (`sscma_client_ops.c:222-228`)
   and the bulk executor owns the io lock + SPI bus for the
   whole transfer (`sscma_client_io_spi.c:1387ff`), with its
   own drain + CMD_RESET + restart escalation
   (`BULK_MAX_ATTEMPTS` = 3, `:152`, retry loop `:1428-1440`).
   Ladder rungs 1–2 must never run concurrently with a bulk
   txn — the drain would clock out bulk data. Because all SPI
   work is serialized through the worker queue (WI-3), this is
   structural, not a locking problem to solve anew.
5. **Worker/process task topology.** All SPI transactions run
   on the dedicated SPI worker task (priority 15, core 0,
   `sscma_client_io_spi.c:241`, created `:367-378`) from a txn
   queue (types at `:215-218`). `sscma_client_reset`
   (`sscma_client_ops.c:831-870`) suspends the *process* task;
   it must not be called from the process task, nor from the
   SPI worker (the re-handshake submits worker txns —
   self-deadlock). Rungs ≥3 therefore run in the new supervisor
   task (WI-3), which is neither.
6. **Himax session state is lost at any reset rung.** Crop
   window and bulk state do not survive; `eyes_rehandshake_`
   (`eyes.c:299-326`) restores the sensor preset (opt 8,
   `:314`) only. Callers detect the loss via the link
   generation counter (WI-3) and re-issue.
7. **`link_up_` has one writer component.** Today it is written
   only inside `eyes.c` (init `:607`, relink worker `:357`,
   recovery `:658`/`:692`, the reboot-detect clear `:244`).
   WI-2/WI-3 preserve this: the io layer owns the failure
   *counter*; only eyes code writes `link_up_`.
8. **Flood discipline.** No new log line may be emitted per
   poll failure anywhere. New logging on the failure path is
   transition-only or rate-limited summary (WI-1). The
   incident's cost was ~24,300 lines through the telemetry
   uplink in 21 minutes.
9. **No wire-protocol change.** The ladder uses only existing
   transport commands (AVAILABLE/READ/WRITE/RESET) and the
   existing recovery primitive; S3 0.4.50 must interoperate
   with Himax 0.4.20.61, and Himax 0.4.20.62 with S3 0.4.49.1
   (§7). REARM/DIAG emit formats on the Himax are not changed
   (host tooling parses them; WI-5).

---

## 2. WI-1 — S3 observability: flood suppression, poll backoff, heartbeat field

*RCA §4 step 1 (S4). Files: `firmware/components/sscma_client/src/sscma_client_io_spi.c`,
`firmware/components/sscma_client/src/sscma_client_ops.c`,
`firmware/components/link/link.c`.*

### 2.1 Flood suppression (io_spi.c)

- The gate-failure error at `sscma_client_io_spi.c:821`
  (`spi_send_cmd: Himax not RX-armed …`) and the wait-level
  timeout warning at `:1031` (`spi_miso_wait_level_`) fire as a
  warn+error pair on **every** failed poll (~51 ms cadence in
  the incident). Change both to transition-only: log on the
  healthy→failing edge and on the failing→healthy edge, using
  a file-static last-state flag in the io layer. The failure
  itself is still *counted* (WI-2); only the logging changes.
- Add one rate-limited summary, emitted from the process task
  (never the worker): while a failure streak is ongoing, one
  line per 100 failed polls — `himax gate still failing,
  streak=<n>`. This preserves a heartbeat of evidence in the
  log without the flood.

### 2.2 Poll backoff (ops.c)

- The process task loop (`sscma_client_ops.c:207-230`) delays a
  fixed 1 ms per iteration (`:218`) and loops immediately on a
  failed `sscma_client_available` (`:230`). Add a
  consecutive-failure count local to the loop: after each
  failed available(), the iteration delay grows 1 → 10 → 50 →
  250 ms (cap); any successful available() (ESP_OK, regardless
  of rlen) resets it to 1 ms. Healthy-path AT latency is
  unchanged (the success cadence stays 1 ms); the 250 ms cap
  bounds post-recovery pickup delay, per RCA S4's risk note.
- The existing bulk skip (`:222-228`) is untouched — backoff
  applies only to actual available() failures.

### 2.3 Heartbeat field (link.c)

- `link_heartbeat_task` (`link.c:885`) builds the 60 s
  heartbeat with flat cJSON fields (`:893ff`) and one nested
  object precedent (`stack_hwm`, ~`:925-952`). Add a nested
  `himax_link` object now with two fields fed by §2.2's
  process-task count, exported through a small accessor in
  ops.c: `state` ("up"/"down" — down = streak > 0 at build
  time) and `streak`. WI-2 re-points these at the shared io
  counter and WI-3 extends the object (§8.3); shipping the
  field shape in WI-1 means the page/bridge see it from the
  first build.

### 2.4 Test

Bench, stock Himax .61, new S3 build only:
1. Baseline spitest 16/16 and one `dbg png` — log cadence and
   capture behaviour unchanged from 0.4.49.1.
2. Force gate failures (drop the Himax rail via the serial
   console's existing rail control and leave it down, as in
   RCA §4.1): assert the log shows exactly one failing-edge
   line, then summary lines only (≈1 per 100 polls), and the
   heartbeat carries `himax_link.state="down"` with a growing
   `streak`. Compare against the incident rate (~1,157
   lines/min): target ≤ a handful of lines/min.
3. Restore the rail: assert one healthy-edge line, backoff
   visibly resets (next AT command latency normal), heartbeat
   returns to `state="up"`, `streak=0`.

---

## 3. WI-2 — S3 link-health accounting: shared gate-failure counter, honest `link_up_`

*RCA §4 step 2 (S1 accounting + S2 + S5). Files:
`sscma_client_io_spi.c`, `sscma_client_ops.c`, `eyes.c`.*

### 3.1 The counter — placement and counting rules

One consecutive-transport-failure counter, owned by the io
layer (`sscma_client_io_spi.c`, file-static), with accessors
declared in the sscma_client component's internal header for
ops.c/eyes.c consumers: `sscma_client_gate_streak()` and
`sscma_client_gate_streak_reset()`.

All gated SPI work funnels through the worker task's exec
functions; the hooks go there (per §0 note 2):

| Event | Site | Effect |
|---|---|---|
| `spi_send_cmd_` LOW-gate timeout | `sscma_client_io_spi.c:820-823` | increment |
| AT write gate timeout in `spi_txn_write_exec` (does **not** pass through `spi_send_cmd_`) | `:431-434` | increment |
| AVAILABLE exec wait-HIGH timeout (command transmitted, count never staged) | `spi_txn_available_exec`, `:649ff` error return | increment |
| Any `spi_send_cmd_` success | `:813ff` return path | reset to 0 |
| Write-exec transmission success | `spi_txn_write_exec` post-gate path | reset to 0 |
| AVAILABLE exec full success (count clocked) | `spi_txn_available_exec` success return | reset to 0 |

Counting semantics, stated once and normative: **the streak
counts consecutive worker transactions that failed at a gate —
never transmitted, or transmitted but never answered at the
transport layer.** A Himax *error reply* at the AT layer is a
completed transport transaction and resets the streak (a
Himax that can refuse a command is alive). Bulk-internal
traffic flows through `spi_send_cmd_` too (via
`spi_send_cmd_noresp_`, `:880-883`), so gate failures inside a
bulk attempt increment — harmless, because WI-3 inhibits the
ladder while `bulk_active` (invariant 4), the bulk executor
runs its own escalation, and a completed bulk resets the
streak like any success. After a bulk *abort*, the streak
stands where the failures left it and the ladder may engage —
that is desired: a bulk abort on a dead link is exactly the
ladder's case.

This placement also discharges RCA S5 without new call-site
wiring: `eyes_set_sensor_opt`'s failures (`eyes.c:1089-1099`,
3 attempts with 300 ms delays) and `eyes_set_crop`'s
(`eyes.c:1442`) reach the io layer as write/available txns, so
their gate failures feed the same counter instead of dying in
a one-line warning. Their *retry behaviour does not change*
in this work item.

### 3.2 `link_up_` honesty (S2)

- When the streak crosses the WI-3 engagement threshold
  (§8.1), the WI-3 supervisor (eyes.c) sets `link_up_ = false`;
  successful recovery sets it true again (the existing writes
  at `eyes.c:692` and `:357` already do this on their paths).
  No other component writes the flag (invariant 7).
- Effect: `capture_png`'s existing link-down gate
  (`eyes.c:1982-1990`) starts working as designed — on a
  wedged link it attempts `eyes_recover_camera(false)` itself
  instead of marching into the 3-attempt `set_sensor` retry
  and failing. The supervisor and this gate can race to
  recover; both call the same primitive, and recovery is
  idempotent at this level (a second call on a healthy link
  costs one reset + handshake). The supervisor's
  recovery-in-progress flag (§4.3) is checked by neither
  legacy caller — acceptable, and noted so the implementer
  doesn't invent a lock the design doesn't have.
- Named risk (RCA §3-S2): the reboot-relink worker's
  condition (`eyes.c:240-254`, debounce `RELINK_DEBOUNCE_MS`
  = 10000 at `:129`) keys on `link_up_ && handshake_done_`.
  A supervisor-driven clear followed by the Himax's INIT@
  burst must not double-trigger relink while the ladder's own
  re-handshake is in flight. This is a required test case
  (§3.3), not a reason to change the relink logic in this
  work item.

### 3.3 Test

Bench, stock Himax .61:
1. Rail down (serial console): streak climbs in the heartbeat;
   at the threshold, `link_up_` flips — observable as a
   `dbg png` now taking the recovery branch first (log line
   `capture_png: link down, attempting recovery`,
   `eyes.c:1983`) instead of leading with `set_sensor`
   attempts.
2. Rail restored mid-streak (before threshold): streak resets
   on the next successful poll; no `link_up_` transition.
3. Relink non-double-trigger: with the link wedged by rail
   drop, restore the rail and let the *supervisor* recover;
   assert exactly one re-handshake sequence in the log and no
   relink-worker recovery on top of it (one `camera recovered`
   line, `eyes.c:690`).
4. AT-layer refusal is not a transport failure: issue a
   command the Himax answers with an error (e.g. an invalid
   crop geometry, which the Himax rejects with a code): streak
   stays 0.

---

## 4. WI-3 — S3 escalation ladder: drain, no-gate resync, RST rung, rail rung

*RCA §4 step 3 (§2.3 ladder + S3 + S1 execution). Files:
`firmware/components/eyes/eyes.c` (supervisor, generation
counter), `firmware/components/sscma_client/src/sscma_client_io_spi.c`
(drain variant, resync txn), `firmware/components/link/link.c`
(crop caller re-issue, §4.5).*

### 4.1 Execution context: a supervisor task in eyes.c

New task `eyes_link_health_task` in `eyes.c`, polling the
WI-2 streak every 100 ms. It is the only new task. It must not
be the SSCMA process task (reset suspends it,
`sscma_client_ops.c:831-870`), not the SPI worker (invariant
5), and not `sscma_monitor` (that task is the reply/event
dispatcher, `sscma_client_ops.c:155` — despite the name it
monitors nothing). Create it alongside the other eyes tasks;
size its stack from the heartbeat `stack_hwm` discipline
after first soak (§6), like every other task in this tree.

### 4.2 Rung 1 — drain with parser hand-off (S3)

`spi_drain_tx_` (`sscma_client_io_spi.c:781-811`) clocks
64-byte chunks until MISO falls LOW, **discarding** what it
clocks out (`rx_buffer = dummy`). Its only callers are inside
the bulk executor (`:1344`, `:1419`, 2000 ms bound), where
discarding is correct. On the AT path discarding is wrong: a
staged AT response clocked out by the drain *is* the reply.

- Implement the drain **inside `spi_txn_available_exec`'s
  gate-failure path**: on a `spi_send_cmd_` timeout there, if
  the WI-2 streak ≥ 3, make one bounded drain attempt
  (≤200 ms, not the bulk path's 2 s) while the exec still
  holds the io lock + bus (it took both at `:631-646`, which
  is the drain's documented caller contract, `:775-780`).
  Use a capturing variant of the drain loop that stores
  clocked bytes instead of discarding them.
- Parser hand-off is then free: if the drain captured bytes,
  return them through the exec's normal success path (they
  are, by protocol, the staged response to the poll — the
  process task parses them exactly as if the gated READ had
  produced them). If the line falls LOW with no bytes, return
  the original timeout; the process task's next poll re-proves
  the link on its own. The supervisor never retries a caller's
  command — that rule is what keeps the drain from ever
  duplicating a command.
- Deviation from RCA §3-S3's sketch, recorded deliberately:
  the RCA drains unconditionally on any gate timeout. Gating
  at streak ≥ 3 keeps single transient timeouts (a slow but
  legitimate Himax window) entirely on today's behaviour, at
  the cost of ≤2 extra failed polls (~100–600 ms) before the
  first drain. The bulk-path callers are untouched.

### 4.3 Rung 2 — no-gate CMD_RESET resync

- Extract the RESET half of `sscma_client_spi_unpark`
  (`sscma_client_io_spi.c:980-993`) into a shared helper: send
  `FEATURE_TRANSPORT_CMD_RESET` via `spi_send_cmd_no_gate_`
  (`:894`), wait 50 ms, then wait LOW up to 100 ms. (The
  Himax's RESET handler clears both rings and re-arms,
  `ma_transport_spi.cpp:979-988`.) Unpark keeps calling the
  same helper — one implementation of the resync, two
  callers.
- The supervisor triggers it as a dedicated worker txn (new
  txn type beside `:215-218`), so it inherits the worker's
  serialization against AT and bulk traffic (invariant 4).
  One attempt per ladder episode at this rung.

### 4.4 Rungs 3–4 — the existing recovery primitive, supervised

- Rung 3: `eyes_recover_camera(false)` (`eyes.c:650-695`) —
  RST pulse via `sscma_client_reset` + re-handshake.
- Rung 4: `eyes_recover_camera(true)` — rail cycle
  (300 ms drop) + RST + re-handshake.
- Immediately before either, the supervisor re-checks the
  invariants: `!eyes_snap_busy()`, `!bulk_active`,
  `!test_active`. If any is set, defer 500 ms and re-check
  (bounded per invariant 3). The supervisor holds a
  recovery-in-progress flag for its own sequencing; legacy
  callers of the primitive (invoke streak, `eyes.c:735-749`;
  `capture_png` gate) do not consult it (§3.2) — the flag
  exists so the *ladder* never stacks its own rungs.
- Episode state machine: an episode opens when the streak
  crosses the engagement threshold and closes on a successful
  poll (streak reset) or a successful rung. Rung order per
  episode: drain (inline, §4.2) → resync (§4.3) → rung 3 →
  rung 4. Between failed rungs ≥3, backoff 5 s → 15 s → 60 s
  (cap). After the rail cap (§8.1) the supervisor latches
  "link down" and retries rung 3 only, every 10 min — a
  permanently dead Himax gets a quiet, honest, non-spinning
  S3 (RCA §4.1's acceptance shape). Threshold values are the
  §8.1 decision; the mechanism takes them as named constants
  at the top of the ladder code.
- One log line per rung attempt and per rung outcome (name,
  streak at entry, result). This is the telemetry the §6
  soak's rung-by-rung evidence requirement reads.

### 4.5 Link generation counter + the crop caller

- New `link_gen_` (uint32, `eyes.c`), incremented on **every**
  successful `eyes_recover_camera` return and on the relink
  worker's successful re-handshake (`eyes.c:356-358`) — the
  uniform rule is "Himax session state was lost here".
  Accessor `eyes_link_gen()` beside `eyes_link_up()`
  (`eyes.c:618-624`). Also surfaced in the heartbeat (§8.3).
- Reference consumer: the `dbg crop` handler path. The crop
  callers (the link.c dbg handler and the strip logic at
  `eyes.c:1557`) capture `eyes_link_gen()` before
  `eyes_set_crop` (`eyes.c:1442`); on an `ESP_ERR_TIMEOUT`
  return with a changed generation, re-issue the set **once**.
  This is the exact incident shape: the crop set is the
  command most likely to be in flight when the link state is
  lost, and a silent one-shot retry converts the incident's
  user-visible failure into a non-event — without hiding it,
  since the recovery itself is logged and counted (§4.4).

### 4.6 Test

Bench, per RCA §4.1/§4.2 (adapted to what exists):
1. **Dead-Himax (owner's-rule acceptance).** Himax rail held
   down: ladder climbs to rung 4, exhausts the rail cap,
   latches; assert the S3 never reboots (heartbeat `uptime_s`
   monotonic across the whole test), stays responsive (sealed
   ping answered throughout), log volume stays at the WI-1
   rate, heartbeat reports the link down throughout.
2. **Restore.** Rail back: the latched rung-3 retry (or the
   next episode) recovers the link in-session — INIT@ burst,
   re-handshake, `link_up_` true, heartbeat `gen` incremented
   — still no S3 reboot.
3. **Crop-trigger drill (the natural injection).** Run the
   §6 soak loop on this build against stock Himax .61. Any
   wedge onset must be recovered by the ladder in-session
   (expected first success at rung 3; rungs 1–2 will fail
   against this end-state — that is itself evidence, and the
   rung log must show the full climb, not a skip).
4. **No false recovery.** 1 h of normal bench traffic
   (captures, spitest, PTT takes) with zero ladder activity
   beyond an occasional rung-1 drain — the
   `himax_link.rec_*` counters (§8.3) are the assertion.

---

## 5. WI-4 — Himax: close the uncovered state (H-A) + verify the arm (H-C)

*RCA §4 step 4. File:
`sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp`
(the only file).*

### 5.1 H-A(1) — `_spi_arm_rx` must never fail silently

At `ma_transport_spi.cpp:504-507`, `_spi_arm_rx()` returns
early when `!_spi_dev || !_spi_opened`, setting nothing. When
`!_spi_opened` (and `_spi_dev` is non-null), set
`_spi_rx_arm_failed = true` before returning, so Guard 3
engages. The `!_spi_dev` case stays silent (transport never
initialized; Guard 3 has nothing to escalate *to*).

### 5.2 H-A(2) — a failed `spi_open` in Guard 3's teardown is an arm failure, retried

Guard 3's teardown (`:1336-1355`): on ~10 counted arm
failures it clears the staged/done flags, closes, and
re-opens — `spi_close()` at `:1337`, `spi_open()` at
`:1339-1341`, `_spi_opened` set from the result. It then
**unconditionally clears `_spi_rx_arm_failed`** (`:1343`)
and calls `_spi_arm_rx()`, which — if the open failed —
silently no-ops per §5.1's bug. All of {staged, done,
arm_failed} are now false, `_spi_opened` false, RX unarmed,
PB2 released: the incident's end-state, with no guard able
to fire ever again.

Change: if `spi_open()` fails in the teardown, keep
`_spi_rx_arm_failed = true` (do not clear it), skip the arm
call, and count the failure on the same Guard-3 counter —
the next Guard-3 block entry retries open+arm on the
existing cadence. With H-A(1), add the companion rule in the
Guard-3 block head (`:1296ff`): when `!_spi_opened &&
_spi_dev`, an open attempt precedes the arm retry (at most
one per escalation cycle). Stated as an invariant for
review: **no reachable state has {`_spi_tx_staged`,
`_spi_tx_done`, `_spi_rx_arm_failed`} all false with RX
unarmed and `_spi_opened` false.**

### 5.3 H-A(3) — Guard 2's teardown needs no structural change (verify, don't edit blind)

Guard 2's teardown (`:1501-1511`: `spi_close()` at `:1503`,
`spi_open()` at `:1505-1507`, then `_spi_arm_rx()`) has the
same failed-open hole, but H-A(1) closes it by construction:
after a failed open, Guard 2's `_spi_arm_rx()` call now sets
`_spi_rx_arm_failed`, Guard 3 engages, and H-A(2)'s retry
owns the open. The spec asks the implementer to *trace this
chain in review and cover it in the §5.5 injection test*,
not to add a second mechanism.

### 5.4 H-C — verify the arm took (conditional sub-item)

In Guard 1's re-arm branch (`:1455-1488`), after
`_spi_arm_rx()`: read back the PB2 mux/GPIO13 state and, if
PB2 is not in GPIO13-LOW mode, set `_spi_rx_arm_failed` so
Guard 3 engages instead of the system believing it re-armed.
**Condition:** this requires a readback API in the HX
SCU/GPIO driver; none is used anywhere in the current app
tree (checked 2026-11-09). If the driver exposes one
(`hx_drv_scu` get-pinmux or GPIO get-output), implement as
sketched — the readback happens after the LOW drive, so it
adds no re-arm latency (invariant 2). If no readback exists,
drop H-C rather than inventing driver surface in this
change: H-A + H-B (WI-5) still close the state, and the S3
side observes the line directly — a didn't-take arm is
exactly what the WI-2 counter sees. Record which branch was
taken in the build notes.

### 5.5 Test

Himax builds use `make cleanbuild` with the build-hash
regeneration (AGENTS.md hygiene — the fingerprint must
differ from .61's).
1. Stock S3 0.4.49.1 + new Himax: spitest 16/16, baseline
   captures — no behaviour change on the healthy path.
2. **Injection (test hooks, §5.6):** force `spi_open` to fail
   once inside the next Guard-3 teardown. Pre-fix behaviour:
   permanent wedge (the incident). Required post-fix
   behaviour: Guard 3 retries the open, the link re-arms, no
   wedge. This is H-A's acceptance test.
3. Force the H-A(3) chain: fail the open inside a *Guard 2*
   teardown (abandoned-response injection: stage a response,
   never clock it, with the open-failure hook armed).
   Required: `_spi_rx_arm_failed` set by the follow-up arm,
   Guard 3 recovers the transport.
4. Crop subset: 20 full-path crop sets (clear before each,
   §6 method) against stock S3 — wedge incidence on the
   Himax side alone should collapse; any remaining onset is
   recovered only by bouncing (expected on the old S3) and
   is recorded, not hidden.

### 5.6 Test hooks (applies to WI-4 and WI-5)

The H1/H2 injection hooks RCA §4.2 calls for are added in
the Himax tree behind a compile-time switch
(`WATCHER_LINK_TEST_HOOKS`, default **off**), in the spirit
of the existing `CMD_TEST_PATTERN`. Release checklist item:
the shipped .62 binary is built with the switch off, and
the build fingerprint/banner check (§7) is done on that
binary.

---

## 6. WI-5 — Himax: stuck-unarmed watchdog (H-B) + poll-gap instrumentation (H-D)

*RCA §4 step 5. File: `ma_transport_spi.cpp` (same as WI-4;
WI-4 and WI-5 may land as one Himax build, but are reviewed
as separate changes).*

### 6.1 H-B — flag-independent stuck-unarmed watchdog

Beside Guard 2 in `spi_poll()` (after `:1517`, before the
bulk feeder). Track "no guard claims this state" time:
each poll, if `_spi_opened && !_spi_tx_staged &&
!_spi_tx_done && !_spi_rx_arm_failed && !_spi_bulk_armed`,
advance a continuous-unarmed timer (a static
`_spi_unclaimed_since_us`, 0 when the predicate is false);
if the predicate has held for **>2 s**, perform the
Guard-2-style teardown (close/open/arm, same code path —
factor the teardown into one helper used by Guard 2,
Guard 3, and H-B), bump `_spi_wd_aborts` (so REARM stats
count it) and emit a distinct DIAG line
(`E DIAG STUCK_UNARMED teardown`) so H-B firings are
separately countable. Rationale for the bound and gates
(RCA §3-H-B): 2 s is ~6× the worst legitimate unarmed
window (Guard 2's 300 ms); any *known* in-flight state
(staged, done, bulk) exempts it; `_spi_opened == false` is
H-A's domain and is excluded to avoid two owners retrying
the open. Note the predicate deliberately includes
`!_spi_rx_arm_failed`: an arm-failure sequence in progress
belongs to Guard 3.

### 6.2 H-D — spi_poll gap instrumentation (the H1/H2 discriminator)

At the head of `spi_poll()` (`:1266`): keep a static
last-call timestamp and a running max gap; when a single
gap exceeds ~500 ms, emit one DIAG line via the existing
deferred ring (`diag_nb_emit`, drained at the head of
`spi_poll` itself): `E DIAG POLL_GAP <gap_ms>`. No other
output changes: **do not extend the REARM stats line**
(`:300-330`) — host tooling parses its format (invariant 9).
In any future wedge this single number separates H1 (gaps
stay ~1 ms — the AT loop at `ma_server_at.cpp:100-119`
keeps calling `spi_poll()` with its 20 µs sleep) from H2
(one gap ≈ the wedge duration).

### 6.3 Test

1. **H-B injection:** a test hook (§5.6) that parks the
   transport in the unclaimed state — RX unarmed,
   `_spi_opened` true, all three flags false. Required:
   H-B fires within ~2 s + one poll cadence, teardown runs,
   RX re-arms, and the S3 (even stock 0.4.49.1) sees at most
   a ~2 s link hiccup instead of a wedge.
2. **H2 injection:** hook stalls the AT loop 60 s after the
   next command. Required: on recovery, a `POLL_GAP` DIAG
   ≈ 60 s appears in the Himax UART stream; on the S3 side
   the WI-3 ladder (0.4.50) recovers the link via rung 3
   during the stall — the two fixes compose, and the DIAG
   proves *why* the Himax was silent.
3. Healthy-path regression: spitest 16/16 + the §6 soak
   with zero `STUCK_UNARMED` and zero `POLL_GAP` lines —
   H-B/H-D must be invisible when nothing is wrong.

---

## 7. Versioning and release mechanics

| Side | From | To | Where the version lives |
|---|---|---|---|
| S3 | 0.4.49.1 | **0.4.50** | `FW_VERSION`, `firmware/main/main.c:35` |
| Himax | 0.4.20.61 | **0.4.20.62** | `WATCHER_FW_ID`, `library/sscma_micro/sscma/server/at/callback/watcher_build_hash.h:6` — regenerated by `gen_build_hash.sh` per build (AGENTS.md); bump at the generator's source input, never by hand-editing the generated header |

- **S3 0.4.50 contents:** WI-1 + WI-2 + WI-3, plus the
  timer-probe removal (below). A feature-bearing minor bump,
  per the project's one-version-one-artifact rule.
- **Timer probes — recommendation: fold the removal into
  0.4.50; do not ship a separate 0.4.49.2.** The probes are
  `timer_list_probe_` (`eyes.c:1949`) and its three call
  sites (`:1970`, `:2039`, `:2075`) — all inside
  `capture_png`, the same function whose link gate WI-2 makes
  honest (`:1982-1990`). Reasoning: (1) one regression pass
  over `capture_png` instead of two; (2) the removal is
  deletion-only diagnostics (log lines) — it cannot change
  capture behaviour, and a diff review can isolate it from
  the ladder changes; (3) a separate 0.4.49.2 would consume
  its own flash + soak cycle on a bench whose link is
  *currently wedged*, in service of removing log lines from
  a build that is about to be superseded anyway. The
  counter-argument — 0.4.49.1's soak should complete on the
  exact shipped artifact — is noted; that soak was already
  interrupted by the wedge investigation, and 0.4.50's §6
  soak is the stricter test. Owner confirms in §8.5.
- **Himax 0.4.20.62 contents:** WI-4 + WI-5 (test hooks off).
  Build with `make cleanbuild`; expect a fresh
  `WATCHER_BUILD_HASH` fingerprint — if it matches .61's,
  the build is stale (AGENTS.md).
- **Sequencing:** S3 first (RCA §4). 0.4.50 is soaked against
  stock Himax .61 (the ladder must recover the *known*
  defect unaided), then .62 lands, then the pair runs the
  full §6 acceptance. Release files follow the house rule:
  `watcher-v0.4.50-merged.bin` via `idf.py merge-bin` with an
  absolute output path, delivered with `sha256sum` + build
  fingerprint (AGENTS.md release naming).

---

## 8. Acceptance soak — 20-cycle crop regression (targeted, not generic)

*This is RCA §4 step 6, redesigned per Addendum 2 §A2.4.*

### 8.1 Control run (validates the test itself)

On the current pair (S3 0.4.49.1 + Himax 0.4.20.61), after a
device bounce clears the present wedge: run the §8.2 loop.
Expected: a wedge onset within the first few full-path sets
(observed rate 2/4) that does **not** recover — proving the
loop detects the defect. Stop at the first onset; recover by
the usual bounce. 3–5 cycles suffice; do not run all 20
against a build whose failure mode is already known.

### 8.2 The soak loop

Scripted from the host bridge with sealed commands, reusing
the live chunk collector built during the incident session
(`~/workspace/bench-captures/collect_png.py` — telemetry.log
truncates payloads, so collection is live, per-chunk decode
then concatenate). One cycle:

1. `dbg crop clear` (forces the next set down the full path —
   `clearCropWindow` zeroes `_crop_w`,
   `ma_camera_himax.cpp:502`)
2. `dbg crop` set — alternating geometry: odd cycles strip
   `0,0,2592,486`, even cycles center `648,486,1296,972`
3. `dbg png` — reassemble and assert the exact dimensions
   (2592×486 or 1296×972) and the reply's `ok:true`
4. next cycle

20 cycles = 20 full-path sets + 20 captures. **The clear in
step 1 is load-bearing**: without it, identical byte counts
make every second set a fast-path recrop
(`ma_camera_himax.cpp:425-435`) and the soak silently stops
testing the trigger.

### 8.3 Pass criteria and the mid-soak wedge protocol

- **Pass:** 20/20 cycles complete; every PNG `ok:true` at
  exact dimensions; zero manual interventions (no console
  reconnect, no power cycle, no serial-console rescue).
- **If a wedge onsets mid-soak:** do **not** intervene. The
  ladder must recover in-session; when it does, retry the
  interrupted cycle once and continue. Record the
  rung-by-rung evidence from telemetry: the WI-1 edge lines,
  the drain outcome, the resync outcome, the
  `eyes_recover_camera` result line (`camera recovered
  (rail_cycled=…)`, `eyes.c:690`), the `link_gen_` bump, and
  the heartbeat `himax_link` transitions across the event.
  A soak that finishes 20/20 **with** one ladder recovery is
  a pass with evidence — the fix demonstrated, not merely
  untriggered. A wedge the ladder fails to recover is a
  **fail**, full stop: preserve the log window and, if the
  Himax UART is attached, its DIAG stream (the H1/H2
  discriminator, §6.2).
- **Runs required:** (a) 0.4.50 + stock .61 (ladder-only
  proof — recoveries expected and welcome); (b) 0.4.50 +
  0.4.20.62 (the pair — onsets should now be rare; a clean
  run plus the §5.5/§6.3 injection results carry the Himax
  side).
- **Companions** (RCA §4.4): spitest 16/16, snaptest
  byte-exact, the standing 20-frame soak, §4.6's dead-Himax
  and no-false-recovery tests, and one full PTT take +
  streaming-audio sanity pass (0.4.49's feature set must not
  regress under 0.4.50).

---

## 9. Rollback and recovery notes

**S3 (0.4.50).** Flashing is page-mediated: the console
Flash tab hash-verifies the merged bin and a human clicks
flash; rollback is the same path with the 0.4.49.1 artifact
(keep its merged bin, sha256, and boot fingerprint from the
2026-11-09 delivery alongside the 0.4.50 delivery).
0.4.50's changes are additive recovery behaviour on top of
0.4.49.1's paths; the defined rollback trigger is
**false-positive recovery**: `himax_link.rec_*` climbing
without a corresponding wedge (threshold for discussion:
>2 unexplained recoveries in a soak hour, or any recovery
that interrupts a bulk capture — invariant 4 says that
cannot happen, so one occurrence is a stop-and-investigate
event by itself). Absent that, a 0.4.50 problem looks like
0.4.49.1 behaviour and rollback is unhurried.

**Himax (0.4.20.62).** Himax flashes are the owner's run
(his tooling and runbook; Muse does not flash the Himax).
Rollback is a reflash of the .61 image (sha256
`f1e5a0c3…` in the project record). A .62 regression
presents early — spitest or capture failures in the §5.5
companions, before the soak proper — because WI-4/WI-5
touch only the transport's re-arm/teardown paths. Verify
the post-flash DIAG banner prints the .62 `WATCHER_FW_ID`
and a fresh build hash before running anything else.

**Pairing.** No wire change (invariant 9): any combination
of {0.4.49.1, 0.4.50} × {.61, .62} interoperates. .62 on the
old S3 strictly reduces wedge incidence but cannot recover
a residual wedge in-session; 0.4.50 on .61 recovers wedges
but at the old incidence. The pair is the fix; neither half
alone is.

---

## 10. Open decisions for the owner

### 10.1 Ladder thresholds and caps

*Decision:* the numbers WI-3's constants take.
**Recommendation:**
- Drain engages at streak ≥ 3 (inline, §4.2) — below that,
  today's behaviour is untouched.
- Rung 2 (CMD_RESET resync) at streak ≥ 25; rung 3
  (`eyes_recover_camera(false)`) at streak ≥ 50; rung 4
  (rail) only after rung 3 has failed *in this episode*, or
  immediately at streak ≥ 50 if the previous episode ended
  in a recovery <10 min ago (a link that just relapsed
  doesn't get the benefit of the doubt twice).
- A wall-clock floor of ≥1 s of continuous failure before
  rung 2 and above. Reasoning: the Himax's own worst
  legitimate self-recovery window is Guard 2's 300 ms
  teardown + Guard 1's 30 ms ceiling; 1 s gives it >3×
  margin to win the race, so the ladder only ever acts on
  states the Himax has already failed to fix itself. At the
  incident's ~51 ms poll cadence, streak 25 ≈ 1.3 s (the
  backoff of WI-1 stretches this to a few seconds — the
  count and the floor are deliberately redundant).
- Rail-cycle cap: **3 per rolling hour**; on cap, latch
  "link down" with rung-3-only retries every 10 min (§4.4).
  Reasoning: three rail cycles distinguish "Himax hung"
  from "Himax dead" with room to spare; beyond that, cycles
  only mask possibly-dying hardware and wear the power path
  — and every cycle is heartbeat-visible (§10.3) so the cap
  can be revisited from data.

### 10.2 `dbg eyes_reset` over remote paths

*Decision:* the status of the manual recovery command for
MQTT-origin callers. Per §0 note 1, the current state is
inconsistent: native-MQTT callers are rejected at the device
(`link.c:2259-2262`), bridged `sealed_fwd` callers are not
(the inner dispatch is untagged, `link.c:1259`) — the RCA's
"serial-only by design" describes intent, not the bridged
reality.
**Recommendation:** make it explicit and uniform —
(a) force-tag origin on *both* remote dispatch paths (the
bridged recursion tags the inner command's origin the same
way `link_sealed_dispatch` does, `:2507-2508`), so policy
cannot differ by transport again; (b) then **allow**
`eyes_reset` on remote paths under the device's existing
control model — sealed envelope, verify-time scope in the
highest tier (the tier that already covers `dbg at` and
flash; never in a least-privilege HID/cursor key's scope);
(c) for remote callers the `rail` option **defaults to 0**
(warm RST only): a rail cycle from remote requires an
explicit `"rail":1` in the same high-scope command, while
the serial console keeps its current default of 1.
Rationale: the ladder (WI-3) is the unattended path and
makes this mostly moot — but during the incident, an
attended remote `eyes_reset` would have cut a 21-minute
outage to seconds, and the device's ruled model is that it
checks authenticity + scope, never wisdom. The agent-side
rule (ask the owner before dangerous device commands)
still applies on top, unchanged.

### 10.3 Heartbeat `himax_link` field shape

*Decision:* the shape WI-1 starts and WI-3 completes.
**Recommendation:** one nested object (precedent:
`stack_hwm` in the same heartbeat, `link.c:~925-952`) —
flat fields would collide with the heartbeat's existing
names and scatter link state across the object. Fields:
`state` ("up" | "down" | "recovering"), `streak`
(current consecutive gate failures), `gen` (link
generation, §4.5), `rec_drain`, `rec_reset`, `rec_rst`,
`rec_rail` (successful recoveries per rung, since boot),
`last_rung` ("drain" | "reset" | "rst" | "rail" | "none").
Boot is the epoch for the counters (stated here so nobody
reads them as lifetime accounting). Console *rendering* of
the new object is out of scope — console changes are
batched separately under the console discipline; the bridge
already logs the raw heartbeat, which is all the §6/§8
evidence gathering needs.

### 10.4 H-F (camera re-init observability)

*Decision:* whether to add DIAG boundary markers inside
`setCropWindow` (`ma_camera_himax.cpp:400-497`).
**Recommendation: not now.** RCA marks H-F
evidence-dependent; Addendum 2 narrowed H2's locus but did
not produce evidence *for* it. WI-5's H-D poll-gap counter
is the cheaper, decisive instrument and ships in the same
build. Revisit H-F only if .62's field data shows poll gaps
during an anomaly — at which point the markers can be aimed
at the measured stall instead of the guessed one.

### 10.5 Timer-probe removal: fold into 0.4.50?

*Decision:* §7's recommendation — fold the
`timer_list_probe_` removal into 0.4.50 rather than shipping
a separate 0.4.49.2. **Recommendation: fold** (reasoning in
§7). The alternative remains defensible if the owner wants
0.4.49.1's soak to complete on its exact shipped artifact
first; it costs one extra flash cycle on a wedged bench.

---

*End of spec. Implementation begins only on the owner's go,
per work-item group: S3 (WI-1…WI-3, one 0.4.50 build) and
Himax (WI-4…WI-5, one 0.4.20.62 build) are separately
approvable, per the sequencing in §7.*
