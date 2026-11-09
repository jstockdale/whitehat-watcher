# Link wedge 2026-11-09 — new failure or regression?

**Question:** is today's S3↔Himax SPI link hang a NEW failure or a
REGRESSION?
**Scope:** analysis only — logs, memory, docs, git. No bench commands,
no code changes. Mechanism-level root cause is the sibling report
(`docs/link-wedge-rca-2026-11-09.md`); this report answers the
historical question and cites that document for code internals.
**Firmware in the incident:** S3 0.4.49.1, Himax 0.4.20.61
(DIAG `watcher-himax-0.4.20.61-spi bfad1e39ecc5fb43`, build date
printed by the Himax as "Nov 9 2026 01:25:08",
telemetry.log:2582741).

---

## Verdict

**LONG-STANDING LATENT defect, first triggered today through a
newly-unlocked code path. Not a regression in the code that changed,
and not a new failure mode.**

Three separable claims, each evidenced below:

1. **The failure *family* is old.** The S3 waiting forever for the
   Himax to return RX-armed (MISO LOW) is the project's oldest link
   failure mode. The error signature exists in telemetry from
   **2026-10-06**, and prolonged deadlocks of this exact family fill
   2026-10-06/07 (bulk-handoff era). The transport bookkeeping hole
   the sibling RCA identifies as the most likely mechanism (H1) sits
   in `ma_transport_spi.cpp`, which is **byte-identical in git since
   2026-11-03** (Himax 0.4.20.59) — it predates today's Himax build
   by three versions.
2. **The failure *outcome* — a permanent in-session wedge with zero
   self-recovery — is the first in the retained record.** Across
   7 weeks of telemetry (2026-09-23 → 2026-11-09, 2,582,752 lines),
   every prior occurrence of the signature is one of: a ~1-second
   boot transient, a ~1-second transient during a deliberate Himax
   reboot, or the October bring-up deadlocks (which occurred inside
   short-lived, reboot-storming development sessions and were the
   object of active fixes). Today's 24,313-line, 21-minute flood in
   a single 11.2-hour-uptime session, ended only by operator action,
   has no precedent in the log.
3. **Himax 0.4.20.61 — the only code change in the link's blast
   radius since the last-known-good era — is the *enabling* change,
   not the defective one.** Its diff is confined to buffer ownership
   inside `setCropWindow` (`ma_camera_himax.cpp:435-497`); it touches
   no transport, re-arm, or AT-server code. What it changed is
   *reachability*: before .61, `AT+CROP` could never complete (it
   died with ENOMEM, Himax code 6), so the full sensor re-init that
   follows a successful crop-set had never run end-to-end on the
   bench. .61 let it run — successfully at 20:23 UTC, and at
   21:16 UTC the same call on the same firmware pair was followed
   178 ms later by the wedge. A change that breaks previously
   working behaviour deterministically does not pass the identical
   sequence 45 minutes earlier on the identical build.

**Confidence:** high on claims 1 and 2 (direct counts over the full
retained log) and high that .61's diff is not itself the defect
(diff inspection + same-day A/B). Medium on the precise latent
mechanism (sibling RCA's H1 vs H2 cannot be separated without a
Himax UART capture of a recurrence — §6).

---

## 1. The incident, log-verified

All timestamps UTC (PST = UTC−8). Source:
`host-agent/mqtt-bridge/telemetry.log` (hereafter "the log").

| Time (UTC) | Event | Evidence |
|---|---|---|
| 20:23:47 | `AT+CROP 648,486,1296,972` → code 0 (first successful crop ever) | log:2525598 |
| 20:25:17, 20:27:49 | two `dbg png` captures, `ok:true` | log:2525979, 2526423 |
| 20:28:15 | `AT+CROP 0,0,2592,486` (strip) → code 0 | log:2526460 |
| 20:30:08 | strip capture `ok:true` | log:2526951 |
| 20:30:28 | crop cleared | log:2526986 |
| 20:31:58 | full-frame capture `ok:true` — link healthy | log:2527367 |
| 21:16:51 | `AT+CROP 0,0,2592,486` → code 0 (uptime 40,466,032 ms) | log:2529910 |
| 21:16:51 (+178 ms) | **first** `MISO-flag timeout` + `spi_send_cmd: Himax not RX-armed (cmd=0x03), failing (Himax down?)` (uptime 40,466,210) | log:2529912-2529913 |
| 21:17:30–32 | `dbg png` dies before bulk: `set_sensor opt 8 attempt 0/1/2 failed: ESP_ERR_TIMEOUT` → `capture_png: mode 1296x972 select failed` → reply `ok:false` | log:2531546–2531596 |
| 21:16:51–21:37:44 | continuous failure flood, ~51 ms cadence, single boot session (uptime monotonic 40.47M → 41.72M ms); ~24,313 error lines in hour 21 | hourly count, §2 |
| 21:23:15 | even a bare crop *query* returns `ESP_ERR_TIMEOUT` — AT path fully dead | log:2545967 |
| 21:37:44 | last flood line (uptime 41,718,829) | log:2582215 |
| 21:37:45 | S3 reboots via console connect (serial VID/PID line, fresh boot log, heartbeat `uptime_s=10` at 21:37:55) — flood stops with the session | log:2582216ff |
| 21:47:00 | Himax bootloader banner + `INIT@SENSOR code 0` — link restored | log:2582727, 2582743 |

Corrections to the incident's original framing (both also in the
sibling RCA §0): the wedge began **after the successful crop-set,
before the png** — the png is a victim, not the trigger, and the
bulk/SPIDUMP path was never entered. And the flood ran ~21 minutes,
not "25+": first failure 21:16:51, last 21:37:44, ended by the
operator's console-connect S3 reboot; full Himax re-init followed
at 21:47:00.

State before the wedge was unremarkable: 11.2 h uptime, heartbeats
`errors=0`, normal idle wake-word traffic until the second the crop
command arrived (log:2529820–2529910).

---

## 2. Log archaeology — every occurrence of the signature

The log is one continuous file, 2026-09-23 11:38 UTC → 2026-11-09
21:47 UTC, 2,582,752 lines, 268 MB. No rotated/older telemetry logs
exist beside it (`subscriber.log` is a same-period duplicate feed).

### 2.1 Census of `not RX-armed` (both message variants)

The message has two historical forms:

- **Warning form** — `... not RX-armed (cmd=0x03), proceeding anyway
  (Himax <0.4.20.38)`: the S3 logged and transmitted anyway. Exists
  only **2026-10-06 19:23:11 → 19:56:23 UTC** (571 lines), while the
  Himax ran <0.4.20.38 (log:206130, 211479).
- **Error form** — `... failing (Himax down?)`: fail-fast. First
  occurrence **2026-10-06 21:35:58 UTC** (log:213381), on S3
  0.4.18.38 — the build that changed MISO-timeout handling from
  warn-and-proceed to fail-fast `ESP_ERR_TIMEOUT`
  (`~/memory/2026-10-06.md:124`). This form is today's signature.

Per-day counts (error form; total both forms = 36,015):

| Date | Count | Character (established below) |
|---|---|---|
| 2026-10-06 | 6 (+571 warn) | 1-s burst after a png vs a just-flashed Himax |
| 2026-10-07 | 10,791 | reboot-storm bring-up; broken Himax re-arm (.41) |
| 2026-10-08 | 98 | boot transients + one deliberate-reboot transient |
| 2026-10-09 | 131 | boot transients + two 1-s clusters |
| 2026-11-02 | 15 | boot/idle transients (era of .58/.59 work) |
| 2026-11-03 | 32 | boot transients |
| 2026-11-04 | 19 | boot transients |
| 2026-11-05 | 2 | boot transients |
| 2026-11-06 | 1 | boot transient |
| 2026-11-07 | 13 | 8 boot transients + 5 during a deliberate Himax reboot |
| 2026-11-08 | 4 | boot transients |
| **2026-11-09** | **24,322** | 9 boot transients (01/02/08 h) + **24,313-line wedge flood (hour 21)** |

### 2.2 The benign classes (why the raw counts mislead)

Three recurring classes account for essentially every pre-today
occurrence. All were verified by reading the surrounding log, not
just counting:

- **Boot transients.** At S3 boot, the first `spi_send_cmd` attempts
  race the Himax's own boot: 2–6 error lines at device uptime
  ≈2,337–2,431 ms, then normal operation. Seen on 2026-11-09 itself
  (01:40:30 and 02:36:50 UTC, log:2467555, 2470489), throughout
  Nov 2–8 (86 lines, median uptime 2,380 ms), and at the start of
  the Oct 7 04:26 boot (log:232296). Self-recover in ~1 s, every
  time.
- **Deliberate-Himax-reboot transients.** When the Himax is
  rebooted on purpose (flash/boot), polls during its ~1 s boot
  window fail, then the S3 detects the reboot (INIT@ broadcast) and
  re-links. Verified instances: **2026-10-08 03:01:54** — 5 lines
  at uptime 74.16M ms inside the second the BL-Modem banner prints
  (log:305128ff); **2026-11-07 06:47:44** — 5 lines at uptime
  40.49M ms during a Himax boot to 0.4.20.60 immediately after a
  16/16 spitest pass, followed in the same second by
  `eyes: Himax reboot detected (event INIT@SENSOR); re-linking`
  and a completed re-link handshake (log:2318088ff). Not wedges:
  designed-for, recovered in-session by the relink path.
- **The Oct 6 21:35 burst.** A `dbg png` sent 1 s after a Himax
  flash-boot (0.4.20.40) failed with `ESP_ERR_TIMEOUT`
  (log:213381ff; reply at log:2134xx, `_mid b924de59`). Exactly 6
  error lines, then the storm stopped; the next Himax contact is a
  fresh INIT@ sequence at 21:51:52 amid an active flashing session.
  A burst, not a persistence.

### 2.3 The October deadlock saga (same family, different locus)

The one period where this family produced *sustained* deadlocks is
2026-10-06/07, and it is extensively documented in the dated memory
logs (citations are `~/memory/<date>.md:<line>`):

- **2026-10-06, S3 0.4.18.37 + Himax 0.4.20.39:** `dbg png` reached
  the bulk READ_CRC phase; Himax processed READ_CRC requests while
  the S3 logged MISO-flag timeouts (warn-and-proceed era); a fresh
  Himax boot fingerprint ~7 s later indicates a **Himax reboot
  during/after the failed transfer** (2026-10-06.md:123).
- **2026-10-06, S3 0.4.18.38 + Himax 0.4.20.40:** `AT+SPIDUMP`
  succeeded (1,259,712-byte frame), READ_CRC staged, then the S3
  timed out waiting for RX-armed LOW on cmd 0x03 and failed with
  `ESP_ERR_TIMEOUT`. Recorded diagnosis: *"the observed deadlock is
  specifically in bulk handoff: Himax had staged READ_CRC data/TX
  state while S3 later waited for the next RX-armed LOW command
  phase"* — and explicitly **no Himax reboot** in this instance
  (2026-10-06.md:141-142).
- **2026-10-06, S3 0.4.18.42:** same bulk-handoff failure —
  repeated RX-armed timeouts on cmds 0x03/0x05/0x06 →
  `ESP_ERR_TIMEOUT`, no PNG (2026-10-06.md:212).
- **2026-10-06/07, S3 0.4.18.51 + Himax 0.4.20.41:** spitest 16/16,
  png SPIDUMP ok, one READ_CRC, then repeated RX-armed timeouts on
  0x03/0x05/0x06; "John temporarily disconnected the device because
  of log volume" (2026-10-06.md:280). This is the session visible in
  telemetry at 2026-10-07 04:26 (S3 0.4.18.50/51, Himax
  `watcher-himax-0.4.20.41-spi`, log:232210ff).
- **2026-10-07 (whole day):** 10,791 error lines — but across
  **136 S3 boots between 03:00–08:00 UTC** (a watchdog reboot
  storm; boots at 04:49:48, :58, 04:50:09, :13, …). Failing-line
  uptimes: median 152 s, max 547 s. No single session wedged for
  hours; the count accumulated across short-lived boots during
  bring-up. Root cause found at the time: **0.4.20.41's status-based
  re-arm used the wrong SSI base address (0x40810000 instead of
  0x50810000), so `TXFLR==0 && !BUSY` never became true and the
  Himax stayed stuck HIGH** (2026-10-07.md:110; the corrected base
  is the documented expectation in
  `docs/a0-rearm-investigation-prep-2026-11-02.md:51`, and .60's
  boot log prints `SPI base: drv=0x50810000 rc=0` — log, 2026-11-07
  06:47:44).

Classification: **same family, different signature-in-detail.** The
family trait — Himax does not return RX-armed LOW and the S3 has no
in-session way out — is identical. The locus differs: October's
deadlocks struck *mid-bulk-transfer*, after a READ_CRC response was
staged; today's struck *on the plain AT path*, 178 ms after an
ordinary AT command completed, with bulk never entered. October's
proximate causes were found and fixed in their era (fail-fast gate
in S3 0.4.18.38; correct SSI base and, later, the whole A0 re-arm
programme). They are ancestors of today's incident, not instances
of it.

### 2.4 The only other capture timeout: 2026-10-05

`capture_png: mode 1296x972 select failed: ESP_ERR_TIMEOUT` occurs
exactly **twice** in the entire log: 2026-10-05 03:41:39
(log:142199) and today. The Oct 5 instance (S3 0.4.18.2, Himax
pre-.38 `watcher-himax-0.4.20-spi`) **self-recovered in-session**:
the Himax answered `AT+ID?` at 03:42:41 and `AT+SENSORS?` at
03:43:02, ~1 minute later, no reboot. Same failing operation,
transient outcome — the pre-fail-fast era's behaviour, and a useful
control: a mode-select timeout alone does not wedge this link;
today's wedge needed the Himax-side state the crop-set left behind.

### 2.5 Other incidents named in the brief — classified

- **2026-11-02 spitest cascade** (Himax .59, gap-0): 11/16 after a
  2-byte-spill parser misalignment ate commands → S3 MISO-flag
  timeouts; **link self-recovered without reboot**; fixed by .60's
  CRC-validated realignment scan (2026-11-03.md:50). *Same family
  (desync → RX-armed waits), different signature and a recovery —
  a cousin, and the direct ancestor of the .60 transport now
  running.*
- **2026-11-02 chunk-loss RCA** (`docs/chunk-loss-rca-2026-11-02.md`):
  truncated/spliced PNG chunks over the **host serial/telemetry
  path** (log-ring laps, bridge splice bug) on S3 0.4.19/0.4.20.
  *Unrelated — different link, different layer.*
- **Baud-arc garbling** (late Oct–Nov): console UART at off-rates
  (921600 garbles, host-side dead zones at 576000/691200/806400) —
  the S3↔console CH342 path. *Unrelated to the SPI link.*
- **2026-10-05-era "RX flood → S3 WDT"** (0.4.20.30 TX-timeout
  mis-set, per `~/AGENTS.md`): the flood direction reversed (Himax
  stuck in TX flooding the S3) and the victim was the S3's
  watchdog. *Same family tree (TX-done/re-arm timing), different
  signature; fixed by the transfer_time+1500 µs rule.*

---

## 3. Memory & docs archaeology — the re-arm programme (why "latent")

The October saga was closed out by the A0 programme (late Oct →
2026-11-03), which rebuilt the Himax's re-arm from a timer guess
into a signal-driven mechanism, documented in
`docs/a0-rearm-investigation-prep-2026-11-02.md` and the dated logs:

- .58 (instrumentation): across 11,408 responses, drain signals
  clear 0–1 ms after DMA-done; one signal (INT_STATUS bit 11) never
  fires (2026-11-02.md:300).
- .59 (2026-11-03 05:00 UTC): signal-driven Guard 1 ships; bulk wall
  35.7 s → 3.7 s, zero parser drops across 923 bulk commands,
  spitest 16/16, snaptest byte-exact — **"A0's production goal is
  MET"** (2026-11-03.md:48). Residual: the §2.5 spill race, fixed
  in .60.
- .60: parser scan-realignment; validated Nov 3 (2026-11-03.md:84-86
  discusses its one CRC flake as a *different, long-standing*
  family).

From .59's validation (2026-11-03) to today, the link's record in
telemetry is: captures and spitests succeeding, and nothing but
§2.2's benign transients. That is the **last-known-good era**, and
it is six days long (Nov 3–9). The guards' architecture — Guard 1
(drained-path re-arm), Guard 2 (300 ms staged watchdog), Guard 3
(arm-failure escalation), all living in `spi_poll()` — dates from
this programme. The sibling RCA (§1.3–1.4) shows a reachable state
in that architecture that *no guard covers* (all re-arm flags
false, RX unarmed, PB2 released — entered if a guard teardown's
`spi_open` fails and the follow-up `_spi_arm_rx()` returns silently
on `!_spi_opened`, `ma_transport_spi.cpp:504-507`). If that reading
is right, **the hole has been present in .59, .60 and .61 alike** —
it simply never had a trigger pulled before today. That is the
definition of latent, and it is why "regression in .61" fails: the
suspect code did not change in .61.

---

## 4. Git archaeology

Repos: one git lineage serves both trees
(`~/workspace/projects/watcher-input-assistant`, mirrored by the
clean copy `~/workspace/whitehat-watcher-clean`). History is
coarse — this is itself a finding (§6).

### 4.1 What the commits can and cannot show

- `30c8137` 2026-09-29 — initial commit.
- `984fc9f` 2026-11-03 — *"A0 complete: S3 0.4.23 + Himax
  0.4.20.58-.60 — REARM probes, signal-driven Guard 1, parser
  scan-realignment."* **Every link-path file on both sides was last
  committed here**: `firmware/components/sscma_client/src/
  sscma_client_io_spi.c`, `sscma_client_ops.c`,
  `ma_transport_spi.cpp`, `ma_camera_himax.cpp` (git log per file:
  only 984fc9f and 30c8137).
- `dc32493`, `77b39dd` 2026-11-09 — the S3 0.4.48/0.4.49 commits.
  In the link's blast radius they touch **only**
  `firmware/components/eyes/eyes.c` (+41/−9 across both) and
  `eyes.h`. `git diff --stat 984fc9f..77b39dd` over
  `firmware/components/sscma_client/` and the Himax `we2/` tree is
  **empty**: the SPI transport on both sides is byte-identical to
  the Nov 3 A0 state in the committed record.
- Everything since (S3 0.4.49.1's final state, the Himax .61 build)
  is **uncommitted working-tree state** in the working repo.

### 4.2 The one working-tree change in the blast radius

`git diff HEAD` over all link-path files returns exactly one file:
`ma_camera_himax.cpp`, +43/−19, entirely inside
`CameraHimax::setCropWindow` (lines 435-497). The change: reuse the
persistent 1.26 MB raw buffer when it is large enough instead of
allocating a second one; free only self-allocated buffers on error
paths (`fresh_alloc`). Before it, the second allocation failed with
`MA_ENOMEM` whenever a raw preset had been initialised first — "in
every real capture sequence," per the added comment. This is
Himax **0.4.20.61** (built Nov 9 01:25, flashed by the owner
~10:01 UTC today).

Nothing else in the path moved: `ma_transport_spi.cpp` (transport,
TX-done handling, Guards 1–3, .59 re-arm, .60 realignment) and the
S3's `sscma_client` (gate, bulk/SPIDUMP read) are unchanged from
984fc9f; `ma_server_at.cpp` (AT dispatch) is unchanged in the
working tree.

### 4.3 The crop path's own history

`AT+CROP` was attempted **exactly four times in the entire retained
log, all today** (grep `eyes: set_crop` over the full log):

1. 09:22:30 — rejected, Himax code 6 (ENOMEM), on Himax .60
   (log:2494560).
2. 20:23:47 — code 0, on .61 (log:2525598).
3. 20:28:15 — code 0 (strip; same byte count → fast recrop path)
   (log:2526460).
4. 21:16:51 — code 0, then the wedge (log:2529910).

`setCropWindow` itself exists at HEAD (984fc9f) — the feature was
committed Nov 3 but, per the log, never successfully exercised
until .61 removed the ENOMEM. The full path it takes from a cleared
state (`deInit()` → buffer step → `drv_ov5647_set_raw_buffer` →
`drv_ov5647_init_raw_fullres`) ran end-to-end for the first time at
20:23:47 today. At 20:28 it was the fast recrop path instead
(same-size window move). At 21:16 it was the full path again — and
wedged. Two full-path executions in history: one success, one
wedge. That is a coin-flip trigger on a brand-new path into
six-day-old transport code — the fingerprint of a latent race/state
hole being *reached*, not of a change *breaking* a working path
(the path never worked before today at all).

### 4.4 Candidate-change table

| Change | Date | In wedge path? | Assessment |
|---|---|---|---|
| S3 fail-fast RX-armed gate (0.4.18.38) | 2026-10-06 | Defines today's error string & the flood behaviour | Era change; converts silent desync into a loud, non-recovering wait — but the *wait* is not the wedge |
| Himax .41 wrong SSI base (0x40810000) | 2026-10-05 build | Caused Oct 7 storm | Fixed in-era; .60/.61 boot-print the correct 0x50810000 |
| A0: signal-driven Guard 1 + guards (.59), scan-realignment (.60) | 2026-11-03 (984fc9f) | **Yes — the uncovered-state hole (RCA H1) lives here** | Latent carrier; six days of clean service before a trigger reached it |
| S3 0.4.48/0.4.49 (streaming, I2C-WDT) | 2026-11-08/09 (dc32493, 77b39dd) | eyes.c only; no SPI-client change | No link-path delta; same S3 build ran the 20:23 successes |
| **Himax .61 setCropWindow buffer reuse** | **2026-11-09 (uncommitted)** | Adjacent (camera, not transport) | **Enabler**: removed the ENOMEM that had always aborted the crop full path upstream of the transport's latent hole |

---

## 5. Synthesis

"New or regression?" dissolves into three answers:

- **New symptom instance?** Yes — the first permanent, in-session,
  no-self-recovery wedge in 7 weeks of retained telemetry, and the
  first hang ever triggered from the AT/crop path rather than the
  bulk path.
- **Regression?** No — under the only useful definition (a change
  broke behaviour that previously worked). The transport code is
  unchanged since 2026-11-03; the S3 gate is unchanged in behaviour
  since 2026-10-06; the one new change (.61) provably ran the
  identical sequence successfully on the identical firmware pair
  45 minutes before the wedge, and the path it "broke" (crop) had
  never worked before it existed. Nothing that worked yesterday
  stopped working because of an edit.
- **Long-standing latent?** Yes — this is the correct bin. The
  failure family is documented from 2026-10-05; the specific
  uncovered transport state the RCA identifies dates to the
  2026-11-03 Guard architecture (.59) at the latest and plausibly
  to the October guard/teardown design it grew from; the S3 side
  has lacked any failure-accounting/escalation on the AT path for
  the gate's entire life (RCA §2). .61's crop fix was the first
  key that fit the lock: it let a heavy Himax-side re-init
  (deInit + full-res sensor init) complete and be followed,
  178 ms later, by whatever the re-init left behind in the
  transport. Whether the entry was the RCA's H1 (teardown +
  failed `spi_open` → silent no-arm state) or H2 (AT/executor
  thread stalled in the camera stack) is the one question this
  report cannot close from the retained record — the Himax's own
  DIAG stream, which would discriminate, goes to its UART and was
  not captured during the incident.

Practical consequence for prioritisation: because the defect is
latent in the *transport + recovery-trigger* layer rather than in
.61, reverting or bisecting .61 would only re-lock the door (crop
goes back to ENOMEM) without fixing anything — the hole would
remain for the next heavy AT command to find. The fix belongs
where the RCA puts it: Himax-side uncovered-state closure and
S3-side failure counting with an escalation ladder.

---

## 6. Gaps in the record, and what would raise confidence

1. **Git granularity.** The repo's history is two commits for the
   entire link stack (2026-09-29, 2026-11-03); S3 0.4.24→0.4.49.1
   and Himax .61 exist only as working-tree state and version
   strings in memory/logs. Per-version bisection from git is
   impossible; the versioned *bench record* (dated memory logs +
   telemetry banners) is the real history, and this report leans
   on it accordingly. (The version strings are reliable; Himax
   build fingerprints in the Oct 5-7 era were partly stale —
   0.4.20.40 and .41 print the same fp `bdb53f70a364ed94` — so
   era attributions there rest on the printed version + build date,
   not the fp.)
2. **Himax-side visibility.** The Himax DIAG/UART stream is only in
   telemetry when the console forwards it; it was not captured for
   the incident window. A recurrence with UART capture (RCA §4.3:
   scripted crop/strip loop, ≥200 iterations) would settle H1 vs
   H2 and either raise the latent-mechanism confidence to high or
   redirect it.
3. **Log retention.** Telemetry begins 2026-09-23; the link's
   September bring-up (pre-gate era) is visible only through dated
   memory notes. "First occurrence in the retained record" is the
   strongest claim the evidence supports — September cannot be
   audited for an earlier persistent wedge. (The September notes
   describe a different transport era — pre-MISO-flag — so an
   identical-signature event could not have been logged then in
   any case.)
4. **The 45-minute idle.** Between the last success (20:31:58) and
   the wedge (21:16:51) the device only listened for the wake word.
   Nothing in the record shows any Himax traffic in that window, so
   the trigger is the crop-set itself (or its coincidence with the
   S3's next poll) — but a slow-burn state change during those
   45 minutes cannot be excluded from the S3-side log alone.
5. **`.61` banner evidence.** No `watcher-himax-0.4.20.61` DIAG line
   appears in telemetry until the post-wedge reboot (21:47:00);
   .61's presence during 20:23–21:16 is established behaviourally
   (code 6 on .60 at 09:22 → code 0 at 20:23, matching the buffer
   fix exactly) plus the owner's flash record — solid, but
   indirect.

**Evidence that would raise confidence:** (a) a reproduced wedge
under the RCA §4.2/§4.3 fault-injection/UART-capture setup,
discriminating H1/H2; (b) a Himax flag/register dump in the wedged
state (`_spi_opened`, guard flags) — decisive for H1; (c) 200+
scripted crop full-path cycles on .61 with zero wedges would
*lower* confidence in the crop-trigger reading and push toward a
 rarer timing coincidence — worth knowing either way.

---

## Appendix — key numbers

- Log span: 2026-09-23 11:38 UTC → 2026-11-09 21:47 UTC;
  2,582,752 lines.
- `not RX-armed`, both variants, whole log: 36,015
  (warn form 571, all 2026-10-06; error form 35,444).
- Error-form first: 2026-10-06 21:35:58 UTC (log:213381).
- Incident flood: 24,313 error lines in 2026-11-09 hour 21
  (day total 24,322; the other 9 are 01/02/08 h boot- and
  reboot-transients), cadence ~51 ms, single S3 session,
  uptime 40.47M→41.72M ms.
- Prior single-session flood maximum: Oct 7's 10,791 lines were
  spread over ≥136 boots (median failing-line uptime 152 s).
- `capture_png … ESP_ERR_TIMEOUT`, whole log: 2 (2026-10-05,
  2026-11-09).
- `AT+CROP` attempts, whole log: 4, all 2026-11-09.
- Link transport files unchanged in git since: 984fc9f,
  2026-11-03. Only working-tree link-path delta: `ma_camera_
  himax.cpp` setCropWindow (+43/−19) = Himax 0.4.20.61.

---

## Addendum — 2026-11-09 22:39 UTC: second wedge, appendix superseded

Appended 2026-11-09 ~23:05 UTC from the live telemetry log.

A second wedge occurred at 22:39:18 UTC, 179 ms after a code-0 crop
set (first incident: 178 ms) — full detail in the addendum to
`link-wedge-rca-2026-11-09.md`. Appendix numbers above were compiled
at 21:47 UTC and are superseded as follows:

- `AT+CROP` attempts, whole log: was 4 → now 6 set_crop log lines
  (5 with code 0, plus the 09:22 code-6 rejection on Himax .60).
  Of the 5 code-0 sets: 3 clean, 2 wedged.
- `not RX-armed`, both variants, whole log: was 36,015 → 65,264
  lines as of 23:04 UTC, the second flood still running at that
  check (~29.5k lines since the 22:39 onset).

The verdict is unchanged — long-standing latent failure, not a
regression; 0.4.20.61 the enabler, not the defect — and is now
stronger: the trigger (crop-set response path) reproduced twice in
one day with the same ~180 ms onset signature.
