/* task_psram.c — PSRAM-backed FreeRTOS task stacks.
 *
 * See task_psram.h for safety constraints. This is for ESP32-S3 with
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y.
 */

#include "task_psram.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "task_psram";

task_psram_handle_t *task_psram_create(TaskFunction_t fn,
                                       const char *name,
                                       uint32_t stack_bytes,
                                       void *arg,
                                       UBaseType_t priority,
                                       BaseType_t core_id)
{
    /* StackType_t is 4 bytes on ESP32-S3; round up to whole words. */
    uint32_t words = (stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);

    task_psram_handle_t *h = heap_caps_malloc(sizeof(*h),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h) {
        ESP_LOGE(TAG, "no mem for handle (%s)", name);
        return NULL;
    }
    memset(h, 0, sizeof(*h));

    h->stack = heap_caps_malloc(words * sizeof(StackType_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h->stack) {
        ESP_LOGE(TAG, "no PSRAM for stack (%s, %lu bytes)", name,
                 (unsigned long)(words * sizeof(StackType_t)));
        heap_caps_free(h);
        return NULL;
    }
    h->stack_words = words;

    TaskHandle_t task;
    if (core_id == tskNO_AFFINITY) {
        task = xTaskCreateStatic(fn, name, words, arg, priority,
                                 h->stack, &h->tcb);
    } else {
        task = xTaskCreateStaticPinnedToCore(fn, name, words, arg, priority,
                                             h->stack, &h->tcb, core_id);
    }
    if (!task) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed (%s)", name);
        heap_caps_free(h->stack);
        heap_caps_free(h);
        return NULL;
    }
    h->task = task;  /* v0.4.6: expose for task_psram_delete */

    ESP_LOGI(TAG, "%s: %lu-byte stack in PSRAM", name,
             (unsigned long)(words * sizeof(StackType_t)));
    return h;
}

void task_psram_delete(task_psram_handle_t *handle, TaskHandle_t task)
{
    if (!handle) {
        return;
    }
    if (task) {
        vTaskDelete(task);
    }
    heap_caps_free(handle->stack);
    heap_caps_free(handle);
}
