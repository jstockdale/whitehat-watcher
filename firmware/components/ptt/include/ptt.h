#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Init I2S mic (ES7243/ES7243E via I2C0), ADPCM encoder, and the PTT
 *  monitor task. Requires eyes_init() to have run first (it owns I2C0).
 *  Requires wheel_init() for wheel_button_down(). */
esp_err_t ptt_init(void);

/** True after successful ptt_init(). */
bool ptt_ready(void);

/** True while the button is held past the PTT threshold and audio is
 *  being captured. */
bool ptt_is_recording(void);

/** True while PTT has claimed the wheel button (long-press in progress).
 *  The input component checks this to suppress its short-press action
 *  while PTT owns the button. Push+scroll (button held while the wheel
 *  rotates) is the mode-cycle gesture — it suppresses PTT via
 *  ptt_note_mode_cycle(). Safe to call when PTT is not initialized
 *  (returns false). */
bool ptt_button_claimed(void);

/** Called by the input component when it detects push+scroll (button held
 *  while the wheel rotates) and cycles the mode ring. Suppresses PTT
 *  claiming for the remainder of this button press. Idempotent. */
void ptt_note_mode_cycle(void);

/** Start a PTT recording programmatically (wake-word auto-PTT). The
 *  ptt_task picks this up on its next poll, takes I2S0 from the wake-word
 *  component, and records until ptt_stop_external(), the 30s safety cap,
 *  or a wheel-click. Safe to call when not initialized (no-op). */
void ptt_start_external(void);

/** End an external (wake-word triggered) recording early and send the
 *  audio. No-op if no external recording is active. */
void ptt_stop_external(void);

/** True while an external (wake-word triggered) recording is active. */
bool ptt_is_external(void);

#ifdef __cplusplus
}
#endif
