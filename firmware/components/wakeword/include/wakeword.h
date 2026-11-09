#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Wake-word detection via ESP-SR. See DESIGN.md for architecture.
 *
 *  The wake-word component owns the I2S mic when enabled and idle.
 *  PTT suspends it (wakeword_stop) before taking I2S, resumes after
 *  (wakeword_start). When disabled, the component is inert and PTT
 *  works exactly as before.
 *
 *  v0.3.4: full ESP-SR integration. Model is "Hi, ESP" (wn9_hiesp) as
 *  the working interim; "Hey Muse" / "Hey Watcher" need Espressif
 *  custom training and drop into the 2nd wakenet slot when available. */

/** Detection callback: invoked on the wake-word task when the phrase is
 *  recognized. Default action (set by app): trigger PTT flow. */
typedef void (*wakeword_cb_t)(int index);

/** Initialize the wake-word component. Does NOT start listening;
 *  call wakeword_set_enabled(true) to activate. Safe no-op if ESP-SR
 *  is not yet integrated. */
esp_err_t wakeword_init(void);

/** True after successful wakeword_init(). */
bool wakeword_ready(void);

/** Enable/disable wake-word listening. Persisted to NVS.
 *  When enabling: initializes AFE, takes I2S, starts detection task.
 *  When disabling: stops task, releases AFE/PSRAM. */
void wakeword_set_enabled(bool enabled);
bool wakeword_enabled(void);

/* v0.4.38: true while the feed/fetch tasks are actually running —
 * distinct from enabled_ (the NVS opt-in), so a dead or un-resumed
 * listener is visible in the dbg reply instead of silent. */
bool wakeword_listening(void);

/** Suspend listening and release I2S (called by PTT before recording).
 *  No-op if not enabled or already suspended. */
esp_err_t wakeword_stop(void);

/** Resume listening and re-acquire I2S (called by PTT after recording).
 *  No-op if not enabled or not suspended. */
esp_err_t wakeword_start(void);

/** Set the detection callback (default: trigger PTT). */
void wakeword_set_callback(wakeword_cb_t cb);

/** Lifetime detection count (for diagnostics). */
uint32_t wakeword_detection_count(void);

/** v0.4.44: level snapshot frozen at the last detection — the ambient
 *  room RMS (rolling noise-floor estimate from the feed, in PCM sample
 *  units) and the speech RMS (loudest feed chunk in the ~1.5 s around
 *  the detection — the wake phrase itself). PTT uses these to derive a
 *  dynamic silence threshold for the triggered take instead of any
 *  fixed value: the environment is measured, never assumed. Either
 *  value is 0 if no detection has happened yet. */
void wakeword_detect_levels(float *ambient_rms, float *speech_rms);

/** Live ambient floor estimate (0 until the feed has run). */
float wakeword_ambient_rms(void);

/** Wake-word phrase selection (persisted to NVS). BOTH is the default.
 *  HEY_MUSE / HEY_WATCHER currently fall back to the "Hi, ESP" model
 *  until Espressif trains the custom models — the selection is stored
 *  now so it's ready when they arrive. Takes effect on next enable. */
typedef enum {
    WAKEWORD_PHRASE_BOTH = 0,
    WAKEWORD_PHRASE_HEY_MUSE = 1,
    WAKEWORD_PHRASE_HEY_WATCHER = 2,
} wakeword_phrase_t;
void wakeword_set_phrase(wakeword_phrase_t phrase);
wakeword_phrase_t wakeword_get_phrase(void);

#ifdef __cplusplus
}
#endif
