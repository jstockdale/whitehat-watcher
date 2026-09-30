#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Device UI modes. The mode ring is the Watcher's top-level UI: the
 *  wheel, touch surface and wheel-click are reinterpreted per mode.
 *  POINTER, MEDIA and TIMER have behavior; KEYS and EYES are still
 *  placeholders shown on the display until their rounds land. */
typedef enum {
    MODE_POINTER = 0, /**< Touchpad + scroll wheel (default). */
    MODE_KEYS,        /**< Bluetooth HID keyboard (placeholder). */
    MODE_MEDIA,       /**< Media remote: wheel=volume, click=play/pause. */
    MODE_TIMER,       /**< Timer / stopwatch (placeholder). */
    MODE_EYES,        /**< Eyes on the screen (placeholder). */
    MODE_COUNT
} mode_t;

/** Initialize the mode ring (starts in MODE_POINTER). */
void mode_init(void);

/** Current mode. */
mode_t mode_get(void);

/** Short display name: "pointer", "keys", "media", "timer", "eyes". */
const char *mode_name(mode_t m);

/** Set by display name; returns false on unknown name. */
bool mode_set_by_name(const char *name);

/** Advance to the next mode, firing the change callback. */
void mode_next(void);

/** Fired on every mode change (from any task). May be NULL. */
typedef void (*mode_change_cb_t)(mode_t new_mode);
void mode_set_change_cb(mode_change_cb_t cb);

#ifdef __cplusplus
}
#endif
