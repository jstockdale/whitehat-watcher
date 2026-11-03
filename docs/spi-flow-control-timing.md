# S3↔Himax SPI Flow Control — Timing Gaps & Assumptions

**Date:** 2026-10-05  
**Firmware:** S3 0.4.18.18 + Himax 0.4.20.28  
**Status:** Implemented, awaiting bench validation

## Design: S3-Paced Protocol

We control both ends. Instead of the Himax guessing the S3's timing with
fixed timeouts (whack-a-mole), the S3 now **guarantees** timing behavior
that the Himax can rely on.

### S3 Guarantees (0.4.18.18)

1. **No pre-read delay:** The S3 clocks responses immediately after the
   Himax signals ready (via TX DMA completion). The 4ms `spi_sync_tx_ready_()`
   delay is reduced to 1ms (covers Himax peek/CRC/DCache prep).

2. **1ms inter-command gap (bulk path):** After receiving an AVAILABLE
   response, the S3 waits 2ms before sending READ_CRC. This gives the Himax
   time to re-arm RX (needs 1.5ms).

3. **Scope:** The 2ms gap is currently ONLY in the bulk AVAILABLE→READ_CRC
   path (`spi_bulk_poll_available_()`). Other command paths (TEST_PATTERN,
   AT commands) do NOT have a guaranteed gap.

### Himax Assumptions (0.4.20.28)

1. **TX→RX turnaround:** 1500µs after TX DMA callback
   - Transfer time at 12MHz: `(len * 8) / 12` µs
   - Plus 1500µs: 1000µs for S3's guaranteed gap + 500µs safety margin

2. **Bulk feeder watchdog:** 1s timeout. If no READ_CRC within 1s of
   CMD_START, disarm the feeder. Prevents "rx buffer is full" flood when
   the S3's reset is lost.

## Known Gaps (Iterate Later)

### Gap 1: Non-bulk paths lack the S3 gap guarantee
The 2ms inter-command gap is ONLY in the bulk path:
- After AVAILABLE → before READ_CRC: 2ms (`spi_bulk_poll_available_()`)
- After READ_CRC → before next AVAILABLE: 2ms (v0.4.18.19, bulk loop)
- TEST_PATTERN commands: S3 sends back-to-back with no guaranteed gap
- AT commands (via SPI): No gap guarantee
- **Risk:** Himax may not re-arm in time for the next command
- **Mitigation:** The 1500µs Himax timer is conservative; TEST_PATTERN
  responses are small (fast transfer), so the Himax re-arms quickly.
- **Future:** Add a universal 1ms inter-command gap in `spi_send_cmd_()`,
  or implement per-path gaps.

### Gap 1a: Inter-chunk gap was missing (FIXED in 0.4.18.19)
**Found 2026-10-05:** After chunk 0 passed, chunk 1 failed. The 2ms gap
existed after AVAILABLE but NOT after READ_CRC. The S3 sent the next
AVAILABLE immediately after clocking chunk data; the Himax hadn't re-armed.
**Fix:** Added 2ms `vTaskDelay` after each successful `spi_bulk_read_chunk_()`
in the bulk loop. The bulk sequence is now:
AVAILABLE → 2ms → READ_CRC → 2ms → AVAILABLE → 2ms → READ_CRC → ...

### Gap 2: Himax prep time is estimated, not measured
The S3's 1ms `spi_sync_tx_ready_()` assumes Himax prep (ring peek + CRC16
+ DCache clean) takes < 1ms.
- **Measured?** No. Estimated at ~100µs worst-case for 4090 bytes.
- **Risk:** Under heavy Himax CPU load, prep might exceed 1ms.
- **Future:** The Himax could signal "ready" via a GPIO, or the S3 could
  poll a status register. For now, 1ms gives 10x margin over the estimate.

### Gap 3: No CS-edge detection
The Himax cannot detect when the S3 finishes clocking. The 1500µs timer
is anchored to the TX DMA callback (which fires BEFORE the S3 clocks).
- **Assumption:** S3 clocks immediately (no pre-delay) + transfer_time +
  1ms gap = Himax re-arms in the gap window.
- **Risk:** If the S3's SPI driver has unexpected latency, the Himax
  might re-arm too early (while S3 is still clocking) or too late
  (missing the next command).
- **Future:** If the DW SSI driver exposes CS status, use it for precise
  transfer-complete detection. Alternatively, add a dedicated "ready"
  GPIO from Himax to S3.

### Gap 4: Wall-clock timer resolution
`ma_get_time_us()` uses `xTaskGetTickCount() * portTICK_PERIOD_MS * 1000`.
- **Resolution:** Millisecond, not microsecond! The "1500µs" timer has
  ~1ms quantization error.
- **Impact:** The actual wait could be 1000µs to 2000µs (not 1500µs).
- **Mitigation:** The 500µs safety margin absorbs some quantization,
  but this is fragile.
- **Future:** Use a hardware timer (e.g., DW TIMERS) for true microsecond
  resolution. Or, accept the quantization and increase margins.

### Gap 5: Bulk feeder watchdog is a heuristic
The 1s feeder timeout disarms the bulk feeder if no READ_CRC arrives.
- **Why 1s?** Arbitrary. Long enough for normal bulk transfers (which
  take ~100ms for 1.2MB), short enough to recover from a lost reset.
- **Risk:** A very slow S3 (e.g., under heavy load) might take >1s
  between chunks, causing a false disarm.
- **Future:** The feeder should disarm on explicit CMD_STOP/CMD_RESET
  only, with the watchdog as a last resort. Or, the S3 should send
  periodic keepalives during long transfers.

## Timing Summary

| Parameter | Value | Source | Margin |
|-----------|-------|--------|--------|
| Himax TX→RX turnaround | 1500µs | Himax timer | 500µs over S3 gap |
| S3 inter-command gap (bulk) | 2000µs | S3 `vTaskDelay(2)` | 500µs over Himax need |
| S3 pre-clock delay | 1000µs | S3 `spi_sync_tx_ready_()` | 10x over Himax prep est. |
| Himax prep (peek/CRC/DCache) | ~100µs (est.) | — | Not measured! |
| Bulk feeder watchdog | 1000ms | Himax timer | Arbitrary |

## Validation Plan

1. **Spitest (16/16):** Verifies basic SPI TX/RX turnaround for small
   transfers (256B to 4090B). Must pass before bulk testing.
2. **Single PNG:** Verifies the bulk AVAILABLE→READ_CRC path with the
   2ms S3 gap and 1500µs Himax timer.
3. **Whole-frame SHA256:** Verifies data integrity across all chunks.
4. **Stress:** Multiple back-to-back PNGs to catch race conditions.

## If It Fails

- **Spitest fails:** The 1500µs Himax timer is wrong for the TEST_PATTERN
  path (which lacks the S3 gap guarantee). Increase to 2000µs or add a
  gap in the S3 TEST_PATTERN path.
- **Bulk CRC fails at chunk 0:** The S3's 1ms pre-clock delay is
  insufficient for Himax prep. Increase `spi_sync_tx_ready_()` or
  measure the actual Himax prep time.
- **Bulk CRC fails at chunk N>0:** The 2ms inter-command gap is
  insufficient, or the Himax timer quantization (Gap 4) caused an
  early/late re-arm. Increase the S3 gap to 3ms.
- **"rx buffer is full" returns:** The feeder watchdog (1s) is not
  firing, or the S3 is genuinely flooding. Check if CMD_RESET is
  being lost (Himax not re-armed when S3 sends it).
