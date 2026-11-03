# SPI Link Debug Summary — 2026-10-05

## Problem Statement
The S3 (ESP32-S3) cannot reliably receive bulk data from the Himax (HX6538) over SPI.
Two failure modes observed:
1. **0xFF fill**: S3 receives all `0xFF` — Himax not driving MISO (TX startup latency)
2. **0x05 fill**: S3 receives `0x05` — Himax timed out and re-armed RX mid-transfer

## Architecture
- **S3**: SPI master, 12MHz, sends commands, clocks response data
- **Himax**: SPI slave (DesignWare), half-duplex (TX *or* RX, not both simultaneously)
- **Protocol**: Command phase (S3→Himax) → Data phase (Himax→S3), strictly sequential
- **Flow control**: Himax uses a timeout to switch from TX back to RX mode

## Key Insight: Timeout Must Be Shorter Than S3 Pace
The Himax DW SPI slave cannot do full-duplex. After TX DMA completes, it must
switch back to RX mode via a timeout. This timeout MUST satisfy:
```
pre_clock < timeout_offset < pre_clock + inter_cmd_gap
```
Where:
- `pre_clock`: S3 delay between command and data clocking (for Himax TX setup)
- `inter_cmd_gap`: S3 delay between data end and next command
- `timeout_offset`: Himax timeout beyond transfer_time

If timeout is too long: Himax stays in TX mode, S3's next command is lost → RX flood → WDT.
If timeout is too short: Himax re-arms RX mid-transfer → S3 reads garbage (0x05).

## Change History

### S3 Firmware

| Version | Change | Spitest | PNG/Bulk | Notes |
|---------|--------|---------|----------|-------|
| 0.4.18.22 | 1ms READ_CRC pre-clock, 2ms TEST_PATTERN, 2ms gaps | 16/16 | FFs | Baseline: differentiated delays work |
| 0.4.18.23 | (minor) | 16/16 | — | — |
| 0.4.18.24 | Chunk 4090→4086 | 16/16 | FFs | Chunk size not the issue |
| 0.4.18.25 | 16 dummy bytes, chunk 4074 | 16/16 | FFs | Dummy bytes fundamentally broken: Himax doesn't drive MISO at all during dummy clocks |
| 0.4.18.26 | `cs_ena_pretrans=192` (16µs HW), no dummy, chunk 4090 | 16/16 | FFs | HW delay doesn't fix: Himax needs >1ms, not 16µs |
| 0.4.18.27 | Removed 2ms gaps | — | **CRASH** | WDT: Himax (100ms timeout) stayed in TX, S3 flood with 0xE9 |
| 0.4.18.28 | READ_CRC pre-clock 1ms→2ms | 16/16 | **0x05s** | 2ms causes Himax timeout mid-transfer (4.3ms < 4.7ms) |

### Himax Firmware

| Version | Change | Result | Notes |
|---------|--------|--------|-------|
| 0.4.20.29 | Pop fix (`_spi_tx_pop_len`) | Working | Baseline: spitest 16/16 with S3 0.4.18.26 |
| 0.4.20.30 | Timeout 1500µs→100ms | **CRASH** | Himax never re-arms RX → S3 RX flood → WDT → boot loop |
| 0.4.20.31 | Timeout 100ms→2500µs (stale build, didn't recompile) | Boot loop | Build system didn't recompile .cpp; ran 100ms code |
| 0.4.20.32 | Recompiled with 2500µs | spitest 0/16 | Timeout too long for small packets: len=16, 2510µs > S3 next-cmd 2020µs |
| 0.4.20.33 | Timeout 2500µs→1500µs | spitest 16/16 | Correct: 1510µs < 2020µs (len=16); 4226µs < 6726µs (len=4090) |

## Root Causes Identified

### 1. TX Startup Latency (0xFFs)
The Himax's SPI slave TX DMA needs >1ms after the command to be ready to drive MISO.
- TEST_PATTERN works with 2ms (Himax generates data, takes time)
- READ_CRC fails with 1ms (Himax peeks from ring, fast, but SPI peripheral not ready)
- `cs_ena_pretrans` (16µs) is insufficient; the delay needs to be milliseconds, not microseconds
- Dummy bytes don't work: the Himax doesn't drive MISO *at all* during the dummy clocks

### 2. Timeout Coupling (0x05s)
The Himax timeout is measured from DMA completion, but the S3's pre-clock delay
eats into the budget:
- S3 0.4.18.28: 2ms pre-clock + 2.7ms transfer = 4.7ms total
- Himax 0.4.20.33: timeout = 4.2ms from DMA completion (t=0.1ms) = 4.3ms from command
- 4.3ms < 4.7ms → Himax re-arms RX while S3 is still clocking → 0x05 garbage

### 3. Build System Staleness
The Himax `make` doesn't always detect .cpp changes. `touch` + `make` is required.
0.4.20.31 was delivered with stale code (100ms timeout) despite source saying 2500µs.

## Current Stable Pairing
- **S3 0.4.18.26**: cs_ena_pretrans, 1ms READ_CRC, 2ms gaps, chunk 4090
- **Himax 0.4.20.33**: pop fix, timeout=transfer_time+1500µs
- **Result**: spitest 16/16, no crashes, PNG gets FFs (TX startup issue unresolved)

## Root Cause Analysis: The 05s Pattern (2026-10-05)

**Evidence:**
- Himax DIAG proves real data is staged: `E DIAG READ_CRC data: 40 43 43 41...`
- S3 receives all 05s: `[0000]: 05 05 05 05...`
- 0.4.18.18 (almost worked): S3 got `FF -> valid data` (started early, Himax still in TX)
- Current: S3 gets `05s` (Himax NOT in TX mode when S3 clocks)

**The ms-resolution timer problem:**
- `ma_get_time_us()` = `xTaskGetTickCount() * portTICK_PERIOD_MS * 1000`
- Resolution: **1 millisecond**, not microsecond!
- The "5226us" timeout has ±1000us quantization error
- S3 has RTOS jitter (±1ms on vTaskDelay)
- Combined error: ±2ms on a 5.2ms timeout = 40% uncertainty!

**Why 0.4.18.18 almost worked:**
- S3 pre-clock: 1ms (too short, Himax FIFO not ready)
- S3 got FFs (empty FIFO) then valid data (Himax still in TX mode, hadn't timed out yet)
- The Himax timeout (4226us) hadn't fired because the S3 was fast

**Why current fails:**
- S3 pre-clock: 2ms (longer, gives Himax time)
- But Himax timeout (5226us) fires early due to quantization
- Himax re-arms RX mid-transfer, FIFO drains, S3 gets 05s (empty FIFO default)

**The fundamental issue:** Timeout-based flow control with ms-resolution timers and RTOS jitter is inherently fragile. We need event-driven, not time-driven.

**Solutions (in order of preference):**
1. **GPIO ready signaling** (Phase 2): Himax drives GPIO when TX FIFO ready. S3 waits for GPIO. Requires hardware (investigating).
2. **CS-edge detection**: Himax detects S3 CS deassertion (transfer done). Event-driven, no guessing. Requires hardware support.
3. **Hardware microsecond timer**: Use DWT cycle counter or hx_drv_timer for true us resolution. Better guess, but still a guess.
4. **Longer timeout**: Increase margin to cover worst-case. Simple, but slow and still fragile.

## GPIO Research (In Progress)
Agent investigating hardware connections between Himax HX6538 and ESP32-S3 for flow control signaling.

## Current Stable Pairing (Updated)
- **S3 0.4.18.30** (pending): 2ms pre-clock, 2ms gaps restored, 64-byte diagnostic
- **Himax 0.4.20.35**: pop fix, AVAILABLE `_spi_tx_pending_len=2`, adaptive timeout
- **Expected**: Full PNG transfer with real data, no FFs, no 0x05s

## Next Steps
1. Fix TX startup: Himax needs >1ms, S3 gives 1ms (FFs) or 2ms (timeout). Need to decouple.
   - Option A: Increase Himax timeout to cover 2ms pre-clock (but breaks small packets)
   - Option B: Make Himax timeout command-aware (different for TEST_PATTERN vs READ_CRC)
   - Option C: Phase 2 GPIO READY signal (proper fix, deferred)
2. Instrument: S3 should log leading 0xFF count per chunk to measure actual TX latency
3. Once stable: Remove 2ms gaps for speed (requires Himax timeout redesign)
