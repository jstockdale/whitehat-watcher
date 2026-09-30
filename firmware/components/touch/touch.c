/* SPD2010 integrated capacitive touch, on its own I2C bus.
 *
 * Hardware (SenseCAP Watcher v1.0, from the official BSP + schematics):
 *   I2C1: SDA=GPIO39, SCL=GPIO38, 400 kHz, touch is the only device on it
 *   Touch controller address: 0x53 (touch lives inside the SPD2010 on the
 *   LCD flex; there is no separate touch chip on the main board)
 *   TP interrupt arrives on PCA9535 expander pin 5, but the vendor BSP never
 *   uses it -- touch is polled, and so are we.
 *   Power: touch is fed by the LCD 3.3V rail (expander pin 9), so this must
 *   run after the power sequence in eyes_init()/display_init().
 *
 * Coordinates come back in display pixel space (412x412); neither the
 * driver nor LVGL applies any scaling.
 *
 * v0.2.1: the touch controller sometimes stops answering a few seconds
 * after boot (I2C transactions fail, then the bus wedges and the polling
 * task goes silent). touch_read() now counts consecutive failures and
 * runs touch_recover(): tear down the driver stack, bit-bang a bus clear
 * (9 SCL clocks + STOP) to release a stuck slave, and bring the bus back
 * up. A mutex serializes the input task and the debug-console reader
 * across the recovery window. */

#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_spd2010.h"

static const char *TAG = "touch";

#define TOUCH_I2C_NUM   I2C_NUM_1
#define TOUCH_I2C_SDA   39
#define TOUCH_I2C_SCL   38
#define TOUCH_I2C_CLK   400000

/* Polls (at 100 Hz) of consecutive failure before a recovery attempt. */
#define TOUCH_FAILS_BEFORE_RECOVER 100
/* Polls to wait after a *failed* recovery before trying again. */
#define TOUCH_RECOVER_COOLDOWN_POLLS 1000

static bool ready_ = false;
static esp_lcd_touch_handle_t tp_ = NULL;
static esp_lcd_panel_io_handle_t io_ = NULL;
static SemaphoreHandle_t lock_ = NULL;
static int consec_fails_ = 0;
static int recover_cooldown_ = 0;

/* Bring up the I2C driver, panel IO, and touch driver. The bus must be
 * idle; callers hold lock_ and set ready_. */
static esp_err_t touch_hw_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = TOUCH_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_CLK,
    };
    esp_err_t ret = i2c_param_config(TOUCH_I2C_NUM, &i2c_conf);
    if (ret == ESP_OK) {
        ret = i2c_driver_install(TOUCH_I2C_NUM, i2c_conf.mode, 0, 0, 0);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C1 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_io_i2c_config_t io_cfg =
        ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    ret = esp_lcd_new_panel_io_i2c(TOUCH_I2C_NUM, &io_cfg, &io_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO create failed: %s",
                 esp_err_to_name(ret));
        i2c_driver_delete(TOUCH_I2C_NUM);
        return ret;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_X_MAX,
        .y_max = TOUCH_Y_MAX,
        .rst_gpio_num = GPIO_NUM_NC, /* shared with LCD reset (no GPIO) */
        .int_gpio_num = GPIO_NUM_NC, /* polled; vendor BSP ignores TP_INT */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ret = esp_lcd_touch_new_i2c_spd2010(io_, &tp_cfg, &tp_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch driver create failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(io_);
        io_ = NULL;
        i2c_driver_delete(TOUCH_I2C_NUM);
        return ret;
    }

    /* Throwaway reads to let the controller settle, as the BSP does. The
     * SPD2010 needs a beat after power-up before it answers, and its
     * driver logs an error per failed read -- retry quietly (muting the
     * tag during the settle) instead of spraying boot errors. NOTE: the
     * mute must be ESP_LOG_NONE; WARN still shows ERROR logs. */
    esp_log_level_set("SPD2010", ESP_LOG_NONE);
    esp_err_t rd = ESP_FAIL;
    for (int i = 0; i < 10 && rd != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        rd = esp_lcd_touch_read_data(tp_);
    }
    esp_log_level_set("SPD2010", ESP_LOG_ERROR);
    if (rd != ESP_OK) {
        ESP_LOGW(TAG, "touch settle read failed; polling may recover it");
    }
    return ESP_OK;
}

/* Tear the driver stack down (best effort; the bus may be wedged). */
static void touch_hw_teardown(void)
{
    if (tp_) {
        esp_lcd_touch_del(tp_);
        tp_ = NULL;
    }
    if (io_) {
        esp_lcd_panel_io_del(io_);
        io_ = NULL;
    }
    i2c_driver_delete(TOUCH_I2C_NUM);
}

/* Bit-bang a standard I2C bus clear: up to 9 SCL clocks to let a stuck
 * slave finish its byte, then a STOP condition. Call with the I2C driver
 * deleted so the pins are plain GPIO. */
static void touch_bus_clear(void)
{
    gpio_config_t bc = {
        .pin_bit_mask = (1ULL << TOUCH_I2C_SDA) | (1ULL << TOUCH_I2C_SCL),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&bc) != ESP_OK) {
        return;
    }
    /* Release both lines; if SDA is held low a slave is stuck. */
    gpio_set_level(TOUCH_I2C_SDA, 1);
    gpio_set_level(TOUCH_I2C_SCL, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(TOUCH_I2C_SCL, 0);
        esp_rom_delay_us(5);
        gpio_set_level(TOUCH_I2C_SCL, 1);
        esp_rom_delay_us(5);
        if (gpio_get_level(TOUCH_I2C_SDA)) {
            break; /* slave released SDA */
        }
    }
    /* STOP: SDA rises while SCL is high. */
    gpio_set_level(TOUCH_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(TOUCH_I2C_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(TOUCH_I2C_SDA, 1);
    esp_rom_delay_us(10);
}

esp_err_t touch_init(void)
{
    if (ready_) {
        return ESP_OK;
    }
    if (!lock_) {
        lock_ = xSemaphoreCreateMutex();
        if (!lock_) {
            ESP_LOGE(TAG, "touch init failed: xSemaphoreCreateMutex "
                     "returned NULL (no heap for the input lock)");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(lock_, portMAX_DELAY);
    esp_err_t ret = touch_hw_init();
    if (ret != ESP_OK) {
        /* One bus-clear + retry before giving up: a wedged I2C bus at
         * boot (the v0.2.1 failure mode) must not permanently kill the
         * touchpad when a bus clear would have freed it. */
        ESP_LOGW(TAG, "touch hw init failed (%s); bus-clear and one retry",
                 esp_err_to_name(ret));
        touch_hw_teardown();
        touch_bus_clear();
        ret = touch_hw_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "touch hw init failed after bus-clear retry: %s",
                     esp_err_to_name(ret));
        }
    }
    if (ret == ESP_OK) {
        ready_ = true;
        ESP_LOGI(TAG, "SPD2010 touch ready (I2C1 0x53, polled)");
    }
    xSemaphoreGive(lock_);
    return ret;
}

bool touch_ready(void)
{
    return ready_;
}

/* Attempt bus recovery. Caller holds lock_. */
static void touch_recover(void)
{
    ESP_LOGW(TAG, "touch bus unresponsive, attempting recovery");
    ready_ = false;
    touch_hw_teardown();
    touch_bus_clear();
    if (touch_hw_init() == ESP_OK) {
        ready_ = true;
        consec_fails_ = 0;
        ESP_LOGI(TAG, "touch bus recovered");
    } else {
        ESP_LOGE(TAG, "touch bus recovery failed, cooling down");
        recover_cooldown_ = TOUCH_RECOVER_COOLDOWN_POLLS;
    }
}

int touch_read(uint16_t *x, uint16_t *y, uint16_t *strength)
{
    if (!x || !y || !strength) {
        return 0;
    }
    if (!lock_) {
        return 0;
    }
    xSemaphoreTake(lock_, portMAX_DELAY);
    int n = 0;
    if (ready_ && tp_) {
        if (esp_lcd_touch_read_data(tp_) == ESP_OK) {
            esp_lcd_touch_point_data_t pts[TOUCH_MAX_POINTS];
            uint8_t cnt = 0;
            if (esp_lcd_touch_get_data(tp_, pts, &cnt, TOUCH_MAX_POINTS)
                    == ESP_OK) {
                if (cnt > TOUCH_MAX_POINTS) {
                    cnt = TOUCH_MAX_POINTS;
                }
                for (uint8_t i = 0; i < cnt; i++) {
                    x[i] = pts[i].x;
                    y[i] = pts[i].y;
                    strength[i] = pts[i].strength;
                }
                n = (int)cnt;
            }
            consec_fails_ = 0;
        } else {
            consec_fails_++;
        }
        if (recover_cooldown_ > 0) {
            recover_cooldown_--;
        } else if (consec_fails_ >= TOUCH_FAILS_BEFORE_RECOVER) {
            touch_recover();
        }
    }
    xSemaphoreGive(lock_);
    return n;
}
