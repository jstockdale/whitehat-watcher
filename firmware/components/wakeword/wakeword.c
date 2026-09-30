/* Wake-word detection via ESP-SR (v0.3.4).
 *
 * Architecture: the wake-word component owns I2S0 while listening.
 * PTT takes I2S0 via wakeword_stop() before recording, returns it via
 * wakeword_start() after. Only one owner at a time — no conflicts.
 *
 * Current model: "Hi, ESP" (wn9_hiesp) — working interim. "Hey Muse" and
 * "Hey Watcher" need Espressif custom training (see DESIGN.md). The AFE
 * supports 2 simultaneous models; they drop into wakenet_model_name_2
 * when trained. NVS `wakeword_phrase` selects the active phrase set.
 *
 * Default: OFF (opt-in). Enable via {"t":"dbg","cmd":"wakeword","on":1}.
 */

#include "wakeword.h"
#include "task_psram.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"

static const char *TAG = "wakeword";

/* ---- I2S config (same mic as PTT: ES7243 on I2S0) ---- */
#define WW_SAMPLE_RATE   16000
#define WW_I2S_PORT      I2S_NUM_0
#define WW_PIN_MCLK      GPIO_NUM_10
#define WW_PIN_BCLK      GPIO_NUM_11
#define WW_PIN_WS        GPIO_NUM_12
#define WW_PIN_DIN       GPIO_NUM_15

/* ---- NVS ---- */
#define NVS_NS            "watcher"
#define NVS_KEY_ENABLED   "wakeword_en"
#define NVS_KEY_PHRASE    "wakeword_phrase"

/* Interim model: "Hi, ESP". Shipped via the "model" partition
 * (CONFIG_SR_WN_WN9_HIESP). */
#define WW_MODEL_HIESP    "wn9_hiesp"
/* Placeholders for Espressif-trained models (not yet available). */
#define WW_MODEL_HEY_MUSE    "wn9_hey_muse"
#define WW_MODEL_HEY_WATCHER "wn9_hey_watcher"

/* ---- state ---- */
static bool ready_ = false;
static bool enabled_ = false;
static bool suspended_ = false;
static wakeword_cb_t cb_ = NULL;
static uint32_t detections_ = 0;
static wakeword_phrase_t phrase_ = WAKEWORD_PHRASE_BOTH;

static i2s_chan_handle_t ww_i2s_ = NULL;
static TaskHandle_t ww_task_ = NULL;
/* v0.4.6: ww_feed runs on PSRAM stack (was 16KB internal). The psram handle
 * is kept; ww_task_ mirrors h->task for the stop logic. */
static task_psram_handle_t *ww_psram_ = NULL;

static srmodel_list_t *models_ = NULL;
static afe_config_t *afe_config_ = NULL;
static const esp_afe_sr_iface_t *afe_iface_ = NULL;
static esp_afe_sr_data_t *afe_data_ = NULL;

/* ---- NVS helpers ----
 * v0.4.7: ALL NVS access via the nvs task. ww_feed runs on a PSRAM
 * stack — direct nvs_* calls panic (v0.4.6 proved reads too).
 * Declared extern (wakeword doesn't REQUIRES seal; the link-time
 * dependency is one-directional, same pattern as hid_ble/net). */
extern esp_err_t seal_dev_nvs_write_u8_sync(const char *ns, const char *key,
                                            uint8_t val, uint32_t timeout_ms);
extern esp_err_t seal_dev_nvs_read_u8_sync(const char *ns, const char *key,
                                           uint8_t *out, uint32_t timeout_ms);

static void nvs_load(void)
{
    uint8_t v = 0;
    if (seal_dev_nvs_read_u8_sync(NVS_NS, NVS_KEY_ENABLED, &v,
                                  5000) == ESP_OK) {
        enabled_ = (v != 0);
    }
    if (seal_dev_nvs_read_u8_sync(NVS_NS, NVS_KEY_PHRASE, &v,
                                  5000) == ESP_OK &&
        v <= WAKEWORD_PHRASE_HEY_WATCHER) {
        phrase_ = (wakeword_phrase_t)v;
    }
}

static void nvs_save_enabled(void)
{
    seal_dev_nvs_write_u8_sync(NVS_NS, NVS_KEY_ENABLED,
                               enabled_ ? 1 : 0, 5000);
}

static void nvs_save_phrase(void)
{
    seal_dev_nvs_write_u8_sync(NVS_NS, NVS_KEY_PHRASE,
                               (uint8_t)phrase_, 5000);
}

/* ---- I2S ---- */

/* Create and enable the I2S0 RX channel for the wake-word feed.
 * Same pins/format as PTT (16kHz/16-bit/mono, mic on RIGHT slot).
 * Returns ESP_OK on success. */
static esp_err_t ww_i2s_create(void)
{
    if (ww_i2s_) {
        return ESP_OK;
    }
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(WW_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &ww_i2s_),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WW_SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_RIGHT, /* mic on RIGHT slot */
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = WW_PIN_MCLK,
            .bclk = WW_PIN_BCLK,
            .ws = WW_PIN_WS,
            .dout = GPIO_NUM_NC,
            .din = WW_PIN_DIN,
            .invert_flags = {0},
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(ww_i2s_, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s std init: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_enable(ww_i2s_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err));
        goto fail;
    }
    return ESP_OK;

fail:
    if (ww_i2s_) {
        i2s_del_channel(ww_i2s_);
        ww_i2s_ = NULL;
    }
    return err;
}

static void ww_i2s_delete(void)
{
    if (ww_i2s_) {
        i2s_channel_disable(ww_i2s_);
        i2s_del_channel(ww_i2s_);
        ww_i2s_ = NULL;
    }
}

/* ---- AFE ---- */

/* Resolve the wakenet model name(s) for the current phrase setting.
 * Today only "Hi, ESP" exists; the Hey Muse / Hey Watcher models are
 * placeholders until Espressif trains them. Both phrase selections
 * fall back to wn9_hiesp with a warning. */
static const char *ww_model_for_phrase(void)
{
    switch (phrase_) {
    case WAKEWORD_PHRASE_HEY_MUSE:
        ESP_LOGW(TAG, "hey_muse model not yet trained, using %s", WW_MODEL_HIESP);
        return WW_MODEL_HIESP;
    case WAKEWORD_PHRASE_HEY_WATCHER:
        ESP_LOGW(TAG, "hey_watcher model not yet trained, using %s", WW_MODEL_HIESP);
        return WW_MODEL_HIESP;
    case WAKEWORD_PHRASE_BOTH:
    default:
        return WW_MODEL_HIESP;
    }
}

static esp_err_t afe_init(void)
{
    if (afe_data_) {
        return ESP_OK;
    }
    /* Load models from the "model" partition (flashed as srmodels.bin
     * by the esp-sr build step when CONFIG_SR_WN_* is selected). */
    models_ = esp_srmodel_init("model");
    if (!models_) {
        ESP_LOGE(TAG, "esp_srmodel_init failed (model partition?)");
        return ESP_FAIL;
    }

    /* "M" = single mic, no reference channel. AFE_TYPE_SR for speech
     * recognition, LOW_COST for the S3. */
    afe_config_ = afe_config_init("M", models_, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config_) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    /* Tune for a single-mic wearable: no AEC (no speaker reference),
     * NS + AGC + VAD on, WakeNet with our phrase model. */
    afe_config_->aec_init = false;
    afe_config_->se_init = true;
    afe_config_->ns_init = true;
    afe_config_->agc_init = true;
    afe_config_->vad_init = true;
    afe_config_->vad_mode = VAD_MODE_3;
    afe_config_->wakenet_init = true;
    afe_config_->wakenet_model_name = (char *)ww_model_for_phrase();
    afe_config_->wakenet_model_name_2 = NULL; /* 2nd slot for future model */
    afe_config_->wakenet_mode = DET_MODE_90;
    /* Prefer PSRAM for AFE buffers — internal RAM is precious. */
    afe_config_->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config_->afe_perferred_core = 1;
    afe_config_->afe_perferred_priority = 5;
    afe_config_->afe_ringbuf_size = 50;

    afe_iface_ = esp_afe_handle_from_config(afe_config_);
    if (!afe_iface_) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        return ESP_FAIL;
    }
    afe_data_ = afe_iface_->create_from_config(afe_config_);
    if (!afe_data_) {
        ESP_LOGE(TAG, "afe create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AFE ready (model=%s)", afe_config_->wakenet_model_name);
    return ESP_OK;
}

static void afe_deinit(void)
{
    if (afe_data_ && afe_iface_) {
        afe_iface_->destroy(afe_data_);
        afe_data_ = NULL;
    }
    afe_iface_ = NULL;
    if (afe_config_) {
        afe_config_free(afe_config_);
        afe_config_ = NULL;
    }
    /* models_ is freed on full disable; kept across suspend/resume. */
}

/* ---- detection task ---- */

/* Feed I2S -> AFE, fetch results, fire on wake-word. Runs until
 * wakeword_stop() deletes it (or self-deletes on detection so the
 * callback can safely take I2S for PTT). */
static void ww_task_fn(void *arg)
{
    (void)arg;
    int chunksize = afe_iface_->get_feed_chunksize(afe_data_);
    int16_t *buf = heap_caps_malloc(chunksize * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no mem for feed buffer");
        ww_task_ = NULL;
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "listening (chunksize=%d)", chunksize);
    for (;;) {
        size_t n = 0;
        esp_err_t r = i2s_channel_read(ww_i2s_, buf,
                                      chunksize * sizeof(int16_t), &n,
                                      pdMS_TO_TICKS(200));
        if (r == ESP_OK && n > 0) {
            afe_iface_->feed(afe_data_, buf);
        }
        afe_fetch_result_t *res = afe_iface_->fetch(afe_data_);
        if (res && res->wakeup_state == WAKENET_DETECTED) {
            detections_++;
            ESP_LOGI(TAG, "WAKE WORD detected (model %d, count %lu)",
                     res->wakenet_model_index, (unsigned long)detections_);
            /* We're done with I2S: delete our channel BEFORE the
             * callback runs, so ptt_start_external() -> rec_start()
             * -> wakeword_stop() -> i2s_acquire() has a clean handoff.
             * The callback runs in this task's context; we delete
             * ourselves after it returns. */
            ww_i2s_delete();
            wakeword_cb_t cb = cb_;
            ww_task_ = NULL;
            heap_caps_free(buf);
            if (cb) {
                cb();
            }
            vTaskDelete(NULL);
            /* NOT REACHED */
        }
    }
}

static esp_err_t ww_task_start(void)
{
    if (ww_task_) {
        return ESP_OK;
    }
    /* v0.4.6: PSRAM stack, 4096 words = 16384 bytes (was 16KB internal). */
    ww_psram_ = task_psram_create(ww_task_fn, "ww_feed", 4096 * 4, NULL, 5,
                                 tskNO_AFFINITY);
    if (!ww_psram_) {
        ww_task_ = NULL;
        return ESP_FAIL;
    }
    ww_task_ = ww_psram_->task;
    return ESP_OK;
}

static void ww_task_stop(void)
{
    if (ww_psram_) {
        task_psram_handle_t *h = ww_psram_;
        ww_psram_ = NULL;
        ww_task_ = NULL;
        if (xTaskGetCurrentTaskHandle() == h->task) {
            /* Self-delete: cannot free the PSRAM stack we're running on.
             * vTaskDelete(NULL) and leak the 16KB (documented, rare —
             * only on wake-word detection). The handle is also leaked;
             * both are bounded and PSRAM has 7.6MB free. */
            vTaskDelete(NULL);
            /* NOTREACHED */
        } else {
            task_psram_delete(h, h->task);
        }
    } else if (ww_task_) {
        TaskHandle_t t = ww_task_;
        ww_task_ = NULL;
        /* Safe even if t is the calling task (self-delete). */
        vTaskDelete(t);
    }
}

/* ---- internal start/stop ---- */

/* Bring up the full listening pipeline: I2S -> AFE -> task.
 * Assumes no other component owns I2S0. */
static esp_err_t ww_listen_start(void)
{
    ESP_RETURN_ON_ERROR(ww_i2s_create(), TAG, "i2s create failed");
    ESP_RETURN_ON_ERROR(afe_init(), TAG, "afe init failed");
    ESP_RETURN_ON_ERROR(ww_task_start(), TAG, "task start failed");
    suspended_ = false;
    return ESP_OK;
}

/* Tear down the listening pipeline but keep the AFE instance (fast
 * resume). Called by wakeword_stop() for the PTT handoff. */
static void ww_listen_stop(void)
{
    ww_task_stop();
    ww_i2s_delete();
    suspended_ = true;
}

/* ---- public API ---- */

esp_err_t wakeword_init(void)
{
    if (ready_) {
        return ESP_OK;
    }
    nvs_load();
    ready_ = true;
    ESP_LOGI(TAG, "ready (enabled=%d, phrase=%d)", enabled_, phrase_);
    if (enabled_) {
        /* User opted in previously: start listening now. */
        if (ww_listen_start() != ESP_OK) {
            ESP_LOGW(TAG, "auto-start listening failed");
        }
    }
    return ESP_OK;
}

bool wakeword_ready(void)
{
    return ready_;
}

void wakeword_set_enabled(bool enabled)
{
    if (enabled == enabled_) {
        return;
    }
    enabled_ = enabled;
    nvs_save_enabled();
    if (enabled) {
        ESP_LOGI(TAG, "enabling wake-word listening");
        if (ww_listen_start() != ESP_OK) {
            ESP_LOGE(TAG, "listen start failed");
            enabled_ = false;
            nvs_save_enabled();
        }
    } else {
        ESP_LOGI(TAG, "disabling wake-word");
        ww_listen_stop();
        afe_deinit();
        if (models_) {
            esp_srmodel_deinit(models_);
            models_ = NULL;
        }
    }
}

bool wakeword_enabled(void)
{
    return enabled_;
}

esp_err_t wakeword_stop(void)
{
    if (!ready_ || !enabled_ || suspended_) {
        return ESP_OK;
    }
    /* PTT is taking I2S0: stop our feed task and delete our channel.
     * The AFE instance persists for a fast resume. Safe to call from
     * the detection task itself (self-delete on detection path). */
    ww_listen_stop();
    return ESP_OK;
}

esp_err_t wakeword_start(void)
{
    if (!ready_ || !enabled_ || !suspended_) {
        return ESP_OK;
    }
    /* PTT is done: re-acquire I2S0 and resume listening. */
    return ww_listen_start();
}

void wakeword_set_callback(wakeword_cb_t cb)
{
    cb_ = cb;
}

uint32_t wakeword_detection_count(void)
{
    return detections_;
}

void wakeword_set_phrase(wakeword_phrase_t phrase)
{
    if (phrase > WAKEWORD_PHRASE_HEY_WATCHER) {
        return;
    }
    if (phrase == phrase_) {
        return;
    }
    phrase_ = phrase;
    nvs_save_phrase();
    ESP_LOGI(TAG, "phrase set to %d (takes effect on next enable)", phrase);
    /* Note: changing the model requires AFE re-init. The user should
     * toggle wake-word off/on, or reboot, for it to take effect. */
}

wakeword_phrase_t wakeword_get_phrase(void)
{
    return phrase_;
}
