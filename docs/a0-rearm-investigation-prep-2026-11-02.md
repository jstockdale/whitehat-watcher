# A0 Investigation Prep — Himax TX→RX Re-Arm (prepared 2026-11-02, pre-bench)

Status: PREPARED, NOT STARTED. John approved the plan on 2026-11-02
("S3 build first, then Himax A0"; A0 meticulous, signal-first, counters
designed in from the start; further optimization deferred until this plan
is working). This doc is the desk investigation done ahead of the bench
session so A0 starts from code facts, not archaeology.

## Baseline A0 must beat (measured, S3 0.4.23, 2026-11-02)

- Bulk wall: 27,414 ms / 27,599 ms over two verification snaptests
  (pre-A2 baseline median 35.66 s over 50 transfers).
- `bulk_stats`: polls=615, small=307, reads full=307, runt=0, crc_fail=0,
  attempts=1 on both runs.
- **wait_low = 24,422 / 24,590 ms = 89% of wall** — the Himax re-arm toll,
  now measured on-device. A0's target: wait_low collapses toward ~1 s;
  bulk ≈ 3 s. Acceptance (from the 2026-11-02 re-baseline): 20 frames,
  0 CRC failures/retries, runts <1% of READ_CRC (DIAG), wait_low <10% of
  bulk wall, bulk ≤3 s.

## Where the toll lives (verified in the current tree)

File: `sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp`

- Guard 1 in `ma::spi_poll()` (~1 kHz, thread context): when
  `_spi_tx_done` is set, it waits `timeout_us = (len > 64) ? 30000 : 25000`
  from `_spi_tx_done_us`, then calls `_spi_arm_rx()`. Comment in code:
  "25ms re-arm timer (interim bridge until Tests A/B)."
- `_spi_tx_done` is set in `_spi_tx_callback` (line ~750), the
  `spi_write_dma` completion callback. Per the v0.4.20.7 note, that
  callback fires when the TX DMA has fed the SSI FIFO — **before** the S3
  has clocked the bytes out. So DMA-done is NOT transfer-complete; that
  gap is exactly why a timer was layered on top. Any signal-first design
  must find a signal that means "the master finished clocking," not
  "DMA finished feeding."
- Guard 2 (300 ms staged watchdog on `_spi_tx_staged`) and Guard 3
  (arm-failure escalation, ~10 consecutive `_spi_arm_rx()` failures →
  teardown) are independent of Guard 1 and must keep working unchanged.

## What .41 tried, and why it faulted (from the code's own history)

- v0.4.20.41 Phase A1 attempted status-based re-arm by directly reading
  DW SSI registers: TXFLR (base+0x20) and SR (base+0x28, bit0 BUSY).
- v0.4.20.43 REVERTED it: the direct reads bus-faulted during bulk
  transfers (Himax reboot). The code's conclusion: "The ALIAS address
  didn't help — the peripheral is likely secure-only or the context
  doesn't permit it." A 1 ms timer replaced it at the time; .52 later
  raised that to the current 25/30 ms guards.
- v0.4.20.52 (Claude Q1) added `_spi_ssi_base_cached`, taken once at init
  via `spi_control(SPI_CMD_GET_SPI_BASE_REG)` and cross-checked against
  `DW_SSPI_S_RELBASE` (expected 0x50810000) — explicitly "single source
  of truth for any future register reads." Note the trap: the cached
  base does NOT by itself make thread-mode reads safe; .43's fault
  happened with an alias base too. Raw register reads are the path that
  already failed once — do not retry them as the first move.

## The untested path: driver-mediated status (hx_drv_spi.h)

The Himax SPI driver exposes status queries through `spi_control`,
i.e. executed by the driver in its own context rather than raw MMIO
from the AT thread. None of these appear in `ma_transport_spi.cpp`
today (only `SPI_CMD_GET_SPI_BASE_REG` is used). Ranked candidates:

1. **`SPI_CMD_GET_BUSY_STATUS` (36)** — "Get flag value to check if spi
   busy." Closest driver-level equivalent of the SR BUSY bit .41 wanted.
   If BUSY tracks SSI activity while the master clocks, polling it from
   spi_poll() after `_spi_tx_done` gives a real drained signal.
2. **`SPI_CMD_GET_TXAVAIL` (22)** — bytes of TX FIFO space available.
   TX FIFO empty ⇔ avail back to full capacity. A second, independent
   drained indicator; can cross-check BUSY.
3. **`SPI_CMD_GET_STATUS` (0)** — the device `status` field; semantics
   need confirming from driver behavior (cheap to probe and log).
4. **`SPI_CMD_GET_INT_STATUS` (25)** — raw DWC_ssi interrupt status,
   including bit 11 "SSI Done Interrupt Raw Status." Potentially the
   true transfer-complete edge, readable via the driver.

Caveat to test, not assume: for a demand-paced slave DMA transfer, BUSY
/ TXAVAIL may still clear before the final bytes are clocked out (the
same FIFO-vs-wire gap as the DMA callback). The first A0 experiment is
therefore instrumentation-only: log BUSY / TXAVAIL / INT_STATUS sampled
in spi_poll() alongside `_spi_tx_done_us` during ordinary transfers,
and compare their transitions against when the S3 actually finishes
clocking (correlate with S3-side wait_low and the known-good 25/30 ms
boundary). Only a signal proven to track the wire gets to replace the
timer.

## Fallbacks, in order (per John's signal-first, ladder-last ruling)

1. Driver-mediated status (above) — preferred.
2. CS-edge: if the slave-select line is observable (driver event or a
   GPIO view of CS), CS deassert is the definitive end-of-transfer.
   Not yet investigated — no CS handling found in the transport file;
   check driver callbacks (`SPI_CMD_SET_XFERCB`, 6, "transfer success
   callback") during the same instrumentation pass.
3. Timer ladder 25/30 ms downward, each rung gated by spitest 16/16 +
   a 20-frame soak — only if 1 and 2 provably don't exist.

## Counters to design in from the start (John's rule, 2026-11-02)

Himax-side, DIAG-emitted, per bulk transfer (mirroring S3 bulk_stats):
- re-arm count and total/avg/max re-arm latency (tx_done → RX armed)
- which path armed RX (signal vs Guard 1 timer vs Guard 2 watchdog)
- BUSY/TXAVAIL poll counts before drained (during the experiment phase)
- Guard 2 aborts (`_spi_wd_aborts` already exists — surface it per run)
- consecutive arm-failure high-water (Guard 3 already counts)

## Open questions

- "Tests A/B" are referenced by Guard 1's comment and the research doc
  but defined in neither — likely from the wh-link Claude Q4 exchange.
  Recover their definition (or redefine them) before treating Guard 1
  as removable.
- Exact semantics of `dev_spi_info::status` for GET_STATUS, and whether
  GET_BUSY_STATUS reflects SSI BUSY or a driver software flag — to be
  settled by the instrumentation pass, not by the header alone.
- Which context the driver's spi_control handlers run in, confirming
  they are safe from the AT thread where raw MMIO faulted.

## Phase 2 design — v0.4.20.58 instrumentation build (decided 2026-11-02, bench session)

INSTRUMENTATION ONLY. Zero behavior change: Guard 1 (25/30 ms), Guard 2
(300 ms staged watchdog), Guard 3 (arm-failure escalation) are untouched,
and no probe result influences any decision. SET_XFERCB is deliberately
NOT probed in .58 (it could overwrite a callback the prebuilt driver
uses internally); the four probes are read-only `spi_control` queries.

While `_spi_tx_done` is pending, each `spi_poll()` pass samples:
- `SPI_CMD_GET_BUSY_STATUS` (36) — first idle read
- `SPI_CMD_GET_TXAVAIL` (22) — first read back at capacity (capacity is
  learned post-re-arm, when the TX FIFO is empty by construction)
- `SPI_CMD_GET_INT_STATUS` (25) — first read with bit 11 (SSI Done) set;
  "preset" (already set at first sample) is counted separately because
  a sticky raw-status bit would show up exactly that way
- `SPI_CMD_GET_STATUS_REG` (42) — bit 0 (BUSY); header notes it valid
  for a master polling build flag, so it is probed, not trusted

Per response: first-poll latency, each signal's first-clear latency
relative to `_spi_tx_done_us`, busy_seen, txavail-at-first-sample.
A probe returning an error disables itself after one DIAG note.
Aggregates are emitted as `REARM_STATS` / `REARM_LAT` DIAG lines at
CMD_STOP, at feeder-watchdog disarm, and every 32 non-bulk responses;
verbatim `REARM_ONE` lines for the first 4 responses after boot and for
any response where a live probe had NOT cleared by the time Guard 1
fired (`not_clear` — the safety-critical count).

Decision rule for the follow-up build (.59): a signal qualifies to
drive re-arm iff across spitest + soak + bulk it (a) always clears,
(b) worst-case clear latency ≪ 25 ms, and (c) `not_clear` == 0. The
winning signal then arms RX after signal + a small fixed allowance
(FIFO-residue wire time), with Guard 1's timer demoted to a fallback
ceiling (arm anyway + count it if the signal never comes). The exact
allowance and ceiling come from the .58 distributions, not from a
guess. "Tests A/B" remain undefined in every doc on disk; this
evidence gate replaces them unless John/Claude supply the originals.

## Phase 3 results — .58 on the bench (2026-11-02 evening)

Flashed by John (fingerprint 1dd6b8dd44dfd122 confirmed in the boot
DIAG line). Spitest 16/16 (John's console run), one snaptest
byte-exact, bulk_stats identical to .57 behavior (wall 27,543 ms,
wait_low 24,556) — instrumentation is transparent, as designed.

Dataset: 11,408 REARM_ONE records (the verbatim flood — see bugs
below — accidentally produced a complete per-response record).

- BUSY (cmd 36), SR (cmd 42), TXAVAIL (cmd 22): first-clear latency
  after DMA-done is 0 µs in ~91% of records, 1000 µs in ~9%, across
  ALL length classes (2 / 18 / 258 / 1002 / 4092). busy_seen = 0 in
  11,408: sampling starts at DMA-done, and for demand-paced slave DMA
  the wire is already done or within the ≤33-frame FIFO residue
  (~23 µs) by then. taf (TXAVAIL at first sample) = 32 = capacity in
  every record.
- INT_STATUS bit 11 (SSI Done): set in 0 of 11,408. The ioctl works;
  the bit never asserts on this path (likely IMR-gated). NOT a usable
  signal on this driver build.
- One len=2 record at b=s=75,000 µs with busy_seen=0: spi_poll
  starvation (first sample at +75 ms found everything already idle),
  not wire activity. Poll cadence is part of the latency budget.
- Per-signal not-clear at the 25/30 ms timer: BUSY 0, SR 0,
  TXAVAIL 1 (the boot response, before capacity was learned).
  All three pass the Phase 2 gate with that boot-edge caveat.

Instrumentation bugs to fix in .59 (mine, found by John in the idle
flood): not_clear was any-probe (the never-firing INT probe made
every response "not clear"), and the verbatim-on-not-clear rule then
emitted a REARM_ONE per response forever. .59: per-signal nc counts;
verbatim = first 4 responses + first 4 not-clear, hard budget.

## Phase 4 design — v0.4.20.59 signal-driven re-arm

Guard 1 becomes: while `_spi_tx_done` is pending, arm RX at the
first spi_poll where (a) BUSY idle AND SR BUSY clear AND TXAVAIL at
capacity (capacity-unknown counts as drained for TXAVAIL — boot
edge), and (b) elapsed since `_spi_tx_done_us` ≥ 2000 µs (the S3's
~2 ms minimum inter-command gap, AGENTS.md rule era; covers the
FIFO-residue clock-out and the master's turnaround). Expected:
per-response re-arm ~2–3 ms vs 25/30 ms; bulk wall ~3–4 s.
Fallback ceiling: if the signals never read drained, arm anyway at
the old 25/30 ms and count it (`fallback_arms`) — the timer becomes
the ceiling, not the pace. Guards 2 and 3 unchanged. REARM_STATS
kept (fixed aggregation) with new fields: signal_arms, fallback_arms,
max_fp (poll starvation). Validation gate: spitest 16/16, then
20-frame soak, then acceptance vs the 0.4.23 baseline (bulk ≤3 s,
wait_low <10% of wall, crc_fail=0, runts <1%).

BUILT + read-back verified 2026-11-02 19:54 PST: ELF and packaged
image carry `watcher-himax-0.4.20.59-spi`, fingerprint
`b1e04767fad3103c`, and the new REARM format strings.
`watcher_sscma_v0.4.20.59_spi.img`, 622,592 B, ckBS magic, SHA-256
`19888ca52c8ae5bb7be0dc8c6874f29a97a7685c263079bc4adfda0572070401`,
staged at `~/workspace/your_files/himax-flash-kit/v0.4.20.59/`.

## Phase 5 results — .59 validation (2026-11-02 evening)

Flashed by John; boot DIAG confirmed fp b1e04767fad3103c. Seal floor
read + resynced first (63759 — the Oct lesson applied).

- Idle: REARM periodics sig=32/32, fb=0, all nc=0, amax=2000µs
  (arms land on the 2ms floor), flood gone.
- BULK (the production path): wall 3,717 ms (0.4.23 baseline
  27,414-27,599; original 35,660), wait_low 849 ms (was 24,556),
  wait_high 698, polls=615, runt=0, crc_fail=0, attempts=1, ZERO
  parser-drops across 923 commands. Snaptest frame byte-exact
  (2,204,896 B — bigger scene), log_laps=0. The residual wait_low
  (~0.92ms/command) is the S3's flag-poll quantization, no longer
  the Himax arm.
- Spitest: mixed. First run (05:01Z, ~90s after the Himax reboot,
  link still busy with boot traffic): 11/16 — ignition at pat2/4090,
  then pat3 all fail; Himax logged 10 parser-drops (buf[0]=0x00,
  buf[2]=0x00 — spill beyond the parser's 2-byte realignment), S3
  logged MISO-flag timeouts (commands eaten by the drops). Every
  later run clean: gap0 re-run 16/16, gap10 16/16, singles pass,
  iterations=5 gap0 80/80. 130 tests total, 5 failures, all in that
  first cascade; the link self-recovered (later commands fine, no
  reboot). Zero parser-drops on .58 in 87 min incl. a spitest — the
  slow arm made the race vanishingly rare; the 2ms arm exposes it
  when the S3's spi_drain_tx (64B bursts while HIGH, 2ms delay loop)
  runs against a busy link at gap-0 cadence. Bulk never drains
  between commands and is unaffected.
- Verdict: A0's goal is met on the production path (35.7s -> 3.7s,
  zero CRC failures). The drain/spill race is the one residual;
  candidate fix for a .60 (John's call): widen the Himax parser's
  realignment from fixed offset-2 to a scan for FEATURE_TRANSPORT +
  valid cmd + header-CRC at any offset (the frame CRC validates the
  alignment), making any spill size harmless. Alternatives: S3 drain
  stops clocking sooner; Himax flushes RX FIFO at arm. NOT yet run:
  the full 20-frame soak (each frame is ~3-4.5 min of serial chunk
  time; 1 frame + 923 bulk commands + 130 spitests done).

## Phase 6 — v0.4.20.60 parser scan-realignment (John approved)

Design (implemented 2026-11-03): the fixed offset-2 check stays as
the fast legacy case; frames it rejects now go through a scan over
all offsets 0..250. A candidate must carry FEATURE_TRANSPORT (0x10),
a command in [0x01..0x09], and a valid CRC — header CRC at [o+4..o+5]
over the 4 header bytes for control commands (0xFFFF = CMD_RESET's
legacy filler), packet-CRC trailer for CMD_WRITE (0xFFFF legacy or
unreceivable trailer accepted, mirroring the WRITE handler). The scan
only runs on frames the legacy checks already rejected, so no frame
accepted today can be rejected by it; the WRITE handler's existing
len clamp (cmd_off+4+len > 256) covers deep-offset payloads.
Recovery is recorded as DIAG where=10 ("parser-realign: off=<o>
cmd=0x..") so spill sizes stay visible. BUILT + read-back verified
2026-11-03 01:48 PST: ELF and packaged image carry
`watcher-himax-0.4.20.60-spi`, fingerprint `09d632f9fc904d1e`, and
the new `I DIAG parser-realign` string; image 622,592 B, ckBS magic,
SHA-256 `f46fe4219102f82830de12216996ce981ee149f57306f1ab20593e19dbc55710`,
staged at `~/workspace/your_files/himax-flash-kit/v0.4.20.60/`.
Delivery: the upload service reaped sessions all night (7 attempts);
John pulled the staged app-files copy against the published SHA, and
the curl link landed the next morning (expires 2026-11-05T17:03Z).

VALIDATED 2026-11-03 (John flashed; boot DIAG fp 09d632f9fc904d1e
confirmed; seal floor 63792 read + resynced first): spitest gap-0
iterations=5 TWICE under the same post-boot conditions that produced
.59's 11/16 cascade. Run 1: 79/80 — the scan recovered TWO spilled
TEST_PATTERN commands (parser-realign off=64 — exactly one 64B drain
burst, confirming the spill source/size — and off=9), ZERO
parser-drops, ZERO MISO timeouts. Run 1's single failure was a
response-side CRC flake (rx bytes uniformly 0x0A; 73 historical
occurrences of that flake across firmware eras — not the drop mode,
not a regression). Run 2: 80/80. Session totals: 159/160, 2 realigns,
0 drops. Snaptest: bulk wall 4,026 ms, wait_low 1,157 ms, crc_fail=0,
attempts=1; frame byte-exact (1,372,988 B), log_laps=0. Verdict:
the .59 residual is FIXED — spills of any size are now recovered
in-parser instead of eating commands. A0 is complete: bulk
35.7 s → ~4 s, zero CRC failures, parser hardened.

## Build/flash discipline for the A0 build (standing rules)

- `make cleanbuild` only (regenerates WATCHER_BUILD_HASH, full clean);
  never trust a fingerprint that matches a previous build.
- Read back the built artifact (fingerprint, markers) before staging.
- Himax flash is John's XMODEM run; ask first, announce, narrate.
- Gate: spitest 16/16, then 20-frame soak, then the acceptance numbers
  above against 0.4.23's bulk_stats baseline.
