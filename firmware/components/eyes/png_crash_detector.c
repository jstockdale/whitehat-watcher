/* PNG crash detector using RTC memory.
 *
 * John (2026-10-08): The S3 reboots silently during PNG encode with no
 * log output and USB re-enumeration (hard reset). The panic handler can't
 * print because USB disconnects too fast.
 *
 * This module uses RTC memory (survives software reset) as a simple
 * crash detector: set a flag before PNG encode, clear it after. If the
 * flag is still set on boot, the PNG encode crashed.
 *
 * This is simpler and more reliable than hooking the Xtensa panic handler.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"  /* v0.4.18.70 (Claude F6f): for esp_reset_reason() */

static const char *TAG = "png_crash";

#define PNG_CRASH_MAGIC 0x504E4743  /* "PNGC" */

typedef struct {
    uint32_t magic;      /* 0x504E4743 if PNG was in progress */
    uint32_t stage;      /* Which stage: 1=bulk done, 2=encode start, 3=encode done */
} rtc_png_state_t;

RTC_DATA_ATTR static rtc_png_state_t rtc_png_state;

/* Call before starting PNG encode */
void png_crash_detector_arm(uint32_t stage)
{
    rtc_png_state.magic = PNG_CRASH_MAGIC;
    rtc_png_state.stage = stage;
}

/* Call after PNG encode completes successfully */
void png_crash_detector_disarm(void)
{
    rtc_png_state.magic = 0;
    rtc_png_state.stage = 0;
}

/* Check on boot. Returns stage number if crash detected, 0 otherwise. */
uint32_t png_crash_detector_check(void)
{
    if (rtc_png_state.magic == PNG_CRASH_MAGIC) {
        uint32_t stage = rtc_png_state.stage;
        /* Clear so we don't report again */
        rtc_png_state.magic = 0;
        rtc_png_state.stage = 0;
        return stage;
    }
    return 0;
}

/* Print crash info on boot */
void png_crash_detector_report(void)
{
    uint32_t stage = png_crash_detector_check();
    if (stage == 0) {
        return;
    }
    
    const char *stage_str = "unknown";
    switch (stage) {
        case 1: stage_str = "bulk complete, before encode"; break;
        case 2: stage_str = "encode in progress"; break;
        case 3: stage_str = "encode done, during streaming"; break;
    }
    
    ESP_LOGE(TAG, "=== PNG CRASH DETECTED ===");
    ESP_LOGE(TAG, "Crashed during: %s (stage %lu)", stage_str, (unsigned long)stage);
    /* v0.4.18.70 (Claude F6f): Include reset reason to distinguish panic vs
     * WDT vs brownout. */
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGE(TAG, "Reset reason: %d", (int)rr);
    ESP_LOGE(TAG, "The S3 rebooted silently during PNG processing.");
    ESP_LOGE(TAG, "==========================");
}
