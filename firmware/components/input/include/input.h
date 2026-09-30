#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the pointer-mode input task. Translates touch into relative HID
 *  mouse movement (tap = left click), wheel detents into scroll, and the
 *  wheel-click button into a left click. Starts when EITHER touch or wheel
 *  is ready (a missing device is skipped by the poll loop); if neither is
 *  ready, or one is missing, a background supervisor retries the failed
 *  init(s) with backoff until both are up. HID reports are only emitted
 *  while a BLE host is connected; otherwise input is tracked silently. */
esp_err_t input_init(void);

/** True after successful input_init(). */
bool input_ready(void);

/** Enable/disable pointer HID emission (default: enabled). Touch and wheel
 *  are still polled while disabled, so the `touch`/`wheel` debug commands
 *  keep working. */
void input_set_pointer_enabled(bool enabled);
bool input_pointer_enabled(void);

/** Verbose per-event logging from the input task (gesture transitions,
 *  wheel detents, HID calls). Off by default. */
void input_set_trace(bool on);
bool input_trace(void);

/** Live gesture state: 0 idle, 1 contact, 2 point, 3 drag. */
int input_gesture_state(void);
const char *input_gesture_state_name(void);

/** Gesture tuning constants (dwell ms, deadzone px, tap ms, tap px). */
void input_gesture_config(int *dwell_ms, int *radius_px, int *tap_ms,
                          int *tap_px);

/** Lifetime counters: taps, drag-arms, point sessions. */
void input_gesture_stats(uint32_t *taps, uint32_t *drags, uint32_t *points);

/** Media remote state (local tracking for the MEDIA mode UI). */
bool input_media_playing(void);
int input_media_volume(void);

#ifdef __cplusplus
}
#endif
