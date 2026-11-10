# Link-hardening acceptance soak — 2026-11-09 — **INCOMPLETE / STOPPED**

**Status:** STOPPED by owner order at 02:00:31 UTC (18:00:31 PST),
after 1 of 20 cycles completed its command sequence. **No verdict** — the soak did not
finish, and it surfaced a *new failure morphology* on the hardened pair at cycle 2:
a persistent ~3 Hz gate flap (streak-3 → recovered, repeating) that starves longer
AT exchanges. All soak processes are halted; **no device commands have been sent
since the stop** (last soak command: run-2 cycle-1 crop clear, reply 02:00:08 UTC,
before the stop order arrived).

**Pair under test:** S3 fw 0.4.50 + Himax 0.4.20.62 (DIAG-verified 01:45:14 UTC,
fp `fc0bac1be88797e1`). S3 on one continuous boot from ~01:36:40 UTC throughout
(heartbeat uptime monotonic 670 → 1571 s; **no reboot, no panic, no task_wdt** in
the window).

---

## 1. The escalation — exact timing

| UTC | Event |
|---|---|
| 01:58:13 | Last png_chunk of run-1 cycle-1 capture A2: `seq 263`, 16 B payload whose b64 decodes to the PNG IEND tail — the frame's content had fully streamed; no `last:true` chunk and no success reply followed. |
| **01:58:55** | **Escalation onset.** First terminal failure streak: `W (1342612) sscma_client.io.spi: MISO-flag timeout waiting for RX-armed LOW (want 0)` + `himax gate failing: transport gate timeout (failure streak started)`. |
| 01:58:56 → | Flap establishes: `gate failing (streak started)` → `gate recovered after 3 consecutive failure(s)`, period ~289–340 ms, **continuous** — 432 streak/recovered pairs logged from 01:58:55 to the 02:01 window cut (device `log_dropped` rose 279 → 391 in-window, → 807 by 02:03, so logged pairs *understate* the true rate). Owner's console: **episodes/minute exploded from 2 to 158 at 01:59 UTC** (≈2.6/s, consistent with the logged flap period). |
| 01:59:07–08 | First casualty: cycle-2 png's mode select fails 3× (`eyes: set_sensor opt 8 attempt 0/1/2 failed: ESP_ERR_TIMEOUT`), `capture_png: mode 1296x972 select failed`, terminal reply `{"t":"dbg","cmd":"png","_mid":"28a1c224","ok":false,"err":"ESP_ERR_TIMEOUT"}` at 01:59:08. |
| 02:00:08 | Run-2 cycle-1 crop clear also fails into the flap: `ok:false, ESP_ERR_TIMEOUT`. Short single exchanges had succeeded seconds before onset (see §3); multi-exchange operations (mode select) and later even the clear cannot complete. |
| 02:03:42 | Flap still ongoing at report time (same streak-3 pattern). S3 healthy; heartbeats uninterrupted. |

**What the soak was doing at onset (01:58:55):** run 1, **cycle 2's crop SET**
(center `648,486,1296,972`, full `setCropWindow` path after clear) — the set's own
reply was `ok:true` at 01:58:55 and the first failure streak began **the same
second** (device uptime 1342612 ms). The wedge window matches both 2026-11-09
crop wedges (failure ≤1 s after a full-path crop-set reply) — but the morphology
differs: instead of the old permanent stuck-HIGH, the gate now re-sticks every
~340 ms and is cleared after exactly 3 failures each time, indefinitely.

**Ladder evidence during the flap:** *none* in telemetry — zero `episode open`,
zero `ladder rung 2/3/4`, zero `gate still failing` summary lines after onset.
The streak resets at 3 (rung-1 drain territory) before the supervisor's
episode-open threshold (≥25) can ever engage; whether the streak-3 clears are
rung-1 drains is not directly logged (`spi_drain_tx` lines appear at 01:54:11
but not during the flap). Caveat: +112 dropped device-log lines in-window
(279 → 391) mean absence from telemetry is strong but not absolute evidence.

## 2. Earlier ladder / recovery evidence (pre-escalation, verbatim)

.62 boot handshake micro-episodes (01:45:14, Himax reboot after John's flash):

```
01:45:14 W (521461) sscma_client.io.spi: MISO-flag timeout waiting for RX-armed LOW (want 0)
01:45:14 W (521461) sscma_client.io.spi: himax gate failing: transport gate timeout (failure streak started)
01:45:14 I (521599) sscma_client.io.spi: himax gate recovered after 2 consecutive failure(s)
01:45:14 W (521765) eyes: Himax reboot detected (event INIT@TRIGGER); re-linking
01:45:14 E (521765) eyes: re-link worker create failed        <-- 0.4.50 defect note
```

Cycle-1 capture start (01:49:38–40) — streak machinery + recovery, working:

```
01:49:38 W (785842) himax gate failing: transport gate timeout (failure streak started)
01:49:39 I (786103) himax gate recovered after 1 consecutive failure(s)
01:49:39 W (786154) himax gate failing: transport gate timeout (failure streak started)
01:49:39 I (786266) himax gate recovered after 2 consecutive failure(s)
01:49:39 I (786275) eyes: Himax link connected (event INIT@TRIGGER)
01:49:40 I (787138) eyes: camera recovered (rail_cycled=0)
```

Rung 1 (drain) firing at cycle-1 capture A2 start (01:54:11) — **pass-with-evidence
for the drain rung** (gate stuck HIGH, drain clocked it out, gate recovered,
stream proceeded):

```
01:54:11 I (1058382) sscma_client.io.spi: spi_drain_tx: Himax HIGH, draining TX FIFO...
01:54:11 I (1058384) sscma_client.io.spi: spi_drain_tx: Himax LOW (RX-armed), drain complete
```

Flap sample (01:58:55 onward, ×432 logged pairs):

```
01:58:55 W (1342612) himax gate failing: transport gate timeout (failure streak started)
01:58:56 I (1342901) himax gate recovered after 3 consecutive failure(s)
01:58:56 W (1342953) himax gate failing: transport gate timeout (failure streak started)
01:58:56 I (1343241) himax gate recovered after 3 consecutive failure(s)
... (period ~289-340 ms, still running at 02:03:42, uptime 1630433)
```

## 3. Per-cycle record

Geometries per protocol (odd = strip `0,0,2592,486`, even = center
`648,486,1296,972`; clear before every set). Two driver runs occurred (see §4).

### Run 1 — cycle 1 (strip) — sequence complete
| Step | Result |
|---|---|
| clear (01:49:19) | `ok:true` 01:49:20 (`_mid 65efe114`) |
| set `0,0,2592,486` (01:49:28) | `ok:true` 01:49:28 (`_mid be1b8a4a`), full path |
| capture A1 (png sent 01:49:37) | Stream completed; collector saved `cycle-01-strip.png`, **2,005,458 B**, after its full verification (contiguous seqs, PNG signature, PIL decode). File later deleted — see §4. |
| capture A2 (png sent 01:54:09) — *redundant; driver-criteria artifact, §4* | Rung-1 drain at start (01:54:11, above); stream reached final content chunk seq 263 at 01:58:13, then collector hit its 240 s deadline (~01:58:35) with no `last:true`/reply; assembly outcome unrecorded (driver killed later, §4). |
| clear (01:58:39) | `ok:true` (`_mid a89aa893`) |

### Run 1 — cycle 2 (center) — **escalation cycle**
| Step | Result |
|---|---|
| clear (01:58:47) | `ok:true` (`_mid 86efd302`) |
| set `648,486,1296,972` (01:58:55) | `ok:true` (`_mid 5e9e3b38`) — **first failure streak begins the same second** |
| capture (png sent 01:59:06) | **FAILED** — set_sensor ×3 ESP_ERR_TIMEOUT 01:59:07–08; reply `ok:false ESP_ERR_TIMEOUT` 01:59:08 (`_mid 28a1c224`) |
| clear | never ran (driver halted by operator intervention ~01:59:2x) — **center crop remains SET on the Himax** |

### Run 2 — cycle 1 (strip) — aborted by stop order
| Step | Result |
|---|---|
| clear (~02:00:04) | `ok:false ESP_ERR_TIMEOUT` 02:00:08 (`_mid f7655758`) — into the ongoing flap |
| — | Driver entered wedge-watch; **STOP order 02:00:31 UTC**; driver killed ~02:01:05. No further commands sent. |

**Cycles completed: 1 of 20** (run-1 cycle 1). **Captures verified on disk: 0**
(soak directory empty at stop — see §4).

## 4. Driver-criteria artifact and record loss (operator disclosure)

Run 1's driver required a `dbg/png` terminal JSON reply in telemetry as a
capture-success criterion. On fw 0.4.50 a **successful** png emits no such reply
(the last success reply in telemetry is 22:35 UTC on 0.4.49.1; failure replies
*are* emitted — cf. 01:59:08). Consequences:

- Capture A1 was judged "failed" despite the collector's verified save, causing
  the redundant capture A2 (which is when the 01:54:11 drain and the long
  second stream occurred).
- At the criteria fix, the operator **deleted run 1's results JSON, driver log,
  and `cycle-01-strip.png`** and relaunched (run 2). Run 1's per-cycle record
  above is reconstructed from telemetry, which is complete for every command,
  reply, chunk, and log line cited. The deleted capture is *not* recoverable;
  its verification rests on the collector's own checks observed live (file
  present, 2,005,458 B, written only after contiguous reassembly + PNG
  signature + PIL decode). Dims were not independently re-read before deletion.

## 5. Counters (telemetry window 01:45:00 → ~02:01 UTC, extracted slice preserved at /tmp/soak_window.log)

- `failure streak started` lines: **436** (4 pre-onset: 2 @ 01:45:14, 2 @ 01:49:38–39; **432 from 01:58:55**), each paired with a `gate recovered after N` line (436).
- `MISO-flag timeout` lines: **5** (transition-only logging on 0.4.50) vs the
  227,344-line historic flood baseline on 0.4.49.1 — the flood is gone; the
  failure is now a flap, not a flood.
- png_chunk lines: 512 (≈248 capture A1 + ≈264 capture A2).
- `POLL_GAP` / `STUCK_UNARMED` (.62 instrumentation): **0** in window.
- `task_wdt` / panic: **0**. Heartbeats: continuous, uptime monotonic.
- Device `log_dropped`: 279 (01:47) → 391 (~02:00) → **807** (02:03) — the flap
  floods the *device* log stream even though the bridge log stays clean.

## 6. Behavioral notes on the hardened pair (evidence for the RCA follow-up)

1. **New morphology:** post-crop-set gate failure is now a ~3 Hz flap
   (stuck → streak-3 clear → re-stuck ~340 ms later), not the permanent wedge.
   Rung 1 (or the streak-3 gate recovery) clears each micro-episode, but the
   state re-forms immediately and no higher rung can engage (streak never
   reaches the ≥25 episode threshold). Multi-exchange AT operations starve.
2. **Onset correlation holds:** flap began the same second a full-path
   `setCropWindow` (center geometry) completed — third data point for the
   crop-set completion window (cf. 178/179 ms on .61/0.4.49.1).
3. **Successful `dbg png` emits no terminal JSON reply on 0.4.50** (failure
   replies are emitted). Tooling that sequences on the reply will misjudge
   successes.
4. **`re-link worker create failed`** at 01:45:14 (eyes) during the .62 boot
   re-link; the link nevertheless connected at 01:49:39 via INIT@TRIGGER +
   `camera recovered (rail_cycled=0)`.
5. Capture A2's stream delivered the frame's final content chunk (PNG IEND at
   seq 263) but no `last:true` chunk and no completion signal followed —
   the stream's tail handshake also appears affected by the deteriorating link
   state ~40 s before the first logged gate streak.

## 7. State at stop

- Soak driver and all collectors: **killed and verified gone** (only the
  parent session's bench-lock holder pid 8993 and the bridge subscriber
  pid 1720 remain, untouched).
- Device: S3 healthy on 0.4.50 (same boot, uptime 1571 s at 02:03:44);
  Himax link **still flapping** at 02:03:42; **crop `648,486,1296,972` remains
  set** on the Himax (cycle-2 set succeeded; no clear ever completed after it).
- No commands sent after the stop order. Recovery decision (bounce / wait /
  ladder observation) is the owner's — none taken by this run.

*Preserved evidence: this report; telemetry slice `/tmp/soak_window.log`
(13,499 lines, 01:45:00 → ~02:01 UTC); run-2 driver log `/tmp/soak_driver.log`
(cycle-1 header only — cycle never closed). Captures directory
`~/workspace/bench-captures/2026-11-09/soak/` is empty (see §4).*
