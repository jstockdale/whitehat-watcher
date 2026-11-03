# A0 20-Frame Soak — Run Sheet (prepared 2026-11-03, background prep, NOT run)

Purpose: the 20-frame soak is the one validation gate still open on A0
after Himax 0.4.20.60 was flashed and validated on 2026-11-03. This
sheet makes the next bench session start at the soak instead of
re-deriving the plan. Nothing here has been run; the soak needs John's
bench grant like any device session.

## Why this run, why now

- .60 validation (2026-11-03, John's flash + green light): gap-0
  spitest ×5 twice under fresh-post-boot conditions — 79/80 then 80/80,
  with two `DIAG parser-realign` recoveries (`off=64` = exactly one
  64 B drain burst, `off=9`), **zero parser-drops, zero MISO timeouts**.
  Bulk 4,026 ms, wait_low 1,157 ms, crc_fail=0, attempts=1; one frame
  saved byte-exact (1,372,988 B), log_laps=0.
- A0's production goal is met (bulk 35.7 s → ~4 s). What the short
  validation cannot show is stability across *sustained* frame traffic
  — that is exactly what the soak is for.
- Session totals so far on the fast re-arm: 1 frame + 923 bulk commands
  (on .59) + 160 spitests (on .60). The soak takes the frame count
  from 1 to 21.

## Acceptance criteria (from docs/spi-subsystem-research-2026-10-09.md §11
and docs/a0-rearm-investigation-prep-2026-11-02.md validation gate)

- 20 consecutive frames, each saved byte-exact.
- crc_fail = 0 and attempts = 1 on every bulk transfer.
- Parser-drops = 0 across the whole soak (realign recoveries are
  acceptable and expected if the spill recurs — log every offset).
- Runts < 1% of reads.
- log_laps = 0 in every heartbeat for the duration.

## Time budget

Each frame is ~3–4.5 min of bridged serial chunk time, so 20 frames
is **~60–90 min of bench time**, plus preflight. Plan it as one held
session (bench lock held throughout, per John's 2026-11-02 rule), not
as something to squeeze between other work.

## Preflight (in order, per standing bench rules)

1. Connectivity FIRST: `python3 host-agent/mqtt-bridge/bridge_health.py`
   — exit 0 (subscriber alive + telemetry fresh) before any sends.
2. Acquire the bench lock (`bench_lock.py`); if held, stop and report
   the holder.
3. Read the seal floor from the device heartbeat / SEAL log line and
   resync (`seal_client.py --resync <floor>`) before the first send.
4. Record starting state from telemetry: S3 fw version (heartbeat
   `fw=` field — do not assume; 0.4.21 was current on 2026-11-02 and
   a 0.4.22 decision was open), Himax fw (expect
   `watcher-himax-0.4.20.60-spi`, fingerprint `09d632f9fc904d1e`),
   and the cumulative counters: log_laps, log_dropped, crc_fail,
   parser-drops, parser-realign count.
5. Record the live bridge handler state: sha256 of
   `host-agent/mqtt-bridge/snaptest_handler.py`. (Checked 2026-11-03
   during prep: the live handler contains strict base64 validation
   and the `_png_walk_ok` CRC walk; a
   `snaptest-reassembler-hygiene.patch.DRAFT` also still exists on
   disk. Note which is authoritative in the run log before trusting
   soak saves either way.)

## Run

- 20 × snaptest frame transfers, back to back, sequencing each send
  on device-observable completion in telemetry — NOT on the local
  send_command waiter fully exiting (John's 2026-11-02 correction).
- Per frame, bracket the counters (pre/post) and log one row:

| # | bulk ms | wait_low ms | crc_fail | attempts | realigns (Δ) | parser-drops (Δ) | log_laps | log_dropped (Δ) | saved bytes | byte-exact |
|---|---------|-------------|----------|----------|--------------|------------------|----------|-----------------|-------------|------------|
| 1 |         |             |          |          |              |                  |          |                 |             |            |
| … |         |             |          |          |              |                  |          |                 |             |            |
| 20 |        |             |          |          |              |                  |          |                 |             |            |

Reference points from the .60 validation frame: bulk 4,026 ms,
wait_low 1,157 ms, saved 1,372,988 B. Frame byte size varies with the
captured image — byte-exactness against the device-reported size is
the check, not a fixed size.

## Pause / abort criteria (preserve evidence first, then stop)

- **Any parser-drop > 0** — this is the failure mode .60 was built to
  eliminate; a single drop means the scan missed a case. Preserve the
  Himax DIAG stream around it and stop; this becomes the .61 question
  (and the Claude review pack's §4 material).
- log_laps > 0 in any heartbeat — the ring-lap truncation mode is
  back; bracket which frame it landed in.
- Two consecutive chunk-send aborts (the 0.4.21 chunk-42 class: ring
  push lost a try-lock race, loud abort, no corrupt image). One abort
  is a data point — note the frame and continue; two in a row is a
  rate problem, stop and report.
- Realign offsets: any offset is a recovery, but record new offset
  values — off=64 (one drain burst) and off=9 are known; a much
  larger offset would say the spill source is bigger than measured.

## Close-out

- Restore any state changed during testing (bench rule, 2026-09-24);
  release the bench lock.
- Append the results table + verdict to
  `docs/a0-rearm-investigation-prep-2026-11-02.md` as the soak phase,
  mirroring how Phases 5/6 were recorded.
- If it passes clean, A0 is fully closed: the remaining queue is
  Track B (B1 mirror-queue fix before any native MQTT chunk traffic)
  and the SD audit build — both separate sessions, neither started
  by this soak.
