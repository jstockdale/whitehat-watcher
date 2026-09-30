cmake_minimum_required(VERSION 3.16)

# Project-local components first, then read-only references into the official
# Watcher firmware's vendor clone. vendor/ is never modified; if we need to
# change BSP behavior we fork the component into firmware/components/ instead.
# (sscma_client is such a fork: its 64 KiB RX + 4 KiB TX buffers and reply/task
# allocations were moved to PSRAM to preserve internal DMA headroom.)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components
    ${CMAKE_CURRENT_LIST_DIR}/../vendor/watcher-firmware/components/esp_io_expander_pca95xx_16bit
)

# ---------------------------------------------------------------------------
# Global compile definitions. These MUST come before project(): component
# targets are created during project(), so definitions added after it never
# reach component sources.
#
# - LV_MEM_POOL_*: LVGL's 64 KiB static heap pool (the single biggest internal
#   RAM consumer at ~66.7 KiB .bss) is allocated from PSRAM instead of internal
#   DRAM. LVGL keeps its TLSF pool management; only the backing store moves.
#   Draw buffers stay internal DMA-capable (allocated separately in display.c).
# - CONFIG_SSCMA_*: the forked sscma_client gates PSRAM allocation of its
#   reply/image buffers and task stacks behind these macros (it ships no
#   Kconfig), so they are defined here for the whole build.
# - MALLOC_CAP_SIMD: esp-dl (pulled by ESP-SR 2.5.5) uses this capability
#   flag, but it only exists in IDF >= 5.4 and we build on 5.3. Defining
#   it as 0 makes those allocations fall back to default caps on 5.3
#   (no extra placement constraint); harmless on 5.4+ where the real
#   definition in esp_heap_caps.h takes precedence... except it won't:
#   a -D define overrides the header. So scope this carefully: it must
#   ONLY apply when building on IDF < 5.4. We handle that by defining it
#   here unconditionally but to 0 — on 5.4+ the header's BIT value would
#   be shadowed, which would silently drop the SIMD placement hint.
#   Since this project pins IDF 5.3, 0 is correct here. If we ever move
#   to IDF 5.4+, DELETE this line.
# ---------------------------------------------------------------------------
# NOTE: LV_MEM_POOL_ALLOC must be a plain identifier, not a function-style
# macro: CMake's add_compile_definitions() silently drops function-style
# macros, and passing one via add_compile_options() breaks on shell-quoting
# through make/ccache. So the allocator lives in cmake/watcher_lv_pool.h,
# which is copied into ${CMAKE_BINARY_DIR}/config (on every component's
# include path) and pulled into lvgl's lv_mem.c via LV_MEM_POOL_INCLUDE.
configure_file(${CMAKE_CURRENT_LIST_DIR}/cmake/watcher_lv_pool.h
               ${CMAKE_BINARY_DIR}/config/watcher_lv_pool.h COPYONLY)
add_compile_definitions(
    "LV_MEM_POOL_INCLUDE=<watcher_lv_pool.h>"
    "LV_MEM_POOL_ALLOC=watcher_lv_pool_alloc"
    CONFIG_SSCMA_ALLOC_SMALL_SHORTTERM_MEM_EXTERNALLY=1
    CONFIG_SSCMA_PROCESS_TASK_STACK_ALLOC_EXTERNAL=1
    MALLOC_CAP_SIMD=0
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(watcher_input_poc)

# ---------------------------------------------------------------------------
# v0.4.3: PSRAM/flash build audit. Fails the build if any flash-write API
# (nvs_set_*/nvs_commit/spi_flash_*/esp_ota_*/partition writes) is
# reachable from a PSRAM-stack task. The v0.4.1 set_name crash proved the
# manual grep does not survive new code; this runs on every build (ALL)
# and the app target depends on it, so a violation breaks the build.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(psram_flash_audit ALL
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/tools/audit_psram_flash.py
        COMMENT "Auditing PSRAM-stack tasks for direct flash writes"
        VERBATIM)
    add_dependencies(app psram_flash_audit)
else()
    message(WARNING "Python3 not found; skipping psram_flash_audit")
endif()
