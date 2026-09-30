#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Power up the LCD (via the IO expander owned by eyes), initialize the
 *  SPD2010 QSPI panel + LVGL, and show the status screen.
 *  Best-effort: returns ESP_FAIL when the panel can't come up, in which
 *  case the rest of the firmware keeps working headless.
 *  Requires eyes_init() to have run first (it creates the IO expander). */
esp_err_t display_init(void);

bool display_ready(void);

/** Update the status lines. Thread-safe; no-ops until display_init()
 *  has succeeded. Safe to call from the BLE / Wi-Fi event callbacks. */
void display_set_ble(bool connected);
void display_set_wifi(bool connected, const char *ip);

/** Backlight brightness 0-100 (LEDC PWM, like the official BSP). */
esp_err_t display_set_backlight(int percent);

/** Raw full-screen fill in RGB565, bypassing LVGL. Diagnostic: proves the
 *  panel + backlight are alive independently of the LVGL UI. */
esp_err_t display_fill(uint16_t rgb565);

/** SPI diagnostic: one full-frame draw_bitmap, like an LVGL flush.
 *  Returns the exact esp_err_t for host-side diagnosis. */
esp_err_t display_test_fullframe(void);

/** Current device UI mode ("pointer", "media", ...). Thread-safe;
 *  no-ops until display_init() has succeeded. */
void display_set_mode(const char *mode);

/** Stopwatch readout text ("MM:SS.cs"). Thread-safe; no-ops until
 *  display_init() has succeeded. */
void display_set_timer(const char *text);

/** Timer readout color as 0xRRGGBB (e.g. green=running, amber=paused,
 *  red=finished). Thread-safe; no-ops until display_init() has succeeded. */
void display_set_timer_color(uint32_t rgb);

/** Show/hide the stopwatch readout (used on timer-mode enter/exit).
 *  Thread-safe; no-ops until display_init() has succeeded. */
void display_set_timer_visible(bool show);

/** Show/hide the on-screen keyboard (used on keys-mode enter/exit).
 *  Hides the status screen while the keyboard is up. Thread-safe;
 *  no-ops until display_init() has succeeded. */
void display_set_keyboard_visible(bool show);

/** Show/hide the media remote UI (used on media-mode enter/exit).
 *  Hides the status screen while visible. Thread-safe;
 *  no-ops until display_init() has succeeded. */
void display_set_media_visible(bool show);

/** Update the media play/pause icon (true=playing). Thread-safe. */
void display_set_media_playing(bool playing);

/** Update the media volume bar (0-100). Thread-safe. */
void display_set_media_volume(int volume);

/** Show a full-screen scrollable message (host response text, e.g.
 *  Muse's reply after a wake-word PTT). Hides the current UI; wheel
 *  scrolls, click dismisses. Thread-safe; no-ops until display_init(). */
void display_show_message(const char *text);

/** Dismiss the message view, restoring the previous UI. Thread-safe. */
void display_hide_message(void);

/** True while the message view is showing. */
bool display_message_visible(void);

/** Scroll the message view (positive detents = down). Thread-safe. */
void display_message_scroll(int detents);

#ifdef __cplusplus
}
#endif
