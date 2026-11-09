/* Watcher RGB LED language — single WS2812 on GPIO40.
 *
 * A 50ms esp_timer drives the pattern engine (non-blocking). Background
 * states are priority-arbitrated: the highest-priority *active* request wins.
 * Transient patterns (flashes, camera countdown) preempt the background for
 * their duration, then the background resumes.
 *
 * Priority (high to low): ERROR > RECORDING > STARTUP_ISSUE >
 *   BLE_ADVERTISING > AI_THINKING > WORKING > PARTY > OFF
 *
 * Note: there is no "idle glow" — when nothing is active the LED is dark.
 * Startup runs a 3s rainbow flourish as a transient, then
 * the LED goes dark until something needs it.
 */

#include "led.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "led_strip.h"

static const char *TAG = "led";

#define LED_GPIO            40
#define LED_COUNT           1
#define TICK_MS             50

/* ---- color helpers ---- */
typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t C_GREEN  = {0, 255, 0};
static const rgb_t C_BLUE   = {0, 0, 255};
static const rgb_t C_RED    = {255, 0, 0};
static const rgb_t C_YELLOW = {255, 255, 0};
static const rgb_t C_PURPLE = {160, 32, 255};
static const rgb_t C_WHITE  = {255, 255, 255};
static const rgb_t C_OFF    = {0, 0, 0};

/* ---- state ---- */
static led_strip_handle_t strip_ = NULL;
static esp_timer_handle_t tick_ = NULL;
static SemaphoreHandle_t lock_ = NULL;
static bool ready_ = false;

/* active background requests; effective = highest priority (lowest index)
 * that is set. Falls back to OFF (dark) when nothing else is active. */
static bool active_[LED_STATE_COUNT];
static led_state_t effective_ = LED_STATE_OFF;

/* transient overlay */
typedef enum {
    TR_NONE,
    TR_FLASH_GREEN_3X,   // 3x green flash (BLE connected)
    TR_SUCCESS,          // solid green 1s
    TR_ERROR,            // 2x red flash
    TR_CAM_COUNTDOWN,    // 3x white blink then hold off
    TR_CAM_RESULT_OK,    // green flash
    TR_CAM_RESULT_FAIL,  // red flash
    TR_BUTTON_ACK,       // 50ms white tick
    TR_STARTUP_RAINBOW,  // 3s rainbow flourish (fade in -> breathe -> fade out)
} transient_t;

static transient_t transient_ = TR_NONE;
static int64_t transient_start_us_ = 0;
static bool cam_held_off_ = false; /* set after countdown, cleared by result */

/* ---- boot transient queue (v0.4.4) ----
 * During boot, transients (startup rainbow, BLE-connected green flash, ...)
 * must play sequentially, not preempt each other. While boot_queueing_ is
 * true, start_transient() enqueues into this FIFO instead of preempting; the
 * 50ms tick pops the next one when no transient is active. led_boot_complete()
 * (called from main when boot finishes) disables queueing and drops leftovers.
 * Static storage: internal DRAM (.bss), no heap, no extra task. */
#define BOOT_QUEUE_DEPTH 8
static bool boot_queueing_ = false;
static QueueHandle_t boot_queue_ = NULL;
static StaticQueue_t boot_queue_struct_;
static uint8_t boot_queue_buf_[BOOT_QUEUE_DEPTH * sizeof(transient_t)];

static int64_t now_us(void) { return esp_timer_get_time(); }

/* HSV (h 0-359) -> RGB, full saturation/value. For the rainbow. */
static rgb_t hsv2rgb(int h)
{
    h %= 360;
    int c = 255, x = (255 * (60 - abs((h % 120) - 60))) / 60;
    int m = 0;
    int r, g, b;
    if (h < 60)      { r = c; g = x; b = 0; }
    else if (h < 120){ r = x; g = c; b = 0; }
    else if (h < 180){ r = 0; g = c; b = x; }
    else if (h < 240){ r = 0; g = x; b = c; }
    else if (h < 300){ r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    rgb_t out = {(uint8_t)(r + m), (uint8_t)(g + m), (uint8_t)(b + m)};
    return out;
}

static void show(rgb_t c)
{
    if (!strip_) return;
    led_strip_set_pixel(strip_, 0, c.r, c.g, c.b);
    led_strip_refresh(strip_);
}

static rgb_t scale(rgb_t c, float k)
{
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    rgb_t o = {(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k)};
    return o;
}

/* elapsed ms since t0 */
static uint32_t el_ms(int64_t t0) { return (uint32_t)((now_us() - t0) / 1000); }

/* square-wave blink: period_ms full cycle, duty fraction on */
static bool blink_on(uint32_t t_ms, uint32_t period_ms, float duty)
{
    return (t_ms % period_ms) < (uint32_t)(period_ms * duty);
}

/* breathe 0..1..0 over period_ms */
static float breathe_k(uint32_t t_ms, uint32_t period_ms)
{
    float ph = (float)(t_ms % period_ms) / (float)period_ms; /* 0..1 */
    return 0.5f - 0.5f * cosf(ph * 2.0f * (float)M_PI);
}

static void recompute_effective_locked(void)
{
    for (int i = 0; i < LED_STATE_COUNT; i++) {
        if (active_[i]) { effective_ = (led_state_t)i; return; }
    }
    effective_ = LED_STATE_OFF;
}

/* transient durations */
static uint32_t transient_duration_ms(transient_t tr)
{
    switch (tr) {
    case TR_FLASH_GREEN_3X: return 900;
    case TR_SUCCESS:        return 800; /* one breath pulse */
    case TR_ERROR:          return 700;
    case TR_CAM_COUNTDOWN:  return 600;
    case TR_CAM_RESULT_OK:
    case TR_CAM_RESULT_FAIL: return 600;
    case TR_BUTTON_ACK:     return 120;
    case TR_STARTUP_RAINBOW: return 3000;
    default:                return 0;
    }
}

/* start a transient immediately; caller must hold lock_. Bypasses the boot
 * queue (used by the tick pump and by start_transient when not queueing). */
static void start_transient_locked(transient_t tr)
{
    /* ERROR and RECORDING are never preempted by transients. Drop the
     * transient and log rather than hiding a critical state. All other
     * backgrounds can be preempted normally. */
    if (effective_ == LED_STATE_ERROR || effective_ == LED_STATE_RECORDING ||
        effective_ == LED_STATE_REC_ENDING) {
        ESP_LOGW(TAG, "transient %d dropped (background %d active)",
                 (int)tr, (int)effective_);
        return;
    }
    transient_ = tr;
    transient_start_us_ = now_us();
    if (tr == TR_CAM_COUNTDOWN) {
        /* after the countdown the LED holds off until led_camera_result() */
    }
}

static void start_transient(transient_t tr)
{
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (boot_queueing_ && boot_queue_ != NULL) {
        /* Boot: queue for sequential playback instead of preempting.
         * Never block the caller; drop (with a warning) if full. */
        if (xQueueSend(boot_queue_, &tr, 0) != pdTRUE) {
            ESP_LOGW(TAG, "boot transient %d dropped (queue full)", (int)tr);
        }
        xSemaphoreGive(lock_);
        return;
    }
    start_transient_locked(tr);
    xSemaphoreGive(lock_);
}

/* render one tick; called with lock held */
static void render_locked(void)
{
    /* boot queue pump: when idle, start the next queued transient so boot
     * lights play sequentially to completion. After led_boot_complete()
     * (boot_queueing_ == false) the pump continues to drain leftovers —
     * new transients preempt via start_transient_locked() and never enter
     * the queue, so this only drains what was queued during boot. */
    if (transient_ == TR_NONE && boot_queue_ != NULL) {
        transient_t next;
        if (xQueueReceive(boot_queue_, &next, 0) == pdTRUE) {
            /* never let a boot transient hide a critical background */
            if (effective_ == LED_STATE_ERROR || effective_ == LED_STATE_RECORDING ||
        effective_ == LED_STATE_REC_ENDING) {
                ESP_LOGW(TAG, "boot transient %d dropped (background %d active)",
                         (int)next, (int)effective_);
            } else {
                transient_ = next;
                transient_start_us_ = now_us();
            }
        }
    }

    uint32_t t = el_ms(transient_start_us_);

    /* ---- transient overlay ---- */
    if (transient_ != TR_NONE) {
        uint32_t dur = transient_duration_ms(transient_);
        if (t > dur + 500) {
            /* stuck-transient watchdog: a transient must never wedge the
             * boot queue (or the overlay). Force-complete with a warning. */
            ESP_LOGW(TAG, "transient %d stuck (%lu ms > %lu max), force-completing",
                     (int)transient_, (unsigned long)t, (unsigned long)dur);
            transient_ = TR_NONE;
            show(C_OFF);
            /* fall through to background */
        } else if (t >= dur) {
            transient_t done = transient_;
            transient_ = TR_NONE;
            if (done == TR_CAM_COUNTDOWN) {
                cam_held_off_ = true;
                show(C_OFF);
                return;
            }
            if (done == TR_STARTUP_RAINBOW) {
                /* rainbow flourish complete -> dark */
                transient_ = TR_NONE;
                show(C_OFF);
                return;
            }
            /* fall through to background */
        } else {
            switch (transient_) {
            case TR_FLASH_GREEN_3X:
                show(blink_on(t, 300, 0.5f) ? C_GREEN : C_OFF);
                return;
            case TR_SUCCESS:
                /* single breath pulse: 0 -> full green -> 0 over 800ms */
                show(scale(C_GREEN, breathe_k(t, 800)));
                return;
            case TR_ERROR:
                show(blink_on(t, 350, 0.5f) ? C_RED : C_OFF);
                return;
            case TR_CAM_COUNTDOWN:
                show(blink_on(t, 200, 0.5f) ? C_WHITE : C_OFF);
                return;
            case TR_CAM_RESULT_OK:
                show(blink_on(t, 300, 0.6f) ? C_GREEN : C_OFF);
                return;
            case TR_CAM_RESULT_FAIL:
                show(blink_on(t, 300, 0.6f) ? C_RED : C_OFF);
                return;
            case TR_BUTTON_ACK:
                show(C_WHITE);
                return;
            case TR_STARTUP_RAINBOW:
                /* 3s flourish: fade in (0.5s) -> breathe 1Hz for 2s ->
                 * fade out (0.5s). Hue cycles continuously throughout. */
                {
                    float env;
                    if (t < 500) {
                        env = (float)t / 500.0f;
                    } else if (t < 2500) {
                        env = breathe_k(t, 1000);
                    } else {
                        env = 1.0f - (float)(t - 2500) / 500.0f;
                    }
                    rgb_t c = hsv2rgb((int)((t * 360) / 1500));
                    show(scale(c, env));
                }
                return;
            default:
                break;
            }
        }
    }

    if (cam_held_off_) {
        show(C_OFF);
        return;
    }

    /* ---- background state ---- */
    uint32_t bt = (uint32_t)(now_us() / 1000);
    switch (effective_) {
    case LED_STATE_ERROR:
        show(scale(C_RED, 0.25f + 0.75f * breathe_k(bt, 1200)));
        break;
    case LED_STATE_REC_ENDING:
        /* blinking red — still recording, but trailing silence has
         * been heard and the take will end shortly (v0.4.44). */
        show(blink_on(bt, 500, 0.5f) ? C_RED : C_OFF);
        break;
    case LED_STATE_RECORDING:
        show(C_RED); /* steady — mic is hot */
        break;
    case LED_STATE_STARTUP_ISSUE:
        show(scale(C_YELLOW, 0.25f + 0.75f * breathe_k(bt, 1500)));
        break;
    case LED_STATE_BLE_ADVERTISING:
        show(blink_on(bt, 500, 0.5f) ? C_BLUE : C_OFF); /* 2 blinks/sec */
        break;
    case LED_STATE_AI_THINKING:
        show(scale(C_PURPLE, 0.2f + 0.8f * breathe_k(bt, 1000)));
        break;
    case LED_STATE_WORKING:
        /* breathing green — device working on something, OK state.
         * Slightly slower than AI thinking (1200ms vs 1000ms) so the two
         * are distinguishable if seen back-to-back. */
        show(scale(C_GREEN, 0.2f + 0.8f * breathe_k(bt, 1200)));
        break;
    case LED_STATE_PARTY:
        show(hsv2rgb((int)((bt * 360) / 3000)));
        break;
    case LED_STATE_IDLE_CONNECTED:
        /* legacy: no longer used as fallback, treated as off */
        show(C_OFF);
        break;

    case LED_STATE_OFF:
    default:
        show(C_OFF);
        break;
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(lock_, 0) != pdTRUE) return; /* never block the timer */
    render_locked();
    xSemaphoreGive(lock_);
}

/* ---------------- public API ---------------- */

esp_err_t led_init(void)
{
    if (ready_) return ESP_OK;

    lock_ = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lock_, ESP_ERR_NO_MEM, TAG, "no mutex");

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000, /* 10MHz */
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip_),
                        TAG, "strip init failed");
    led_strip_clear(strip_);

    memset(active_, 0, sizeof(active_));
    active_[LED_STATE_OFF] = true;
    effective_ = LED_STATE_OFF;

    /* boot transient queue: static storage (internal DRAM), no heap. */
    boot_queue_ = xQueueCreateStatic(BOOT_QUEUE_DEPTH, sizeof(transient_t),
                                     boot_queue_buf_, &boot_queue_struct_);
    ESP_RETURN_ON_FALSE(boot_queue_, ESP_ERR_NO_MEM, TAG, "no boot queue");
    boot_queueing_ = true;

    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .name = "led_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &tick_), TAG, "timer create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_, TICK_MS * 1000),
                        TAG, "timer start");

    ready_ = true;

    /* startup flourish: 3s rainbow (fade in -> breathe -> fade out) -> dark */
    start_transient(TR_STARTUP_RAINBOW);
    ESP_LOGI(TAG, "ready (GPIO%d)", LED_GPIO);
    return ESP_OK;
}

bool led_ready(void) { return ready_; }

void led_set_state(led_state_t state)
{
    if (!ready_ || state < 0 || state >= LED_STATE_COUNT) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (!active_[state]) {
        active_[state] = true;
        recompute_effective_locked();
    }
    xSemaphoreGive(lock_);
}

void led_clear_state(led_state_t state)
{
    if (!ready_ || state < 0 || state >= LED_STATE_COUNT) return;
    if (state == LED_STATE_OFF) return; /* fallback, never cleared */
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (active_[state]) {
        active_[state] = false;
        recompute_effective_locked();
    }
    xSemaphoreGive(lock_);
}

void led_flash_green(void)    { if (ready_) start_transient(TR_FLASH_GREEN_3X); }
void led_flash_success(void)  { if (ready_) start_transient(TR_SUCCESS); }
void led_flash_error(void)    { if (ready_) start_transient(TR_ERROR); }
void led_button_ack(void)     { if (ready_) start_transient(TR_BUTTON_ACK); }

void led_camera_countdown(void)
{
    if (!ready_) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    cam_held_off_ = false;
    xSemaphoreGive(lock_);
    start_transient(TR_CAM_COUNTDOWN);
}

void led_camera_off(void)
{
    if (!ready_) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    transient_ = TR_NONE;
    cam_held_off_ = true;
    xSemaphoreGive(lock_);
    show(C_OFF);
}

void led_camera_result(bool ok)
{
    if (!ready_) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    cam_held_off_ = false;
    xSemaphoreGive(lock_);
    start_transient(ok ? TR_CAM_RESULT_OK : TR_CAM_RESULT_FAIL);
}

/* v0.4.5: boot is finished. Stop queueing NEW transients (restore preempt
 * behavior for anything requested after this point) but let the tick pump
 * drain whatever is already queued — stale boot lights finish instead of
 * being dropped. Safe to call once from main. */
void led_boot_complete(void)
{
    if (!ready_) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    boot_queueing_ = false;
    /* Do NOT drain the queue here. The 50ms tick pump (render_locked) will
     * continue to pop queued transients until empty, so the BT green (or
     * whatever was queued during boot) actually plays instead of being
     * silently dropped. New transients after this point preempt via
     * start_transient_locked(), as before. */
    xSemaphoreGive(lock_);
    ESP_LOGI(TAG, "boot complete, queue draining (%d pending)",
             boot_queue_ ? (int)uxQueueMessagesWaiting(boot_queue_) : 0);
}
