/* task_psram.h — Create FreeRTOS tasks with stacks in PSRAM.
 *
 * Why: ESP32-S3 internal RAM is precious (~320KB total, DMA region is only
 * ~72KB). Task stacks default to internal RAM. With CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y,
 * we can safely place stacks in PSRAM for tasks that:
 *   - Never run while flash cache is disabled (no NVS writes, no OTA, no spi_flash ops)
 *   - Are not timing-critical ISRs or high-priority real-time tasks
 *   - Don't use DMA-capable buffers on their stack
 *
 * DANGER: If a task with PSRAM stack runs during cache-disable (e.g. NVS write
 * initiated from that task), the CPU will crash with "Cache disabled but cached
 * memory region accessed". Audit each task before moving it.
 *
 * The TCB (StaticTask_t) stays in internal RAM (small, ~150 bytes, accessed by
 * scheduler on every context switch — keep it fast).
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a task with its stack allocated from PSRAM.
 * The stack is freed automatically when the task is deleted (via a cleanup
 * callback registered with vTaskSetThreadLocalStoragePointer... actually we
 * track it manually — caller must call task_psram_delete() or ensure the
 * task deletes itself via task_psram_self_delete()).
 *
 * Simpler: we allocate stack + TCB wrapper from PSRAM and leak on delete.
 * For long-running tasks that never exit, this is fine. For transient tasks,
 * use task_psram_create() + task_psram_delete().
 */
typedef struct {
    StaticTask_t tcb;
    StackType_t *stack;
    uint32_t stack_words;
    TaskHandle_t task;  /* v0.4.6: FreeRTOS handle, for vTaskDelete via task_psram_delete */
} task_psram_handle_t;

/* Create a task with PSRAM stack. Returns handle (for deletion) or NULL on failure.
 * The handle must be kept alive for the task's lifetime. For tasks that never
 * exit, the handle can be static/global. */
task_psram_handle_t *task_psram_create(TaskFunction_t fn,
                                       const char *name,
                                       uint32_t stack_bytes,
                                       void *arg,
                                       UBaseType_t priority,
                                       BaseType_t core_id);  /* tskNO_AFFINITY for no affinity */

/* Delete a task created with task_psram_create(). Frees the PSRAM stack. */
void task_psram_delete(task_psram_handle_t *handle, TaskHandle_t task);

#ifdef __cplusplus
}
#endif
