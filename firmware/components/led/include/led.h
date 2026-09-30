#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Background LED states, in priority order (highest first).
 *  led_set_state() activates a request; the highest-priority active request
 *  is what you see. led_clear_state() releases a request.
 *
 *  Color language:
 *    red    = error (pulsing) or mic hot (steady)
 *    yellow = boot problem, recoverable
 *    blue   = BLE advertising, waiting to pair
 *    purple = AI thinking (host-side)
 *    green  = device working (breathing) or success (single pulse)
 *    dark   = idle, nothing to show */
typedef enum {
    LED_STATE_ERROR,             // pulsing red — highest priority, always wins
    LED_STATE_RECORDING,         // steady red — mic is hot
    LED_STATE_STARTUP_ISSUE,     // pulsing yellow — boot problem, recoverable
    LED_STATE_BLE_ADVERTISING,   // blue blink (2/sec) — waiting to pair
    LED_STATE_AI_THINKING,       // breathing purple — AI is thinking
    LED_STATE_WORKING,           // breathing green — device working, OK state
    LED_STATE_PARTY,             // rainbow cycle — just for fun
    LED_STATE_IDLE_CONNECTED,    // legacy: treated as off (no idle glow)
    LED_STATE_OFF,               // LED dark — default when nothing is active
    LED_STATE_COUNT,
} led_state_t;

/** Init the WS2812 strip on GPIO40, run the startup sequence
 *  (3s rainbow flourish -> dark), then settle into LED_STATE_OFF.
 *  Safe to call once. */
esp_err_t led_init(void);

/** True after successful led_init(). */
bool led_ready(void);

/** Activate a background state request. The displayed state is always the
 *  highest-priority active request, so a lower-priority set is remembered
 *  but not shown until higher ones clear. */
void led_set_state(led_state_t state);

/** Release a background state request (e.g. PTT calls this for
 *  LED_STATE_RECORDING when the button is released). */
void led_clear_state(led_state_t state);

/* --- transient patterns: preempt the background, then restore it --- */

/** 3x green flash — BLE just connected. */
void led_flash_green(void);

/** Single green breath pulse (~800ms in-out) — success / confirm. */
void led_flash_success(void);

/** 2x red flash — error. */
void led_flash_error(void);

/** 3x short white blinks (600ms), then LED stays OFF until
 *  led_camera_result(). The "say cheese" countdown. */
void led_camera_countdown(void);

/** Force LED off (during capture — zero light contamination). */
void led_camera_off(void);

/** Green flash if ok else red flash, then restore background. */
void led_camera_result(bool ok);

/** 50ms white tick — button press acknowledged. */
void led_button_ack(void);

/** Boot is finished: stop queueing transients (any still queued are dropped
 *  — stale boot lights must not play late) and restore the normal preempt
 *  behavior. Call once from main when the "watcher input PoC X.Y.Z up"
 *  line prints. During boot, transients play sequentially via a FIFO
 *  instead of preempting each other. */
void led_boot_complete(void);

#ifdef __cplusplus
}
#endif
