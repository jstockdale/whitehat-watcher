# RAM Audit for 0.4.6 — Phase 1 Analysis (read-only)

**Date:** 2026-09-27
**Baseline:** 0.4.4 `dbg mem`: internal_free=11,566 B, largest_internal=112 B, psram_free=7,629,296 B
**Status:** ANALYSIS ONLY — no code changes made. Phase 2 applies these after 0.4.5 is built.

## Summary

Internal RAM is critically constrained: ~11 KB free, largest contiguous block 112 bytes.
PSRAM has 7.6 MB free. The wins are in heap caps and task stacks, not static BSS.

| Category | Internal RAM used | Movable to PSRAM |
|---|---|---|
| Static BSS/data (our code) | ~2 KB | ~0 (already tiny) |
| Static BSS/data (libraries) | ~56 KB | No (ESP-SR/ESP-DL/HAL) |
| Task stacks (internal) | ~184 KB | **~168 KB** |
| Heap (plain malloc) | ~20 sites | **Most** |

## Finding 1: Task stacks — the big win (~168 KB)

Ten tasks use internal stacks via plain `xTaskCreate`. Stack sizes are in **words**
(4096 words = 16 KB). Total: ~184 KB of internal RAM.

| Task | File | Stack | Internal | PSRAM-safe? |
|---|---|---|---|---|
| snap | eyes.c:692 | 4096w (16 KB) | Yes | **Yes** — snapshot processing, PSRAM-heavy already |
| snaptest | eyes.c:1101 | 4096w (16 KB) | Yes | **Yes** — test task |
| hid_keepalive | hid_ble.c:778 | 4096w (16 KB) | Yes | **Yes** — BLE keepalive, not ISR |
| input | input.c:519 | 4096w (16 KB) | Yes | **Yes** — input processing |
| input_retry | input.c:495 | 2048w (8 KB) | Yes | **Yes** — retry logic |
| link_rx | link.c:1954 | 4096w (16 KB) | Yes | **Yes** — RX parse; prio 9 but not ISR/DMA. PSRAM stack OK. |
| mqtt_native | mqtt_native.c:683 | 6144w (24 KB) | Yes | **Yes** — MQTT/TLS; deep stack needed but PSRAM-safe |
| nvs_writer | seal_dev.c:449 | 4096w (16 KB) | Yes | **NO** — John's rule: NVS writer must stay internal |
| ww_feed | wakeword.c:317 | 4096w (16 KB) | Yes | **Yes** — audio feed, PSRAM-safe |
| sscma_process | sscma_client_ops.c:531 | configurable | Mixed | Already uses PSRAM stack when configured |

**Recommendation:** Migrate 8 tasks (all except `nvs_writer`) to `task_psram_create()`.
The `task_psram` component already exists and is used by link_worker, link_mon,
link_hb, and ptt. This frees ~168 KB of internal RAM.

**Risk:** `link_rx` runs at priority 9. PSRAM access has higher latency than internal
RAM, but the task is not ISR-context and does not do DMA. The existing `link_worker`
already runs on PSRAM stack at similar priority without issues. Acceptable.

**Do NOT touch:** `nvs_writer` (must stay internal per John's NVS design rule).

## Finding 2: Plain malloc/calloc without caps (~20 sites)

These allocate from internal DRAM by default. Most are PSRAM-safe:

| Location | Size | PSRAM-safe? |
|---|---|---|
| eyes.c:679 `struct snap_job` | small | **Yes** |
| eyes.c:1090 `struct snaptest_job` | small | **Yes** |
| link.c:813 `char *js` (JSON) | variable | **Yes** — link worker is PSRAM task |
| link.c:1712 `char *copy` | variable | **Yes** — check context, likely JSON |
| ptt.c:406 `char *b64` (base64) | ~5.5 KB | **Yes** — PTT is PSRAM task |
| seal/wms2.c:128 `scratch` | variable | **Yes** — crypto scratch, not ISR/DMA |
| seal/mqtt_native.c:136 `uint8_t *msg` | variable | **Yes** — MQTT message buffer |
| seal/mqtt_native.c:182 `m.line` | variable | **Yes** |
| seal/mqtt_native.c:265 `js` | variable | **Yes** |
| seal/mqtt_native.c:328 `char *js` | variable | **Yes** |
| seal/mqtt_native.c:480 `m.data` | variable | **Yes** |
| seal/seal_dev.c:358 `nvs_writer_done_t` | small | **Maybe** — used by nvs_writer (internal task). The struct itself is just a completion flag; PSRAM should be OK, but verify no ISR touches it. |
| sscma_client (various) | small structs | **Yes** — except DMA buffers |

**Recommendation:** Change to `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` or
`heap_caps_calloc(1, ..., MALLOC_CAP_SPIRAM)`.

**Do NOT touch:**
- `sscma_client_io_spi.c:147` — DMA staging buffer, correctly uses `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`
- `display.c:539` — LCD chunk buffer, correctly uses `MALLOC_CAP_DMA`

## Finding 3: Static BSS — minimal wins

Our components' static BSS/data in internal RAM totals ~2 KB (link 564 B, hid_ble 552 B,
mqtt_native 515 B, wia1 161 B, led 146 B). Not worth moving.

Library BSS (~56 KB) is dominated by:
- ESP-SR `gtcrn_math.c` — 16,416 B (wakeword NN math tables)
- ESP-DL `dl_nn_activation.c` — 5,936 B
- ESP-DL `dl_module_base.cpp` — 5,029 B

These are third-party; moving them risks breaking NN inference timing. **Do not touch.**

## Finding 4: Fragmentation

`largest_internal=112 B` with 11 KB free indicates severe fragmentation. Contributors:
- Frequent small `malloc`/`free` cycles (JSON parsing in link.c, MQTT messages)
- Mixed allocation sizes from internal heap

Moving the plain-malloc sites to PSRAM (Finding 2) will reduce internal heap churn.
Moving task stacks to PSRAM (Finding 1) will free large contiguous internal blocks.

## Phase 2 Plan (after 0.4.5 is built)

1. Migrate 8 tasks to `task_psram_create()` (all except nvs_writer).
2. Add `MALLOC_CAP_SPIRAM` to the ~15 safe plain-malloc sites.
3. Build, run audit_psram_flash.py, WMS2 74/74, WIA1 196/196.
4. Verify `dbg mem` shows improved internal_free and largest_internal.
5. Write V0.4.6_BUILD_REPORT.md with before/after numbers.

## Files NOT to touch (0.4.5 agent's territory)

- `components/led/` — LED queue fixes
- Reboot-related code (WDT, main.c boot sequence)
- Any file the 0.4.5 agent is actively modifying — ask before touching
