# Link-hardening audit — Himax half (fw 0.4.20.62)

**Date:** 2026-11-09/10. **Type:** read-only audit. No code was
changed, no device command was sent, nothing was flashed.
**Scope:** WI-4 (H-A, H-C) and WI-5 (H-B, H-D) as built in
`sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp`,
plus the round's Himax invariants, build hygiene, and test-hook
state. The S3 half (0.4.50) is out of scope except where its
behaviour is evidence.

**Method.** The .62 change set was established mechanically, not
from the build report: unified diff of the working tree against
the clean-copy pin of the .61 base (commit `9e34913`) — 423 diff
lines, one source file — and a whole-tree `diff -rq` of
`sscma-fork/` between the pin and the working tree. Every claim
below was then verified by reading the as-built source; line
numbers cite the working-tree file above ("MT" below) unless
another file is named. Live evidence is from
`host-agent/mqtt-bridge/telemetry.log` and
`docs/link-hardening-soak-2026-11-09.md`.

**Identity check.** The audited source is what is running: the
working tree's generated `watcher_build_hash.h` carries
`WATCHER_FW_ID "watcher-himax-0.4.20.62-spi"` and
`WATCHER_BUILD_HASH "fc0bac1be88797e1…"`, and the device's boot
DIAG lines in telemetry (2026-11-10 01:45:14 and 01:49:39 UTC)
print exactly `watcher-himax-0.4.20.62-spi fc0bac1be88797e1`.

---

## 1. Per-item verification table

| Item | Specced (spec §) | As built (evidence) | Verdict |
|---|---|---|---|
| H-A(1) `_spi_arm_rx` | Never fail silently: `!_spi_opened` (dev live) sets `_spi_rx_arm_failed`; `!_spi_dev` stays silent (§5.1) | MT:532-568 — dev check silent (533-535); opened check sets flag (536-545); DMA failure sets flag (553-555); success clears flag, sets tracker, muxes PB2→GPIO13 (564), drives LOW (566). All four return paths audited | **Sound** |
| H-A(1) parked arm (build deviation 3) | §5.2 invariant must hold for both arm entry points | MT:575-605 — identical split in `_spi_arm_rx_parked`; DMA success sets tracker (596), muxes PB2→SPI_S_DO (parked release) | **Sound** |
| H-A(2) teardown flag retention | Failed `spi_open` in Guard-3 teardown = arm failure; flag stays set, arm skipped (§5.2) | MT:1543-1565 — `_spi_transport_cycle()` result gates the flag clear; on failure the flag stays set and no arm is attempted (comment at 1560-1565 states the pre-.62 defect it fixes) | **Sound** |
| H-A(2) block-head open retry | Open attempt precedes the arm retry when `!_spi_opened && _spi_dev`, "at most one per escalation cycle" (§5.2) | MT:1517-1519 — `_spi_try_open()` runs at **every** Guard-3 block entry while closed (≈1 kHz poll cadence), not once per ~10-count teardown cycle | **Sound under the build's reading; spec wording ambiguous** — see F-5 |
| Build deviation 1 (poll head relaxed) | Head returns only when `!_spi_opened && !_spi_rx_arm_failed` so Guard 3 is reachable while closed | MT:1466. Block-by-block audit in §2.1: Guard 3 is the only block that acts materially while closed; all other blocks are predicate-inert or ring-only in that state | **Sound, with two bounded interactions noted** (§2.1) |
| H-A(3) Guard-2 chain (verify-only) | Failed open in Guard 2's teardown → follow-up arm sets flag via H-A(1) → Guard 3 owns it (§5.3) | Traced: Guard 2 (MT:1711-1735) cycles via the shared helper, then calls `_spi_arm_rx()` unconditionally (1729); a still-closed transport sets the flag at 543; Guard 3 engages next poll | **Sound** |
| H-C arm readback | Guard 1 only: after `_spi_arm_rx()`, PB2 mux = GPIO13 and pad LOW, else set arm-failed; abstain on API error (§5.4) | MT:1404-1422 (`_spi_verify_arm_took`), sole call site MT:1695 in Guard 1, skipped when the arm DMA-failed. Signal-path analysis in §2.2 | **As specced — but the specced coverage is the gap** — see F-2 |
| H-B stuck-unarmed watchdog | Predicate over flags, >2 s, shared teardown, `_spi_wd_aborts++`, `E DIAG STUCK_UNARMED teardown`, gated off during bulk (§6.1) | MT:1745-1771 — predicate is the spec's list **plus `!_spi_rx_armed`** (deviation 2), 2,000,000 µs bound (1750), emit at 1753, shared cycle + park-aware arm. Tracker site audit in §2.3 | **As built per deviation — but blind to the flap class** — see F-1 |
| H-D poll-gap instrumentation | Last-call timestamp + running max at poll head; `E DIAG POLL_GAP <ms>` via deferred ring on a gap >~500 ms; REARM format untouched (§6.2) | MT:1431-1450 — inter-call gap, 500,000 µs threshold, `diag_nb_emit` at 1445, gated to "live" states (opened ‖ arm-failed). `_poll_max_gap_us` is updated (1443) but **never emitted or reset anywhere** | **Mostly sound; max-gap half is dead instrumentation** — see F-4 |
| REARM stats format | Not extended (invariant 9) | `_rearm_stats_emit` (MT:335-353) is byte-identical to the .61 pin — the .62 diff has no hunk in it | **Sound** |
| Invariant 2 / TX-done timing | Worst-case re-arm ≤ ~30 ms, never gated on the master; no added latency on the re-arm path (spec §1.2) | Guard-1 timing untouched: signal-driven arm, ≥2 ms floor (MT:1682), 25/30 ms fallback ceiling (MT:1673). H-C's two driver reads run **after** `_spi_arm_rx()` returns (1688→1695), i.e. after PB2 is already driven LOW (564-566) — zero added latency on the S3-visible transition | **Sound** — with a documentation-drift note (§2.4) |
| Build hygiene | Every touched file must move the fingerprint | Whole-tree diff: exactly two files differ from the pin — MT and the generated `watcher_build_hash.h`. `gen_build_hash.sh` KEY_SOURCES = `ma_transport_spi.cpp`, `ma_transport_spi.h`, `ma_camera_himax.cpp` — the touched source is the first entry | **Sound** |
| Test hooks (§5.6) | Cmd 0x0A behind `WATCHER_LINK_TEST_HOOKS`, default OFF, unreachable in release | `#define WATCHER_LINK_TEST_HOOKS 0` (MT:67-69); every hook reference sits inside `#if WATCHER_LINK_TEST_HOOKS` (70-77, 760-765, 1120-1151, 1362-1367, 1450-1461). In release, cmd 0x0A reaches `default:` → `_spi_arm_rx()` (MT:1148-1150) | **Sound** |
| Version mechanics | .62 string bumped at the generator's source input (§7) | `gen_build_hash.sh` extracts `WATCHER_FW_ID` *from* the header it regenerates, so the header is both the generated output and the FW ID's only source; the one-line string edit there (disclosed in build report §4) is the only workable reading of the spec's rule | **Sound; spec wording nit, not a violation** |

---

## 2. Item audits in detail

### 2.1 Deviation 1 — the relaxed `spi_poll` head (MT:1466)

Claim under audit: with the head returning only when
`!_spi_opened && !_spi_rx_arm_failed`, the rest of the poll body
is inert in the closed+flagged state except Guard 3. Verified
block by block, in poll order:

- **H-D block (1431-1450):** timestamp arithmetic only; "live"
  includes closed+flagged by design, so gaps stay measured in
  the H1 end-state. Intended.
- **Deferred DIAG prints + `diag_nb_drain()` (1468-1504, 263):**
  UART-side only; drain is additionally gated on
  `!_spi_tx_staged && !_spi_bulk_armed`. Harmless while closed —
  arguably beneficial (diagnostics stay visible in the wedge
  state).
- **Guard 3 (1505-1581):** the intended actor. ✓
- **Guard 1 (1660-1715):** requires `_spi_tx_done`. Every
  teardown path clears `_spi_tx_done` *before* cycling (Guard 3
  at 1543-1547, Guard 2 at 1719-1722), and the only
  setter is `_spi_tx_callback`. Residual entry: a TX-DMA
  completion interrupt racing a `spi_close` could set the flag
  after the clear, leaving `_spi_tx_done == true` while closed.
  Consequence is bounded: probe sampling calls `spi_control` on
  a closed device (driver errors would one-shot-kill the re-arm
  probes — they abstain thereafter, degrading Guard 1 to its
  timer ceiling), and after 25/30 ms `_spi_arm_rx()` runs into
  H-A(1), which sets the flag Guard 3 already owns. Self-healing;
  no wrong hardware state driven. Not observed in the field.
- **Guard 2 (1711-1735):** requires `_spi_tx_staged`, cleared
  before every cycle call (same sites as above) and set only by
  command handlers, which require an armed RX, hence an open
  transport. Inert while closed. ✓
- **H-B (1745-1771):** requires `_spi_opened`. Inert. ✓
- **Bulk feeder + its 1 s watchdog (1775-1798):** can run while
  closed if `_spi_bulk_armed` survived (no teardown clears it).
  Its actions in that state are a ring `push()` and, after 1 s of
  no READ_CRC, self-disarm plus a stats emit — no SPI hardware
  calls. The bulk *exclusion* the audit was asked to cross-check
  is intact where it matters: every hardware-touching recovery
  path (Guard 2, Guard 3 teardown, H-B) is either flag-gated or
  the intended Guard 3, and H-B's predicate carries the explicit
  `!_spi_bulk_armed` term. ✓

One further reachability note: `SPI::deInit()` returns early
when `!_spi_opened` (MT:1272) and never clears
`_spi_rx_arm_failed`. In the failed-open state a deInit call is
therefore a no-op and, if the AT loop kept polling, Guard 3
would keep retrying opens after the transport was asked to shut
down. Pre-.62 code, reachable in practice only at device
teardown; recorded as F-6 (minor).

### 2.2 H-C — what the readback actually measures (MT:1404-1422, call site 1695)

Signal path, from the code:

- **The net.** One wire: Himax PB2 ↔ S3 GPIO6 (RCA §1.1). Two
  possible drivers on the Himax end, selected by the SCU pinmux
  for PB2: `SCU_PB2_PINMUX_GPIO13` (PB2 connected to GPIO13's
  output) or `SCU_PB2_PINMUX_SPI_S_DO` (PB2 connected to the DW
  SPI slave's DO, which tristates while deselected → the S3's
  pull-up takes the net HIGH). GPIO13 is
  `GPIO_GROUP_1 << 4 | SUB_GPIO5` (`drivers/inc/hx_drv_gpio.h:195`),
  with a designed `GPIO13_PIN_MUX_PB2_F2` iomux mapping.
- **What the arm does.** `_spi_arm_rx()`: `spi_read_dma` first;
  on success it programs the mux to GPIO13 (MT:564) and writes
  the GPIO13 output latch LOW (MT:566). The latch is never
  written HIGH anywhere in the tree (grep: no
  `GPIO13, GPIO_OUT_HIGH` in `library/` or the sscma app), and
  init pre-loads it LOW (MT:1196 region), so the mux switch
  itself is the LOW transition.
- **What H-C reads.** (1) `hx_drv_scu_get_all_pinmux_cfg()` →
  `cfg.pin_pb2` (struct field at
  `drivers/inc/hx_drv_scu_export.h:2408`): the SCU's pinmux
  *configuration register* read back — proof of what the SCU was
  told, at the Himax end. (2) `hx_drv_gpio_get_in_value(GPIO13)`:
  documented in `drivers/inc/hx_drv_gpio.h:368-377` as "gets the
  input value of a specified GPIO pin" (usage note at :38-40:
  "get the external voltage level") — i.e. by API contract the
  pad's sensed level through GPIO13's input path, at the Himax
  end of the net, at one instant a few µs after the arm.
- **What it does not measure.** Anything at the S3 end (the S3
  samples the same net on GPIO6 at its own instants, including
  during staged phases when HIGH is correct); the net's state
  at any time after the verify instant (a staging handler can
  legitimately re-mux PB2 to SPI_S_DO µs later — that is the
  protocol, not a fault); and, because the GPIO driver is a
  prebuilt library (`prebuilt_libs/*/libdriver.a`, no source in
  tree), whether the input path of an *output-configured* pin
  returns the true pad level in all cases. Field evidence is
  consistent with it working: across the soak's healthy traffic
  H-C produced no false failures (a false positive sets
  `_spi_rx_arm_failed` → Guard-3 retries → teardown at 10 →
  `_spi_wd_aborts` climbs; the cumulative `wd=` counter stayed 0
  — §3).
- **Second-writer check.** Within the shipped application
  (`library/` + `app/scenario_app/sscma/`), the transport file
  is the *only* writer of the PB2 mux and of GPIO13's output.
  The other `set_PB2_pinmux` hits in the tree are unbuilt
  example scenario apps using the SPI-*master* DO mux for a
  different function. Mux contention from another in-app writer
  is refuted from source.
- **Abstention.** If the SCU read errors, the function returns
  before the mux check; if the GPIO read errors, after it. A
  *persistent* API error therefore disables H-C (wholly or
  partly) for the life of the boot with **no diagnostic of any
  kind** — no emit, no counter. The codebase's own convention
  elsewhere is a one-shot death note (the re-arm probes emit
  `E REARM probe … dead rc=…` once per dead probe,
  MT:366-434). H-C does not follow it. See F-3.

### 2.3 H-B and deviation 2 — the `_spi_rx_armed` tracker audit

Every set/clear site (grep-complete over MT; `spi_read_dma` has
exactly two call sites in the whole `library/` tree, both arm
functions):

| Site | Action |
|---|---|
| MT:561 (`_spi_arm_rx` DMA success) | set |
| MT:596 (`_spi_arm_rx_parked` DMA success) | set |
| MT:645 (`_spi_rx_callback` entry) | clear — RX DMA completion is what disarms RX |
| MT:1386 (`_spi_transport_cycle`) | clear — the close kills any outstanding DMA |
| MT:1300 (`SPI::deInit`) | clear (hygiene) |

Divergence hunt, as tasked — *tracker says ARMED while no RX
transaction is actually armed*:

- **DMA half: no divergence found.** Both `spi_read_dma`
  callers set the tracker on success and only on success; the
  only hardware events that end an RX DMA are its completion
  (callback, clears at 645) and `spi_close`, whose only callers
  are `_spi_transport_cycle` (clears at 1386, before closing)
  and `deInit` (clears at 1300). No code path arms RX DMA
  without setting the tracker, and none kills it without
  clearing. TX staging does not disturb the tracker, correctly:
  handlers run inside the RX callback, which has already
  cleared it at entry.
- **Protocol half: the divergence is real, and it is the
  finding.** "Armed" on this link is a conjunction: RX DMA
  outstanding **and** PB2 muxed to GPIO13 driven LOW. The
  tracker records only the first conjunct. `_spi_arm_rx()`
  performs the two halves back-to-back (DMA at 553, mux at 564,
  drive at 566) with the mux/drive return values unchecked; if
  the second half is lost while the first succeeds, the tracker
  reads ARMED, `_spi_rx_arm_failed` reads false, and — unless
  the arm happened to be Guard 1's (the only H-C-verified one)
  — no Himax mechanism ever looks at the line again. H-B is
  then permanently exempted by its own `!_spi_rx_armed` term.
  This is F-2's mechanism stated from H-B's side.

**Was H-B capable in principle of seeing the flap? No.**
During the flap the tracker was true essentially throughout
(RX DMA armed between transactions; the callback-to-rearm gaps
are µs–ms), and the claim flags cycled normally. H-B's
predicate — tracker false, continuously, >2 s, with nothing
staged or done — never came close to holding. The signal H-B
would need is a *line-state* observable, not a DMA-state one:
either a sustained pad-HIGH-while-claiming-armed measurement
(the pad is readable — H-C proves the API path), or a
staged-age measurement that the F3 parse-retire cannot reset
(§3, FH-2). Sketches in §4 (S-1, S-2); nothing implemented.

### 2.4 Invariant 2 and the "transfer_time + 1500 µs" question

The audit brief asks to confirm the TX-done timeout is still
transfer_time+1500 µs. **It is not in the code, and has not
been since 0.4.20.59** — the string `1500` survives in MT only
inside the historical comment block (MT:1613-1630) narrating the
timer era. The shipped mechanism (unchanged by .62) is
signal-driven Guard 1: drain-signal agreement plus a 2 ms floor
(MT:1682), with a 25/30 ms fallback ceiling (MT:1673). RCA
§3-H-E already records that the signal-driven form honours the
AGENTS.md rule's intent "by construction"; the spec's
invariant 2 restates the *contract* (≤ ~30 ms worst case, never
gated on the master) rather than the retired formula. Verdict:
**.62 preserves the contract; the formula wording in AGENTS.md
is documentation drift predating this round, not a .62
regression.** Within .62's own changes, the only addition near
the re-arm path is H-C, placed after the arm (see table), so
the round adds no re-arm latency.

### 2.5 Guard 3 cadence note (supports F-5)

The Guard-3 block runs on every `spi_poll()` while the flag is
set (~1 ms cadence from the AT loop,
`library/sscma_micro/sscma/server/at/ma_server_at.cpp:113-114`).
As built, each entry performs the arm retry and, while closed,
one `_spi_try_open()`. The ~10-count escalation
(MT:1543-1545) therefore spans ~10 ms, not a slow cadence, and a
persistently failing `spi_open` is retried at ~1 kHz at block
head plus a full close/open cycle at ~100 Hz. That is the
pre-.62 Guard-3 shape (counter-based since .52) extended to the
closed state by H-A(2); it is bounded, thread-context, and
self-terminating on success — but it is more aggressive than
spec §5.2's "at most one [open] per escalation cycle" under the
teardown-cycle reading of "cycle".

---

## 3. What state was the Himax in during the flap — what the code permits

Facts are labelled **[code]** (provable from the as-built source
plus the telemetry record) or **[hypothesis]** (code-consistent,
not provable from the Himax side alone).

### 3.1 Code-proven facts

1. **[code] No Himax teardown fired during the flap — or at any
   time after the 01:49 UTC reboot.** `_spi_wd_aborts` is
   incremented by Guard 2 (MT:1718), by Guard 3's teardown
   (MT:1542), and by H-B (MT:1752), and is reset *nowhere* (the
   stats emit at MT:335-353 resets only `_rearm_stats`). Every
   REARM line sampled during the storm reads `wd=0`. Therefore
   Guard 2 never timed out a stage, Guard 3 never escalated,
   and H-B never fired, for the storm's whole ~6 h.
2. **[code] The AT loop never stalled.** H-D emits on any
   inter-`spi_poll` gap >500 ms (MT:1444-1446); the loop is
   `spi_poll()` + 20 µs sleep (`ma_server_at.cpp:113-114`).
   `POLL_GAP` appears **zero** times in the entire telemetry
   log (grep count 0), and `STUCK_UNARMED` likewise (0). The
   H2-form mechanism (AT-thread stall) is refuted for the flap,
   exactly as H-D was built to decide.
3. **[code] Guard 1 re-arms ran continuously and healthily.**
   Sampled REARM windows during the storm: `n=32 ab=0 sig=32
   fb=0 … wd=0` — every re-arm signal-driven (drain-agreed),
   none by the fallback timer, none abandoned. The Himax was
   completing TX→re-arm cycles at a normal cadence throughout:
   from its side, transactions were flowing.
4. **[code] H-C did not fail an arm during the flap.** A
   positive mismatch sets `_spi_rx_arm_failed` (MT:1410, 1418),
   which forces Guard-3 retries and, at 10 consecutive, a
   teardown — `wd` would move (fact 1). It didn't. Caveat, also
   code: H-C *abstaining* on every check (persistent API error)
   is indistinguishable from H-C passing, from outside
   (§2.2, F-3). The healthy pre-flap traffic makes wholesale
   abstention unlikely but the flap window itself cannot
   separate the two.
5. **[code] The S3's HIGH phases are consistent with the
   protocol's own "staged" state.** PB2 reads HIGH at the S3
   whenever the mux sits at SPI_S_DO — which is the designed
   state from a handler's `spi_write_dma` success until the
   response is clocked out and Guard 1 re-arms (handlers at
   MT:855-858, 946, 1000, 1024-1028; re-arm at MT:1688). HIGH
   is not, by itself, evidence of a Himax fault; a *sustained,
   repeating* HIGH at idle is evidence of a transaction-rhythm
   fault, which no Himax guard's vocabulary describes (F-1).
6. **[code] The cure is consistent with volatile-state
   corruption on either side.** The sealed `eyes_reset` at
   07:54:40 UTC (S3 RST pulse → full Himax reboot → transport
   `init()` re-arm at MT:1261, rings and flags re-initialised,
   S3 re-handshake) cleared the storm immediately and
   permanently. Nothing persistent (config, calibration, crop
   state survives a reboot by design) was involved.

### 3.2 Hypotheses the code permits (ranked)

- **FH-1 — Staged-response / drain rhythm (leading).** A
  response sits staged (mux at DO, line HIGH); the S3's
  AVAILABLE/write gates fail against it; at streak 3 the S3's
  WI-3 drain clocks the staged bytes out and hands them to the
  parser as the poll response; the next poll stages the next
  count, and the cycle repeats at the observed ~289-340 ms
  period (3 × 50 ms S3 gate timeouts + poll backoff + one
  transaction). For: the soak caught a drain doing exactly this
  once, cleanly, at 01:54:11 (`spi_drain_tx: Himax HIGH …
  Himax LOW (RX-armed), drain complete`); the flap's onset
  second is a full-path crop SET completion — the same ≤179 ms
  window as both .61 wedges (RCA Addendum 2) — and the soak's
  cycle-1 stream ended with its final chunk delivered but no
  completion signal, i.e. the rhythm was already degrading
  before onset. Against: the Himax code alone cannot say why
  the S3's normal clock-out path stopped clearing stages; that
  half of the evidence lives in the S3's drain/parser hand-off
  (WI-3, out of this audit's scope).
- **FH-2 — F3 parse-retire disarms Guard 2 in slices.** The F3
  block (MT:746-759) clears `_spi_tx_staged` at every valid
  command parse, on the premise that a new command proves the
  previous response was fully clocked out. The documented
  2-byte-spill geometry (MT:655-670: response-read clocks land
  in the RX buffer ahead of the next command) is a case where a
  parse can occur without the previous stage having been fully
  clocked. If parses keep arriving while a stage is only partly
  drained, Guard 2's 300 ms staged-age is reset at every parse
  and can never fire (consistent with fact 1), while the line
  spends most of each ~300 ms cycle HIGH (consistent with the
  period's proximity to Guard 2's bound). This is the only
  Himax-side mechanism found that lets a stage persist
  indefinitely in bounded slices without any guard firing.
- **FH-3 — H-C blind window (real, but explains a wedge, not
  this flap).** A lost mux/drive on a non-Guard-1 arm (§2.2,
  §2.3) wedges permanently: nothing restores LOW, because every
  recovery path needs either a transaction (needs LOW first)
  or a flag nobody set. It cannot produce a self-clearing
  ~3 Hz flap. Recorded because the flap investigation is what
  surfaced it, and it is the strongest argument for S-2 below.
- **Refuted from source:** a second in-app writer to PB2/GPIO13
  (§2.2); Guard 2 firing (fact 1); an AT-loop stall (fact 2);
  the H1 end-state itself (`_spi_opened == false` with all
  flags false) — in that state the S3 would have seen a
  *permanent* HIGH and .62's Guard 3 would additionally have
  been retrying the open at poll cadence; the observed pattern
  (periodic success, continuous Guard-1 cycling) excludes it.

**Net statement of the Himax's flap state:** transport open,
RX DMA arming and completing normally, Guard 1 cycling on
drain-agreed signals, no guard in an abnormal state, the flag
line spending most of each ~300 ms cycle in its designed
"response staged" configuration. The Himax was, by every
instrument it has, healthy — and two of its three new
instruments (H-B, H-D) were *structurally incapable* of saying
otherwise, because the fault lives in a quantity none of them
measure: how long the line stays HIGH, and why.

---

## 4. Findings, ranked

### CONFIRMED DEFECT

**F-4 (minor, instrumentation) — H-D's running max gap is
write-only.** `_poll_max_gap_us` is updated at MT:1442-1444 and
read nowhere; it is never emitted and never reset. Spec §6.2
asked for the max to be kept *and* the >500 ms emit; the
diagnostic value of "worst gap since boot" (e.g. distinguishing
a single 60 s stall from repeated 600 ms stalls) is lost.
*Sketch (S-3):* include the running max in the POLL_GAP line's
own payload — the line is new in .62, its format is this
project's to define — and reset the max on emit.

### DESIGN GAP

**F-1 (headline) — H-B, and with it every Himax guard, is
blind to the flap class of fault.** H-B's predicate (MT:1745)
requires the armed-tracker false continuously for >2 s. The
flap state — tracker true, flags cycling, line HIGH most of
each ~300 ms cycle, multi-exchange operations starving on the
S3 — holds none of H-B's terms at any moment. More generally:
Guards 1-3 and H-B all key on *bookkeeping flags*; the
quantities that actually described the fault (sustained
line-HIGH duty cycle; true staged age) are measured by none of
them. Confirmed by ~6 h of live evidence: zero firings of any
kind while the link was unusable for multi-exchange work.
*Sketches:* S-1 (pad-state watchdog term) and S-2b
(F3-proof staged age) below.

**F-2 — H-C verifies 1 of ~20 arm sites.** `_spi_verify_arm_took`
is called only after Guard 1's arm (MT:1695). `_spi_arm_rx()`
has ~20 call sites (RX-callback handlers at MT:729, 782, 807,
814, 864, 946, 1000, 1034, 1047, 1067, 1101, 1149; Guard 2 at
1729; Guard 3 at 1525, 1554; H-B at 1758; `SPI::init` at 1261)
and `_spi_arm_rx_parked()` four more; none is verified. The
H-C failure mode (DMA half succeeds, mux/drive half lost) on
any handler arm reproduces the original .61 wedge signature —
permanent HIGH, all Himax flags healthy — with H-B exempted by
the tracker (§2.3) and Guard 3 never engaged. The spec scoped
H-C to Guard 1 (§5.4); the audit's judgment is that the
scoping, not the implementation, is the defect: the arms that
gate the *next* S3 command during normal traffic are the
handler arms, and they are exactly the unverified ones.
*Sketch:* S-2.

**F-3 — H-C can be silently disabled forever by a persistent
readback-API error.** `_spi_verify_arm_took` (MT:1404-1422)
returns without a trace on either API error. Unlike the
re-arm probes' one-shot death notes, there is no emit, counter,
or flag, so a build/driver combination in which
`hx_drv_scu_get_all_pinmux_cfg` always errors ships an H-C
that has never once run, indistinguishably from a working one.
*Sketch:* one-shot `diag_nb_emit` on first abstention per boot
per API, mirroring the `_probe_dead_*` pattern (MT:366-434).

**F-6 (minor) — deInit in the failed-open state is a no-op
that leaves Guard 3 running.** `SPI::deInit()` early-returns
when `!_spi_opened` (MT:1272) and never clears
`_spi_rx_arm_failed`; in the H1 end-state the transport cannot
be cleanly shut down and, if polled, keeps retrying opens.
Pre-.62 code, but H-A made the closed+flagged state a live
state rather than a terminal one. *Sketch:* perform flag
hygiene (`_spi_rx_arm_failed = false`, `_spi_rx_armed = false`)
before the early return.

### IMPROVEMENT

**F-5 — Guard 3's open retry runs at poll cadence while
closed.** Block-head `_spi_try_open()` executes on every
Guard-3 entry (MT:1517-1519), ~1 kHz, with a full teardown
cycle at each ~10-count (~10 ms). Functionally sound and
self-terminating, but heavier than spec §5.2's stricter
reading and needlessly hot against a dead peripheral.
*Sketch:* attempt the block-head open only on the entry where
the fail counter is 0 (i.e. once per escalation cycle), letting
the existing teardown cadence own retries.

**F-7 — H-B and Guard 2 share `_spi_wd_aborts`.** By spec
§6.1's own design (H-B firings are "separately countable" only
via the STUCK_UNARMED DIAG line), the REARM `wd=` counter
mixes Guard-2, Guard-3, and H-B teardowns. Fine while the DIAG
stream is captured; lossy when only REARM stats survive.
*Sketch:* none proposed — spec-intentional; noted so future
readers don't misread `wd=` as Guard 2 alone.

### VERIFIED SOUND

- **H-A(1), both arm paths** — every return path of
  `_spi_arm_rx` (MT:532-568) and `_spi_arm_rx_parked`
  (MT:575-605) either arms, sets `_spi_rx_arm_failed`, or is
  the by-design `!_spi_dev` silence. No spurious-set path
  exists in-tree: every closed-transport arm call is a genuine
  recovery attempt (Guard 2 / H-B post-cycle arms), whose
  flag-set is the intended H-A(3) hand-off.
- **H-A(2)** — the §5.2 invariant now holds for every state
  with `_spi_dev != nullptr`: {staged, done, arm_failed} all
  false with RX unarmed and transport closed is unreachable,
  because arming checks `_spi_opened` first (MT:536) and every
  close path either re-opens, sets the flag, or hands the state
  to Guard 3 with the flag held (MT:1543-1565).
- **H-A(3)** — the Guard-2 → H-A(1) → Guard-3 chain traces
  exactly as the spec required (MT:1721-1729 + 536-545 +
  1505-1581); no second mechanism was added, correctly.
- **Deviation 1** — Guard 3 is reachable while closed, and the
  rest of the poll body is provably inert in that state
  (§2.1), including the bulk cross-check.
- **Deviation 2 (tracker)** — the tracker is a faithful record
  of RX-DMA state (§2.3 site table). The deviation was
  necessary (the spec's literal predicate is true in normal
  idle) and correctly implemented. Its limit is F-1/F-2's
  subject, not an implementation error.
- **Deviation 4 residual (init arm unverified)** — confirmed
  as built (MT:1261, no verify) and correctly disclosed in the
  build report; it is subsumed by F-2's sketch.
- **Deviation 5** — Guard 2 via the shared cycle now attempts
  an open when already closed (MT:1382-1393); with H-A(1) the
  failure surfaces into Guard 3 instead of vanishing.
  Recovery-positive, as claimed.
- **H-D mechanism** — measures what §6.2 intended (inter-call
  gap at the poll head, live-gated, deferred-ring emit), and
  its silence during the flap is positive evidence (fact 2),
  not a malfunction. (F-4 concerns only the unused max.)
- **REARM format, invariant 2 contract, KEY_SOURCES hygiene,
  test-hook reachability, version mechanics** — per the table
  in §1.

### Fix sketches (not implemented)

- **S-1 (for F-1) — give spi_poll a line-state term.** At a
  decimated cadence in `spi_poll()` (e.g. every 64th poll),
  when the tracker claims armed and nothing is staged/done,
  sample the pad via the H-C path (`hx_drv_gpio_get_in_value`).
  Sustained pad-HIGH in that state for >2 s is the flap/wedge
  signature no current guard can see: emit a distinct DIAG
  (`E DIAG STUCK_FLAG_HIGH teardown`) and run the shared
  teardown + re-arm. Cost: one GPIO read per ~64 ms, off every
  timing-critical path.
- **S-2 (for F-2) — verify every arm, inside the arm.** Move
  the `_spi_verify_arm_took()` call into `_spi_arm_rx()` and
  `_spi_arm_rx_parked()` themselves (parked variant checks
  mux == SPI_S_DO instead of GPIO13), so all ~24 sites inherit
  it; keep it post-DMA, pre-return. The per-arm cost is the two
  driver reads H-C already pays on Guard-1 arms.
- **S-2b (for F-1/FH-2) — a staged age F3 cannot reset.** Record
  the stage timestamp at `spi_write_dma` success as today, but
  clear it only when the stage provably ended: TX DMA
  completion *plus* TXAVAIL back at learned capacity (the
  Guard-1 drain evidence), or a teardown. Guard 2 then bounds
  true stage age even across F3 retires. Confirm FH-2 first
  with a Himax UART capture of a recurrence — this sketch
  should not land on hypothesis alone.
- **S-3 (for F-4)** — payload the running max into the
  POLL_GAP line and reset it on emit.

---

## 5. Audit limits

- The S3 half of the flap (WI-3 drain behaviour and the
  parser hand-off during the storm) was **not** audited here;
  FH-1/FH-2 both have an S3-side half that this document
  deliberately does not adjudicate.
- `hx_drv_gpio_get_in_value` and `hx_drv_scu_get_all_pinmux_cfg`
  are prebuilt-library functions; their in-tree contracts are
  headers only. A bench check that H-C can actually fire
  (force the mux wrong in a hooks build, confirm Guard 3
  engages) remains the one verification of H-C that source
  reading cannot supply — the §5.5 injection tests were
  compile-verified per the build report, and this audit found
  no record of their bench results.
- Telemetry evidence is the bridge's log; the Himax's own UART
  DIAG stream for the storm window was not independently
  captured (the [himax] lines in telemetry are the relayed
  subset).
