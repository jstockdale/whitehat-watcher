#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Touch panel resolution (SPD2010 touch reports in display pixel space). */
#define TOUCH_X_MAX 412
#define TOUCH_Y_MAX 412
/** Driver reports up to 10 simultaneous contacts. */
#define TOUCH_MAX_POINTS 10

/** Bring up I2C1 (GPIO39 SDA / GPIO38 SCL, 400 kHz) and the SPD2010 touch
 *  controller at 0x53. The touch IC shares the LCD power rail, so call this
 *  after display_init()/eyes_init() have run the power sequence. Polled:
 *  the vendor BSP never uses the TP interrupt pin. */
esp_err_t touch_init(void);

/** True after successful touch_init(). */
bool touch_ready(void);

/** Poll the controller. Returns the number of active contacts (0..10);
 *  x/y/strength hold the first `n` contacts (caller provides arrays of
 *  TOUCH_MAX_POINTS). Coordinates are 0..411 in display pixel space. */
int touch_read(uint16_t *x, uint16_t *y, uint16_t *strength);

#ifdef __cplusplus
}
#endif
