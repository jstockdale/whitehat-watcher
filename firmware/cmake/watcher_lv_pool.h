#pragma once
/* LVGL memory-pool backing store: allocated from PSRAM instead of LVGL's
 * default internal-DRAM static pool (which was the single biggest internal
 * RAM consumer at ~66 KiB .bss). LVGL keeps its own TLSF pool management;
 * only the backing store moves.
 *
 * Included by lvgl's lv_mem.c via the LV_MEM_POOL_INCLUDE compile definition;
 * LV_MEM_POOL_ALLOC (also a compile definition) names the allocator below.
 * This header is configure_file()'d into ${CMAKE_BINARY_DIR}/config, which
 * ESP-IDF puts on every component's include path.
 */
#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *watcher_lv_pool_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
