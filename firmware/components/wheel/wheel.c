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

bool wheel_button_down(void)
{
    if (!ready_ || !expander_) {
        return false;
    }
    uint32_t levels = 0;
    /* get_level takes a pin mask and returns the input register. */
    if (esp_io_expander_get_level(expander_, (1u << WHEEL_BTN_PIN),
                                 &levels) != ESP_OK) {
        return false;
    }
    return (levels & (1u << WHEEL_BTN_PIN)) == 0; /* active-low */
}
