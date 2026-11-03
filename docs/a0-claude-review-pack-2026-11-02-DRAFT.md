# A0 Claude Review Pack — DRAFT (prepared 2026-11-02, background prep, NOT sent)

Purpose: John reminded the bench session on 2026-11-03 ~03:02 UTC that
Claude (wh-link) is available to review A0 issues if the v0.4.20.58 build
throws something unforeseen. This pack is pre-drafted so that, if that
happens, John only has to (1) fill in the results blanks in §3 from the
DIAG output, (2) attach the files listed in §5, and (3) paste §2 onward
to Claude. Nothing here has been sent to Claude; sending is John's relay,
as always.

GOTCHA UP FRONT (for John): .58 is instrumentation-only — no probe result
changes any behavior, and Guard 1's 25/30 ms timer still does all the
re-arming. So a weird .58 result can confuse us, but it cannot break the
link by itself. If something *does* break on .58, the first question for
Claude is §4-D, not the signal questions.

---

## §1 — When to use this pack

- Any probe in .58 errors out, never clears, or shows `not_clear > 0`.
- The four signals disagree with each other (e.g. BUSY clears at ~1 ms
  but INT_STATUS bit 11 is preset/sticky on most responses).
- A signal looks perfect on spitest but degrades during bulk/soak.
- Or simply: the .58 distributions are ambiguous against the .59
  decision rule in §2, and we want a second read before designing .59.

## §2 — Paste-ready text for Claude (fill §3 blanks first)

> Claude — follow-up on the Watcher S3↔Himax SPI link you reviewed
> before (you're our standing reviewer on this link). Quick context,
> then a bounded set of questions. We are NOT asking you to design the
> fix yet; we want your read of the evidence and of our decision rule.
>
> **Where we are.** The S3 side (0.4.23) now measures the Himax re-arm
> toll directly: over two verification bulk transfers, wall = 27,414 /
> 27,599 ms, and `wait_low` (S3 waiting for the Himax to re-arm RX) =
> 24,422 / 24,590 ms — 89% of the wall. Runt reads are eliminated,
> crc_fail = 0. Guard 1 on the Himax waits a fixed 25 ms (responses
> ≤64 B) / 30 ms (>64 B) after TX-done before re-arming RX; that timer
> is the toll. Projected prize if a real transfer-complete signal can
> replace it: bulk ≈ 3 s.
>
> **History you may remember.** v0.4.20.41 tried status-based re-arm via
> direct DW SSI register reads (TXFLR base+0x20, SR base+0x28 bit0) and
> v0.4.20.43 reverted it — the reads bus-faulted during bulk. v0.4.20.52
> (your Q1) caches the SSI base via `SPI_CMD_GET_SPI_BASE_REG`
> (0x50810000) "for future register reads," but we are treating raw MMIO
> as a proven-failed path and are NOT retrying it as a first move.
> Also: Guard 1's code comment calls the timer an "interim bridge until
> Tests A/B." We cannot find a definition of Tests A/B in any doc on
> disk — if they came from your Q4 exchange, please supply the original
> definition (Question 1).
>
> **What .58 does (instrumentation only, zero behavior change).**
> FW ID `watcher-himax-0.4.20.58-spi`, build hash `1dd6b8dd44dfd122`
> (full SHA in the attached header). While `_spi_tx_done` is pending,
> each ~1 kHz `spi_poll()` pass samples four read-only, driver-mediated
> `spi_control` queries — no raw register reads, and
> `SPI_CMD_SET_XFERCB` is deliberately NOT probed (it could overwrite a
> callback the prebuilt driver uses internally):
> 1. `SPI_CMD_GET_BUSY_STATUS` (36) — first idle read
> 2. `SPI_CMD_GET_TXAVAIL` (22) — first read back at learned TX-FIFO
>    capacity (capacity is learned post-re-arm, FIFO empty by
>    construction)
> 3. `SPI_CMD_GET_INT_STATUS` (25) — first read with bit 11 (SSI Done)
>    set; "preset" (already set at first sample) counted separately
> 4. `SPI_CMD_GET_STATUS_REG` (42) — bit 0 (BUSY); the header notes this
>    one valid for a master polling build flag, so it is probed, not
>    trusted
>
> Aggregates are emitted as `REARM_STATS` / `REARM_LAT` DIAG lines, with
> verbatim `REARM_ONE` lines for the first 4 responses after boot and
> for any response where a live probe had NOT cleared by the time
> Guard 1 fired (`not_clear` — the count we treat as safety-critical).
> A probe that returns an error disables itself after one DIAG note.
> Guards 2 (300 ms staged watchdog) and 3 (arm-failure escalation) are
> untouched.
>
> **Our decision rule for .59 (please critique it — Question 2).**
> A signal qualifies to drive re-arm iff across spitest + 20-frame soak
> + bulk it (a) always clears, (b) worst-case clear latency ≪ 25 ms,
> and (c) `not_clear == 0`. The winner then arms RX after signal + a
> small fixed allowance (FIFO-residue wire time), with Guard 1's timer
> demoted to a fallback ceiling. Allowance and ceiling come from the
> .58 distributions, not from a guess.
>
> **§3 results go here when pasted** (see the table below this block in
> the pack file — copy the filled table into the paste).

### Questions

1. **Tests A/B.** Do you hold the original definition? If yes, does the
   .58 evidence gate above satisfy, supersede, or miss them?
2. **Decision rule.** Is "always clears + worst-case ≪ 25 ms +
   not_clear == 0" the right gate? Anything you would add — e.g. a
   minimum separation between signal-clear and the known-good 25/30 ms
   boundary, or a per-response-length split (≤64 B vs >64 B)?
3. **FIFO-vs-wire gap.** Our core worry: for a demand-paced slave DMA
   transfer, BUSY / TXAVAIL may clear when the FIFO drains, before the
   master has clocked the final bytes out — the same gap that made the
   DMA-done callback unusable. Do the .58 latencies in §3 let you tell
   whether a given signal tracks the wire or only the FIFO? What
   pattern in the data would distinguish them?
4. **Context safety.** Raw MMIO from the AT thread bus-faulted in .41.
   Is there any reason `spi_control` queries from `spi_poll()` (thread
   context, ~1 kHz) could hit the same class of problem — secure-world
   restrictions, driver re-entrancy, or disturbing the driver's own
   state machine? Any probe you would drop on that basis alone?
5. **Sticky INT_STATUS.** If bit 11 shows up "preset" on most responses,
   is that expected raw-status behavior (set at TX-done, cleared on
   read/arm), or does it disqualify GET_INT_STATUS as an edge? Is there
   a documented clear mechanism we should be using between responses?

## §3 — Results to fill in before sending (from .58 DIAG / telemetry)

| Item | Value |
|---|---|
| Build fingerprint (DIAG line at boot) | `1dd6b8dd44dfd122…` — confirm full hash from the flashed artifact |
| Spitest result | __ /16 |
| Soak result | __ frames, CRC fails __ |
| REARM_STATS line(s), verbatim | (paste) |
| REARM_LAT line(s), verbatim | (paste) |
| REARM_ONE `not_clear` examples, verbatim | (paste, or "none — not_clear = 0") |
| Any probe self-disabled (error note)? | yes/no — which probe, what error |
| S3 `bulk_stats` for the same runs (wall / wait_low) | (paste) |
| Anything else unforeseen (reboots, arm failures, Guard 2/3 trips) | (describe, with timestamps) |

## §4 — Contingent extra questions (add only the one that matches)

- **A. Signals disagree:** BUSY clears ~__ ms, TXAVAIL ~__ ms,
  INT_STATUS preset on __% — which do you trust, and what one extra
  measurement would settle it?
- **B. A probe errors/disables itself:** the error was __ on probe __.
  Driver limitation, wrong argument shape, or context problem?
- **C. Everything clears at ~1 ms, suspiciously fast:** is that the
  FIFO-vs-wire gap showing up (Q3), and would you accept a fixed
  allowance on top — how would you size it without guessing?
- **D. Something actually broke on .58** (reboot, lost transfer, Guard
  trips): since .58 changes no behavior, what interaction between
  read-only `spi_control` polling at ~1 kHz and the live transfer
  could cause this? Where would you instrument next?

## §5 — Files to attach when relaying

- `docs/a0-rearm-investigation-prep-2026-11-02.md` (baseline, history,
  .58 design and decision rule — the full context doc)
- `sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp`
  (the .58 working tree version, with the probe block and hooks)
- `sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/drivers/inc/hx_drv_spi.h`
  (the `SPI_CMD_*` definitions and header notes)
- The filled §3 table / raw DIAG excerpt (do NOT send the whole
  telemetry.log — it is ~44 MB; excerpt the REARM_* and bulk_stats
  lines only)

## Constraints Claude should be told (already in §2, restated)

- No raw-MMIO retry as a first move (.41/.43 fault is settled history).
- Guards 2 and 3 stay untouched.
- SET_XFERCB stays unprobed until its internal use is understood.
- Timer ladder (25/30 ms downward, spitest + soak gated) is the last
  fallback, only if the signal path provably does not exist.
