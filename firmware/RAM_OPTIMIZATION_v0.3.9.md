# Watcher Firmware v0.3.9 — Internal RAM Optimization

## Problem Statement

**Critical bug (v0.3.8):** `E (3438) input: task create failed` on boot. The ready packet shows `"pointer":false` and `W (3442) main: pointer mode not started`.

**Root cause:** `internal_free` = 3474 bytes, but `input_task` requires a 4096-byte stack. `xTaskCreate(input_task, "input", 4096, ...)` fails because 3474 < 4096. Core pointer functionality is broken.

**Target:** 50KB free internal RAM (John's request). This change prioritizes fixing the input task creation; 50KB may require riskier changes documented as future work.

## Changes Made (v0.3.9 source, NOT built)

### 1. New `task_psram` component (new files)

**Files:**
- `components/task_psram/CMakeLists.txt`
- `components/task_psram/include/task_psram.h`
- `components/task_psram/task_psram.c`

**What:** Helper to create FreeRTOS tasks with stacks in PSRAM via `xTaskCreateStatic`. The stack is allocated with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, TCB stays in internal RAM (scheduler accesses it every context switch).

**Safety constraints (documented in header):**
- Task must NEVER run while flash cache is disabled (no NVS writes, no OTA, no spi_flash ops from the task)
- Task must not be timing-critical ISR or high-priority real-time
- Task must not use DMA-capable buffers on its stack (DMA buffers allocated separately)

**Why safe:** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` is already enabled in sdkconfig.

### 2. Link component: 3 tasks → PSRAM (saves 8,192 bytes)

**File:** `components/link/link.c`, `components/link/CMakeLists.txt`

| Task | Stack | Priority | Moved? | Reason |
|------|-------|----------|--------|--------|
| `link_worker` | 4096 | 8 | ✅ PSRAM | Command processing, JSON parsing. No flash ops. Safe. |
| `link_rx` | 4096 | 9 | ❌ Internal | UART drain at priority 9. Timing-critical; PSRAM cache-miss jitter could cause UART overflow. |
| `link_mon` | 2048 | 3 | ✅ PSRAM | Monitor task, mostly sleeps. Safe. |
| `link_hb` | 2048 | 2 | ✅ PSRAM | Heartbeat every 60s, formats JSON. Safe. |

**Savings:** 4096 + 2048 + 2048 = **8,192 bytes** internal RAM freed.

**Code change:** Replaced `xTaskCreate()` with `task_psram_create()` for the three safe tasks. Added `#include "task_psram.h"` and static handles. Updated CMakeLists REQUIRES.

### 3. PTT component: task → PSRAM (saves 4,096 bytes)

**File:** `components/ptt/ptt.c`, `components/ptt/CMakeLists.txt`

| Task | Stack | Priority | Moved? | Reason |
|------|-------|----------|--------|--------|
| `ptt` | 4096 | 5 | ✅ PSRAM | I2S audio + base64 + JSON. No NVS/flash ops (verified by grep). I2S DMA buffers allocated separately with MALLOC_CAP_DMA (stay internal). Safe. |

**Savings:** **4,096 bytes** internal RAM freed.

### 4. WiFi static RX buffers: 10 → 6 (saves 6,400 bytes)

**File:** `sdkconfig.defaults`

Added: `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6`

**Rationale:** WiFi is used occasionally (legacy HTTP snapshot POST, `wifi` command). Each static RX buffer is 1600 bytes. Reducing 10→6 saves 6,400 bytes. Dynamic RX buffers (32) handle bursts. 

**Risk:** Moderate. WiFi RX throughput may drop under heavy load, but for occasional HTTP POSTs it's fine. Do not reduce further without measuring.

**Savings:** 4 × 1600 = **6,400 bytes** internal RAM freed.

**Note:** Requires `idf.py reconfigure` after editing sdkconfig.defaults (sdkconfig already exists).

## Total Savings

| Change | Bytes freed |
|--------|-------------|
| link_worker → PSRAM | 4,096 |
| link_mon → PSRAM | 2,048 |
| link_hb → PSRAM | 2,048 |
| ptt → PSRAM | 4,096 |
| WiFi RX buffers 10→6 | 6,400 |
| **Total** | **18,688** |

**Projected free:** 3,474 + 18,688 = **~22,162 bytes** internal RAM free.

**Input task fix:** 22,162 > 4,096 ✓ The input task will now be created successfully.

## What Was NOT Changed (and why)

### Kept in internal RAM (timing-critical or risky)
- **`link_rx` (4096, prio 9):** UART drain. PSRAM cache misses could cause UART ring overflow.
- **`input` (4096, prio 3):** 100Hz pointer task. Timing-sensitive; also the task we're trying to fix — moving it to PSRAM would be circular.
- **`hid_keepalive` (4096, prio 3):** Had stack overflow at 2048. DO NOT SHRINK without measuring high-water mark.
- **`snap`/`snaptest` (4096, transient):** Created on-demand for Himax SPI transfers. Stack doesn't need DMA, but they're transient (don't affect boot RAM). Skip for now.
- **`ww_feed` (4096, prio 5):** **SKIPPED due to self-deletion pattern.** The task calls `vTaskDelete(NULL)` on wake-word detection. With PSRAM stack, this would leak 4KB per detection cycle. Would need refactoring to pass handle for cleanup. Documented as future work.
- **LVGL draw buffers (2×8240 = 16,480):** MUST stay internal (SPI DMA). Non-negotiable.
- **NimBLE (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y`):** Could move to PSRAM via `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`, but BLE HID timing is critical. High risk, needs testing. Future work.
- **ESP-SR AFE:** Closed-source. Allocates significant internal RAM. No Kconfig option found for PSRAM. High risk. Future work.

### Already in PSRAM (no change needed)
- LVGL 64KB pool ✓
- Log ring (12KB) ✓
- SSCMA client (64KB RX + 4KB TX) ✓
- Snapshot buffer (512KB) ✓
- Display message text ✓

## Risks and Mitigations

1. **PSRAM task crash during cache-disable:** If a PSRAM-stack task runs during NVS write or spi_flash operation, CPU crashes with "Cache disabled but cached memory region accessed".
   - **Mitigation:** Audited each moved task — none do flash ops. `link_worker` handles commands but no NVS/OTA commands exist (verified by grep). `ptt` does no flash ops.

2. **PSRAM cache-miss jitter:** PSRAM is slower than internal RAM and can have cache misses.
   - **Mitigation:** Only moved non-timing-critical tasks (priorities 2, 3, 5, 8). Kept priority 9 (link_rx) and 100Hz (input) internal.

3. **WiFi stability:** Reduced RX buffers may affect throughput.
   - **Mitigation:** Conservative reduction (10→6, not 10→4). Dynamic buffers (32) provide headroom. WiFi is used occasionally, not for streaming.

4. **Build system:** New component `task_psram` must be discovered by CMake.
   - **Mitigation:** Standard component layout (CMakeLists.txt + include/). Added to REQUIRES in link and ptt.

## Testing Required (John to approve)

1. **Build v0.3.9** — verify no compile errors in task_psram, link, ptt.
2. **Boot test** — verify `input: task create failed` is GONE, `pointer:true` in ready packet.
3. **Functional test** — pointer mode works (touch → HID mouse).
4. **Stress test** — PTT recording, link commands, heartbeat — verify no PSRAM-related crashes.
5. **WiFi test** — verify HTTP snapshot POST still works with reduced buffers.
6. **Long-run** — monitor `internal_free` in heartbeat, verify no degradation.

## Future Work (to reach 50KB)

The 50KB target requires riskier changes:
- **NimBLE to PSRAM** (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`): Could free 10-20KB, but BLE timing risk. Needs extensive testing.
- **ESP-SR AFE optimization:** Investigate if AFE can use PSRAM. Closed-source, high risk.
- **`ww_feed` refactoring:** Fix self-deletion leak, then move to PSRAM (saves 4KB).
- **More aggressive WiFi tuning:** Reduce dynamic buffers, TX buffers (needs measurement).

**Recommendation:** Ship v0.3.9 with the safe 18.7KB win, verify input task works, then evaluate riskier options with measurements.

## Files Modified

**New:**
- `components/task_psram/CMakeLists.txt`
- `components/task_psram/include/task_psram.h`
- `components/task_psram/task_psram.c`

**Modified:**
- `components/link/link.c` (3 tasks → PSRAM)
- `components/link/CMakeLists.txt` (add task_psram dependency)
- `components/ptt/ptt.c` (1 task → PSRAM)
- `components/ptt/CMakeLists.txt` (add task_psram dependency)
- `sdkconfig.defaults` (WiFi RX buffers 10→6)

**NOT built.** Awaiting John's approval to build v0.3.9.
