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
#include "png_stream.h"
#include "png_crash_detector.h"

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
#include "mbedtls/sha256.h"
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

#define SNAP_BUF_SIZE    (2560 * 1024)  /* v0.4.18.57: 2.5MB for PNG (was 1MB).
                                         * Claude analysis 2026-10-07: observed 0.45 B/px
                                         * -> 1.7MB for 1296x972. 2.5MB covers +50%.
                                         * True ceiling 3.9MB for incompressible. */
#define INVOKE_TIMEOUT_MS 15000

/* v0.4.18.37: Forward declaration for shared base64 chunk helper */
static size_t b64_encode_chunk_(const uint8_t *buf, size_t n, char *b64_out);

/* PNG frame dimensions (used by snap_grab_frame_png_ and eyes_capture_png) */
#define PNG_FRAME_W      1296
#define PNG_FRAME_H      972
#define PNG_FRAME_BYTES  ((size_t)PNG_FRAME_W * PNG_FRAME_H)
#define PNG_OUT_CAP      ((size_t)2 * 1024 * 1024)

/* Himax sensor presets (AT+SENSOR=0,1,<preset>), 1-based, matching the
 * capture-only Himax firmware's _presets[] table (v0.4.18):
 *  1=240x240 Small, 2=416x416 Small, 3=480x480 Small (boot default),
 *  4=640x480 Small, 5=240x240 Medium, 6=416x416 Medium, 7=480x480 Medium,
 *  8=1296x972 RAW8 (raw capture buffer; required for AT+SPIDUMP bulk pull).
 * Full sensor res (2592x1944) is via AT+CROP strip windows, not a preset. */
#define SENSOR_PRESET_640x480   4
#define SENSOR_PRESET_1296x972  8
/* NOTE: 1296x972 is now the boot default (set via discovery in the
 * handshake); SENSOR_OPT_DEFAULT is retained for the snapshot path. */
#define SENSOR_OPT_DEFAULT SENSOR_PRESET_1296x972

/* v0.4.18: discovered sensor mode table. Populated from AT+SENSORS? at
 * handshake/link-up; maps resolutions to Himax preset IDs so the S3
 * selects modes by what it wants (WxH), not by hardcoded preset numbers.
 * New Himax presets appear automatically. Guarded by sensor_modes_mux_. */
#define SENSOR_MODES_MAX 16
typedef struct {
    uint16_t width;
    uint16_t height;
    int      preset_id;   /* 1-based opt_id for AT+SENSOR */
} sensor_mode_t;
static sensor_mode_t sensor_modes_[SENSOR_MODES_MAX];
static int sensor_modes_count_ = 0;
static SemaphoreHandle_t sensor_modes_mux_ = NULL;

static sscma_client_handle_t client_ = NULL;
static sscma_client_io_handle_t io_ = NULL;  /* v0.4.18.11: for SPI self-test */
static bool ready_ = false;
/* Set by the on_connect callback: the AT handshake completed and the
 * Himax is streaming events. Distinct from ready_ (init sequence ok). */
static bool link_up_ = false;
/* v0.4.14 (P0.1): Himax-reboot detection state. The Himax broadcasts
 * INIT@* on every boot (4x: SENSOR/ALGORITHM/TRIGGER/MODEL). If on_connect
 * fires while link_up_ is already set AND handshake_done_ is true AND the
 * link has been up longer than RELINK_DEBOUNCE_MS, the Himax rebooted
 * under us (watchdog, brownout, RST glitch) — clear the link and re-run
 * the handshake in a worker task. The debounce ignores the boot burst
 * (the 4 boot broadcasts arrive within ~1 s of link-up); a reboot inside
 * the debounce window is still caught by the snapshot-failure streak
 * (INVOKE_RECOVER_STREAK) which runs eyes_recover_camera. The pending
 * request list is deliberately NOT flushed on reboot: the process task
 * may be iterating it and freeing entries under its cursor would be a
 * use-after-free; waiters self-drain via their timeouts, and the
 * handshake's 4 set_model retries tolerate one stolen reply.
 * relink_inflight_ guards against duplicate workers (the monitor task is
 * single-threaded so a plain bool suffices). */
static bool handshake_done_ = false;
static int64_t link_up_at_ms_ = 0;
static bool relink_inflight_ = false;
#define RELINK_DEBOUNCE_MS 10000

/* ------------------------------------------------------------------ */
/* v0.4.50 link hardening (WI-2/WI-3): link generation, recovery        */
/* accounting, and the link-health supervisor (ladder owner, below).   */
/*                                                                      */
/* link_gen_ increments every time Himax session state is lost and     */
/* re-established (successful eyes_recover_camera, successful reboot   */
/* re-link). Session-scoped Himax state (crop window, sample mode)     */
/* does not survive a reset, so a state command that timed out across  */
/* a generation change may have died with the old session; its caller  */
/* may re-issue it exactly once (spec §2.5).                           */
/* ------------------------------------------------------------------ */
static volatile uint32_t link_gen_ = 0;
static volatile bool link_recovery_in_progress_ = false;
static uint32_t rec_reset_ = 0;   /* rung-2 (CMD_RESET) credited recoveries */
static uint32_t rec_rst_ = 0;     /* rung-3 (RST) credited recoveries */
static uint32_t rec_rail_ = 0;    /* rung-4 (rail) credited recoveries */
static const char *last_rung_ = "none";
static bool supervisor_started_ = false;
static void eyes_link_health_task_(void *arg);

uint32_t eyes_link_gen(void)
{
    return link_gen_;
}
#define RELINK_WORKER_STACK 4096
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
/* v0.4.18.63 (John 2026-10-08): AOB PSRAM buffer for RAW frame.
 * Allocated at boot (when PSRAM is unfragmented), kept for lifetime.
 * Used by snap_grab_frame_png_ and capture_png instead of on-demand
 * malloc, avoiding fragmentation-related allocation failures.
 * Sized exactly at PNG_FRAME_BYTES (1296x972 max from Himax). */
#define RAW_FRAME_BUF_SIZE PNG_FRAME_BYTES
static uint8_t *raw_frame_buf_ = NULL;
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
                       const sscma_client_reply_t *reply, void *user_ctx);
/* Forward declarations: handshake helpers used by on_connect, defined below. */
static esp_err_t eyes_set_model_retry_(const char *why);
static esp_err_t eyes_rehandshake_(const char *why);
static esp_err_t eyes_discover_sensor_modes_(void);
esp_err_t eyes_select_sensor_mode(uint16_t width, uint16_t height);
static void eyes_relink_async_(void);
static void feed_rtc_wdt(void);

static void on_connect(sscma_client_handle_t client,
                       const sscma_client_reply_t *reply, void *user_ctx)
{
    (void)client;
    (void)user_ctx;
    const char *ev = "?";
    if (reply && reply->payload) {
        const cJSON *name = cJSON_GetObjectItem(reply->payload, "name");
        if (name && name->valuestring) {
            ev = name->valuestring;
        }
    }
    /* v0.4.14 (P0.1): Himax-reboot detection — see the state comment above. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (link_up_ && handshake_done_ &&
        (now_ms - link_up_at_ms_) > RELINK_DEBOUNCE_MS &&
        !relink_inflight_) {
        ESP_LOGW(TAG, "Himax reboot detected (event %s); re-linking", ev);
        link_up_ = false;
        handshake_done_ = false;
        /* Drop any stale partial frame so the fresh INIT stream parses
         * cleanly. Suspends/resumes the SSCMA process task; safe here
         * (we run in the monitor task). */
        sscma_client_flush_rx(client_);
        eyes_relink_async_();
        return;
    }
    link_up_ = true;
    link_up_at_ms_ = now_ms;
    ESP_LOGI(TAG, "Himax link connected (event %s)", ev);
}

/* Retry AT+MODEL=1 until the Himax answers (4 attempts, ~300 ms apart).
 * Shared by eyes_init, eyes_recover_camera, and the reboot re-link worker.
 * why tags the log lines with the calling context.
 *
 * v0.4.15: a capture-only Himax (CAPTURE_ONLY_MODE, no TFLM engine) answers
 * AT+MODEL=1 with MA_ENOTSUP, which maps to ESP_ERR_NOT_SUPPORTED. That is
 * not a dead Himax — v0.4.13 logged "Himax is capture-only (no MODEL
 * support); continuing with sensor-only init" and proceeded. The P0
 * refactor dropped that tolerance (bench regression found 2026-10-04:
 * "Himax not answering; eyes staying down" on a healthy Himax), so it is
 * restored here: ENOTSUP breaks the retry loop as success, and the caller
 * continues with the sensor preset. Any other error still retries. */
static esp_err_t eyes_set_model_retry_(const char *why)
{
    esp_err_t merr = ESP_FAIL;
    for (int i = 0; i < 4; i++) {
        merr = sscma_client_set_model(client_, 1);
        if (merr == ESP_OK) {
            break;
        }
        if (merr == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "%s: Himax is capture-only (no MODEL support); "
                          "continuing with sensor-only init", why);
            merr = ESP_OK;
            break;
        }
        ESP_LOGW(TAG, "%s set_model attempt %d failed: %s", why, i,
                 esp_err_to_name(merr));
        feed_rtc_wdt();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return merr;
}

/* Full AT re-handshake: model + sensor mode discovery + default to 1296x972.
 * Shared by eyes_recover_camera and the reboot re-link worker. (eyes_init
 * inlines the same steps around its boot-mode diagnostic log.)
 * v0.4.18: discover modes via SENSORS?, then explicitly select 1296x972.
 * Setting the raw mode at boot allocates the 1.26MB buffer while the heap
 * is fresh, minimizing fragmentation risk. Falls back to the hardcoded
 * preset 8 if discovery fails. */
static esp_err_t eyes_rehandshake_(const char *why)
{
    esp_err_t merr = eyes_set_model_retry_(why);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "Himax not answering (%s)", why);
        return merr;
    }
    /* Discover available modes; not fatal if it fails (fallback below). */
    eyes_discover_sensor_modes_();
    /* Default to 1296x972: the primary capture mode. */
    esp_err_t serr = eyes_select_sensor_mode(1296, 972);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "%s: 1296x972 not discovered, trying preset 8", why);
        int applied = 0;
        char detail[64] = {0};
        serr = eyes_set_sensor_opt(SENSOR_PRESET_1296x972, &applied,
                                   detail, sizeof(detail));
    }
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "%s sensor mode select failed: %s", why,
                 esp_err_to_name(serr));
        return serr;
    }
    handshake_done_ = true;
    /* The link becomes usable now — restart the reboot debounce window
     * here (not at the INIT arrival) so a reboot during the handshake
     * itself isn't mistaken for the boot burst. */
    link_up_at_ms_ = esp_timer_get_time() / 1000;
    return ESP_OK;
}

/* Re-link worker task: runs the AT re-handshake after a detected Himax
 * reboot. Runs in its own task because the handshake blocks for seconds
 * and on_connect runs in the monitor task. On success the link is marked
 * up; on failure it stays down and the snapshot-failure streak will drive
 * eyes_recover_camera when the camera is next needed.
 *
 * v0.4.18.3: flush stale RX state before the handshake (a Himax reboot
 * mid-transaction leaves a partial frame in the S3's RX buffer, which
 * corrupts the next response parse and causes cascading timeouts).
 * Retry the full handshake with backoff — the Himax can take 10+ seconds
 * after INIT before the AT server answers (seen on 0.4.20 flash:
 * 4 set_model attempts over 10s all timed out, worker gave up). */
#define RELINK_MAX_ROUNDS 3
#define RELINK_ROUND_DELAY_MS 5000
static void relink_worker_(void *arg)
{
    (void)arg;
    /* The INIT broadcast proves the AT server is up; give the sensor
     * pipeline a moment to settle before the handshake. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    /* Clear any stale partial frame from the interrupted pre-reboot
     * transaction. No hardware reset — the Himax just booted. */
    sscma_client_flush_rx(client_);
    for (int round = 0; round < RELINK_MAX_ROUNDS; round++) {
        ESP_LOGW(TAG, "re-link handshake starting (round %d/%d)",
                 round + 1, RELINK_MAX_ROUNDS);
        if (eyes_rehandshake_("relink") == ESP_OK) {
            link_up_ = true;
            /* v0.4.50: the Himax rebooted — session state was lost and
             * re-established under a new generation. */
            link_gen_++;
            link_up_at_ms_ = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "re-link handshake complete");
            relink_inflight_ = false;
            vTaskDelete(NULL);
        }
        ESP_LOGW(TAG, "re-link round %d failed; retrying in %d ms",
                 round + 1, RELINK_ROUND_DELAY_MS);
        /* Flush again — a failed round may have left partial data. */
        sscma_client_flush_rx(client_);
        vTaskDelay(pdMS_TO_TICKS(RELINK_ROUND_DELAY_MS));
    }
    ESP_LOGE(TAG, "re-link handshake failed after %d rounds; link stays down",
             RELINK_MAX_ROUNDS);
    relink_inflight_ = false;
    vTaskDelete(NULL);
}

/* Kick off the async re-link after a detected reboot. Called from
 * on_connect (monitor task). */
static void eyes_relink_async_(void)
{
    if (relink_inflight_) {
        return;
    }
    relink_inflight_ = true;
    BaseType_t ok = xTaskCreate(relink_worker_, "eyes_relink",
                                RELINK_WORKER_STACK, NULL,
                                tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "re-link worker create failed");
        relink_inflight_ = false;
    }
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t eyes_init(void)
{
    snap_events_ = xEventGroupCreate();
    sensor_modes_mux_ = xSemaphoreCreateMutex();
    /* 160 KiB must stay in PSRAM: a silent internal fallback here would eat
     * two-thirds of our internal DMA headroom and wedge the display. */
    snap_buf_ = heap_caps_malloc(SNAP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!snap_buf_ || !snap_events_) {
        ESP_LOGE(TAG, "no PSRAM for snapshot buffer");
        return ESP_ERR_NO_MEM;
    }
    /* v0.4.18.63: AOB RAW frame buffer in PSRAM at boot.
     * John 2026-10-08: allocate early to avoid fragmentation; keep for
     * lifetime. Sized at PNG_FRAME_BYTES + 10% headroom. Used by PNG/JPEG
     * capture paths instead of on-demand malloc. */
    raw_frame_buf_ = heap_caps_malloc(RAW_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!raw_frame_buf_) {
        ESP_LOGE(TAG, "FATAL: no PSRAM for RAW frame buffer (%u bytes). "
                 "PNG capture will fail.", (unsigned)RAW_FRAME_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RAW frame buffer: %u bytes @ %p (PSRAM AOB, +10%% headroom)",
             (unsigned)RAW_FRAME_BUF_SIZE, raw_frame_buf_);

    /* I2C0 for the IO expander */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        /* v0.4.48.1: 100 kHz, not 400 kHz. I2C0 carries only the
         * expander and codec control — nothing here needs fast-mode
         * speed — and the recurring interrupt-WDT panic RCA
         * (2026-11-09) implicates marginal-bus transaction timeouts
         * at 400 kHz as the trigger that drops the legacy driver
         * into its FSM-reset path. Standard mode quadruples the
         * timing margins for ~zero functional cost. */
        .master.clk_speed = 100000,
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
        /* v0.4.18.56 (doc verification §7): Pin SPI ISR to core 0.
         * The ISR was implicitly on core 0 (where app_main runs
         * spi_bus_initialize). Make it explicit, and pin the SPI worker
         * task to the same core to avoid cross-core ISR wakeup latency.
         * This is Seeed's own spi-stall-analysis fix for this link. */
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_0,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 init failed");
        return ESP_FAIL;
    }
    /* v0.4.18.35 (Phase 2): MISO-flag handshake. Enable pull-up on MISO (GPIO6).
     * The Himax releases PB2 to Hi-Z when response is ready; the pull-up
     * takes MISO HIGH. When Himax drives PB2 LOW (GPIO13), MISO reads LOW.
     * Pad pull-up works independently of SPI matrix routing. */
    gpio_pullup_en(PIN_SPI_MISO);

    sscma_client_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_SSCMA_CS,
        .sync_gpio_num = -1, /* No Himax SYNC line on Watcher HW; use pure SPI polling */
        .spi_mode = 0,
        /* v0.4.18.51 (Phase A2.1): wait_delay = 0. All vTaskDelay(wait_delay)
         * sites become no-ops. The MISO gates (wait-LOW before cmd, wait-HIGH
         * before clock) replace timing guesses. Himax now re-arms on hardware
         * status (TXFLR/SR), not wall-clock, so delays are unnecessary. */
        .wait_delay = 0,
        /* v0.4.18.56 (doc verification §5): Request 12 MHz, hardware gives
         * 80/7 ≈ 11.43 MHz (closest achievable divider). Record 11.43 MHz
         * as the real SCLK in all timing math — the old 12 MHz assumption
         * made Himax-side transfer_time estimates ~5% optimistic. */
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
    io_ = io;  /* v0.4.18.11: store for SPI self-test */

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
    esp_err_t merr = eyes_set_model_retry_("init");
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
        ESP_LOGE(TAG, "Himax not answering; eyes staying down");
        return ESP_FAIL;
    }

    ready_ = true;

    /* v0.4.18: discover modes, then default to 1296x972 (primary capture
     * mode). Allocating the 1.26MB raw buffer at boot, while the heap is
     * fresh, minimizes fragmentation risk. */
    eyes_discover_sensor_modes_();
    esp_err_t serr = eyes_select_sensor_mode(1296, 972);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "init: 1296x972 not discovered, trying preset 8");
        int applied = 0;
        char detail[64] = {0};
        serr = eyes_set_sensor_opt(SENSOR_PRESET_1296x972, &applied,
                                   detail, sizeof(detail));
    }
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "sensor mode init failed: %s", esp_err_to_name(serr));
        ready_ = false;
        return ESP_FAIL;
    }
    handshake_done_ = true;
    /* See eyes_rehandshake_: the link is usable now; restart the reboot
     * debounce window. */
    link_up_at_ms_ = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "eyes ready");

    /* v0.4.50: start the link-health supervisor (escalation-ladder
     * owner). Internal-RAM stack like the re-link worker: it calls
     * eyes_recover_camera, whose NVS-adjacent paths must not run from a
     * PSRAM stack. */
    if (!supervisor_started_) {
        supervisor_started_ = true;
        if (xTaskCreate(eyes_link_health_task_, "eyes_link_hlth", 4096,
                        NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "link-health supervisor create failed");
            supervisor_started_ = false;
        }
    }
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
    /* v0.4.14 (P0.1): shared AT re-handshake (set_model retries + sensor
     * preset); also used by eyes_init and the reboot re-link worker. */
    esp_err_t herr = eyes_rehandshake_("recovery");
    if (herr != ESP_OK) {
        return herr;
    }
    ESP_LOGI(TAG, "camera recovered (rail_cycled=%d)", cycle_rail ? 1 : 0);
    /* v0.4.3: the Himax is answering again — the link is up. */
    link_up_ = true;
    /* v0.4.50: session state was lost and re-established. */
    link_gen_++;
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
/* v0.4.50 link-health supervisor: the escalation ladder (spec §2.3).  */
/*                                                                     */
/* Polls the io gate streak every 100 ms. An episode opens on the      */
/* consecutive streak (>= 25) OR — v0.4.51 (G1) — on the io layer's    */
/* trailing-60 s failure window (>= 60): the 2026-11-09 flap failed    */
/* in bursts of 3 with a legitimate completion between bursts, a       */
/* duty-cycle shape a consecutive-only trigger cannot see. Rung        */
/* conditions read (streak OR failures-since-episode-open); the        */
/* failure floor (1 s ≈ 3x margin over the Himax's own 300 ms Guard-2  */
/* window — a healthy Himax in a Guard-2 wait clears long before       */
/* rung 2 can fire) runs from episode open. The episode closes only    */
/* after the streak has read 0 continuously for a 2 s health dwell     */
/* (G1): a flapping link produces a failure sample at least every      */
/* ~300 ms, so the dwell cannot complete mid-flap. Failed rungs >= 3   */
/* back off exponentially (5 s -> 15 s -> 60 s cap) between attempts;  */
/* v0.4.51 (owner amendment, fix-spec §18.5) adds a 5 s minimum        */
/* spacing after ANY rung >= 3 attempt — the fail-only backoff let a   */
/* procedurally successful but unhealing rung re-fire at procedure     */
/* cadence. The rail rung is capped at 3 executions per rolling hour;  */
/* past the cap the ladder latches to RST-only retries every 10 min    */
/* until the link recovers. There is deliberately NO rung 5: the S3    */
/* never reboots for a wedged peripheral. Recovery credit (G4) goes    */
/* to a rung only if it fired inside this episode and the link         */
/* generation has not moved since its success; a generation move no    */
/* rung explains books as "external" (drain recoveries are counted     */
/* by the io layer itself). One log line per rung attempt and outcome. */
/* ------------------------------------------------------------------ */
#define LINK_STREAK_RESET 25       /* rung 2: CMD_RESET resync */
#define LINK_STREAK_RST 50         /* rungs 3/4 eligibility */
#define LINK_FAIL_FLOOR_MS 1000    /* rungs >= 2 continuous-failure floor */
#define GATE_RATE_THRESHOLD 60     /* v0.4.51 (G1): window60 episode trigger */
#define LINK_HEALTH_DWELL_MS 2000  /* v0.4.51 (G1): episode close dwell */
#define LINK_BACKOFF_1_MS 5000
#define LINK_BACKOFF_2_MS 15000
#define LINK_BACKOFF_CAP_MS 60000
#define LINK_RAIL_CAP 3
#define LINK_RAIL_WINDOW_MS (3600LL * 1000LL)
#define LINK_LATCH_RETRY_MS (10LL * 60LL * 1000LL)
#define LINK_RELAPSE_MS (10LL * 60LL * 1000LL)
#define LINK_DEFER_SLICE_MS 500
#define LINK_DEFER_MAX_MS 65000

static bool link_bus_busy_(void)
{
    return eyes_snap_busy() || sscma_client_bulk_active(client_) ||
           sscma_client_io_spi_test_active(io_);
}

/* v0.4.51 (G1): episode-health state. The supervisor task is the only
 * writer of link_healthy_since_ms_; rungs >= 3 and the bus-wait below
 * read the predicate. "Unhealthy" is defined on the STREAK — the 60 s
 * window decays too slowly ever to hold an episode open: episode
 * OPENING is disjunctive (streak or rate), episode health is
 * streak-only. link_episode_active_ mirrors the supervisor's episode
 * flag for the heartbeat's state string. Mirrors only: the supervisor
 * loop keeps its own locals and may lag these by one 100 ms pass. */
static volatile bool link_episode_active_ = false;
static volatile int64_t link_healthy_since_ms_ = 0;

static bool link_episode_unhealthy_(int64_t now_ms)
{
    return sscma_client_gate_streak() > 0 ||
           (link_healthy_since_ms_ != 0 &&
            (now_ms - link_healthy_since_ms_) < LINK_HEALTH_DWELL_MS);
}

/* Rungs >= 3 wait for the bus in 500 ms slices (bounded ~65 s) instead
 * of barging in: interrupting a bulk capture or a self-test to RST the
 * camera would create the very failures the ladder exists to heal.
 * Returns false if episode health COMPLETED while waiting — the
 * streak read 0 continuously through the full 2 s health dwell, so
 * the episode is over and the caller must not run the rung (v0.4.51
 * G1: previously it bailed on the first single streak-0 sample, which
 * let a flapping link keep the ladder idle indefinitely) — or the
 * bound expired busy. The health mirror is maintained by this wait
 * too: it runs on the supervisor task, so a wait never lets the dwell
 * bookkeeping stall. */
static bool link_wait_bus_free_(void)
{
    int waited = 0;
    while (link_bus_busy_()) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (sscma_client_gate_streak() == 0) {
            if (link_healthy_since_ms_ == 0) {
                link_healthy_since_ms_ = now_ms;
            }
        } else {
            link_healthy_since_ms_ = 0;
        }
        if (!link_episode_unhealthy_(now_ms)) {
            return false; /* dwell completed: episode over */
        }
        if (waited >= LINK_DEFER_MAX_MS) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_DEFER_SLICE_MS));
        waited += LINK_DEFER_SLICE_MS;
    }
    return link_episode_unhealthy_(esp_timer_get_time() / 1000);
}

static void eyes_link_health_task_(void *arg)
{
    (void)arg;
    bool episode = false;
    bool ep_reset_tried = false;
    bool ep_rst_failed = false;
    bool ep_rail_failed = false;
    bool ep_relapse = false;        /* opened <10 min after an rst/rail close */
    bool ep_cleared_link_up = false;
    const char *ep_credit = "none"; /* last rung executed this episode */
    int64_t ep_open_ms = 0;         /* v0.4.51 (G1): episode open time */
    uint32_t ep_fail_base = 0;      /* v0.4.51 (G1): io total at open (FSEO base) */
    uint32_t ep_open_gen = 0;       /* v0.4.51 (G4): link_gen_ at open */
    uint32_t ep_credit_gen = 0;     /* v0.4.51 (G4): link_gen_ at credited rung success */
    int rung_fail_streak = 0;       /* consecutive failed rungs >= 3 */
    int64_t last_rung_try_ms = 0;
    int64_t latch_next_try_ms = 0;
    int64_t rail_times_ms[LINK_RAIL_CAP] = {0};
    int rail_count = 0;
    int64_t last_close_ms = 0;
    const char *last_close_rung = "none";

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!client_ || !io_) {
            continue;
        }
        uint32_t streak = sscma_client_gate_streak();
        uint32_t window = sscma_client_gate_failures_window();
        uint32_t total = sscma_client_gate_failures_total();
        int64_t now_ms = esp_timer_get_time() / 1000;

        /* Prune the rolling rail window. */
        {
            int kept = 0;
            for (int i = 0; i < rail_count; i++) {
                if (now_ms - rail_times_ms[i] < LINK_RAIL_WINDOW_MS) {
                    rail_times_ms[kept++] = rail_times_ms[i];
                }
            }
            rail_count = kept;
        }
        bool rail_latched = rail_count >= LINK_RAIL_CAP;

        /* v0.4.51 (G1): episode health tracking. The healthy mirror
         * runs every pass, episode or not; the supervisor is its only
         * writer (this loop and the rung bus-wait). */
        if (streak == 0) {
            if (link_healthy_since_ms_ == 0) {
                link_healthy_since_ms_ = now_ms;
            }
        } else {
            link_healthy_since_ms_ = 0;
        }

        if (episode &&
            !link_episode_unhealthy_(now_ms)) {
            /* Episode closed: the streak has read 0 continuously for
             * the full 2 s health dwell. G4 crediting: the credited
             * rung keeps its credit only if it fired inside this
             * episode AND the link generation has not moved since its
             * success snapshot; a generation that moved with no rung
             * still holding credit means something outside the ladder
             * recovered the link — booked "external". */
            episode = false;
            link_episode_active_ = false;
            last_close_ms = now_ms;
            const char *booked = "none";
            if ((strcmp(ep_credit, "reset") == 0 ||
                 strcmp(ep_credit, "rst") == 0 ||
                 strcmp(ep_credit, "rail") == 0) &&
                link_gen_ == ep_credit_gen) {
                booked = ep_credit;
            } else if (link_gen_ != ep_open_gen) {
                booked = "external";
            }
            last_close_rung = booked;
            if (strcmp(booked, "reset") == 0) {
                rec_reset_++;
            } else if (strcmp(booked, "rst") == 0) {
                rec_rst_++;
            } else if (strcmp(booked, "rail") == 0) {
                rec_rail_++;
            }
            last_rung_ = booked;
            if (ep_cleared_link_up) {
                /* The AT layer is answering again (health dwell
                 * completed): restore the honest up state cleared at
                 * episode open. eyes_recover_camera already set it on
                 * the rst/rail path; this also covers drain / reset /
                 * self-healed closes. */
                link_up_ = true;
                ep_cleared_link_up = false;
            }
            ESP_LOGI(TAG, "himax link episode closed (rung=%s gen=%u)",
                     booked, (unsigned)link_gen_);
            continue;
        }

        if (!episode) {
            if (streak < LINK_STREAK_RESET &&
                window < GATE_RATE_THRESHOLD) {
                continue; /* transient: rung 1 handles these in-io */
            }
            /* Open an episode (§3.2: link_up_ drops here, at the rung-2
             * threshold — not on the first transient failure). v0.4.51
             * (G1): the trigger is disjunctive — consecutive streak or
             * the 60 s failure rate. (G4): every per-episode rung
             * timestamp and generation snapshot re-bases HERE, so a
             * stale backoff or a previous episode's generation can
             * never leak into this one. */
            episode = true;
            link_episode_active_ = true;
            ep_reset_tried = false;
            ep_rst_failed = false;
            ep_rail_failed = false;
            ep_credit = "none";
            rung_fail_streak = 0;
            ep_open_ms = now_ms;
            ep_fail_base = total;
            last_rung_try_ms = 0;
            ep_open_gen = link_gen_;
            ep_credit_gen = 0;
            link_healthy_since_ms_ = 0;
            ep_relapse = (last_close_ms != 0 &&
                          (now_ms - last_close_ms) < LINK_RELAPSE_MS &&
                          (strcmp(last_close_rung, "rst") == 0 ||
                           strcmp(last_close_rung, "rail") == 0));
            ep_cleared_link_up = false;
            if (link_up_) {
                link_up_ = false;
                ep_cleared_link_up = true;
            }
            ESP_LOGW(TAG, "himax link episode open: trigger=%s "
                     "streak=%u window60=%u total=%u%s",
                     streak >= LINK_STREAK_RESET ? "streak" : "rate",
                     (unsigned)streak, (unsigned)window, (unsigned)total,
                     ep_relapse ? " (fast relapse)" : "");
        }

        /* v0.4.51 (G1): failures since episode open — the rung-driving
         * count alongside the streak. A flap's episode runs on FSEO
         * long after its streak has been reset by inter-burst
         * completions. */
        uint32_t fseo = total - ep_fail_base;

        /* Rung 2: CMD_RESET resync, once per episode. v0.4.51 (G1):
         * the count condition is disjunctive (streak or FSEO) and the
         * 1 s floor runs from EPISODE OPEN — under the old streak-age
         * form a flap's floor could NEVER be met (the streak never
         * lived 1 s), which is part of why the old ladder never fired. */
        if (!ep_reset_tried &&
            (streak >= LINK_STREAK_RESET || fseo >= LINK_STREAK_RESET) &&
            (now_ms - ep_open_ms) >= LINK_FAIL_FLOOR_MS) {
            ep_reset_tried = true;
            link_recovery_in_progress_ = true;
            ESP_LOGW(TAG, "himax ladder rung 2 (reset): streak=%u fseo=%u",
                     (unsigned)streak, (unsigned)fseo);
            esp_err_t r = sscma_client_io_spi_resync(io_);
            link_recovery_in_progress_ = false;
            if (r == ESP_OK) {
                ep_credit = "reset";
                /* G4: snapshot AFTER the call returns — a generation
                 * the resync itself legitimately produced is inside
                 * the snapshot, and any LATER move voids the credit. */
                ep_credit_gen = link_gen_;
                ESP_LOGW(TAG, "himax ladder rung 2 (reset): transport re-armed");
            } else {
                ESP_LOGW(TAG, "himax ladder rung 2 (reset): failed (%s)",
                         esp_err_to_name(r));
            }
            continue;
        }

        if ((streak < LINK_STREAK_RST && fseo < LINK_STREAK_RST) ||
            (now_ms - ep_open_ms) < LINK_FAIL_FLOOR_MS) {
            continue;
        }

        /* Rungs 3/4. Rail is eligible only after a failed RST in this
         * episode, after a failed rail attempt, or on fast relapse;
         * never past the hourly cap. */
        bool want_rail = (ep_rst_failed || ep_rail_failed || ep_relapse) &&
                         !rail_latched;

        /* Latch pacing (rail cap reached): RST-only, every 10 min. */
        if (rail_latched && latch_next_try_ms != 0 &&
            now_ms < latch_next_try_ms) {
            continue;
        }
        /* Exponential backoff between failed rungs >= 3 — plus the
         * v0.4.51 owner-amendment floor (fix-spec §18.5): a minimum
         * LINK_BACKOFF_1_MS spacing after ANY rung >= 3 attempt. The
         * backoff ladder alone counts only ESP_ERR outcomes, so a
         * procedurally successful but unhealing rung re-fired at
         * procedure cadence (≈ the recovery procedure's own ~2 s,
         * forever). The floor applies to successes and failures
         * alike; the fail ladder still dominates from the second
         * consecutive failure on. */
        int64_t backoff_ms = rung_fail_streak == 0 ? 0 :
                             rung_fail_streak == 1 ? LINK_BACKOFF_1_MS :
                             rung_fail_streak == 2 ? LINK_BACKOFF_2_MS :
                                                     LINK_BACKOFF_CAP_MS;
        int64_t min_gap_ms = backoff_ms < LINK_BACKOFF_1_MS
                             ? LINK_BACKOFF_1_MS : backoff_ms;
        if (last_rung_try_ms != 0 &&
            (now_ms - last_rung_try_ms) < min_gap_ms) {
            continue;
        }

        if (!link_wait_bus_free_()) {
            continue; /* health dwell completed, or bus stayed busy */
        }
        if (!link_episode_unhealthy_(esp_timer_get_time() / 1000)) {
            continue; /* episode ended during the wait; booked next pass */
        }

        last_rung_try_ms = esp_timer_get_time() / 1000;
        link_recovery_in_progress_ = true;
        if (want_rail) {
            /* Count the execution against the hourly cap regardless of
             * outcome: the rail physically cycles either way. */
            if (rail_count < LINK_RAIL_CAP) {
                rail_times_ms[rail_count++] = last_rung_try_ms;
            }
            ESP_LOGW(TAG, "himax ladder rung 4 (rail): streak=%u rail_used=%d/%d",
                     (unsigned)sscma_client_gate_streak(),
                     rail_count, LINK_RAIL_CAP);
            esp_err_t r = eyes_recover_camera(true);
            link_recovery_in_progress_ = false;
            if (r == ESP_OK) {
                ep_credit = "rail";
                ep_credit_gen = link_gen_; /* G4: snapshot after the call */
                ESP_LOGW(TAG, "himax ladder rung 4 (rail): recovered");
            } else {
                ep_rail_failed = true;
                rung_fail_streak++;
                ESP_LOGW(TAG, "himax ladder rung 4 (rail): failed (%s)",
                         esp_err_to_name(r));
            }
        } else {
            if (rail_latched) {
                latch_next_try_ms = last_rung_try_ms + LINK_LATCH_RETRY_MS;
            }
            ESP_LOGW(TAG, "himax ladder rung 3 (rst): streak=%u%s",
                     (unsigned)sscma_client_gate_streak(),
                     rail_latched ? " (rail latched, cap reached)" : "");
            esp_err_t r = eyes_recover_camera(false);
            link_recovery_in_progress_ = false;
            if (r == ESP_OK) {
                ep_credit = "rst";
                ep_credit_gen = link_gen_; /* G4: snapshot after the call */
                ESP_LOGW(TAG, "himax ladder rung 3 (rst): recovered");
            } else {
                ep_rst_failed = true;
                rung_fail_streak++;
                ESP_LOGW(TAG, "himax ladder rung 3 (rst): failed (%s)",
                         esp_err_to_name(r));
            }
        }
    }
}

void eyes_himax_link_info(eyes_himax_link_info_t *out)
{
    if (!out) {
        return;
    }
    uint32_t streak = sscma_client_gate_streak();
    out->streak = streak;
    out->gen = link_gen_;
    out->rec_drain = sscma_client_gate_drain_recoveries();
    out->rec_reset = rec_reset_;
    out->rec_rst = rec_rst_;
    out->rec_rail = rec_rail_;
    out->last_rung = last_rung_;
    /* v0.4.51 (G1 rider): while an episode is open the state reads
     * "down" even between failure bursts, when the streak
     * momentarily reads 0 — the flap's heartbeat never did. */
    out->state = link_recovery_in_progress_ ? "recovering"
                 : (link_episode_active_ ? "down"
                    : (streak == 0 ? "up" : "down"));
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

/* v0.4.18.37: PNG snaptest helper is defined after the PNG encoder
 * types (png_sink_ctx_t, etc.) near eyes_capture_png(). See below. */

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
    /* v0.4.16: the Himax AT server requires a \r or \n terminator to
     * dispatch a command (receiveIf pops up to the delimiter). Without it
     * the command sits in the SPI RX ringbuffer and the request times out.
     * Append \r\n if the caller didn't include a terminator. */
    char term_cmd[128];
    size_t cmd_len = strlen(cmd);
    bool has_term = cmd_len > 0 &&
                    (cmd[cmd_len - 1] == '\r' || cmd[cmd_len - 1] == '\n');
    const char *send_cmd = cmd;
    if (!has_term && cmd_len + 2 < sizeof(term_cmd)) {
        memcpy(term_cmd, cmd, cmd_len);
        term_cmd[cmd_len] = '\r';
        term_cmd[cmd_len + 1] = '\n';
        term_cmd[cmd_len + 2] = '\0';
        send_cmd = term_cmd;
    }
    sscma_client_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    esp_err_t err = sscma_client_request(client_, send_cmd, &reply, true,
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

/* v0.4.18: query AT+SENSORS? and build the resolution->preset mode table.
 * Called at handshake/link-up (and re-link after Himax reboot). The Himax
 * reports presets with 1-based "id" and a "description" like "1296x972
 * RAW8"; AT+SENSOR takes the 1-based opt_id directly. */
static esp_err_t eyes_discover_sensor_modes_(void)
{
    if (!client_ || !ready_) {
        return ESP_FAIL;
    }
    /* v0.4.18 fix: heap-allocate the response buffer. This runs in the
     * main task (via eyes_init) whose stack is too small for a 2KB array. */
    char *resp = (char *)malloc(2048);
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    /* v0.4.18.40: Retry SENSORS? up to 3 times. The Himax AT server can be
     * slow to respond after boot; a single timeout shouldn't wipe the
     * mode table. */
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int retry = 0; retry < 3 && err != ESP_OK; retry++) {
        if (retry > 0) {
            ESP_LOGI(TAG, "mode discovery: SENSORS? retry %d/3", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        err = eyes_at("AT+SENSORS?", resp, 2048);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mode discovery: SENSORS? failed after 3 tries: %s",
                 esp_err_to_name(err));
        free(resp);
        return err;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        ESP_LOGW(TAG, "mode discovery: bad JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    int found = 0;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *sensors = data ? cJSON_GetObjectItem(data, "sensors") : NULL;
    if (cJSON_IsArray(sensors)) {
        cJSON *s = NULL;
        cJSON_ArrayForEach(s, sensors) {
            cJSON *type = cJSON_GetObjectItem(s, "type");
            if (!cJSON_IsString(type) || strcmp(type->valuestring, "Camera") != 0) {
                continue;
            }
            cJSON *presets = cJSON_GetObjectItem(s, "presets");
            if (!cJSON_IsArray(presets)) {
                continue;
            }
            cJSON *p = NULL;
            cJSON_ArrayForEach(p, presets) {
                cJSON *pid = cJSON_GetObjectItem(p, "id");
                cJSON *desc = cJSON_GetObjectItem(p, "description");
                if (!cJSON_IsNumber(pid) || !cJSON_IsString(desc)) {
                    continue;
                }
                unsigned w = 0, h = 0;
                if (sscanf(desc->valuestring, "%u%*1[xX]%u", &w, &h) != 2 ||
                    w == 0 || h == 0) {
                    continue;
                }
                if (found < SENSOR_MODES_MAX) {
                    sensor_modes_[found].width = (uint16_t)w;
                    sensor_modes_[found].height = (uint16_t)h;
                    /* SENSORS? ids are already 1-based opt_ids for AT+SENSOR.
                     * (v0.4.18.1 had an erroneous +1 here, trying mode 9
                     * for 1296x972 instead of mode 8.) */
                    sensor_modes_[found].preset_id = (int)pid->valuedouble;
                    found++;
                }
            }
            break; /* first camera sensor wins */
        }
    }
    cJSON_Delete(root);
    if (found == 0) {
        ESP_LOGW(TAG, "mode discovery: no camera modes found");
        return ESP_ERR_NOT_FOUND;
    }
    if (sensor_modes_mux_) {
        xSemaphoreTake(sensor_modes_mux_, portMAX_DELAY);
    }
    sensor_modes_count_ = found;
    if (sensor_modes_mux_) {
        xSemaphoreGive(sensor_modes_mux_);
    }
    ESP_LOGI(TAG, "mode discovery: %d modes", found);
    for (int i = 0; i < found; i++) {
        ESP_LOGI(TAG, "  %ux%u -> preset %d",
                 sensor_modes_[i].width, sensor_modes_[i].height,
                 sensor_modes_[i].preset_id);
    }
    return ESP_OK;
}

/* v0.4.18: select a sensor mode by resolution, discovered via SENSORS?.
 * Returns ESP_ERR_NOT_FOUND if the Himax offers no such mode. Idempotent:
 * skips the AT+SENSOR if already in the desired mode. */
esp_err_t eyes_select_sensor_mode(uint16_t width, uint16_t height)
{
    int preset = -1;
    if (sensor_modes_mux_) {
        xSemaphoreTake(sensor_modes_mux_, portMAX_DELAY);
    }
    for (int i = 0; i < sensor_modes_count_; i++) {
        if (sensor_modes_[i].width == width &&
            sensor_modes_[i].height == height) {
            preset = sensor_modes_[i].preset_id;
            break;
        }
    }
    if (sensor_modes_mux_) {
        xSemaphoreGive(sensor_modes_mux_);
    }
    if (preset < 0) {
        ESP_LOGW(TAG, "no sensor mode for %ux%u (%d modes known)",
                 width, height, sensor_modes_count_);
        return ESP_ERR_NOT_FOUND;
    }
    /* Skip if already in the desired mode (AT+SENSOR may reject a no-op). */
    {
        int current = -1;
        char detail[64] = {0};
        if (eyes_set_sensor_opt(-1, &current, detail, sizeof(detail)) == ESP_OK &&
            current == preset) {
            ESP_LOGD(TAG, "already in mode %ux%u (preset %d)", width, height, preset);
            return ESP_OK;
        }
    }
    int applied = 0;
    char detail[64] = {0};
    return eyes_set_sensor_opt(preset, &applied, detail, sizeof(detail));
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

/* v0.4.35: forward declaration — the definition sits with the base64
 * helpers further down (see eyes_crc32_ there). */
static uint32_t eyes_crc32_(const uint8_t *data, size_t len);

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
        b64_encode_chunk_(snap_buf_ + off, n, b64);
        if (snaptest_chunk_cb_) {
            snaptest_chunk_cb_(id, mid, seq, total, b64,
                               eyes_crc32_(snap_buf_ + off, n));
        }
    }
}

/* --- PNG capture (P2) ------------------------------------------------ */

static eyes_png_chunk_cb_t png_chunk_cb_ = NULL;

void eyes_set_png_chunk_cb(eyes_png_chunk_cb_t cb)
{
    png_chunk_cb_ = cb;
}

/* --- Crop window + strip capture (P4/P5, v0.4.15) ----------------------- */

static eyes_strip_chunk_cb_t strip_chunk_cb_ = NULL;

void eyes_set_strip_chunk_cb(eyes_strip_chunk_cb_t cb)
{
    strip_chunk_cb_ = cb;
}

/* Parse {"code":N,...} from an AT reply; returns true on code 0. */
static bool at_reply_ok(const char *reply)
{
    if (!reply) {
        return false;
    }
    const char *p = strstr(reply, "\"code\"");
    if (!p) {
        return false;
    }
    p = strchr(p + 6, ':');
    if (!p) {
        return false;
    }
    return atoi(p + 1) == 0;
}

esp_err_t eyes_set_crop(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CROP=%u,%u,%u,%u",
             (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
    char reply[512];
    esp_err_t err = eyes_at(cmd, reply, sizeof(reply));
    if (err != ESP_OK) {
        return err;
    }
    if (!at_reply_ok(reply)) {
        ESP_LOGE(TAG, "set_crop: Himax rejected: %s", reply);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "set_crop: %s", reply);
    return ESP_OK;
}

esp_err_t eyes_clear_crop(void)
{
    char reply[512];
    esp_err_t err = eyes_at("AT+CROP=0,0,0,0", reply, sizeof(reply));
    if (err != ESP_OK) {
        return err;
    }
    if (!at_reply_ok(reply)) {
        ESP_LOGE(TAG, "clear_crop: Himax rejected: %s", reply);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "clear_crop: crop cleared");
    return ESP_OK;
}

/* Stream one strip buffer as base64 chunks through the strip callback. */
static void stream_strip_b64(const uint8_t *buf, size_t len,
                             uint32_t strip_idx, uint32_t strip_count,
                             uint32_t y0, uint32_t h)
{
    int total = (int)((len + 767) / 768);
    char b64[1029];
    for (int seq = 0; seq < total; seq++) {
        size_t off = (size_t)seq * 768;
        size_t n = len - off > 768 ? 768 : len - off;
        size_t full = n - (n % 3);
        size_t o = 0;
        if (full > 0) {
            b64_encode_3(buf + off, full, b64);
            o = strlen(b64);
        }
        size_t tail = n - full;
        if (tail > 0) {
            uint8_t tmp[3] = {0, 0, 0};
            memcpy(tmp, buf + off + full, tail);
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
        if (strip_chunk_cb_) {
            strip_chunk_cb_(strip_idx, strip_count, y0, h,
                            (uint32_t)seq, (uint32_t)total, b64);
        }
    }
}

#define STRIP_W       2592u
#define STRIP_H       486u
#define FULLRES_W     2592u
#define FULLRES_H     1944u
#define STRIP_BYTES   ((size_t)STRIP_W * STRIP_H)

esp_err_t eyes_capture_strips(uint16_t overlap_rows)
{
    if (overlap_rows & 1) {
        ESP_LOGE(TAG, "capture_strips: overlap must be even (Bayer phase)");
        return ESP_ERR_INVALID_ARG;
    }
    if (overlap_rows >= STRIP_H) {
        ESP_LOGE(TAG, "capture_strips: overlap %u >= strip height",
                 (unsigned)overlap_rows);
        return ESP_ERR_INVALID_ARG;
    }
    if (!strip_chunk_cb_) {
        ESP_LOGE(TAG, "capture_strips: no strip chunk callback set");
        return ESP_ERR_INVALID_STATE;
    }

    /* Strip count: n = ceil((H - ov) / (STRIP_H - ov)). At ov=0 this is
     * exactly 4; at ov=16 it is 5 (last strip shorter). */
    uint32_t step = STRIP_H - overlap_rows;
    uint32_t n = (FULLRES_H - overlap_rows + step - 1) / step;
    ESP_LOGI(TAG, "capture_strips: %u strips, overlap %u",
             (unsigned)n, (unsigned)overlap_rows);

    uint8_t *strip = (uint8_t *)heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_SPIRAM);
    if (!strip) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < n && err == ESP_OK; i++) {
        uint32_t y0 = i * step;
        uint32_t h = STRIP_H;
        if (y0 + h > FULLRES_H) {
            h = FULLRES_H - y0;
        }
        ESP_LOGI(TAG, "capture_strips: strip %u/%u y0=%u h=%u",
                 (unsigned)i, (unsigned)n, (unsigned)y0, (unsigned)h);

        /* v0.4.50 (§2.5): if the crop set timed out across a link
         * generation change, the command may have died with the old
         * Himax session — re-issue it exactly once against the new
         * session. Any other failure (or a second timeout) is final. */
        {
            uint32_t gen0 = eyes_link_gen();
            err = eyes_set_crop(0, (uint16_t)y0, STRIP_W, (uint16_t)h);
            if (err == ESP_ERR_TIMEOUT && eyes_link_gen() != gen0) {
                ESP_LOGW(TAG, "capture_strips: crop set lost with Himax "
                              "session (gen %u->%u); re-issuing once",
                         (unsigned)gen0, (unsigned)eyes_link_gen());
                err = eyes_set_crop(0, (uint16_t)y0, STRIP_W, (uint16_t)h);
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "capture_strips: CROP failed for strip %u", (unsigned)i);
            break;
        }

        uint32_t pw = 0, ph = 0;
        size_t got = 0;
        err = eyes_raw_pull(strip, STRIP_BYTES, &got, &pw, &ph);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "capture_strips: pull failed for strip %u", (unsigned)i);
            break;
        }
        if (pw != STRIP_W || ph != h || got != (size_t)STRIP_W * h) {
            ESP_LOGE(TAG, "capture_strips: strip %u geometry mismatch %ux%u len %u",
                     (unsigned)i, (unsigned)pw, (unsigned)ph, (unsigned)got);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        stream_strip_b64(strip, got, i, n, y0, h);
    }

    heap_caps_free(strip);

    /* Always restore the 1296x972 mode, even on failure. */
    esp_err_t cerr = eyes_clear_crop();
    if (err == ESP_OK) {
        err = cerr;
    }
    return err;
}

/* PNG sink: appends to a PSRAM buffer. */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     failed;  /* v0.4.18.57: latched on first refusal (idempotent) */
} png_sink_ctx_t;

/* v0.4.18.58: Streaming PNG sink. Sends each encoder chunk directly via
 * snaptest_chunk_cb_ as it's produced — no 2.5MB blob buffer. The PNG
 * format is streaming-native: signature → IHDR → IDAT* → IEND, each
 * chunk self-contained. Analysis stats (bytes, fnv1a, soi/eoi) are
 * computed incrementally. */
typedef struct {
    int      id;
    const char *mid;
    int      seq;
    bool     failed;
    /* Incremental analysis state (replaces analyze_frame on buffered PNG) */
    size_t   total_bytes;
    uint32_t fnv1a;
    uint64_t sum;
    uint8_t  mn;
    uint8_t  mx;
    uint8_t  first8[8];
    size_t   first8_len;
    uint8_t  last8[8];
    /* Base64 temp: 8KB IDAT + 12B overhead → ~11KB b64 */
    char     b64_tmp[11270];
} png_stream_ctx_t;

static bool png_sink_append(const uint8_t *data, size_t len, void *user)
{
    png_sink_ctx_t *ctx = (png_sink_ctx_t *)user;
    /* v0.4.18.57 (Claude PNG Q3): idempotent refusal. Once the sink has
     * refused (buffer full), every subsequent call returns false WITHOUT
     * touching memory. Belt-and-braces: even if the encoder buggy-flushes
     * after failure, we can't write. */
    if (ctx->failed) {
        return false;
    }
    if (ctx->len + len > ctx->cap) {
        ctx->failed = true;
        /* Report bytes reached for empirical sizing. */
        ESP_LOGE("png_sink", "sink full at %u bytes (cap %u)",
                 (unsigned)ctx->len, (unsigned)ctx->cap);
        return false;
    }
    memcpy(ctx->buf + ctx->len, data, len);
    ctx->len += len;
    return true;
}

/* v0.4.35: zlib CRC32 (reflected, poly 0xEDB88320, init/final XOR
 * 0xFFFFFFFF — the "123456789" -> 0xCBF43926 parametrization), bitwise.
 * A chunk is ~8 KB, so the bit loop costs well under a millisecond and
 * needs no table. The ack-mode host validates each chunk against this
 * before acknowledging (see link_chunk_ack_* in link.c). */
static uint32_t eyes_crc32_(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* v0.4.18.37: Shared helper for base64 chunk encoding.
 * Encodes a 768-byte (or smaller) chunk to base64.
 * Returns the length of the base64 string (excluding NUL). */
static size_t b64_encode_chunk_(const uint8_t *buf, size_t n, char *b64_out)
{
    size_t full = n - (n % 3); /* multiple-of-3 prefix */
    size_t o = 0;
    if (full > 0) {
        b64_encode_3(buf, full, b64_out);
        o = strlen(b64_out);
    }
    size_t tail = n - full;
    if (tail > 0) {
        /* 1 or 2 leftover bytes -> 2 or 3 chars + '=' padding. */
        uint8_t tmp[3] = {0, 0, 0};
        memcpy(tmp, buf + full, tail);
        char t64[5];
        b64_encode_3(tmp, 3, t64);
        size_t want = tail == 1 ? 2 : 3;
        memcpy(b64_out + o, t64, want);
        o += want;
        while (want < 4) {
            b64_out[o++] = '=';
            want++;
        }
    }
    b64_out[o] = '\0';
    return o;
}

/* Stream a buffer as base64 chunks (768 input bytes -> 1024 chars per
 * chunk) through the png chunk callback. */
static void stream_png_b64(const uint8_t *buf, size_t len)
{
    int total = (int)((len + 767) / 768);
    char b64[1029];
    for (int seq = 0; seq < total; seq++) {
        size_t off = (size_t)seq * 768;
        size_t n = len - off > 768 ? 768 : len - off;
        b64_encode_chunk_(buf + off, n, b64);
        if (png_chunk_cb_) {
            png_chunk_cb_(seq, total, b64, eyes_crc32_(buf + off, n));
        }
    }
}

/* v0.4.18.79: Streaming PNG sink for dbg png. Mirrors png_streaming_sink_cb
 * (used by snaptest) but routes through png_chunk_cb_ (sends png_chunk
 * events) instead of snaptest_chunk_cb_. This eliminates the fixed 2MB
 * PNG_OUT_CAP buffer that caused "sink full" failures on less-compressible
 * frames — the encoder streams chunks as they're produced, with no size
 * limit. */
typedef struct {
    int      seq;
    bool     failed;
    size_t   total_bytes;
    /* Base64 temp: 8KB IDAT + overhead → ~11KB b64 */
    char     b64_tmp[11270];
} png_dbg_stream_ctx_t;

static bool png_dbg_streaming_sink_cb(const uint8_t *data, size_t len, void *user)
{
    png_dbg_stream_ctx_t *ctx = (png_dbg_stream_ctx_t *)user;
    if (ctx->failed) {
        return false;
    }
    if (!png_chunk_cb_) {
        ctx->failed = true;
        return false;
    }
    ctx->total_bytes += len;
    /* Base64-encode and stream immediately. total=-1 signals streaming
     * mode (page assembles until IEND). */
    b64_encode_chunk_(data, len, ctx->b64_tmp);
    /* v0.4.18.84: Abort on transport failure (see png_streaming_sink_cb). */
    if (!png_chunk_cb_(ctx->seq, -1, ctx->b64_tmp,
                       eyes_crc32_(data, len))) {
        ctx->failed = true;
        return false;
    }
    ctx->seq++;
    /* Feed the task watchdog periodically (see png_streaming_sink_cb). */
    if ((ctx->seq & 0xF) == 0) {
        vTaskDelay(1);
    }
    return true;
}

/* v0.4.18.58: Streaming PNG sink implementation. Called by the encoder
 * for each output chunk; streams directly via snaptest_chunk_cb_ without
 * buffering the full PNG. */
static bool png_streaming_sink_cb(const uint8_t *data, size_t len, void *user)
{
    /* v0.4.18.67: Entry log to isolate hang location. If we don't see this,
     * the hang is in the sink() call mechanism, not in the callback. */
    ESP_LOGI(TAG, "png_streaming_sink_cb: entry, len=%u", (unsigned)len);
    png_stream_ctx_t *ctx = (png_stream_ctx_t *)user;
    if (ctx->failed) {
        return false;
    }
    if (!snaptest_chunk_cb_) {
        ctx->failed = true;
        return false;
    }
    /* Update incremental analysis */
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        ctx->fnv1a ^= b;
        ctx->fnv1a *= 16777619u;
        ctx->sum += b;
        if (b < ctx->mn) ctx->mn = b;
        if (b > ctx->mx) ctx->mx = b;
        if (ctx->first8_len < 8) {
            ctx->first8[ctx->first8_len++] = b;
        }
        memmove(ctx->last8, ctx->last8 + 1, 7);
        ctx->last8[7] = b;
    }
    ctx->total_bytes += len;
    /* Base64-encode and stream immediately. total=-1 signals streaming
     * mode to the page (assembles until IEND). */
    b64_encode_chunk_(data, len, ctx->b64_tmp);
    /* v0.4.18.84: If the transport signals abort (ring stuck), fail the
     * entire encode. A missing chunk corrupts the PNG — abort is the only
     * safe option (John 2026-10-08). */
    if (!snaptest_chunk_cb_(ctx->id, ctx->mid, ctx->seq, -1, ctx->b64_tmp,
                            eyes_crc32_(data, len))) {
        ctx->failed = true;
        return false;
    }
    ctx->seq++;
    /* v0.4.18.62: Feed the task watchdog periodically. The PNG encode
     * produces ~1945 chunks; without yielding, the task watchdog fires
     * (reset reason 6). Yield every 16 chunks to let the watchdog reset. */
    if ((ctx->seq & 0xF) == 0) {
        vTaskDelay(1);
    }
    return true;
}

/* P2: capture a full frame via SPIDUMP + bulk read, encode as PNG, and
 * stream the PNG as base64 chunks. The raw frame is 1296x972 (1,259,712
 * bytes); the PNG output buffer is sized at 2 MiB (a SUB-filtered PNG of
 * a screen photo at this resolution is typically well under 1 MiB). */

/* v0.4.18.37: PNG version of snap_grab_frame_(). Captures via SPIDUMP +
 * SPI bulk read, encodes to PNG via ROM miniz, stores in snap_buf_.
 * Replaces the JPEG INVOKE path for snaptest (John: "move the entire
 * snaptest to use png instead of jpg").
 * Returns NULL on success, error string on failure. */
static const char *snap_grab_frame_png_(int id, const char *mid,
                                         eyes_snaptest_result_t *res)
{
    /* v0.4.18.63: Use AOB RAW frame buffer (allocated at boot in PSRAM).
     * Falls back to on-demand malloc if AOB failed (should not happen). */
    uint8_t *raw = raw_frame_buf_;
    bool raw_is_aob = true;
    if (!raw) {
        raw = (uint8_t *)heap_caps_malloc(RAW_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        raw_is_aob = false;
    }
    if (!raw) {
        return "no mem for raw frame";
    }

    /* Select 1296x972 mode (required for SPIDUMP raw buffer) */
    esp_err_t serr = eyes_select_sensor_mode(PNG_FRAME_W, PNG_FRAME_H);
    if (serr != ESP_OK) {
        if (!raw_is_aob) heap_caps_free(raw);
        return "sensor mode select failed";
    }

    /* Capture via SPIDUMP + bulk download */
    uint32_t frame_w = 0, frame_h = 0;
    size_t frame_len = 0;
    esp_err_t err = eyes_raw_pull(raw, PNG_FRAME_BYTES, &frame_len, &frame_w, &frame_h);
    if (err != ESP_OK) {
        if (!raw_is_aob) heap_caps_free(raw);
        return "SPIDUMP/bulk failed";
    }
    if (frame_w == 0 || frame_h == 0) {
        frame_w = PNG_FRAME_W;
        frame_h = PNG_FRAME_H;
    }

    /* v0.4.18.58: Stream PNG directly via sink callback — no 2.5MB blob.
     * Each encoder chunk is base64'd and sent via snaptest_chunk_cb_
     * immediately. Analysis stats computed incrementally.
     * v0.4.18.64 (John 2026-10-08): sctx contains 11KB b64_tmp — MUST be
     * in PSRAM, not on stack (stack overflow!). Allocate via PSRAM. */
    png_stream_ctx_t *sctx = (png_stream_ctx_t *)heap_caps_malloc(
        sizeof(png_stream_ctx_t), MALLOC_CAP_SPIRAM);
    if (!sctx) {
        if (!raw_is_aob) heap_caps_free(raw);
        return "no mem for png stream ctx";
    }
    sctx->id = id;
    sctx->mid = mid;
    sctx->seq = 0;
    sctx->failed = false;
    sctx->total_bytes = 0;
    sctx->fnv1a = 2166136261u;
    sctx->sum = 0;
    sctx->mn = 0xFF;
    sctx->mx = 0;
    sctx->first8_len = 0;
    memset(sctx->last8, 0, 8);
    png_stream_cfg_t cfg = {
        .width = frame_w,
        .height = frame_h,
        .cfa = PNG_CFA_BGGR,
        .filter = PNG_FILTER_SUB,
        .tdefl_probes = 0,
        .idat_chunk = 8192,
    };
    png_stream_result_t enc_res;
    /* v0.4.18.59 Phase 1 (Claude panic §4): heap integrity brackets.
     * These are no-ops in release (fast check), but with
     * CONFIG_HEAP_POISONING_COMPREHENSIVE=y they pinpoint the smash
     * location. Bracket: before encode, on error return, after encode,
     * after free(raw). */
    #if CONFIG_HEAP_POISONING_COMPREHENSIVE
    heap_caps_check_integrity_all(true);
    #endif
    /* v0.4.18.63 (John 2026-10-08): PNG crash detector + instrumentation.
     * The S3 was rebooting silently during PNG encode with no logs.
     * Arm the RTC crash detector, log each stage to USB serial. */
    ESP_LOGI(TAG, "PNG encode starting: %lux%lu, raw=%p", (unsigned long)frame_w, (unsigned long)frame_h, raw);
    png_crash_detector_arm(2);  /* stage 2 = encode in progress */
    ESP_LOGI(TAG, "PNG crash detector armed (stage 2)");
    err = png_stream_encode(raw, &cfg, png_streaming_sink_cb, sctx, &enc_res);
    png_crash_detector_disarm();
    ESP_LOGI(TAG, "PNG encode done: err=%d, bytes=%lu, chunks=%lu", 
             err, (unsigned long)sctx->total_bytes, (unsigned long)sctx->seq);
    #if CONFIG_HEAP_POISONING_COMPREHENSIVE
    heap_caps_check_integrity_all(true);
    #endif
    /* v0.4.18.63: Don't free AOB buffer (kept for lifetime) */
    if (!raw_is_aob) heap_caps_free(raw);
    #if CONFIG_HEAP_POISONING_COMPREHENSIVE
    heap_caps_check_integrity_all(true);
    #endif
    if (err != ESP_OK || sctx->failed) {
        heap_caps_free(sctx);
        return "PNG encode failed";
    }
    /* Populate result from incremental streaming stats (replaces
     * analyze_frame on buffered PNG). */
    if (res) {
        /* Preserve err pointer (set by caller) */
        const char *saved_err = res->err;
        memset(res, 0, sizeof(*res));
        res->err = saved_err;
        res->bytes = (int)sctx->total_bytes;
        res->soi = sctx->first8_len >= 8 &&
                   sctx->first8[0] == 0x89 && sctx->first8[1] == 0x50 &&
                   sctx->first8[2] == 0x4E && sctx->first8[3] == 0x47;
        res->eoi = sctx->total_bytes >= 8 &&
                   sctx->last8[0] == 0x49 && sctx->last8[1] == 0x45 &&
                   sctx->last8[2] == 0x4E && sctx->last8[3] == 0x44;
        res->fnv1a = sctx->fnv1a;
        res->min_b = sctx->mn;
        res->max_b = sctx->mx;
        res->mean_b = sctx->total_bytes > 0 ?
                      (uint8_t)(sctx->sum / sctx->total_bytes) : 0;
        /* head_hex: first 16 bytes as hex (we have first 8, pad rest) */
        for (int i = 0; i < 8 && i < (int)sctx->first8_len; i++) {
            snprintf(res->head_hex + i*2, 3, "%02x", sctx->first8[i]);
        }
        /* tail_hex: last 8 bytes as hex */
        for (int i = 0; i < 8; i++) {
            snprintf(res->tail_hex + i*2, 3, "%02x", sctx->last8[i]);
        }
    }
    /* snap_buf_/snap_len_ NOT used for streaming PNG (JPEG path keeps them) */
    /* v0.4.18.64: Free PSRAM-allocated sctx (was stack, caused overflow) */
    heap_caps_free(sctx);
    return NULL; /* success */
}

esp_err_t eyes_capture_png(void)
{
    /* v0.4.18.4: LED "say cheese" countdown, mirroring the snapshot path.
     * Three white blinks over ~600ms, LED off during capture, then green
     * on success / red on failure. Starts BEFORE the link check so the
     * user sees we're operating even if recovery is needed. */
    led_camera_countdown();
    vTaskDelay(pdMS_TO_TICKS(650));
    led_camera_off();

    /* v0.4.18.3: if the S3<->Himax link is down (e.g. re-link worker gave
     * up after a Himax reboot), attempt recovery before failing. The
     * snapshot path has its INVOKE-failure streak; dbg png needs its own. */
    if (!link_up_) {
        ESP_LOGW(TAG, "capture_png: link down, attempting recovery");
        if (eyes_recover_camera(false) != ESP_OK) {
            ESP_LOGE(TAG, "capture_png: link recovery failed");
            led_camera_result(false);
            return ESP_ERR_TIMEOUT;
        }
    }
    uint8_t *raw = raw_frame_buf_;
    bool raw_is_aob = true;
    if (!raw) {
        raw = (uint8_t *)heap_caps_malloc(RAW_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        raw_is_aob = false;
    }
    if (!raw) {
        led_camera_result(false);
        return ESP_ERR_NO_MEM;
    }
    /* v0.4.18: explicitly select the 1296x972 mode discovered via SENSORS?.
     * SPIDUMP needs the raw capture buffer, which only exists in a raw
     * preset (the Himax boots in 480x480 JPEG). The Himax keeps a preset-8
     * fallback inside spiDump, but the mode is ours to set — by resolution,
     * not by hardcoded preset number. */
    {
        esp_err_t serr = eyes_select_sensor_mode(PNG_FRAME_W, PNG_FRAME_H);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "capture_png: mode %ux%u select failed: %s",
                     (unsigned)PNG_FRAME_W, (unsigned)PNG_FRAME_H,
                     esp_err_to_name(serr));
            if (!raw_is_aob) heap_caps_free(raw);
            led_camera_result(false);
            return serr;
        }
    }
    /* v0.4.15 (P4): the SPIDUMP reply carries the frame geometry; a crop
     * window reports its own w/h, the standard path reports 1296x972
     * (older Himax firmware omits w/h -> fall back to the default). */
    uint32_t frame_w = 0, frame_h = 0;
    size_t frame_len = 0;
    esp_err_t err = eyes_raw_pull(raw, PNG_FRAME_BYTES, &frame_len, &frame_w, &frame_h);
    if (err != ESP_OK) {
        if (!raw_is_aob) heap_caps_free(raw);
        led_camera_result(false);
        return err;
    }
    if (frame_w == 0 || frame_h == 0) {
        frame_w = PNG_FRAME_W;
        frame_h = PNG_FRAME_H;
    }
    if ((size_t)frame_w * frame_h != frame_len) {
        ESP_LOGE(TAG, "capture_png: geometry %ux%u != len %u",
                 (unsigned)frame_w, (unsigned)frame_h, (unsigned)frame_len);
        if (!raw_is_aob) heap_caps_free(raw);
        led_camera_result(false);
        return ESP_ERR_INVALID_SIZE;
    }

    /* v0.4.18.79: Stream the PNG directly via png_chunk_cb_ instead of
     * buffering the entire image in a fixed 2MB PNG_OUT_CAP buffer.
     * The fixed buffer caused "sink full" ESP_FAIL on less-compressible
     * frames (e.g. 2,092,061 bytes > 2,097,152 cap). Streaming has no
     * size limit — mirrors the snaptest path (png_streaming_sink_cb). */
    /* v0.4.49.1: the ctx MUST live in PSRAM, not on the stack — its
     * b64_tmp is 11,270 B and this function runs on link_worker's 16 KB
     * internal stack (the snaptest path learned this in v0.4.18.64 and
     * heap-allocates its own ctx; the dbg path reintroduced the stack
     * local in v0.4.18.79). On 0.4.49 every dbg png died with a
     * StoreProhibited in esp_timer's timer_insert on a corrupted timer
     * list — the signature of this stack blowing into internal heap. */
    png_dbg_stream_ctx_t *sctx = (png_dbg_stream_ctx_t *)heap_caps_malloc(
        sizeof(png_dbg_stream_ctx_t), MALLOC_CAP_SPIRAM);
    if (!sctx) {
        if (!raw_is_aob) heap_caps_free(raw);
        led_camera_result(false);
        return ESP_ERR_NO_MEM;
    }
    sctx->seq = 0;
    sctx->failed = false;
    sctx->total_bytes = 0;
    png_stream_cfg_t cfg = {
        .width = frame_w,
        .height = frame_h,
        .cfa = PNG_CFA_BGGR,
        .filter = PNG_FILTER_SUB,
        .tdefl_probes = 0,
        .idat_chunk = 8192,
    };
    png_stream_result_t res;
    ESP_LOGI(TAG, "PNG encode starting (streaming): %lux%lu", (unsigned long)frame_w, (unsigned long)frame_h);
    png_crash_detector_arm(2);
    err = png_stream_encode(raw, &cfg, png_dbg_streaming_sink_cb, sctx, &res);
    png_crash_detector_disarm();
    ESP_LOGI(TAG, "PNG encode done: err=%d, bytes=%u, chunks=%d",
             err, (unsigned)sctx->total_bytes, sctx->seq);
    if (!raw_is_aob) heap_caps_free(raw);
    bool failed = sctx->failed;
    heap_caps_free(sctx);
    if (err != ESP_OK || failed) {
        led_camera_result(false);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    led_camera_result(true);
    return ESP_OK;
}

static void analyze_frame(const uint8_t *buf, size_t len,
                          eyes_snaptest_result_t *res)
{
    memset(res, 0, sizeof(*res));
    res->bytes = (int)len;
    /* v0.4.18.37: PNG signature (8 bytes: 89 50 4E 47 0D 0A 1A 0A) instead of
     * JPEG SOI (FFD8). Check first 4 bytes for the magic. */
    res->soi = len >= 8 && buf[0] == 0x89 && buf[1] == 0x50 &&
               buf[2] == 0x4E && buf[3] == 0x47;
    /* PNG ends with IEND chunk: length(0) + "IEND" + CRC(AE426082).
     * Check last 8 bytes for the IEND CRC. */
    res->eoi = len >= 8 && buf[len-8] == 0x49 && buf[len-7] == 0x45 &&
               buf[len-6] == 0x4E && buf[len-5] == 0x44;
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
    /* v0.4.18.37: snaptest uses PNG (SPIDUMP+bulk+ROM miniz) instead of
     * JPEG (INVOKE). The HTTP snapshot path still uses JPEG.
     * v0.4.18.58: PNG streams directly via snaptest_chunk_cb_ during
     * encode — no snap_buf_ blob. Analysis stats populated incrementally. */
    res.err = snap_grab_frame_png_(job->id, job->mid, &res);
    led_camera_result(res.err == NULL);
    if (res.err) {
        goto done;
    }
    /* analyze_frame skipped for streaming PNG (stats already in res).
     * stream_frame_b64 skipped (chunks already streamed during encode). */
    res.ms = (int)((esp_timer_get_time() - t0) / 1000);

done:
    if (snaptest_cb_) {
        snaptest_cb_(job->id, job->mid, &res);
    }
    /* Note: for streaming PNG, chunks were already sent during encode.
     * The job->send flag is now a no-op for PNG (kept for JPEG path). */
    /* v0.4.18.69: Log stack high-water mark for SRAM optimization. */
    {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "snaptest: stack HWM=%u words (%u bytes free)",
                 (unsigned)hwm, (unsigned)(hwm * sizeof(StackType_t)));
    }
    /* Generation-checked release (see snap_task). */
    snap_release_(job->gen);
    /* v0.4.18.86 (2026-10-08): DO NOT free psram_handle here! The task is
     * running on the PSRAM stack — freeing it before vTaskDelete is a
     * use-after-free (heap poisoning caught this: 0xfefefefe in regs).
     * The 16KB stack leaks per snaptest; a cleanup task should free it
     * after vTaskDelete completes. For now, leak is safer than crash. */
    /* if (job->psram_handle) {
        heap_caps_free(job->psram_handle);
        job->psram_handle = NULL;
    } */
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
    /* v0.4.18.70: Revert to PSRAM (16KB). The 0.4.18.69 internal RAM allocation
     * (12KB) exhausted SRAM (only 16KB free on 0.4.18.67). The link_worker
     * (16KB internal) is the critical one that MUST be internal (does NVS).
     * snaptest stays on PSRAM for now; the interrupt-during-flash risk is
     * theoretical and the SRAM budget doesn't allow it. */
    job->psram_handle = task_psram_create(snaptest_task, "snaptest",
                                          4096 * 4, job, 6, tskNO_AFFINITY);
    if (!job->psram_handle) {
        snap_release_(gen);
        free(job);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* P1: SPI bulk frame pull (AT+SPIDUMP + CRC bulk + SHA-256 verify)      */
/* ------------------------------------------------------------------ */

/* Capture a RAW8 image from the Himax over the SPI bulk path:
 * AT+SPIDUMP captures a fresh frame on the Himax and reports
 * {len, sha256, width, height}; sscma_client_read_bulk drains it
 * (CMD_START / CMD_READ_CRC / CMD_STOP, CRC16 per chunk, whole-transfer
 * retry on mismatch); the received bytes are verified against the
 * Himax-reported SHA-256. Synchronous; the capture plus ~1.2 MB transfer
 * takes several seconds. Returns ESP_OK with out_len/out_w/out_h set
 * on a verified frame. */
esp_err_t eyes_raw_pull(uint8_t *buf, size_t buf_len, size_t *out_len,
                        uint32_t *out_w, uint32_t *out_h)
{
    if (!buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!client_ || !ready_ || !link_up_) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 1. AT+SPIDUMP: capture a fresh frame on the Himax. Generous timeout:
     * the capture itself takes on the order of a second. */
    sscma_client_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    /* v0.4.17: AT commands require \r\n terminator (Himax receiveIf). */
    esp_err_t err = sscma_client_request(client_, "AT+SPIDUMP\r\n", &reply, true,
                                         pdMS_TO_TICKS(15000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw_pull: SPIDUMP failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t frame_len = 0;
    uint32_t frame_w = 0, frame_h = 0;
    char want_hex[65] = {0};
    do {
        if (!reply.payload) {
            break;
        }
        cJSON *data = cJSON_GetObjectItem(reply.payload, "data");
        if (!cJSON_IsObject(data)) {
            break;
        }
        cJSON *len = cJSON_GetObjectItem(data, "len");
        cJSON *sha = cJSON_GetObjectItem(data, "sha256");
        if (!cJSON_IsNumber(len) || !cJSON_IsString(sha)) {
            break;
        }
        frame_len = (size_t)len->valuedouble;
        strncpy(want_hex, sha->valuestring, sizeof(want_hex) - 1);
        /* v0.4.15: geometry is reported for crop windows; older firmware
         * omits it, in which case w/h stay 0 (caller falls back). */
        cJSON *w = cJSON_GetObjectItem(data, "width");
        cJSON *h = cJSON_GetObjectItem(data, "height");
        if (cJSON_IsNumber(w) && cJSON_IsNumber(h)) {
            frame_w = (uint32_t)w->valuedouble;
            frame_h = (uint32_t)h->valuedouble;
        }
    } while (0);
    sscma_client_reply_clear(&reply);
    if (frame_len == 0 || want_hex[0] == '\0') {
        ESP_LOGE(TAG, "raw_pull: bad SPIDUMP reply");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (frame_len > buf_len) {
        ESP_LOGE(TAG, "raw_pull: frame %u > buf %u",
                 (unsigned)frame_len, (unsigned)buf_len);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "raw_pull: Himax reports %u bytes %ux%u sha256=%s",
             (unsigned)frame_len, (unsigned)frame_w, (unsigned)frame_h, want_hex);

    /* 2. Bulk-drain the frame. */
    err = sscma_client_read_bulk(client_, buf, frame_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw_pull: bulk read failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Verify SHA-256 against the Himax-reported hash. */
    uint8_t digest[32];
    if (mbedtls_sha256(buf, frame_len, digest, 0) != 0) {
        ESP_LOGE(TAG, "raw_pull: sha256 compute failed");
        return ESP_FAIL;
    }
    char got_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(got_hex + 2 * i, 3, "%02x", digest[i]);
    }
    if (strcmp(got_hex, want_hex) != 0) {
        ESP_LOGE(TAG, "raw_pull: SHA-256 mismatch");
        ESP_LOGW(TAG, "raw_pull: want %s", want_hex);
        ESP_LOGW(TAG, "raw_pull: got  %s", got_hex);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "raw_pull: %u bytes, SHA-256 verified", (unsigned)frame_len);
    if (out_len) {
        *out_len = frame_len;
    }
    if (out_w) {
        *out_w = frame_w;
    }
    if (out_h) {
        *out_h = frame_h;
    }
    return ESP_OK;
}

/* v0.4.18.11: SPI channel self-test suite. Runs test patterns of various
 * sizes and reports pass/fail for each. Isolates SPI transport issues
 * from camera capture complexity.
 * v0.4.18.19: Accepts optional params JSON with:
 *   - "gap_ms": inter-test delay in ms (default 0). Tests if S3 pacing
 *     affects Himax RX re-arm reliability.
 *   - "pattern": specific pattern ID (0-3). If set, runs only this pattern.
 *   - "len": specific length. If set with pattern, runs single test.
 *   - "iterations": repeat count for single test (default 1). Tests
 *     reliability over multiple runs.
 * Example: {"t":"dbg","cmd":"spitest","gap_ms":5} */
esp_err_t eyes_spi_test(cJSON *results, cJSON *params)
{
    if (!io_) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse params */
    int gap_ms = 0;
    int single_pat = -1;
    int single_len = -1;
    int iterations = 1;
    if (params) {
        cJSON *j = cJSON_GetObjectItem(params, "gap_ms");
        if (j && cJSON_IsNumber(j)) gap_ms = j->valueint;
        j = cJSON_GetObjectItem(params, "pattern");
        if (j && cJSON_IsNumber(j)) single_pat = j->valueint;
        j = cJSON_GetObjectItem(params, "len");
        if (j && cJSON_IsNumber(j)) single_len = j->valueint;
        j = cJSON_GetObjectItem(params, "iterations");
        if (j && cJSON_IsNumber(j) && j->valueint > 0) iterations = j->valueint;
    }

    /* Test matrix: (pattern_id, len) pairs, or single test if specified */
    typedef struct { uint8_t pat; uint16_t len; } spitest_case_t;
    static const spitest_case_t default_tests[] = {
        {0, 16}, {0, 256}, {0, 1000}, {0, 4090},
        {1, 16}, {1, 256}, {1, 1000}, {1, 4090},
        {2, 16}, {2, 256}, {2, 1000}, {2, 4090},
        {3, 16}, {3, 256}, {3, 1000}, {3, 4090},
    };
    const spitest_case_t *tests = default_tests;
    int n_tests = sizeof(default_tests) / sizeof(default_tests[0]);

    /* Single-test mode */
    spitest_case_t single_test;
    if (single_pat >= 0 && single_len >= 0) {
        single_test.pat = (uint8_t)single_pat;
        single_test.len = (uint16_t)single_len;
        tests = &single_test;
        n_tests = 1;
    }

    int passed = 0;
    int total = 0;

    /* v0.4.18.50: Hold the test_active flag for the entire spitest run.
     * Prevents background AT polling from interfering between test cases. */
    sscma_client_io_spi_test_begin(io_);

    for (int iter = 0; iter < iterations; iter++) {
        for (int i = 0; i < n_tests; i++) {
            esp_err_t err = sscma_client_io_spi_test_pattern(
                io_, tests[i].len, tests[i].pat, NULL);
            bool ok = (err == ESP_OK);
            if (ok) passed++;
            total++;

            if (results) {
                cJSON *t = cJSON_CreateObject();
                cJSON_AddNumberToObject(t, "pattern", tests[i].pat);
                cJSON_AddNumberToObject(t, "len", tests[i].len);
                cJSON_AddNumberToObject(t, "iteration", iter);
                cJSON_AddBoolToObject(t, "ok", ok);
                if (!ok) {
                    cJSON_AddStringToObject(t, "err", esp_err_to_name(err));
                }
                cJSON_AddItemToArray(results, t);
            }
            ESP_LOGI(TAG, "spitest pat=%u len=%u iter=%d: %s",
                     tests[i].pat, tests[i].len, iter, ok ? "PASS" : "FAIL");

            /* v0.4.18.19: inter-test gap for timing experiments */
            if (gap_ms > 0 && (i < n_tests - 1 || iter < iterations - 1)) {
                vTaskDelay(pdMS_TO_TICKS(gap_ms));
            }
        }
    }
    sscma_client_io_spi_test_end(io_);
    ESP_LOGI(TAG, "spitest: %d/%d passed (gap_ms=%d)", passed, total, gap_ms);
    return (passed == total) ? ESP_OK : ESP_FAIL;
}
