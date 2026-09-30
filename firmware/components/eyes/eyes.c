/* Eyes: Himax HX6538 camera path.
 *
 * Wiring (SenseCAP Watcher v1.0, from OSHW schematic + official BSP):
 *   I2C0: SDA=GPIO47, SCL=GPIO48 -> PCA9535 expander @ 0x21 (ADDR pins 001)
 *   SPI2: SCLK=GPIO4, MOSI=GPIO5, MISO=GPIO6
 *   SSCMA: CS=GPIO21, SYNC=expander pin 6, RST=expander pin 7, 12 MHz, mode 0
 *   SD card shares SPI2: hold SD CS (GPIO46) high so it stays off the bus.
 *
 * Snapshot flow: SSCMA INVOKE (single shot, image return) -> JPEG copied out
 * of the event callback -> HTTP POST to the host agent's still receiver.
 * Best-effort: if the Himax isn't reachable, eyes stays down and the rest of
 * the firmware (BLE HID + link) keeps working. */

#include "eyes.h"
#include "task_psram.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_io_expander_pca95xx_16bit.h"
#include "sscma_client.h"
#include "sscma_client_io.h"
#include "led.h"
#include "sscma_client_ops.h"
#include "esp_http_client.h"
#include "hal/wdt_hal.h"
#include "eyes.h"

static void feed_rtc_wdt(void);

static const char *TAG = "eyes";

/* Pins (see header comment). NOTE: the SSCMA driver talks to the IO
 * expander through esp_io_expander_*(), whose pin arguments are BITMASKS
 * (IO_EXPANDER_PIN_NUM_n == 1u << n), not pin numbers. Passing a raw pin
 * number here silently addresses the wrong pins: SYNC reads of pin 6
 * become reads of pins 1-2 (charger status, always 0), so the driver
 * believes the Himax never has data. */
#define PIN_I2C_SDA      47
#define PIN_I2C_SCL      48
#define PIN_SPI_SCLK     4
#define PIN_SPI_MOSI     5
#define PIN_SPI_MISO     6
#define PIN_SSCMA_CS     21
#define PIN_SSCMA_SYNC   (1u << 6) /* IO expander pin 6, as a bitmask */
#define PIN_SSCMA_RST    (1u << 7) /* IO expander pin 7, as a bitmask */
#define PIN_SD_CS        46
/* Himax power rail: EXP_Himax_EN, verified against the Seeed schematic
 * (P13 drives Q8's gate: VCC_3V3 -> HIMAX_3V3 -> 1V8 LDO -> all HX6538
 * rails). Bitmask form. Dropping it power-cycles the Himax + sensor. */
#define PIN_HIMAX_RAIL   (1u << 11)

#define SNAP_BUF_SIZE    (512 * 1024)
#define INVOKE_TIMEOUT_MS 15000

/* Sensor resolution opt_ids (Himax SSCMA AT+SENSOR), verified live on the
 * Watcher's Himax firmware 2026-09-23. Its table is custom, NOT the
 * standard SSCMA presets: 1=416x416, 2=480x480, 3=640x480, 4=240x240,
 * 5=1280x720 (advertised but capture times out -- no image returned),
 * 6=1920x1080 (untested).
 * NOTE: 1280x720 JPEG (~100-200KB) must fit in SNAP_BUF_SIZE. If snapshots
 * start failing with truncated frames, bump SNAP_BUF_SIZE or drop to a
 * lower opt. */
#define SENSOR_OPT_DEFAULT 4  /* 640x480 Small, v0.4.3 preset list */

static sscma_client_handle_t client_ = NULL;
static bool ready_ = false;
/* Set by the on_connect callback: the AT handshake completed and the
 * Himax is streaming events. Distinct from ready_ (init sequence ok). */
static bool link_up_ = false;
static EventGroupHandle_t snap_events_;
#define SNAP_GOT_IMAGE BIT0

/* Single in-flight snapshot (guarded by snap_busy_). The flag is claimed
 * in eyes_snap()/eyes_snap_test_async() BEFORE the worker task is created,
 * so two callers can never both pass the check; the worker only releases.
 * snap_busy_since_ records the claim tick: a slot older than 60s is stolen
 * with a warning (recovers from a dead worker task that never released).
 * snap_gen_ is a generation counter bumped on every claim: a stale worker
 * that wakes up late releases only if its generation still owns the slot,
 * so it can never free a slot stolen by a newer request. */
static bool snap_busy_ = false;
static TickType_t snap_busy_since_ = 0;
static uint32_t snap_gen_ = 0;
static portMUX_TYPE snap_spin_ = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *snap_buf_ = NULL;
static size_t snap_len_ = 0;
/* Consecutive snapshot invoke-failure streak (see camera_note_invoke_failure_
 * below). After INVOKE_RECOVER_STREAK straight failures the camera path is
 * presumed desynced and a full recovery runs automatically. */
static int invoke_fail_streak_ = 0;
#define INVOKE_RECOVER_STREAK 3
/* Checksum of the previous snapshot's frame, for the staleness detector
 * in analyze_frame(). */
static uint32_t last_fnv1a_ = 0;
static bool last_fnv_valid_ = false;

/* Defined below (after the base64 codec): decodes the base64 frame text
 * the Himax delivers into raw JPEG bytes, in place. */
static const char *snap_decode_image_(void);

static eyes_emit_cb_t emit_ = NULL;

/* IO expander handle, shared with the display component (LCD power rail). */
static esp_io_expander_handle_t expander_ = NULL;

esp_io_expander_handle_t eyes_io_expander(void)
{
    return expander_;
}

void eyes_set_emit(eyes_emit_cb_t emit)
{
    emit_ = emit;
}

/* ------------------------------------------------------------------ */
/* SSCMA callbacks                                                     */
/* ------------------------------------------------------------------ */

static void on_event(sscma_client_handle_t client,
                     const sscma_client_reply_t *reply, void *user_ctx)
{
    (void)client;
    (void)user_ctx;
    /* Note: reply is recycled after this returns; copy what we need. */
    char *img = NULL;
    int img_size = 0;
    if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) != ESP_OK) {
        return;
    }
    if (snap_busy_ && snap_buf_ && img_size > 0 &&
        img_size <= (int)SNAP_BUF_SIZE) {
        memcpy(snap_buf_, img, img_size);
        snap_len_ = (size_t)img_size;
        xEventGroupSetBits(snap_events_, SNAP_GOT_IMAGE);
    }
    free(img);
}

static void on_log(sscma_client_handle_t client,
                   const sscma_client_reply_t *reply, void *user_ctx)
{
    (void)client;
    (void)user_ctx;
    (void)reply;
}

static void on_connect(sscma_client_handle_t client,
                       const sscma_client_reply_t *reply, void *user_ctx)
{
    (void)client;
    (void)reply;
    (void)user_ctx;
    link_up_ = true;
    ESP_LOGI(TAG, "Himax link connected");
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t eyes_init(void)
{
    snap_events_ = xEventGroupCreate();
    /* 160 KiB must stay in PSRAM: a silent internal fallback here would eat
     * two-thirds of our internal DMA headroom and wedge the display. */
    snap_buf_ = heap_caps_malloc(SNAP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!snap_buf_ || !snap_events_) {
        ESP_LOGE(TAG, "no PSRAM for snapshot buffer");
        return ESP_ERR_NO_MEM;
    }

    /* I2C0 for the IO expander */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    if (i2c_param_config(I2C_NUM_0, &i2c_cfg) != ESP_OK ||
        i2c_driver_install(I2C_NUM_0, i2c_cfg.mode, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return ESP_FAIL;
    }

    esp_io_expander_handle_t expander = NULL;
    if (esp_io_expander_new_i2c_pca95xx_16bit(
            I2C_NUM_0, ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001,
            &expander) != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not found");
        return ESP_FAIL;
    }
    expander_ = expander;

    /* Power-up sequence from the official BSP (bsp_io_expander_init):
     * drive every power output low, raise SYSTEM, wait 100 ms, then raise
     * all rails (SDCARD/LCD/SYSTEM/AI_CHIP/CODEC_PA/GROVE/BAT_ADC) at once.
     * The LCD panel and the Himax AI chip need their rails (and the
     * sequencing) to come up; without this the screen stays dark and the
     * Himax never answers. */
    esp_err_t pwr_err = ESP_OK;
    pwr_err |= esp_io_expander_set_dir(expander, 0x20FFu, IO_EXPANDER_INPUT);
    pwr_err |= esp_io_expander_set_dir(expander, 0xDF00u, IO_EXPANDER_OUTPUT);
    pwr_err |= esp_io_expander_set_level(expander, 0xDF00u, 0);
    pwr_err |= esp_io_expander_set_level(expander, 1u << 10, 1); /* SYSTEM */
    if (pwr_err != ESP_OK) {
        ESP_LOGE(TAG, "power rail setup failed");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (esp_io_expander_set_level(expander, 0xDF00u, 1) != ESP_OK) {
        ESP_LOGE(TAG, "power rails on failed");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "power rails up (SYSTEM then all)");

    /* The SSCMA driver drives RST with set_level() only -- it never sets
     * the pin direction itself. The upstream expander driver rejects
     * set_level on input pins, so take RST as an output here (the power
     * sequence above left it an input). */
    if (esp_io_expander_set_dir(expander, PIN_SSCMA_RST,
                                IO_EXPANDER_OUTPUT) != ESP_OK) {
        ESP_LOGE(TAG, "RST pin direction failed");
        return ESP_FAIL;
    }

    /* SPI2 bus shared with the SD card; park SD CS high. */
    gpio_config_t sd_cs = {
        .pin_bit_mask = (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sd_cs);
    gpio_set_level(PIN_SD_CS, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 init failed");
        return ESP_FAIL;
    }

    sscma_client_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_SSCMA_CS,
        .sync_gpio_num = -1, /* No Himax SYNC line on Watcher HW; use pure SPI polling */
        .spi_mode = 0,
        .wait_delay = 2,
        .pclk_hz = 12 * 1000 * 1000,
        .user_ctx = NULL,
        .io_expander = expander,
        .flags = { .sync_use_expander = true },
    };
    sscma_client_io_handle_t io = NULL;
    if (sscma_client_new_io_spi_bus((sscma_client_spi_bus_handle_t)SPI2_HOST,
                                    &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "sscma io create failed");
        return ESP_FAIL;
    }

    sscma_client_config_t cfg = SSCMA_CLIENT_CONFIG_DEFAULT();
    cfg.event_queue_size = 8;
    cfg.reset_gpio_num = PIN_SSCMA_RST;
    cfg.io_expander = expander;
    cfg.flags.reset_use_expander = true;

    if (sscma_client_new(io, &cfg, &client_) != ESP_OK) {
        ESP_LOGE(TAG, "sscma client create failed");
        return ESP_FAIL;
    }

    const sscma_client_callback_t cb = {
        .on_connect = on_connect,
        .on_event = on_event,
        .on_log = on_log,
    };
    if (sscma_client_register_callback(client_, &cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "sscma callback register failed");
        return ESP_FAIL;
    }
    sscma_client_init(client_);
    /* The Himax may still be booting after the reset pulse above; the
     * factory firmware retries its first query for the same reason.
     * Retry set_model a few times, then check every call's result so a
     * silent Himax is reported honestly instead of "eyes ready". */
    esp_err_t merr = ESP_FAIL;
    for (int i = 0; i < 4; i++) {
        merr = sscma_client_set_model(client_, 1);
        if (merr == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "set_model attempt %d failed: %s", i,
                 esp_err_to_name(merr));
        feed_rtc_wdt();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    /* Log the sensor mode the Himax booted with, so we know the baseline. */
    {
        sscma_client_sensor_t s;
        memset(&s, 0, sizeof(s));
        if (sscma_client_get_sensor(client_, &s) == ESP_OK) {
            ESP_LOGI(TAG, "sensor boot mode: opt_id=%d detail=%s",
                     s.opt_id, s.opt_detail ? s.opt_detail : "(null)");
        } else {
            ESP_LOGW(TAG, "sensor boot mode query failed");
        }
    }
    if (merr != ESP_OK) {
        if (merr == ESP_ERR_NOT_SUPPORTED) {
            /* v0.4.5+: capture-only Himax has no TFLM engine, so MODEL
             * is deliberately unsupported. This is not a failure —
             * proceed with sensor-only init for captures. */
            ESP_LOGI(TAG, "Himax is capture-only (no MODEL support); continuing with sensor-only init");
        } else {
            ESP_LOGE(TAG, "Himax not answering; eyes staying down");
            return ESP_FAIL;
        }
    }

    ready_ = true;

    /* Explicitly initialize the sensor to SENSOR_OPT_DEFAULT (640x480).
     * The Himax boots in an undefined mode (opt_id reads as INT_MIN);
     * without this explicit set, snapshots fail with "camera not ready". */
    {
        int applied_opt = -1;
        char detail[64] = {0};
        esp_err_t serr = eyes_set_sensor_opt(SENSOR_OPT_DEFAULT,
                                             &applied_opt,
                                             detail, sizeof(detail));
        if (serr != ESP_OK) {
            ESP_LOGE(TAG, "sensor opt init failed: %s",
                     esp_err_to_name(serr));
            ready_ = false;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "sensor initialized: opt_id=%d detail=%s",
                 applied_opt, detail);
    }

    ESP_LOGI(TAG, "eyes ready");
    return ESP_OK;
}

bool eyes_ready(void)
{
    return ready_;
}

/* v0.4.3: the AT handshake completed and the Himax is streaming events.
 * on_connect sets it; eyes_recover_camera clears it during recovery and
 * sets it again on success. */
bool eyes_link_up(void)
{
    return link_up_;
}

/* Feed the RTC watchdog (S3 RWDT). The bootloader leaves it armed with a
 * 9 s timeout (CONFIG_BOOTLOADER_WDT_TIME_MS=9000) and app_main's feed loop
 * hasn't started yet during init. A silent Himax makes set_model block
 * ~2 s per attempt; without a feed here, 4-5 failed attempts RTC-WDT the
 * whole device into a bootloop (2026-09-29: v0.3 Himax didn't answer
 * AT+MODEL=1 -> 10 s reboot cycle). Feeding is not disabling: if the loop
 * itself hangs, the WDT still fires. */
static void feed_rtc_wdt(void)
{
    wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rwdt_ctx);
    wdt_hal_feed(&rwdt_ctx);
    wdt_hal_write_protect_enable(&rwdt_ctx);
}

/* ------------------------------------------------------------------ */
/* Camera recovery                                                     */
/* ------------------------------------------------------------------ */

/* Full camera recovery: RST pulse (+ optional Himax rail power-cycle),
 * then AT re-handshake. The caller must guarantee no snapshot is in
 * flight: the auto path runs inside the snap worker that owns the slot,
 * and eyes_reset refuses while snap_busy_. Takes ~2-4 s. Returns ESP_OK
 * if the Himax is answering again. */
esp_err_t eyes_recover_camera(bool cycle_rail)
{
    if (!client_ || !expander_) {
        return ESP_ERR_INVALID_STATE;
    }
    /* v0.4.3: the link is going down for the reset — clear it now so no
     * new snapshot can start mid-recovery. Set again on success below.
     * (sscma_client_reset does NOT re-fire on_connect.) */
    link_up_ = false;
    if (cycle_rail) {
        ESP_LOGW(TAG, "cycling Himax rail off for 300 ms");
        if (esp_io_expander_set_level(expander_, PIN_HIMAX_RAIL, 0)
            != ESP_OK) {
            ESP_LOGE(TAG, "Himax rail drop failed");
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        if (esp_io_expander_set_level(expander_, PIN_HIMAX_RAIL, 1)
            != ESP_OK) {
            ESP_LOGE(TAG, "Himax rail restore failed");
            return ESP_FAIL;
        }
        /* Bulk caps need a moment; the RST pulse below covers the rest. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* RST pulse + parser buffer clear. Suspends/resumes the SSCMA process
     * task internally; safe to call from any other task. */
    if (sscma_client_reset(client_) != ESP_OK) {
        ESP_LOGW(TAG, "sscma_client_reset failed");
        return ESP_FAIL;
    }
    /* The Himax reboots after RST; the AT server needs ~1 s before the
     * re-handshake below (same sequencing as eyes_init). */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_err_t merr = ESP_FAIL;
    for (int i = 0; i < 4; i++) {
        merr = sscma_client_set_model(client_, 1);
        if (merr == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "recovery set_model attempt %d failed: %s", i,
                 esp_err_to_name(merr));
        feed_rtc_wdt();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (merr != ESP_OK) {
        if (merr == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "Himax is capture-only; continuing recovery with sensor-only init");
        } else {
            ESP_LOGE(TAG, "Himax not answering after recovery");
            return merr;
        }
    }
    int applied_opt = -1;
    char detail[64] = {0};
    esp_err_t serr = eyes_set_sensor_opt(SENSOR_OPT_DEFAULT, &applied_opt,
                                         detail, sizeof(detail));
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "recovery sensor opt failed: %s",
                 esp_err_to_name(serr));
        return serr;
    }
    ESP_LOGI(TAG, "camera recovered (rail_cycled=%d)", cycle_rail ? 1 : 0);
    /* v0.4.3: the Himax is answering again — the link is up. */
    link_up_ = true;
    return ESP_OK;
}

bool eyes_snap_busy(void)
{
    bool busy;
    taskENTER_CRITICAL(&snap_spin_);
    busy = snap_busy_;
    taskEXIT_CRITICAL(&snap_spin_);
    return busy;
}

/* ------------------------------------------------------------------ */
/* Snapshot -> HTTP POST                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Snapshot -> shared Himax frame grab                                 */
/* ------------------------------------------------------------------ */

/* Grab one JPEG frame from the Himax into snap_buf_/snap_len_.
 * Returns NULL on success, or a static error string.
 *
 * v0.2.11+: the Himax AT server runs the camera in kRefreshOnReturn stream
 * mode, so every single-shot command returns the frame captured at the END
 * of the PREVIOUS command (confirmed in SSCMA-Micro + sscma-example-we2
 * sources). A bare INVOKE is therefore always one snapshot stale.
 *
 * fresh=true: throwaway primer AT+SAMPLE=1 consumes the stale frame and its
 * completion triggers a fresh sensor capture; the following AT+INVOKE then
 * returns that fresh frame. (SAMPLE's own reply carries data.image, so the
 * primer needs its own wait-and-discard.) If the primer fails to send, we
 * log and fall back to invoke-only.
 * fresh=false: bare INVOKE with no primer — returns whatever frame is
 * currently in the Himax buffer (the "old" image), for comparison/debugging. */
/* Track consecutive snapshot invoke failures. After INVOKE_RECOVER_STREAK
 * straight failures the camera path is presumed desynced (the 0.4.0/0.4.1
 * wedge: transient SPI errors + parser desync) and a full recovery runs
 * instead of failing forever. This runs in the snap worker, which owns
 * the snapshot slot, so no other snapshot can be in flight. A failed
 * recovery resets the streak to avoid a tight recover loop on a truly
 * dead camera. */
static void camera_note_invoke_failure_(void)
{
    if (++invoke_fail_streak_ < INVOKE_RECOVER_STREAK) {
        return;
    }
    invoke_fail_streak_ = 0;
    ESP_LOGW(TAG, "%d consecutive invoke failures; running camera recovery",
             INVOKE_RECOVER_STREAK);
    if (eyes_recover_camera(false) == ESP_OK) {
        ESP_LOGI(TAG, "camera auto-recovery succeeded");
    } else {
        ESP_LOGE(TAG, "camera auto-recovery failed");
    }
}

static const char *snap_grab_frame_(bool fresh)
{
    if (fresh) {
        /* Primer: consume the stale buffered frame. */
        xEventGroupClearBits(snap_events_, SNAP_GOT_IMAGE);
        snap_len_ = 0;
        if (sscma_client_sample(client_, 1) != ESP_OK) {
            ESP_LOGW(TAG, "sample primer failed; falling back to invoke-only");
        } else {
            /* Discard the primer frame; its completion on the Himax triggers
             * the fresh capture we actually want. v0.4.3: check the wait —
             * a silent primer timeout used to fall through to INVOKE with
             * no fresh capture triggered, producing a stale frame with no
             * diagnostic. */
            EventBits_t pb = xEventGroupWaitBits(snap_events_, SNAP_GOT_IMAGE,
                                                 pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(INVOKE_TIMEOUT_MS));
            if ((pb & SNAP_GOT_IMAGE) == 0) {
                ESP_LOGW(TAG, "snap primer: no frame in %d ms; the fresh "
                         "capture may not have triggered (invoke-only "
                         "fallback)", INVOKE_TIMEOUT_MS);
            }
        }
    }
    /* Real capture: with fresh=true this returns the frame triggered by the
     * primer; with fresh=false it returns the buffered (old) frame. */
    xEventGroupClearBits(snap_events_, SNAP_GOT_IMAGE);
    snap_len_ = 0;
    if (sscma_client_invoke(client_, 1, false, true) != ESP_OK) {
        camera_note_invoke_failure_();
        return "invoke failed";
    }
    EventBits_t bits = xEventGroupWaitBits(snap_events_, SNAP_GOT_IMAGE,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(INVOKE_TIMEOUT_MS));
    if ((bits & SNAP_GOT_IMAGE) == 0 || snap_len_ == 0) {
        /* v0.4.3: loud timeout log (was silent — only the returned string
         * carried it). A 15 s INVOKE wait with no frame means the Himax
         * AT server is wedged or the link is down. */
        ESP_LOGW(TAG, "snap INVOKE: no frame in %d ms (snap_len=%u)",
                 INVOKE_TIMEOUT_MS, (unsigned)snap_len_);
        camera_note_invoke_failure_();
        return "no image from Himax (timeout)";
    }
    invoke_fail_streak_ = 0;
    /* The Himax delivers the frame base64-encoded inside the JSON reply;
     * decode to raw JPEG before analysis/streaming. */
    return snap_decode_image_();
}

/* Claim the single in-flight snapshot slot. Records the claim time; a
 * slot older than 60s is stolen with a warning (the worker that owned it
 * is presumed dead). Returns the claim generation on success, 0 when the
 * slot is busy. Logging happens outside the spinlock (ESP_LOG can block
 * on UART — never log while holding a spinlock). */
static uint32_t snap_claim_(void)
{
    bool steal = false;
    TickType_t age = 0;
    uint32_t gen = 0;
    taskENTER_CRITICAL(&snap_spin_);
    TickType_t now = xTaskGetTickCount();
    bool busy = snap_busy_;
    if (busy && (now - snap_busy_since_) >= pdMS_TO_TICKS(60000)) {
        busy = false;
        steal = true;
        age = now - snap_busy_since_;
    }
    if (!busy) {
        snap_busy_ = true;
        snap_busy_since_ = now;
        gen = ++snap_gen_;
    }
    taskEXIT_CRITICAL(&snap_spin_);
    if (steal) {
        ESP_LOGW(TAG, "snapshot slot stuck for %lu ms, stealing it (gen %lu)",
                 (unsigned long)(age * portTICK_PERIOD_MS),
                 (unsigned long)gen);
    }
    return gen;
}

/* Release the slot, but only if the caller's generation still owns it.
 * A stale worker that wakes up after its slot was stolen is a no-op. */
static void snap_release_(uint32_t gen)
{
    taskENTER_CRITICAL(&snap_spin_);
    if (snap_busy_ && snap_gen_ == gen) {
        snap_busy_ = false;
    }
    taskEXIT_CRITICAL(&snap_spin_);
}

struct snap_job {
    char host[64];
    int port;
    int id;
    bool fresh; /* primer + real capture (new image) vs bare invoke (old) */
    char mid[24]; /* originating command's _mid for correlation */
    uint32_t gen; /* camera-slot generation: release only if still owner */
    /* v0.4.6: PSRAM task handle, freed by snap_task on exit. */
    task_psram_handle_t *psram_handle;
};

static void snap_task(void *arg)
{
    struct snap_job *job = (struct snap_job *)arg;
    int64_t t0 = esp_timer_get_time();
    const char *err = NULL;

    /* LED countdown already done by caller. LED is dark; capture now. */

    /* Frame grab shared with the snapshot self-test (v0.2.12: primer +
     * real capture by default; bare invoke when fresh=false). */
    err = snap_grab_frame_(job->fresh);
    led_camera_result(err == NULL);
    if (err) {
        goto done;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/still?id=%d",
             job->host, job->port, job->id);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (!http) {
        err = "http init failed";
        goto done;
    }
    esp_http_client_set_header(http, "Content-Type", "image/jpeg");
    esp_http_client_set_post_field(http, (const char *)snap_buf_, snap_len_);
    esp_err_t herr = esp_http_client_perform(http);
    int status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    if (herr != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "POST failed: %s status=%d",
                 esp_err_to_name(herr), status);
        err = "http post failed";
        goto done;
    }

done: {
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    /* Generation-checked: a stale worker that outlived a 60s steal must
     * not free the newer owner's slot. */
    snap_release_(job->gen);
    if (emit_) {
        if (err) {
            emit_("err", job->id, job->mid, 0, ms, err);
        } else {
            emit_("still_sent", job->id, job->mid, (int)snap_len_, ms, NULL);
        }
    }
    /* v0.4.6: free the PSRAM task resources. The task is self-deleting
     * (vTaskDelete below), so pass NULL for the task handle to skip
     * vTaskDelete inside task_psram_delete — we cannot free the stack
     * we're running on, but the handle (internal RAM) is safe to free.
     * The 16KB PSRAM stack is leaked (documented, bounded — one snapshot
     * at a time via the generation slot). */
    if (job->psram_handle) {
        task_psram_handle_t *h = job->psram_handle;
        job->psram_handle = NULL;
        heap_caps_free(h);  /* free handle only; stack leaks (see above) */
        /* Note: h->stack (16KB PSRAM) is intentionally leaked here.
         * Freeing it while running on it would corrupt the task. */
    }
    free(job);
    vTaskDelete(NULL);
    }
}

esp_err_t eyes_snap(const char *host, int port, int id, bool fresh,
                    const char *mid)
{
    if (!host || !*host || port <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* LED: "say cheese" countdown before we know if camera is ready. */
    led_camera_countdown();
    vTaskDelay(pdMS_TO_TICKS(650));
    led_camera_off();

    if (!ready_) {
        led_camera_result(false); /* red: camera not ready */
        return ESP_FAIL;
    }
    /* v0.4.3: require the live Himax link, not just init-done. ready_
     * only means the init sequence ran; without on_connect the AT
     * server never handshaked and every capture would hang until the
     * monitor deadline. Refuse immediately with the red result. */
    if (!link_up_) {
        ESP_LOGW(TAG, "snap refused: Himax link not up (no on_connect yet)");
        led_camera_result(false); /* red: camera not ready */
        return ESP_FAIL;
    }
    /* Claim the in-flight slot BEFORE the worker task exists (see
     * eyes_snap_test_async). */
    uint32_t gen = snap_claim_();
    if (!gen) {
        return ESP_ERR_INVALID_STATE; /* one snapshot at a time */
    }
    /* v0.4.6: PSRAM for job struct (was internal). Task runs on PSRAM stack. */
    struct snap_job *job = heap_caps_malloc(sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) {
        snap_release_(gen);
        return ESP_ERR_NO_MEM;
    }
    job->gen = gen;
    strncpy(job->host, host, sizeof(job->host) - 1);
    job->host[sizeof(job->host) - 1] = '\0';
    job->port = port;
    job->id = id;
    job->fresh = fresh;
    strncpy(job->mid, mid ? mid : "", sizeof(job->mid) - 1);
    job->mid[sizeof(job->mid) - 1] = '\0';
    job->psram_handle = NULL;
    /* v0.4.6: PSRAM stack, 4096 words = 16384 bytes (was 16KB internal).
     * Transient task; handle freed by snap_task on exit (stack leaks,
     * documented — one snapshot at a time). */
    job->psram_handle = task_psram_create(snap_task, "snap", 4096 * 4, job,
                                          6, tskNO_AFFINITY);
    if (!job->psram_handle) {
        snap_release_(gen);
        free(job);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Himax debug tooling (v0.2.8)                                        */
/* ------------------------------------------------------------------ */

void eyes_status(cJSON *o)
{
    cJSON_AddBoolToObject(o, "ready", ready_);
    cJSON_AddBoolToObject(o, "link_up", link_up_);
    if (!client_) {
        return;
    }
    /* AT queries: best-effort, a silent Himax leaves nulls. */
    sscma_client_info_t *info = NULL;
    if (sscma_client_get_info(client_, &info, false) == ESP_OK && info) {
        cJSON *ji = cJSON_CreateObject();
        if (info->id) cJSON_AddStringToObject(ji, "id", info->id);
        if (info->name) cJSON_AddStringToObject(ji, "name", info->name);
        if (info->hw_ver) cJSON_AddStringToObject(ji, "hw_ver", info->hw_ver);
        if (info->sw_ver) cJSON_AddStringToObject(ji, "sw_ver", info->sw_ver);
        if (info->fw_ver) cJSON_AddStringToObject(ji, "fw_ver", info->fw_ver);
        cJSON_AddItemToObject(o, "info", ji);
    }
    sscma_client_model_t *model = NULL;
    if (sscma_client_get_model(client_, &model, false) == ESP_OK && model) {
        cJSON *jm = cJSON_CreateObject();
        cJSON_AddNumberToObject(jm, "id", model->id);
        if (model->name) cJSON_AddStringToObject(jm, "name", model->name);
        if (model->ver) cJSON_AddStringToObject(jm, "ver", model->ver);
        cJSON_AddItemToObject(o, "model", jm);
    }
    sscma_client_sensor_t sensor;
    memset(&sensor, 0, sizeof(sensor));
    if (sscma_client_get_sensor(client_, &sensor) == ESP_OK) {
        cJSON *js = cJSON_CreateObject();
        cJSON_AddNumberToObject(js, "id", sensor.id);
        cJSON_AddNumberToObject(js, "type", sensor.type);
        cJSON_AddNumberToObject(js, "state", sensor.state);
        cJSON_AddNumberToObject(js, "opt_id", sensor.opt_id);
        if (sensor.opt_detail)
            cJSON_AddStringToObject(js, "opt_detail", sensor.opt_detail);
        cJSON_AddItemToObject(o, "sensor", js);
    }
}

esp_err_t eyes_at(const char *cmd, char *out, size_t out_len)
{
    if (!cmd || !*cmd || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client_ || !ready_) {
        return ESP_FAIL;
    }
    sscma_client_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    esp_err_t err = sscma_client_request(client_, cmd, &reply, true,
                                         pdMS_TO_TICKS(3000));
    if (err != ESP_OK) {
        snprintf(out, out_len, "<no reply: %s>", esp_err_to_name(err));
        return err;
    }
    if (reply.payload) {
        char *js = cJSON_PrintUnformatted(reply.payload);
        if (js) {
            strncpy(out, js, out_len - 1);
            out[out_len - 1] = '\0';
            cJSON_free(js);
        } else {
            snprintf(out, out_len, "<unprintable payload>");
        }
    } else if (reply.data && reply.len > 0) {
        size_t n = reply.len < out_len - 1 ? reply.len : out_len - 1;
        memcpy(out, reply.data, n);
        out[n] = '\0';
    } else {
        snprintf(out, out_len, "<empty reply>");
    }
    sscma_client_reply_clear(&reply);
    return ESP_OK;
}

esp_err_t eyes_set_sensor_opt(int opt_id, int *applied_opt,
                              char *detail_out, size_t detail_len)
{
    if (!client_ || !ready_) {
        return ESP_FAIL;
    }
    if (opt_id >= 0) {
        esp_err_t serr = ESP_FAIL;
        for (int i = 0; i < 3 && serr != ESP_OK; i++) {
            serr = sscma_client_set_sensor(client_, 0, opt_id, true);
            if (serr != ESP_OK) {
                ESP_LOGW(TAG, "set_sensor opt %d attempt %d failed: %s",
                         opt_id, i, esp_err_to_name(serr));
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        if (serr != ESP_OK) {
            return serr;
        }
        /* Give the Himax a beat to settle the new mode before querying. */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    sscma_client_sensor_t s;
    memset(&s, 0, sizeof(s));
    esp_err_t err = sscma_client_get_sensor(client_, &s);
    if (err == ESP_OK) {
        if (applied_opt) {
            *applied_opt = s.opt_id;
        }
        if (detail_out && detail_len > 0) {
            snprintf(detail_out, detail_len, "%s",
                     s.opt_detail ? s.opt_detail : "(null)");
        }
        ESP_LOGI(TAG, "sensor opt now: opt_id=%d detail=%s",
                 s.opt_id, s.opt_detail ? s.opt_detail : "(null)");
    }
    return err;
}

esp_err_t eyes_invoke_noimg(void)
{
    if (!client_ || !ready_) {
        return ESP_FAIL;
    }
    /* show=false -> AT+INVOKE=1,0,1 (results-only, skip JPEG encode).
     * Synchronous; the sscma_client waits ~10s for the reply. */
    esp_err_t err = sscma_client_invoke(client_, 1, false, false);
    ESP_LOGI(TAG, "invoke_noimg: %s", esp_err_to_name(err));
    return err;
}

/* --- snapshot self-test (no Wi-Fi) --------------------------------- */

static eyes_snaptest_cb_t snaptest_cb_ = NULL;
static eyes_snaptest_chunk_cb_t snaptest_chunk_cb_ = NULL;

void eyes_set_snaptest_cb(eyes_snaptest_cb_t cb)
{
    snaptest_cb_ = cb;
}

void eyes_set_snaptest_chunk_cb(eyes_snaptest_chunk_cb_t cb)
{
    snaptest_chunk_cb_ = cb;
}

/* Tiny base64 encoder. n must be a multiple of 3; out needs 4*n/3 + 1. */
static void b64_encode_3(const uint8_t *in, size_t n, char *out)
{
    static const char ALPH[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = ALPH[(v >> 18) & 63];
        out[o++] = ALPH[(v >> 12) & 63];
        out[o++] = ALPH[(v >> 6) & 63];
        out[o++] = ALPH[v & 63];
    }
    out[o] = '\0';
}

/* Tiny base64 decoder. In-place safe: the read index always stays ahead of
 * the write index (4 chars in -> at most 3 bytes out). Returns the decoded
 * length, or 0 on invalid input. Only trailing '=' padding is accepted. */
static int b64_rev_(uint8_t ch)
{
    if (ch >= 'A' && ch <= 'Z') return (int)ch - 'A';
    if (ch >= 'a' && ch <= 'z') return (int)ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return (int)ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static size_t b64_decode_inplace_(uint8_t *buf, size_t len)
{
    if (len == 0 || len % 4 != 0) {
        return 0;
    }
    size_t w = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint8_t c0 = buf[i], c1 = buf[i + 1], c2 = buf[i + 2], c3 = buf[i + 3];
        bool last = (i + 4 == len);
        if (c0 == '=' || c1 == '=') {
            return 0;
        }
        if ((c2 == '=' || c3 == '=') && !last) {
            return 0;
        }
        if (c2 == '=' && c3 != '=') {
            return 0;
        }
        int a = b64_rev_(c0), b = b64_rev_(c1);
        int c = (c2 == '=') ? 0 : b64_rev_(c2);
        int d = (c3 == '=') ? 0 : b64_rev_(c3);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return 0;
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                     ((uint32_t)c << 6) | (uint32_t)d;
        buf[w++] = (uint8_t)((v >> 16) & 0xFF);
        if (c2 != '=') {
            buf[w++] = (uint8_t)((v >> 8) & 0xFF);
        }
        if (c3 != '=') {
            buf[w++] = (uint8_t)(v & 0xFF);
        }
    }
    return w;
}

/* The SSCMA invoke reply carries the frame as a base64 *string* inside the
 * JSON payload (see sscma_utils_fetch_image_from_reply), so right after
 * capture snap_buf_ holds base64 text, not raw JPEG bytes. Decode it in
 * place; on success snap_len_ becomes the JPEG length. Returns NULL on
 * success, or a static error string. */
static const char *snap_decode_image_(void)
{
    if (snap_len_ >= 2 && snap_buf_[0] == 0xFF && snap_buf_[1] == 0xD8) {
        return NULL; /* already raw JPEG */
    }
    size_t dec = b64_decode_inplace_(snap_buf_, snap_len_);
    if (dec == 0) {
        return "image is not valid base64";
    }
    snap_len_ = dec;
    if (snap_len_ < 2 || snap_buf_[0] != 0xFF || snap_buf_[1] != 0xD8) {
        return "decoded image is not a JPEG";
    }
    /* The Himax appends a few dozen zero bytes after EOI; trim them so
     * the frame is a clean SOI..EOI JPEG. The last FFD9 is the EOI
     * marker (0xFF bytes inside scan data are always stuffed with 0x00,
     * so FFD9 cannot occur elsewhere). */
    for (size_t i = snap_len_; i >= 2; i--) {
        if (snap_buf_[i - 2] == 0xFF && snap_buf_[i - 1] == 0xD9) {
            snap_len_ = i;
            break;
        }
    }
    return NULL;
}

/* Stream the captured frame as base64 chunks (768 input bytes -> 1024
 * chars per chunk) through the chunk callback. Only the final chunk can
 * carry RFC 4648 '=' padding. */
static void stream_frame_b64(int id, const char *mid)
{
    size_t len = snap_len_;
    int total = (int)((len + 767) / 768);
    char b64[1029]; /* 768 in -> 1024 chars, +4 worst-case pad + NUL */
    for (int seq = 0; seq < total; seq++) {
        size_t off = (size_t)seq * 768;
        size_t n = len - off > 768 ? 768 : len - off;
        size_t full = n - (n % 3); /* multiple-of-3 prefix */
        size_t o = 0;
        if (full > 0) {
            b64_encode_3(snap_buf_ + off, full, b64);
            o = strlen(b64);
        }
        size_t tail = n - full;
        if (tail > 0) {
            /* 1 or 2 leftover bytes -> 2 or 3 chars + '=' padding. */
            uint8_t tmp[3] = {0, 0, 0};
            memcpy(tmp, snap_buf_ + off + full, tail);
            char t64[5];
            b64_encode_3(tmp, 3, t64);
            size_t want = tail == 1 ? 2 : 3;
            memcpy(b64 + o, t64, want);
            o += want;
            while (want < 4) {
                b64[o++] = '=';
                want++;
            }
        }
        b64[o] = '\0';
        if (snaptest_chunk_cb_) {
            snaptest_chunk_cb_(id, mid, seq, total, b64);
        }
    }
}

static void analyze_frame(const uint8_t *buf, size_t len,
                          eyes_snaptest_result_t *res)
{
    memset(res, 0, sizeof(*res));
    res->bytes = (int)len;
    res->soi = len >= 2 && buf[0] == 0xFF && buf[1] == 0xD8;
    res->eoi = len >= 2 && buf[len - 2] == 0xFF && buf[len - 1] == 0xD9;
    /* FNV-1a 32-bit */
    uint32_t h = 2166136261u;
    uint64_t sum = 0;
    uint8_t mn = 0xFF, mx = 0;
    for (size_t i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 16777619u;
        sum += buf[i];
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
    }
    res->fnv1a = h;
    /* Staleness detector: a snapshot whose checksum matches the previous
     * frame byte-for-byte almost certainly came from a stale Himax buffer. */
    res->fresh = !last_fnv_valid_ || (h != last_fnv1a_);
    last_fnv1a_ = h;
    last_fnv_valid_ = true;
    res->min_b = mn;
    res->max_b = mx;
    res->mean_b = len ? (uint8_t)(sum / len) : 0;
    size_t hn = len < 16 ? len : 16;
    for (size_t i = 0; i < hn; i++) {
        snprintf(res->head_hex + i * 2, 3, "%02x", buf[i]);
    }
    size_t tn = len < 8 ? len : 8;
    for (size_t i = 0; i < tn; i++) {
        snprintf(res->tail_hex + i * 2, 3, "%02x", buf[len - tn + i]);
    }
}

struct snaptest_job {
    int id;
    bool send; /* stream base64 chunks through the chunk callback */
    bool fresh; /* primer + real capture (new image) vs bare invoke (old) */
    char mid[24]; /* originating command's _mid for correlation */
    uint32_t gen; /* camera-slot generation: release only if still owner */
    /* v0.4.6: PSRAM task handle, freed by snaptest_task on exit. */
    task_psram_handle_t *psram_handle;
};

static void snaptest_task(void *arg)
{
    struct snaptest_job *job = (struct snaptest_job *)arg;
    eyes_snaptest_result_t res;
    memset(&res, 0, sizeof(res));
    int64_t t0 = esp_timer_get_time();

    /* LED countdown already done by eyes_snap_test_async() caller.
     * LED is dark here; capture now. */

    /* Frame grab shared with the HTTP snapshot path (v0.2.12). */
    res.err = snap_grab_frame_(job->fresh);
    led_camera_result(res.err == NULL);
    if (res.err) {
        goto done;
    }
    analyze_frame(snap_buf_, snap_len_, &res);
    res.ms = (int)((esp_timer_get_time() - t0) / 1000);

done:
    if (snaptest_cb_) {
        snaptest_cb_(job->id, job->mid, &res);
    }
    if (!res.err && job->send) {
        /* Result first (browser shows stats), then the image stream.
         * Keep the buffer busy until the stream finishes so a second
         * snaptest can't stomp snap_buf_ mid-stream. */
        stream_frame_b64(job->id, job->mid);
    }
    /* Generation-checked release (see snap_task). */
    snap_release_(job->gen);
    /* v0.4.6: free PSRAM task handle (stack leaks, documented — see snap_task). */
    if (job->psram_handle) {
        heap_caps_free(job->psram_handle);
        job->psram_handle = NULL;
    }
    free(job);
    vTaskDelete(NULL);
}

esp_err_t eyes_snap_test_async(int id, bool send, bool fresh, const char *mid)
{
    /* LED: "say cheese" countdown happens BEFORE we know if the camera
     * is ready — people need to see we're operating. If not ready,
     * show red fail after the countdown. */
    led_camera_countdown();
    vTaskDelay(pdMS_TO_TICKS(650));
    led_camera_off();

    if (!ready_ || !client_) {
        led_camera_result(false); /* red: camera not ready */
        return ESP_FAIL;
    }
    /* v0.4.3: require the live Himax link, not just init-done (see
     * eyes_snap). Without on_connect the capture would hang until the
     * monitor deadline with no result; refuse immediately instead. */
    if (!link_up_) {
        ESP_LOGW(TAG, "snaptest refused: Himax link not up (no on_connect yet)");
        led_camera_result(false); /* red: camera not ready */
        return ESP_FAIL;
    }
    /* Claim the in-flight slot BEFORE the worker task exists, so two
     * callers cannot both pass the check. The worker only releases it. */
    uint32_t gen = snap_claim_();
    if (!gen) {
        return ESP_ERR_INVALID_STATE; /* one snapshot at a time */
    }
    /* v0.4.6: PSRAM for job struct (was internal). Task runs on PSRAM stack. */
    struct snaptest_job *job = heap_caps_malloc(sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) {
        snap_release_(gen);
        return ESP_ERR_NO_MEM;
    }
    job->gen = gen;
    job->id = id;
    job->send = send;
    job->fresh = fresh;
    strncpy(job->mid, mid ? mid : "", sizeof(job->mid) - 1);
    job->mid[sizeof(job->mid) - 1] = '\0';
    job->psram_handle = NULL;
    /* v0.4.6: PSRAM stack, 4096 words = 16384 bytes (was 16KB internal).
     * Transient task; handle freed by snaptest_task on exit. */
    job->psram_handle = task_psram_create(snaptest_task, "snaptest",
                                          4096 * 4, job, 6, tskNO_AFFINITY);
    if (!job->psram_handle) {
        snap_release_(gen);
        free(job);
        return ESP_FAIL;
    }
    return ESP_OK;
}
