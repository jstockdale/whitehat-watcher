/* Pointer mode: the Watcher as a BLE touchpad + scroll wheel.
 *
 * Gesture model (v0.3.4.2, dwell-to-drag):
 *   finger down            -> CONTACT: absorbed inside a 4 px deadzone
 *                              around the touch-down point; kills jitter
 *   move out of deadzone    -> POINT: plain relative cursor movement
 *   dwell 1 s in deadzone   -> DRAG: left button held, moves drag
 *   lift from CONTACT       -> tap (<200 ms, <8 px travel) clicks;
 *                              a second tap within 400 ms / 12 px upgrades
 *                              to a double-click (the single click still
 *                              fires immediately on the first tap: no added
 *                              latency)
 *   lift from POINT         -> plain release, NEVER a tap. Bench bug fix:
 *                              quick 5-7 px flicks entered POINT (cursor
 *                              visibly moved) yet still satisfied the old
 *                              state-blind tap window and fired a spurious
 *                              click. Tap is now CONTACT-only.
 *   lift from DRAG          -> button released
 *   wheel detent           -> scroll (2 lines per detent)
 *   wheel-click press      -> left click
 *   wheel long-press       -> push-to-talk (see ptt component)
 *   push + scroll          -> cycle the mode ring (button held while the
 *                              wheel rotates; intuitive like a jog dial)
 *
 * The task polls at 100 Hz. Touch reads are small I2C transactions (~1 ms
 * at 400 kHz); the wheel is interrupt-free (the knob driver polls in its
 * own timer). HID emission is skipped entirely when no host is connected
 * or when pointer mode is disabled via the debug console, but polling
 * continues so `touch`/`wheel` diagnostics stay live.
 *
 * Later modes (device UI ring) repurpose these gestures: MEDIA maps the
 * wheel to volume and the click to play/pause; KEYS maps the wheel to
 * up/down arrow keys and the click to Enter; TIMER maps the click to
 * start/stop and the wheel to reset, with a live stopwatch readout on the
 * display. This task implements POINTER, MEDIA, KEYS and TIMER input
 * behavior. */

#include "input.h"
#include "task_psram.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "touch.h"
#include "wheel.h"
#include "hid_ble.h"
#include "mode.h"
#include "timer.h"
#include "display.h"
#include "led.h"
#include "ptt.h"

static const char *TAG = "input";

/* 412 px touch surface driving a desktop cursor: one full swipe moves
 * ~1650 px. Tune later with real feel. */
#define POINTER_SCALE      4
/* Tap = press shorter than this, with less than this much travel, and
 * -- critically -- the finger must never have left the CONTACT deadzone.
 * Bench finding (v0.3.4.2): taps were firing on touch-and-move because the
 * old lift check was state-blind. TAP_MAX_PX (8) > DRAG_ARM_PX (4), so a
 * quick 5-7 px flick entered POINT, moved the cursor, and still passed the
 * tap window. Now only a CONTACT lift can be a tap; POINT lifts are pure
 * cursor motion. 200 ms keeps single taps snappy and leaves room for the
 * 400 ms double-tap window below. */
#define TAP_MAX_MS         200
#define TAP_MAX_PX         8
/* Second tap within this time and radius of the previous tap's anchor
 * upgrades to a double-click. The first tap's single click fires
 * immediately (no waiting), so the host sees click then double-click --
 * the standard desktop select-then-open sequence. */
#define DBLTAP_MAX_MS      400
#define DBLTAP_MAX_PX      12
/* Drag arms after dwelling inside this radius for this long. Reverted to
 * the original 1000 ms spec per bench feedback (v0.3.4.2): the 1500 ms
 * trial felt wrong and the original timing was fine. The 4 px deadzone
 * stays: 2 px was below the finger-tremor noise floor on this sensor,
 * and touch-down wobble spuriously kicked gestures into POINT mode. */
#define DRAG_ARM_MS        1000
#define DRAG_ARM_PX        4
#define WHEEL_LINES        2
#define BTN_DEBOUNCE_MS    60
#define POLL_PERIOD_MS     10

/* Gesture states, mirrored in input_gesture_state_name(). */
#define GEST_IDLE     0
#define GEST_CONTACT  1
#define GEST_POINT    2
#define GEST_DRAG     3

static bool ready_ = false;
static bool pointer_enabled_ = true;
static bool trace_ = false;
/* Media remote state (local tracking; HID consumer is one-way). */
static bool media_playing_ = false;
static int media_volume_ = 50;
/* File-static so the `gesture` debug command can report live state. */
static int gest_state_ = GEST_IDLE;
static uint32_t stat_taps_ = 0, stat_drags_ = 0, stat_points_ = 0;
/* Double-tap bookkeeping: anchor and time of the most recent fired tap.
 * A tap landing within DBLTAP_MAX_MS / DBLTAP_MAX_PX of it upgrades to a
 * double-click instead of a second single click. */
static int64_t last_tap_at_ = 0;
static int last_tap_x_ = 0, last_tap_y_ = 0;
static bool last_tap_valid_ = false;

#define TRACE(fmt, ...) \
    do { if (trace_) ESP_LOGI(TAG, "[trace] " fmt, ##__VA_ARGS__); } while (0)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/** Push the current timer value to the display, colored by state:
 *  green = running, amber = paused, red = countdown finished. */
static void timer_draw(void)
{
    char buf[16];
    timer_format(buf, sizeof(buf));
    display_set_timer(buf);
    uint32_t color;
    if (timer_finished()) {
        color = 0xff3333;
    } else if (timer_running()) {
        color = 0x33dd66;
    } else {
        color = 0xffcc33;
    }
    display_set_timer_color(color);
}

/** Short-press action for the current mode. */
static void mode_short_press(void)
{
    /* Ring-test diagnostic has input priority: click dismisses it. */
    if (display_ring_test_visible()) {
        display_hide_ring_test();
        return;
    }
    /* Message overlay has input priority: click dismisses it. */
    if (display_message_visible()) {
        display_hide_message();
        return;
    }
    /* External (wake-word) PTT recording: click ends it early. */
    if (ptt_is_external()) {
        ptt_stop_external();
        return;
    }
    switch (mode_get()) {
    case MODE_MEDIA:
        hid_ble_consumer("play_pause");
        media_playing_ = !media_playing_;
        display_set_media_playing(media_playing_);
        break;
    case MODE_KEYS:
        hid_ble_key("enter", NULL);
        break;
    case MODE_TIMER:
        /* On-device: works with no BLE host. */
        timer_toggle();
        timer_draw();
        TRACE("timer %s", timer_running() ? "started" : "stopped");
        break;
    default:
        hid_ble_mouse_click("left");
        break;
    }
}

static void input_task(void *arg)
{
    (void)arg;
    uint16_t x[TOUCH_MAX_POINTS], y[TOUCH_MAX_POINTS], s[TOUCH_MAX_POINTS];

    int anchor_x = 0, anchor_y = 0;
    int last_x = 0, last_y = 0;
    int64_t t0 = 0;
    int travel = 0;

    bool btn_down = false;
    int64_t btn_edge_at = 0;
    /* Set when push+scroll cycled the mode during this press, so the
     * release doesn't also fire a short-press action. */
    bool btn_mode_cycled = false;
    int64_t timer_draw_at = 0;

    ESP_LOGI(TAG, "pointer task running");
    for (;;) {
        bool emit = pointer_enabled_ && hid_ble_connected();
        bool pointer_mode = (mode_get() == MODE_POINTER);

        /* ---- touch gesture state machine (pointer mode only) ---- */
        int n = touch_read(x, y, s);
        int64_t now = now_ms();
        if (n > 0 && pointer_mode) {
            int cx = (int)x[0], cy = (int)y[0];
            switch (gest_state_) {
            case GEST_IDLE:
                gest_state_ = GEST_CONTACT;
                anchor_x = last_x = cx;
                anchor_y = last_y = cy;
                t0 = now;
                travel = 0;
                TRACE("contact @%d,%d", cx, cy);
                break;
            case GEST_CONTACT: {
                int dx = cx - last_x, dy = cy - last_y;
                if (dx != 0 || dy != 0) {
                    travel += abs(dx) + abs(dy);
                    last_x = cx;
                    last_y = cy;
                }
                int ax = cx - anchor_x, ay = cy - anchor_y;
                if (ax * ax + ay * ay > DRAG_ARM_PX * DRAG_ARM_PX) {
                    gest_state_ = GEST_POINT;
                    stat_points_++;
                    TRACE("point (left %dpx deadzone)", DRAG_ARM_PX);
                    /* Deadzone delta swallowed: last_* already current,
                     * so the cursor continues without a jump. */
                } else if (now - t0 >= DRAG_ARM_MS) {
                    gest_state_ = GEST_DRAG;
                    stat_drags_++;
                    TRACE("drag armed after %lldms dwell", now - t0);
                    if (emit) {
                        hid_ble_mouse_button("left", true);
                    }
                }
                break;
            }
            case GEST_POINT: {
                int dx = cx - last_x, dy = cy - last_y;
                if (dx != 0 || dy != 0) {
                    travel += abs(dx) + abs(dy);
                    last_x = cx;
                    last_y = cy;
                    if (emit) {
                        hid_ble_mouse_move(dx * POINTER_SCALE,
                                           dy * POINTER_SCALE);
                    }
                    TRACE("move %+d,%+d", dx, dy);
                }
                break;
            }
            case GEST_DRAG: {
                int dx = cx - last_x, dy = cy - last_y;
                if (dx != 0 || dy != 0) {
                    last_x = cx;
                    last_y = cy;
                    if (emit) {
                        hid_ble_mouse_move(dx * POINTER_SCALE,
                                           dy * POINTER_SCALE);
                    }
                    TRACE("drag %+d,%+d", dx, dy);
                }
                break;
            }
            }
        } else if (gest_state_ != GEST_IDLE) {
            if (n > 0) {
                /* Mode switched mid-touch: cancel the gesture without
                 * leaving a stuck button behind. */
                TRACE("gesture cancelled (mode change)");
                if (gest_state_ == GEST_DRAG && emit) {
                    hid_ble_mouse_button("left", false);
                }
                gest_state_ = GEST_IDLE;
            } else if (gest_state_ == GEST_DRAG) {
                TRACE("drag end");
                if (emit) {
                    hid_ble_mouse_button("left", false);
                }
            } else if (gest_state_ == GEST_CONTACT &&
                       (now - t0) < TAP_MAX_MS && travel < TAP_MAX_PX) {
                /* Tap: the finger never left the CONTACT deadzone (any real
                 * movement promotes to POINT first, and POINT lifts are
                 * deliberately not taps -- see the header comment). */
                stat_taps_++;
                bool dbl = false;
                if (last_tap_valid_ &&
                    (now - last_tap_at_) <= DBLTAP_MAX_MS) {
                    int ddx = anchor_x - last_tap_x_;
                    int ddy = anchor_y - last_tap_y_;
                    if (ddx * ddx + ddy * ddy <=
                        DBLTAP_MAX_PX * DBLTAP_MAX_PX) {
                        dbl = true;
                    }
                }
                TRACE("tap (%lldms, %dpx)%s", now - t0, travel,
                      dbl ? " -> double-click" : "");
                if (emit) {
                    if (dbl) {
                        /* Two rapid press/release pairs (120 ms apart):
                         * the OS reliably joins these into a double-click.
                         * The first tap's single click already fired, so the
                         * host sees click + double-click -- standard. */
                        hid_ble_mouse_dblclick("left");
                    } else {
                        /* Fire immediately: never hold the first click
                         * hostage waiting to see if a second tap comes. */
                        hid_ble_mouse_click("left");
                    }
                }
                /* A double-tap consumes the pair: the next tap starts a
                 * fresh sequence rather than chaining a third click into
                 * another double. */
                last_tap_valid_ = !dbl;
                last_tap_at_ = now;
                last_tap_x_ = anchor_x;
                last_tap_y_ = anchor_y;
            } else {
                TRACE("release from %s, no tap (%lldms, %dpx)",
                      input_gesture_state_name(), now - t0, travel);
                /* A non-tap release breaks any pending double-tap pair. */
                last_tap_valid_ = false;
            }
            gest_state_ = GEST_IDLE;
        }

        /* ---- wheel: scroll in pointer mode, volume in media mode,
         * up/down arrows in keys mode, reset in timer mode
         * (on-device: no BLE needed).
         * Push + scroll (button held while rotating) cycles the mode ring.
         * Only when PTT hasn't claimed the button — once PTT owns a
         * long-press, rotation belongs to the recording, not mode-cycle. */
        int detents = wheel_take_delta();
        if (detents != 0) {
            TRACE("wheel %+d detent(s)", detents);
            if (display_pair_visible()) {
                /* NCP-1: Pairing modal has input priority: wheel selects button. */
                display_pair_wheel_nav(detents);
            } else if (display_message_visible()) {
                /* Message overlay has input priority: wheel scrolls it. */
                display_message_scroll(detents);
            } else if (btn_down && !ptt_button_claimed()) {
                TRACE("push+scroll: cycling mode");
                mode_next(); /* display follows via the change callback */
                ptt_note_mode_cycle(); /* keep PTT off this press */
                btn_mode_cycled = true;
            } else if (mode_get() == MODE_TIMER) {
                if (timer_running()) {
                    /* Ignore scroll while running: prevents accidental reset
                     * mid-timing. Pause first (wheel-click), then scroll. */
                } else if (timer_is_countdown() || timer_elapsed_ms() == 0) {
                    /* Paused at zero (or in countdown mode): scroll sets the
                     * countdown minutes. 1 detent = 1 minute, 0 = stopwatch. */
                    int mins = timer_countdown_minutes() + detents;
                    if (mins < 0) {
                        mins = 0;
                    }
                    if (mins > 99) {
                        mins = 99;
                    }
                    timer_set_countdown(mins);
                    timer_draw();
                    TRACE("countdown %d min", mins);
                } else {
                    /* Paused stopwatch with time on it: scroll resets. */
                    timer_reset();
                    timer_draw();
                    TRACE("timer reset");
                }
            } else if (emit) {
                if (mode_get() == MODE_MEDIA) {
                    int steps = abs(detents);
                    const char *code =
                        detents > 0 ? "vol_up" : "vol_down";
                    for (int i = 0; i < steps; i++) {
                        hid_ble_consumer(code);
                    }
                    /* Track volume locally (5% per detent) for the UI bar.
                     * HID consumer is one-way, so this can drift from the
                     * host's actual volume if changed elsewhere. */
                    media_volume_ += detents * 5;
                    if (media_volume_ < 0) {
                        media_volume_ = 0;
                    }
                    if (media_volume_ > 100) {
                        media_volume_ = 100;
                    }
                    display_set_media_volume(media_volume_);
                } else if (mode_get() == MODE_KEYS) {
                    /* Clockwise (positive) steps down through a list,
                     * counter-clockwise steps up. Direction feel is a
                     * bench check: flip if it feels inverted. */
                    int steps = abs(detents);
                    const char *code = detents > 0 ? "down" : "up";
                    for (int i = 0; i < steps; i++) {
                        hid_ble_key(code, NULL);
                    }
                } else {
                    hid_ble_mouse_wheel(detents * WHEEL_LINES);
                }
            }
        }

        /* ---- wheel-click button (active-low, debounced) ----
         * Short press: mode action (click / play-pause).
         * Hold >= 500ms (no rotation): PTT claims the button (see ptt.c).
         * Push + scroll: cycles the mode ring (handled in the wheel
         * section above); the release then skips the short-press. */
        bool down = wheel_button_down();
        int64_t t = now_ms();
        if (down != btn_down && t - btn_edge_at > BTN_DEBOUNCE_MS) {
            btn_down = down;
            btn_edge_at = t;
            if (down) {
                btn_mode_cycled = false;
                TRACE("wheel-click down");
            } else {
                TRACE("wheel-click up");
                /* NCP-1: Pairing modal intercepts clicks: confirm selected button. */
                if (display_pair_visible()) {
                    display_pair_wheel_click();
                } else if (!btn_mode_cycled && !ptt_button_claimed() &&
                    (emit || mode_get() == MODE_TIMER)) {
                    mode_short_press();
                }
                btn_mode_cycled = false;
            }
        }

        /* NCP-1: Long-press (1.5s) on wheel button while pairing modal is
         * visible aborts the pairing. Checked every loop while held;
         * fires once per press via the aborted latch. */
        static bool pair_aborted_ = false;
        if (display_pair_visible() && btn_down && !pair_aborted_ &&
            (t - btn_edge_at > 1500)) {
            pair_aborted_ = true;
            ESP_LOGW("input", "pairing aborted via wheel long-press");
            hid_ble_abort_pairing();
        }
        if (!btn_down) {
            pair_aborted_ = false;
        }

        /* ---- timer readout: 10 Hz refresh while running in timer mode ---- */
        if (mode_get() == MODE_TIMER && (timer_running() || timer_finished())) {
            if (now - timer_draw_at >= 100) {
                timer_draw_at = now;
                timer_draw();
                /* Countdown just hit zero: flash the LED green (success),
                 * not red — red means error per the approved LED language.
                 * The finished latch keeps us drawing 00:00.00 until the
                 * user acknowledges (wheel-click) or resets (scroll). */
                static bool alerted_ = false;
                if (timer_finished() && !alerted_) {
                    alerted_ = true;
                    led_flash_success();
                    TRACE("countdown finished");
                } else if (!timer_finished()) {
                    alerted_ = false;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* Deferred device retry: a transient touch_init()/wheel_init() failure at
 * boot must not permanently kill the pointer (v0.4.2 failure mode: one
 * failed touch_init() -> input_init() refused -> no pointer until
 * reboot). This supervisor retries the missing device(s) with backoff
 * until both are up, then deletes itself. It also covers the case where
 * input_init() never started the input task at all (neither device ready
 * at boot): each pass re-attempts input_init() once a device comes up.
 * 2 KB internal stack; it never touches NVS/flash. */
#define INPUT_RETRY_STACK   2048
static const int input_retry_delays_s_[] = { 5, 10, 20, 40, 60 };
#define INPUT_RETRY_DELAYS_N \
    (sizeof(input_retry_delays_s_) / sizeof(input_retry_delays_s_[0]))
static TaskHandle_t retry_task_ = NULL;
/* v0.4.6: input task runs on PSRAM stack (was 16KB internal). */
static task_psram_handle_t *input_psram_ = NULL;
/* v0.4.6: retry task runs on PSRAM stack. The psram handle is kept so the
 * underlying TaskHandle (h->task) stays valid for the retry_task_ check. */
static task_psram_handle_t *retry_psram_ = NULL;

static void input_retry_task(void *arg)
{
    (void)arg;
    size_t step = 0;
    for (;;) {
        int delay_s = input_retry_delays_s_[
            step < INPUT_RETRY_DELAYS_N ? step : INPUT_RETRY_DELAYS_N - 1];
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        if (!touch_ready()) {
            ESP_LOGW(TAG, "retrying touch_init (attempt %u)",
                     (unsigned)(step + 1));
            touch_init();
        }
        if (!wheel_ready()) {
            ESP_LOGW(TAG, "retrying wheel_init (attempt %u)",
                     (unsigned)(step + 1));
            wheel_init();
        }
        if (step + 1 < INPUT_RETRY_DELAYS_N) {
            step++;
        }
        if (!ready_ && (touch_ready() || wheel_ready())) {
            /* A device came up after input_init() refused at boot:
             * start the input task now. */
            input_init();
        }
        if (ready_ && touch_ready() && wheel_ready()) {
            ESP_LOGI(TAG, "touch+wheel both up; retry supervisor done");
            retry_task_ = NULL;
            vTaskDelete(NULL);
        }
    }
}

/* Idempotent: at most one supervisor runs.
 * v0.4.6: PSRAM stack (was 8KB internal). */
static void input_start_retry_task(void)
{
    if (retry_task_) {
        return;
    }
    retry_psram_ = task_psram_create(input_retry_task, "input_retry",
                                    INPUT_RETRY_STACK * 4, NULL, 2,
                                    tskNO_AFFINITY);
    if (!retry_psram_) {
        ESP_LOGE(TAG, "retry task create failed");
        retry_task_ = NULL;
    } else {
        retry_task_ = retry_psram_->task;
    }
}

esp_err_t input_init(void)
{
    if (ready_) {
        return ESP_OK;
    }
    /* Either device suffices: a dead touchpad must not kill the working
     * scroll wheel (or vice versa). The poll loop already skips a missing
     * device gracefully -- touch_read() returns 0 when !touch_ready(),
     * wheel_take_delta() stays 0 with no knob callbacks, and
     * wheel_button_down() returns false when !wheel_ready(). */
    bool t = touch_ready();
    bool w = wheel_ready();
    if (!t && !w) {
        ESP_LOGE(TAG, "neither touch nor wheel ready; background retry armed");
        input_start_retry_task();
        return ESP_FAIL;
    }
    /* v0.4.6: PSRAM stack (was 16KB internal). */
    input_psram_ = task_psram_create(input_task, "input", 4096 * 4, NULL, 3,
                                    tskNO_AFFINITY);
    if (!input_psram_) {
        ESP_LOGE(TAG, "input task create failed");
        input_start_retry_task();
        return ESP_FAIL;
    }
    ready_ = true;
    ESP_LOGI(TAG, "pointer mode on (dwell-to-drag) [touch:%s wheel:%s]",
             t ? "up" : "down", w ? "up" : "down");
    if (!t || !w) {
        /* Bring the missing device up late if it was a transient boot
         * failure (v0.4.2: a single touch_init() failure at boot
         * permanently killed the pointer until reboot). */
        input_start_retry_task();
    }
    return ESP_OK;
}

bool input_ready(void)
{
    return ready_;
}

void input_set_pointer_enabled(bool enabled)
{
    pointer_enabled_ = enabled;
    ESP_LOGI(TAG, "pointer %s", enabled ? "enabled" : "disabled");
}

bool input_pointer_enabled(void)
{
    return pointer_enabled_;
}

void input_set_trace(bool on)
{
    trace_ = on;
    ESP_LOGI(TAG, "input trace %s", on ? "on" : "off");
}

bool input_trace(void)
{
    return trace_;
}

int input_gesture_state(void)
{
    return gest_state_;
}

const char *input_gesture_state_name(void)
{
    switch (gest_state_) {
    case GEST_CONTACT:
        return "contact";
    case GEST_POINT:
        return "point";
    case GEST_DRAG:
        return "drag";
    default:
        return "idle";
    }
}

void input_gesture_config(int *dwell_ms, int *radius_px, int *tap_ms,
                          int *tap_px)
{
    if (dwell_ms) {
        *dwell_ms = DRAG_ARM_MS;
    }
    if (radius_px) {
        *radius_px = DRAG_ARM_PX;
    }
    if (tap_ms) {
        *tap_ms = TAP_MAX_MS;
    }
    if (tap_px) {
        *tap_px = TAP_MAX_PX;
    }
}

void input_gesture_stats(uint32_t *taps, uint32_t *drags, uint32_t *points)
{
    if (taps) {
        *taps = stat_taps_;
    }
    if (drags) {
        *drags = stat_drags_;
    }
    if (points) {
        *points = stat_points_;
    }
}

bool input_media_playing(void)
{
    return media_playing_;
}

int input_media_volume(void)
{
    return media_volume_;
}
