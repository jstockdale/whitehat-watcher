/* Scroll wheel: GPIO quadrature encoder + expander push-button.
 *
 * Hardware (SenseCAP Watcher v1.0, from the official BSP):
 *   Encoder phases: GPIO41 (A), GPIO42 (B) -- plain inputs with S3 internal
 *   pull-ups, always powered (passive mechanical encoder).
 *   Wheel-click: PCA9535 expander pin 3, active-low, read by polling.
 *
 * Decoding uses the vendor's `knob` driver (registry espressif/knob 0.1.4,
 * same version the BSP ships): an esp_timer polls every ~3 ms with ~6 ms
 * debounce, no GPIO interrupts. Callbacks run in timer context, so they do
 * nothing but bump a counter; the input task consumes it. */

#include "wheel.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "iot_knob.h"
#include "esp_io_expander.h"
#include "eyes.h"

static const char *TAG = "wheel";

#define WHEEL_GPIO_A   GPIO_NUM_41
#define WHEEL_GPIO_B   GPIO_NUM_42
#define WHEEL_BTN_PIN  (3) /* expander pin, active-low */

static bool ready_ = false;
static knob_handle_t knob_ = NULL;
static esp_io_expander_handle_t expander_ = NULL;
/* Written from the knob timer callback, read from the input task. */
static volatile int detents_ = 0;
static volatile int total_ = 0;
/* The timer callback can preempt the input task between the read and the
 * clear in wheel_take_delta(); the spinlock makes take atomic, and the
 * writers take it too so a take can't interleave a callback's RMW and
 * resurrect consumed detents. */
static portMUX_TYPE spinlock_ = portMUX_INITIALIZER_UNLOCKED;

/* The knob timer callback runs in the esp_timer task and can land on the
 * other core mid-RMW: if it increments between the input task's read and
 * clear, consumed detents get resurrected as phantom scrolls. Both writers
 * and all readers take the spinlock. */
static void on_knob_right(void *arg, void *usr_data)
{
    (void)arg;
    (void)usr_data;
    portENTER_CRITICAL(&spinlock_);
    detents_++;
    total_++;
    portEXIT_CRITICAL(&spinlock_);
}

static void on_knob_left(void *arg, void *usr_data)
{
    (void)arg;
    (void)usr_data;
    portENTER_CRITICAL(&spinlock_);
    detents_--;
    total_--;
    portEXIT_CRITICAL(&spinlock_);
}

esp_err_t wheel_init(void)
{
    if (ready_) {
        return ESP_OK;
    }

    expander_ = eyes_io_expander();
    if (!expander_) {
        ESP_LOGE(TAG, "no IO expander (eyes_init must run first)");
        return ESP_FAIL;
    }

    knob_config_t cfg = {
        .default_direction = 0,   /* positive increase */
        .gpio_encoder_a = WHEEL_GPIO_A,
        .gpio_encoder_b = WHEEL_GPIO_B,
    };
    knob_ = iot_knob_create(&cfg);
    if (!knob_) {
        ESP_LOGE(TAG, "iot_knob_create failed");
        return ESP_FAIL;
    }
    esp_err_t ret = iot_knob_register_cb(knob_, KNOB_RIGHT, on_knob_right,
                                         NULL);
    if (ret == ESP_OK) {
        ret = iot_knob_register_cb(knob_, KNOB_LEFT, on_knob_left, NULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "knob callback register failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ready_ = true;
    ESP_LOGI(TAG, "wheel ready (GPIO41/42, click on expander pin 3)");
    return ESP_OK;
}

bool wheel_ready(void)
{
    return ready_;
}

int wheel_take_delta(void)
{
    int d;
    portENTER_CRITICAL(&spinlock_);
    d = detents_;
    detents_ = 0;
    portEXIT_CRITICAL(&spinlock_);
    return d;
}

int wheel_count(void)
{
    int t;
    portENTER_CRITICAL(&spinlock_);
    t = total_;
    portEXIT_CRITICAL(&spinlock_);
    return t;
}

/** Detents accumulated since the last wheel_take_delta(), without
 *  consuming them. Diagnostic: lets the `wheel` debug command report
 *  pending detents without stealing them from the input task. */
int wheel_pending(void)
{
    int d;
    portENTER_CRITICAL(&spinlock_);
    d = detents_;
    portEXIT_CRITICAL(&spinlock_);
    return d;
}

/** Raw encoder pin levels, for diagnosing a silent wheel. */
void wheel_gpio_levels(int *a, int *b)
{
    if (a) {
        *a = gpio_get_level(WHEEL_GPIO_A);
    }
    if (b) {
        *b = gpio_get_level(WHEEL_GPIO_B);
    }
}

/* ---- button bus-read cache + failure backoff (v0.4.48.1) ----
 *
 * Root cause of the recurring I2C interrupt-WDT panic (sightings on
 * 0.4.40 / 0.4.43 / 0.4.47 / 0.4.48, fully symbolized 2026-11-09):
 * the vendor PCA95xx driver's read cache never engages on our
 * expander — it was created without an INT pin (int_gpio == -1),
 * its cache gate treats that as "always read the bus", and the
 * non-ex create leaves update_interval_us = 0 anyway. So this
 * function, polled every 10 ms by input_task (and by ptt_task),
 * issued a REAL I2C0 transaction ~100x/second, ~8.6M/day. Rare
 * marginal-bus timeouts (30 ms timeout in the vendor read) drop
 * into the legacy driver's timeout path, which runs
 * i2c_hw_fsm_reset — and occasionally that recovery spins in ISR
 * context past the interrupt watchdog: panic, reboot, clean
 * recovery. The watchdog is correct (John's ruling); the exposure
 * was the bug.
 *
 * Fix here (vendor/ is never modified): sample the bus at most
 * every BTN_READ_INTERVAL_MS — a hold-to-talk button with a 500 ms
 * claim threshold and ~30 ms debounce loses nothing at 50 ms
 * sampling — and after a failed read, back off for
 * BTN_FAIL_BACKOFF_MS and serve the last known state, so a bus
 * episode meets a quiet bus instead of a 100 Hz hammer while the
 * driver is resetting it. Episode start/recovery are logged so the
 * cure is measurable in telemetry.
 */
#define BTN_READ_INTERVAL_MS 50
#define BTN_FAIL_BACKOFF_MS  250

static int64_t btn_last_attempt_ms_ = 0;
static bool btn_cached_down_ = false;
static bool btn_have_cache_ = false;
static bool btn_backing_off_ = false;
static uint32_t btn_episodes_ = 0;

bool wheel_button_down(void)
{
    if (!ready_ || !expander_) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    int64_t interval = btn_backing_off_ ? BTN_FAIL_BACKOFF_MS
                                        : BTN_READ_INTERVAL_MS;
    if (btn_have_cache_ && (now - btn_last_attempt_ms_) < interval) {
        return btn_cached_down_;
    }
    btn_last_attempt_ms_ = now;
    uint32_t levels = 0;
    /* get_level takes a pin mask and returns the input register. */
    if (esp_io_expander_get_level(expander_, (1u << WHEEL_BTN_PIN),
                                  &levels) != ESP_OK) {
        if (!btn_backing_off_) {
            btn_backing_off_ = true;
            btn_episodes_++;
            ESP_LOGW(TAG, "expander read failed (episode %u) — "
                          "backing off %d ms, serving last state",
                     (unsigned)btn_episodes_, BTN_FAIL_BACKOFF_MS);
        }
        return btn_cached_down_;
    }
    if (btn_backing_off_) {
        btn_backing_off_ = false;
        ESP_LOGI(TAG, "expander read recovered (episodes: %u)",
                 (unsigned)btn_episodes_);
    }
    btn_cached_down_ = (levels & (1u << WHEEL_BTN_PIN)) == 0; /* active-low */
    btn_have_cache_ = true;
    return btn_cached_down_;
}
