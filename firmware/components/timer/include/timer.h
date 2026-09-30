#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stopwatch + countdown timer for MODE_TIMER. On-device only; no BLE needed.
 *  Backed by esp_timer. The input task is the single writer; readers
 *  (display refresh, debug commands) may observe a slightly stale value,
 *  which is fine for a timer.
 *
 *  Two modes:
 *  - Stopwatch (default): counts up from zero.
 *  - Countdown: set via timer_set_countdown(), counts down to zero, then
 *    latches timer_finished() and stops. */

/** Initialize (stopped, 00:00.00, stopwatch mode). */
void timer_init(void);

/** Start if stopped, stop if running. Clears the finished latch. */
void timer_toggle(void);

/** Stop and zero the progress. Keeps the countdown target. */
void timer_reset(void);

/** True while counting. */
bool timer_running(void);

/** True when a countdown duration is set (> 0 minutes). */
bool timer_is_countdown(void);

/** Latched true when a countdown reaches zero. Cleared by toggle/reset. */
bool timer_finished(void);

/** Set countdown duration in minutes (0 = stopwatch mode, max 99).
 *  Stops and zeroes the progress, keeps the new target. */
void timer_set_countdown(int minutes);

/** Current countdown target in minutes (0 = stopwatch). */
int timer_countdown_minutes(void);

/** Milliseconds elapsed (stopwatch) or remaining (countdown).
 *  For a countdown this hits zero and latches timer_finished(). */
int64_t timer_elapsed_ms(void);

/** Format as "MM:SS.cs" (centiseconds), e.g. "03:07.42".
 *  Always NUL-terminated. */
void timer_format(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
