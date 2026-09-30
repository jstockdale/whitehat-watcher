#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the scroll-wheel rotary encoder (GPIO41 A / GPIO42 B, polled by
 *  the knob driver) and the wheel-click button (PCA9535 expander pin 3,
 *  active-low, polled). Requires eyes_init() for the expander handle. */
esp_err_t wheel_init(void);

/** True after successful wheel_init(). */
bool wheel_ready(void);

/** Consume accumulated wheel detents since the last call. Positive =
 *  clockwise (KNOB_RIGHT), negative = counter-clockwise. Returns 0 when
 *  nothing moved. Safe to call from any task. */
int wheel_take_delta(void);

/** Total detents since boot (clockwise positive). Diagnostic. */
int wheel_count(void);

/** Detents accumulated since the last wheel_take_delta(), without
 *  consuming them. Diagnostic. */
int wheel_pending(void);

/** Raw encoder pin levels (0/1), for diagnosing a silent wheel. */
void wheel_gpio_levels(int *a, int *b);

/** Current wheel-click button state: true while held down (active-low).
 *  The caller is responsible for debouncing. */
bool wheel_button_down(void);

#ifdef __cplusplus
}
#endif
