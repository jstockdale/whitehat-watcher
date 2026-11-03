# ESP32-S3 SPI Subsystem — Speed Optimization Deep Dive
**Date:** 2026-10-09 · **For:** Track A implementation (unified plan)
**Sources:** `firmware/components/sscma_client/src/sscma_client_io_spi.c` (0.4.18.88, via stash),
`firmware/docs/s3-spi-sscma-implementation.md`, `firmware/docs/spi-flow-control-timing.md`,
`firmware/docs/spi-debug-summary-2026-10-05.md`, `firmware/SPI_PHASE_PLAN.md`,
Himax `ma_transport_spi.cpp` (sscma-fork tree), `spidump.hpp`.

---

## 1. Hardware Architecture

### S3 Side (Master)
| Parameter | Value | Source |
|---|---|---|
| SPI host | SPI2_HOST | eyes.c:256 |
| Clock | **12 MHz** | eyes.c:266 (`.pclk_hz = 12*1000*1000`) |
| Mode | 0 (CPOL=0, CPHA=0) | io_cfg |
| DMA | `SPI_DMA_CH_AUTO`, internal staging buffer | io_spi.c |
| Max transaction | `spi_bus_get_max_transaction_len()` (~4092 B with DMA) | io_spi.c:142 |
| `cs_ena_pretrans` | 32 (hardware max, ≈2.8 µs @ 11.43 MHz real) | stash diff |
| Queue size | 1 | dev_config |
| Worker task | Priority 15, atomic transactions | stash (v0.4.18.53) |

**Theoretical ceiling at 12 MHz:** 12 Mbit/s = 1.5 MB/s → 1.26 MB in **~0.84 s**.
Current measured: **~35 s** (2.4% efficiency). The gap is all protocol overhead.

### Himax Side (Slave)
| Parameter | Value | Source |
|---|---|---|
| Peripheral | DesignWare SPI slave 0 (`USE_DW_SPI_SLV_0`) | ma_transport_spi.cpp |
| Duplex | **Half-duplex** (TX *or* RX, never both) | debug summary |
| TX ring (`_spi_rb_tx`) | **4096 bytes** | ma_transport_spi.cpp:183 |
| TX staging (`_spi_tx_buf`) | 4096 bytes | ma_transport_spi.cpp:175 |
| Feeder | `ma::spi_poll()`, AT-server thread, ~1 kHz | implementation.md |
| TX→RX turnaround | `transfer_time + 1500 µs` timeout | AGENTS.md (John's rule) |

**Critical constraint (John's Himax rule):** The DW slave is half-duplex. After
TX DMA completes, it switches back to RX via timeout. The timeout MUST be
shorter than the S3's minimum inter-command time:
- Correct: `transfer_time + 1500 µs` (0.4.20.33)
- For len=4090: 4226 µs < S3's 5726 µs inter-command gap ✓
- For len=16: 1510 µs < 2020 µs ✓

### MISO-Flag Handshake (v0.4.18.35)
MISO (GPIO6) is the ready signal:
- **LOW** = Himax RX armed (safe to send command)
- **HIGH** = Himax TX ready (safe to clock response)

`spi_miso_wait_level_(level, timeout_ms, what)` polls `gpio_get_level(GPIO_NUM_6)`
with `vTaskDelay(1)` per iteration. **Each iteration costs ≥1 ms** (1 FreeRTOS tick).

---

## 2. Bulk Transfer Protocol (Current)

```
S3                          Himax
──                          ─────
AT+SPIDUMP ───────────────►  (captures frame, stages 1.26 MB buffer,
                             replies {len, sha256})
CMD_START ────────────────►  (arms feeder: pushes frame → TX ring)
loop:
  CMD_AVAILABLE ──────────►
  ◄──── MISO HIGH ─────────  (TX ready)
  ◄──── 2-byte count ──────  (ring size, big-endian)
  [if count==0: sleep 100ms, retry up to 30×]
  CMD_READ_CRC(len) ──────►  (len ≤ 4090)
  ◄──── MISO HIGH ─────────  (up to 2 ms wait)
  ◄──── len+2 bytes ───────  (data + CRC16)
  [verify CRC16-CCITT]
  [2 ms inter-chunk gap]
CMD_STOP ─────────────────►  (disarms feeder)
```

### Per-Chunk Timing Breakdown (4090-byte chunk)

| Step | Cost | Notes |
|---|---|---|
| CMD_AVAILABLE send | ~0.2 ms | 256-byte frame @ 12 MHz + MISO wait-LOW |
| MISO wait-HIGH | ~0.1–1 ms | `vTaskDelay(1)` per poll; fast if Himax ready |
| Clock 2-byte count | ~0.01 ms | negligible |
| CMD_READ_CRC send | ~0.2 ms | 256-byte frame |
| TX-ready wait | ~0.5–2 ms | `spi_sync_tx_ready_()`, up to 2 ms timeout |
| Clock 4092 bytes | **~2.7 ms** | 4092×8/12 MHz |
| CRC16 verify | ~0.05 ms | CPU, negligible |
| Inter-chunk gap | **2.0 ms** | v0.4.18.19 fix (Himax re-arm) |
| **Total (no backoff)** | **~5.5–7.5 ms** | |

**Best case for 1.26 MB (308 chunks):** 308 × 6 ms ≈ **1.8 s**.
**Measured:** ~35 s. The difference is the 100 ms backoff firing constantly.

---

## 3. The 100 ms Backoff — Root Cause of 35 s

### Where It Lives
`spi_bulk_poll_available_()` (stash, ~line 578 in diff):
```c
for (int i = 0; i < BULK_POLL_RETRIES; i++) {   // 30 retries
    spi_send_cmd_(AVAILABLE);
    spi_miso_wait_level_(1, 50, ...);            // 50 ms timeout
    clock 2-byte count;
    if (avail > 0) return ESP_OK;                // fast path
    vTaskDelay(pdMS_TO_TICKS(100));              // ← THE BACKOFF
}
return ESP_ERR_TIMEOUT;                          // after 30×100 ms = 3 s
```

### Why It Exists
When the S3 drains faster than the Himax feeder refills the 4 KB TX ring,
AVAILABLE returns 0. The 100 ms sleep gives the feeder time to catch up.
It's backpressure, but **far too coarse**.

### Why It's Killing Performance
Math: 1.26 MB / 35 s = **36 KB/s**. At 4090 bytes/chunk, that's ~9 chunks/s,
or **~110 ms per chunk** — suspiciously close to the 100 ms backoff.

**Hypothesis:** Nearly every AVAILABLE poll hits an empty ring, incurring
a 100 ms sleep. 308 chunks × 100 ms = **30.8 s** ≈ the observed 35 s.

**Why is the ring always empty?** The feeder runs in `ma::spi_poll()` at
~1 kHz in the AT-server thread. Possible causes:
1. The AT-server thread is blocked or slow (it also handles AT commands)
2. The feeder's push rate can't sustain the S3's drain rate
3. The 4 KB ring is too small to absorb the feeder/drainer rate mismatch

**A1 fix:** Replace 100 ms flat backoff with escalating:
```c
// 1 ms × 20, then 10 ms × N
for (int i = 0; i < 20; i++) {
    if (poll_available() > 0) return ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(1));
}
// then 10 ms polls up to the 3 s total budget
```
This alone should take 35 s → single digits: if the feeder needs ~5 ms
to refill, we wait 5 ms instead of 100 ms.

**Instrumentation (Step 0.3):** Add `zero_avail_sleeps` counter to confirm.
If A1 works, this counter shows many 1–5 ms sleeps and zero 100 ms sleeps.

---

## 4. Runt Chunks — The 4090/5-Byte Alternation

### What Happens
The drain loop does:
```c
uint16_t want = min(size - done, avail, BULK_MAX_CHUNK);  // 4090
```

If the Himax reports `avail = 5` (feeder just started, ring nearly empty),
the S3 issues READ_CRC for **5 bytes**. Next poll, `avail = 4090`, reads
full chunk. Then `avail = 5` again. The pattern alternates.

### Why Runts Are Expensive
Each chunk — regardless of size — pays the full protocol overhead:
- CMD_READ_CRC send: ~0.2 ms
- TX-ready wait: ~0.5–2 ms
- 2 ms inter-chunk gap
- MISO waits, CRC setup

A 5-byte runt costs ~4 ms of overhead for 5 bytes = **1250 bytes/s**.
A 4090-byte chunk costs ~5.5 ms for 4090 bytes = **743 KB/s**.
**Runts are ~600× less efficient.**

### A2 Fix
Never issue READ_CRC until `avail >= min(remaining, 4090)`:
```c
// In the drain loop, after poll_available:
uint16_t threshold = (size - done) < BULK_MAX_CHUNK ?
                     (uint16_t)(size - done) : BULK_MAX_CHUNK;
if (avail < threshold) {
    // Don't read a runt; wait for the feeder (bounded retries)
    vTaskDelay(pdMS_TO_TICKS(1));
    continue;  // re-poll
}
```

**Bounded:** If the feeder is stuck, this loops forever. Add a retry
counter (e.g., 3000 × 1 ms = 3 s, matching the existing 3 s budget).

**Edge case — true tail:** The last chunk of the frame may legitimately
be < 4090 bytes. The `min(remaining, ...)` handles this: if only 100 bytes
remain, threshold = 100, and we read when `avail >= 100`.

**Expected:** With A1+A2, the plan predicts **≤ 2.0–2.8 s** for the full frame.

---

## 5. MISO Wait Polling — The 1 ms Tax

### Current
`spi_miso_wait_level_()`:
```c
while ((xTaskGetTickCount() - start) < timeout_ticks) {
    if (gpio_get_level(GPIO_NUM_6) == target_level) return true;
    vTaskDelay(1);  // ← 1 ms minimum per iteration
}
```

Every MISO wait costs **≥1 ms**, even if the Himax responds in 50 µs.
MISO waits happen **3× per chunk**:
1. Before clocking AVAILABLE count ("BULK AVAILABLE HIGH", 50 ms timeout)
2. Before clocking READ_CRC data ("TX-ready HIGH", 2 ms timeout via `spi_sync_tx_ready_`)
3. Implicit in `spi_send_cmd_` wait-LOW gating

That's **≥3 ms per chunk** in pure polling overhead = ~0.9 s for 308 chunks.

### A3 Fix: Hybrid Poll
```c
// First ~600 µs: tight busy-poll with esp_rom_delay_us(20)
// Then: 1-tick yield (current behavior)
uint32_t start_us = esp_timer_get_time();  // or xTaskGetTickCount for ms
while (!timeout) {
    if (gpio_get_level(GPIO_NUM_6) == target_level) return true;
    if ((esp_timer_get_time() - start_us) < 600) {
        esp_rom_delay_us(20);  // ~30 iterations in 600 µs
    } else {
        vTaskDelay(1);
    }
}
```

**Why 600 µs?** The Himax TX DMA setup is typically 100–500 µs (from the
debug history: "Himax needs >1 ms for SPI TX setup" was for the old path;
the MISO-flag path is faster). 600 µs covers the fast case without
busy-spinning forever.

**Why not pure busy-poll?** The SPI worker runs at priority 15. A tight
busy-poll for the full 50 ms timeout would starve all lower-priority tasks
(including the IDLE task that feeds the watchdog). The hybrid approach
gets the latency win for the common case while staying responsive.

**Expected:** −2–6 ms/chunk → saves **~0.6–1.8 s** across 308 chunks.

**Caution:** `esp_rom_delay_us()` is a busy-wait. At priority 15, even
600 µs blocks everything below. This is acceptable (it's shorter than
the 1 ms tick), but don't extend it.

---

## 6. Himax TX Ring — The 4 KB Bottleneck (A4)

### Current
- `_spi_rb_tx`: 4096 bytes (ma_transport_spi.cpp:183)
- Feeder: `ma::spi_poll()` at ~1 kHz, pushes staged frame → ring
- S3 drains: up to 4090 bytes per READ_CRC

### The Math
S3 drain rate (after A1–A3): ~1.5 MB/s burst, ~700 KB/s sustained.
Feeder rate: 1 kHz × up to 4096 bytes = **4 MB/s max** (if ring is empty).

In theory the feeder can keep up. In practice, the 4 KB ring means:
- S3 reads 4090 bytes → ring nearly empty
- Feeder needs ~1 ms to refill 4 KB (at 1 kHz poll)
- S3's next AVAILABLE sees ~0 → backoff (even with A1's 1 ms, it's a stall)

**A4 option 1:** Grow TX ring 4096 → 16384.
- Gives the feeder 4× more headroom
- S3 can read 4× 4090-byte chunks before the ring empties
- **Heap check required first:** The Himax CM55 has limited SRAM. The
  plan says "heap check first."
- Also grow `_spi_tx_buf` (currently 4096) to match, since READ_CRC
  stages `[data][CRC16]` there.

**A4 option 2:** Refill-on-drain in READ_CRC completion.
- In the Himax READ_CRC handler, after popping `len` bytes for the S3,
  immediately trigger the feeder to refill (instead of waiting for the
  next 1 kHz `spi_poll()` tick).
- This is a Himax-side change in `ma_transport_spi.cpp`.
- Lower memory cost than growing the ring.

**Expected:** Either option should achieve `zero_avail_sleeps == 0`
and **≤ 1.5 s** total transfer time.

**Recommendation:** Try option 2 first (no memory cost). If the feeder
can't keep up even with immediate refill, grow the ring.

---

## 7. SPI Clock Speed — Headroom Analysis

### Current: 12 MHz
Theoretical: 1.5 MB/s → 0.84 s for 1.26 MB (ignoring all overhead).

### Can We Go Faster?
**ESP32-S3 master:** Supports up to 80 MHz (APB clock). The SPI driver
will divide down to the requested rate.

**Himax DW slave:** Unknown max. The DesignWare SPI slave IP typically
supports up to ~25 MHz, but the Himax integration (clock domain crossing,
GPIO slew rates, PCB traces) may limit it lower.

**F-S6 (deferred):** 20 MHz experiment.
- At 20 MHz: 2.5 MB/s → 0.5 s theoretical for 1.26 MB.
- Risk: signal integrity, Himax slave timing violations.
- The plan defers this until after A1–A4 soak.

**Recommendation:** Don't touch the clock until A1–A4 are measured.
If A1–A4 get us to ≤1.5 s, the 20 MHz experiment is optional (saves
~0.3 s best case). The protocol overhead (not the clock) is the
dominant cost.

### Why 12 MHz Was Chosen
From the debug history: the Himax TX startup latency and timeout
coupling were debugged at 12 MHz. Changing the clock changes all the
timing relationships (transfer_time in the timeout formula). It's a
separate experiment, not a free speedup.

---

## 8. Implementation Checklist (Track A)

### A1 — Escalating backoff (`sscma_client_io_spi.c`)
- [ ] In `spi_bulk_poll_available_()`: replace `vTaskDelay(pdMS_TO_TICKS(100))`
      with escalating: 1 ms × 20, then 10 ms
- [ ] Keep `BULK_POLL_RETRIES` total budget at ~3 s
- [ ] Add `zero_avail_sleeps` counter (Step 0.3 instrumentation)

### A2 — No runt reads (drain loop in `spi_txn_bulk_exec()`)
- [ ] After `spi_bulk_poll_available_()`, check `avail >= min(remaining, 4090)`
- [ ] If below threshold: 1 ms delay, re-poll (bounded: 3000 retries = 3 s)
- [ ] Add `runt_chunks` counter (should be 0 after fix, except true tail)

### A3 — Hybrid MISO poll (`spi_miso_wait_level_()`)
- [ ] First 600 µs: `esp_rom_delay_us(20)` tight loop
- [ ] After 600 µs: `vTaskDelay(1)` (current behavior)
- [ ] Verify no priority-15 starvation (600 µs < 1 ms tick, safe)

### A4 — Himax feeder (Himax firmware, `ma_transport_spi.cpp`)
- [ ] **Heap check:** can the TX ring grow 4 KB → 16 KB?
- [ ] Option 1: Grow `_spi_rb_tx` and `_spi_tx_buf` to 16384
- [ ] Option 2: Refill-on-drain in READ_CRC completion path
- [ ] `make cleanbuild` (John's rule: always full clean for Himax)
- [ ] Verify fingerprint is fresh (not stale)

### Acceptance (per plan)
- `zero_avail_sleeps` shows no 100 ms sleeps
- `runt_chunks == 0` (except true tail)
- `crc_retries == 0` across 20 frames
- Wall time: **≤ 2.8 s** (S3-only), **≤ 1.5 s** (with Himax)

---

## 9. Risks and Open Questions

1. **Feeder starvation:** If the Himax AT-server thread is blocked (e.g.,
   during a long AT command), the feeder stalls and the S3 hits the 3 s
   timeout. The 1 kHz poll rate assumes the AT thread is responsive.
   **Mitigation:** A1's escalating backoff tolerates brief stalls; the
   3 s abort stays loud.

2. **MISO-flag race:** The MISO HIGH signal means "TX ready," but there's
   a window between the Himax staging data and driving MISO. The
   `cs_ena_pretrans=32` (2.8 µs) covers the electrical setup, but if the
   Himax DMA hasn't finished, we clock zeros. **The CRC catches this**
   (all-zeros fails CRC), triggering a whole-transfer retry. This is
   correct but expensive. If CRC retries are frequent, increase the
   TX-ready wait.

3. **Worker priority 15:** The SPI worker runs above most app tasks.
   During a 2 s bulk transfer, it holds the SPI lock and mostly blocks
   on MISO waits (yielding via `vTaskDelay`). This is by design (v0.4.18.53:
   "AT txns queue behind bulk and timeout cleanly"). Verify that
   telemetry heartbeats still go out during bulk.

4. **4 KB vs 16 KB ring on Himax:** The CM55M's SRAM budget is tight.
   Growing the ring by 12 KB might break other allocations. **Measure
   the heap before and after.** If it doesn't fit, option 2
   (refill-on-drain) is the fallback.

5. **20 MHz clock (F-S6):** Deferred. The signal integrity at 20 MHz
   over the Watcher's PCB traces is unknown. If attempted, validate
   with the spitest pattern (16/16) before running bulk.

---

## 10. Key Code References

| Item | File | Location |
|---|---|---|
| Bulk poll (100 ms) | `sscma_client_io_spi.c` | `spi_bulk_poll_available_()` |
| MISO wait (1-tick) | `sscma_client_io_spi.c` | `spi_miso_wait_level_()` |
| Drain loop | `sscma_client_io_spi.c` | `spi_txn_bulk_exec()` |
| Chunk read | `sscma_client_io_spi.c` | `spi_bulk_read_chunk_()` |
| TX-ready wait | `sscma_client_io_spi.c` | `spi_sync_tx_ready_()` |
| Tunables | `sscma_client_io_spi.c` | `BULK_*` defines |
| Himax TX ring | `ma_transport_spi.cpp` | `_spi_rb_tx` (4096 B) |
| Himax feeder | `ma_transport_spi.h` | `ma::spi_bulk_prepare()` |
| SPIDUMP | `spidump.hpp` | AT+SPIDUMP handler |
| SPI config | `eyes.c` | 12 MHz, SPI2_HOST |

---

*End of research. All code citations from the 0.4.18.88 tree (via stash
119622e) unless noted. The working tree is at an older state; the stash
contains the current firmware source.*


---

## 11. Addendum (2026-11-02) — Measured Re-Baseline: the 100 ms Theory Is Wrong

The Step 0 data (recovered after this report was written) overturns §3's
root-cause attribution and re-scores §8. The phenomenon in §4 is confirmed
and precisely explained; the bottleneck moved to the Himax side.

### Measured bulk wall (50 full-frame transfers, device uptime stamps)
Clean 1-attempt transfers from 2026-10-08 onward: **35.35–35.83 s, median
35.66 s**, essentially deterministic — a fixed per-round-trip toll, not
contention. (Oct 6–7 outliers of 38–85 s are the pre-stabilization regime.)

### Himax DIAG dataset (10,179 READ_CRC records, Himax v0.4.20.13+)
- `avail` at READ_CRC parse = **4095 in 99.1%** of records. The feeder is
  fast and the TX ring is effectively always full — §6's starvation model
  is wrong for the current stack.
- **54.2% of reads are runts** (< 4090) carrying 12.0% of bytes: an exact
  4090/5 alternation (4,662 full vs 4,272 5-byte reads).
- Runt mechanism (replaces §4's "feeder just started"): the AVAILABLE
  snapshot is taken in the same Himax command parse that *pops the
  previous chunk*, catching the ring at 4095 − 4090 = 5. The feeder
  refills within ~1 ms (avail is 4095 again by READ_CRC parse), but the
  S3 acts on the stale 5. A snapshot-ordering artifact, every cycle.

### Where the 35 s actually goes
Himax v0.4.20.52 Guard 1 ("interim bridge until Tests A/B"): after each
TX, RX re-arm waits **25 ms (responses ≤64 B) or 30 ms (>64 B)** from
DMA-done, evaluated in the ~1 kHz `ma::spi_poll()`; every S3 command
waits for it at the MISO-LOW gate. Per 4095-byte pair: 25 (AVAILABLE) +
30 (full read) + 25 (AVAILABLE) + 25 (runt read) = **105 ms × 308 pairs
= 32.3 s ≈ 91% of wall**. Real work is ~1–2 s; the residual ~1.5–2 s is
consistent with poll-loop overshoot across 1,232 re-arms. §3's math
(308 × 100 ms) fit only because 105 ≈ 100 — with 616 reads, a per-read
100 ms backoff would give ~62 s, and the backoff fires only on
`avail == 0`, which is rare (the poll returns timeout instead; the drain
loop's old zero-avail branch was unreachable, as the Oct 24 placement
audit found). This report was written from the S3 stash plus the
.29/.33-era `transfer_time + 1500 µs` rule; the Himax had already moved
through .41 (status re-arm, bus fault) → .43 (1 ms timer) → .52 (25 ms
guards). §2's per-chunk table is also stale: the 2 ms inter-chunk gap
was removed in S3 0.4.18.51.

### Re-scored plan (approved by John 2026-11-02)
- **A1** — demoted to hygiene. Real trap, not the 35 s cause. Landed in
  0.4.23 as an escalating 1 ms → 10 ms ladder under a 3 s budget.
- **A2** — validated; landed in 0.4.23 (threshold re-poll, bounded ~3 s).
  Expectation corrected: A2 alone gives ~26 s, not ≤2.8 s — each re-poll
  also pays the 25 ms re-arm toll (3 tolls per read instead of ~4.2).
- **A3** — deferred; it optimizes ~1 ms quantization inside 25–30 ms
  waits.
- **A4** — shelved; the ring is provably full. (Refill-before-snapshot
  in the AVAILABLE handler would fix runts Himax-side; ring growth
  would mask them. Neither touches the toll.)
- **A0 (new, the lever)** — Himax re-arm fix: a driver-supported
  transfer-complete/status path (.52 already caches the SSI base via
  `SPI_CMD_GET_SPI_BASE_REG` "for future register reads"; .41's direct
  reads bus-faulted), or CS-edge detection; interim fallback is a
  timer ladder (25/30 ms downward) gated by spitest 16/16 + 20-frame
  soaks. A0 + A2 projects to ~308 × ~7 ms ≈ 2.2 s/frame.
- **Step 0.3 counters** — landed in 0.4.23 with one addition the data
  demanded: accumulated wait-LOW/wait-HIGH ms in the `bulk_stats` log
  line, so the Himax toll is measurable from the S3 side before/after
  A0 (expect wait_low ≈ 24–32 s of bulk wall pre-A0, <10% post-A0).

Acceptance (revised): 20 frames, 0 CRC failures/retries, runts <1% of
READ_CRC (DIAG), wait_low <10% of bulk wall after A0, bulk ≤3 s after
A0 + A2.
