# SPI Link Debug: Phase 2 GPIO Signaling Issues

**Date:** 2026-10-05/06
**Author:** Muse (for John Stockdale)
**Purpose:** Detailed writeup for independent review — diagnostics and fixes needed

## Hardware

**Board:** Seeed Studio SenseCAP Watcher
- **Main MCU:** ESP32-S3 (master, SPI at 12MHz, Mode 0)
- **Co-processor:** Himax HX6538 WE2 (slave, Synopsys DesignWare DW_apb_ssi)
- **SPI Connections:**
  - CS: ESP_GPIO21 → Himax PB5
  - SCK: ESP_GPIO4 → Himax PB4
  - MOSI: ESP_GPIO5 → Himax PB3
  - MISO: ESP_GPIO6 → Himax PB2
- **UART:** Himax PB6/PB7 ↔ ESP_GPIO17/18 (for AT commands, logging)
- **I2C:** Shared bus (ESP_GPIO47/48) with PCA9535 expander @ 0x21

**Phase 2 Signaling Path (NEW):**
```
Himax PB8/INT (L1) → D2 diode → R62 100R → PCA9535 P07 (pin 8)
  → PCA9535 INT (pin 22, open-drain, active-low) → ESP_GPIO2
```
- Himax drives PB8 HIGH when TX ready, LOW when busy
- S3 polls P07 via I2C (or waits for GPIO2 interrupt) before clocking
- Purpose: Replace timing guesses (2ms pre-clock delay) with event-driven signaling
- Latency: ~100µs (I2C read) vs 2000µs (old delay)

## Current Firmware

- **S3:** 0.4.18.34
  - `spi_sync_tx_ready_()` polls PCA9535 P07 via `esp_io_expander_get_level()`
  - Back-to-back I2C reads (~100µs each), timeout 2ms (20 polls)
  - Falls back to `vTaskDelay(2ms)` on timeout
  - Location: `firmware/components/sscma_client/src/sscma_client_io_spi.c`
- **Himax:** 0.4.20.37
  - Configures PB8 as GPIO2 output in `SPI::init()`
  - Drives PB8 HIGH after `spi_write_dma()` succeeds (4 sites: READ, READ_CRC, TEST_PATTERN, AVAILABLE)
  - Drives PB8 LOW in `_spi_arm_rx()`
  - Location: `sscma-fork/.../porting/himax/we2/ma_transport_spi.cpp`

## The Problem

**Spitest should be 16/16 (baseline when working). Currently 6/16.**

Spitest sends TEST_PATTERN commands with varying lengths (16, 256, 1000, 4090 bytes) and 4 patterns, verifying CRC.

### Latest Run (2026-10-06 02:41:31, S3 0.4.18.34 + Himax 0.4.20.37)

| Pattern | Len 16 | Len 256 | Len 1000 | Len 4090 |
|---------|--------|---------|----------|----------|
| pat=0   | PASS   | PASS    | PASS     | FAIL     |
| pat=1   | FAIL   | FAIL    | PASS     | FAIL     |
| pat=2   | FAIL   | PASS    | PASS     | FAIL     |
| pat=3   | FAIL   | FAIL    | FAIL     | FAIL     |

**Failure details:**
- `pat=0 len=4090`: Got `a3a3...` (entire 4090 bytes), expected `ebdb` CRC
  - `rx[0..15]: a3 a3 a3 a3 a3 a3 a3 a3 a3 a3 a3 a3 a3 a3 a3 a3`
- `pat=1 len=16`: Got `a3a3`, expected `d59b`
- `pat=1 len=4090`: Got `ffff`, then `f7 f7 f7...` then `ff ff...`
  - `rx[0..15]: f7 f7 f7 f7 f7 f7 f7 f7 f7 f7 f7 f7 f7 f7 f7 f7`
  - `rx[4074..4089]: ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff`

### PNG/Bulk Transfer

With 0.4.18.31 (polling version, before interrupt):
- Chunk 1: `ff_lead=1005` (1005 FFs then data)
- Chunk 2: `ff_lead=0` (PERFECT! No FFs, data `ba ba ba...`)
- Chunk 3: `ff_lead=1034` (1034 FFs then data)

The 05s pattern (from earlier) is GONE. Now we get FFs→data (like 0.4.18.18) or perfect transfers.

## Root Cause Hypothesis

**PB8 is driven too early — on DMA *arm*, not DMA *complete*.**

In the Himax code, `hx_drv_gpio_set_out_value(GPIO2, GPIO_OUT_HIGH)` is called immediately after `spi_write_dma()` returns. But `spi_write_dma()` only *arms* the DMA; the actual transfer from RAM to the SPI TX FIFO happens asynchronously.

The S3 sees PB8 HIGH, polls P07, sees HIGH, and starts clocking immediately. But the FIFO isn't filled yet. The S3 clocks out whatever is in the FIFO (stale `a3` or `f7` data, or FFs if empty).

**Evidence:**
- Small transfers (16, 256 bytes) usually PASS: DMA fills the small FIFO quickly (<100µs), before the S3's I2C poll completes.
- Large transfers (4090 bytes) FAIL: DMA takes longer to fill the 4090-byte FIFO. The S3 sees PB8 HIGH (DMA armed) and clocks before the FIFO is ready. Gets `a3a3...` (stale FIFO content).
- Chunk 2 of PNG was perfect (`ff_lead=0`): The timing happened to align (Himax DMA finished before S3 polled).

**The fix:** Drive PB8 HIGH in `_spi_tx_callback()` (fires when DMA completes, FIFO filled), not in the command handler (when DMA is armed).

## What We've Tried

1. **0.4.18.30 + 0.4.20.35 (timing guesses):** 2ms pre-clock delay, 2ms gaps. Got 05s (Himax timed out early due to ms-resolution timer + RTOS jitter).
2. **0.4.18.31 (polling):** S3 polls P07 every 100µs for 10ms. Got 6/16 spitest, PNG chunk 2 perfect. Proved the hardware path works.
3. **0.4.18.32 (interrupt):** GPIO2 ISR + semaphore. Got 3/16 (worse!). The interrupt chain (expander INT → GPIO2 → ISR → semaphore → task → I2C read) has too much latency; misses short PB8 pulses.
4. **0.4.18.33 (interrupt + check-first):** Check P07 before waiting on semaphore. Still 3/16. The fundamental latency issue remains.
5. **0.4.18.34 (optimized polling):** Back-to-back I2C polls, 2ms timeout. Got 6/16 (same as 0.4.18.31). Better than interrupt, but still not 16/16.

## Key Code Locations

**Himax (ma_transport_spi.cpp):**
- PB8 init: `SPI::init()` ~line 570: `hx_drv_scu_set_PB8_pinmux(SCU_PB8_PINMUX_GPIO2_1, 1)` + `hx_drv_gpio_set_output(GPIO2, GPIO_OUT_LOW)`
- PB8 HIGH: 4 sites after `spi_write_dma()` succeeds (lines ~329, 410, 458, 483)
- PB8 LOW: `_spi_arm_rx()` ~line 180: `hx_drv_gpio_set_out_value(GPIO2, GPIO_OUT_LOW)`
- TX callback: `_spi_tx_callback()` ~line 555: Sets `_spi_tx_done = true` (PB8 should go HIGH here, not in handler!)

**S3 (sscma_client_io_spi.c):**
- `spi_sync_tx_ready_(xfer_len, delay_ms)` ~line 776: Polls P07 or waits for interrupt
- Called from `spi_bulk_read_chunk_()` (line 846) and TEST_PATTERN path (line 932)
- Expander handle: `eyes_io_expander()` from eyes component

## Questions for Review

1. **Is the PB8 timing hypothesis correct?** Should PB8 go HIGH in `_spi_tx_callback()` instead of the command handler?
2. **Is there a way to detect FIFO full?** Does the DW SPI driver expose TX FIFO status?
3. **Why does pat=1 len=4090 get `f7` then `ff`?** The `f7` suggests partial data, then FFs (FIFO empty). Is the DMA being cancelled?
4. **Should we keep PB8 HIGH longer?** Currently driven LOW in `_spi_arm_rx()`. Should it stay HIGH until the next command?
5. **Is the diode D2 causing issues?** The agent noted 0.3V drop. Could this cause marginal HIGH detection?
6. **Any other GPIO options?** The agent found PB8 is the only viable pin (WLCSP package, can't bodge).

## Logs

Full telemetry: `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/telemetry.log`
- Spitest run: `2026-10-06 02:41:31`
- PNG run: `2026-10-06 02:41:xx` (after spitest)

Himax boot: `E DIAG watcher-himax-0.4.20.37-spi bdb53f70a364ed94`
S3 version: `fw=0.4.18.34`

## Desired Outcome

Spitest 16/16 (baseline). Then PNG with consistent `ff_lead=0` (no leading FFs) and valid CRCs. Then we can remove the 2ms gaps and optimize for speed (Phase 2 goal: sub-second full-frame transfers).
