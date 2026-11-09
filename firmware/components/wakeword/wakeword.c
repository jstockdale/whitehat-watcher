/* Wake-word detection via ESP-SR (v0.3.4).
 *
 * Architecture: the mic (ES7243E + I2S0) is owned by the ptt
 * component; wakeword borrows it via ptt_mic_acquire/release/read
 * (extern, the house pattern — no component cycle). v0.4.36: this
 * matters because the codec's ADC is only configured inside
 * esp_codec_dev_open — the old design ran a private raw I2S channel
 * against an unconfigured chip. PTT takes the mic back via
 * wakeword_stop() before recording, returns it via wakeword_start()
 * after. Only one borrower at a time — no conflicts.
 *
 * Models: "Hi/Hey, Muse" (wn10_himuse — trained by Espressif per
 * John's 2026-09-24 request on esp-sr#88, answered 2026-10-30, in the
 * firmware since 0.4.37) and "Hi, ESP" (wn9_hiesp, the interim). The
 * AFE supports 2 simultaneous models: BOTH mode runs himuse as the
 * primary and hiesp as wakenet_model_name_2. "Hey Watcher" was never
 * trained; its selection falls back to himuse with a warning. NVS
 * `wakeword_phrase` selects the active phrase set.
 *
 * Default: OFF (opt-in). Enable via {"t":"dbg","cmd":"wakeword","on":1}.
 */

#include "wakeword.h"
#include "task_psram.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"

static const char *TAG = "wakeword";

/* ---- mic (owned by ptt, borrowed via ptt_mic_*) ---- */
#define WW_SAMPLE_RATE   16000

/* ---- NVS ---- */
#define NVS_NS            "watcher"
#define NVS_KEY_ENABLED   "wakeword_en"
#define NVS_KEY_PHRASE    "wakeword_phrase"

/* Models shipped via the "model" partition (CONFIG_SR_WN_*). */
#define WW_MODEL_HIESP    "wn9_hiesp"
#define WW_MODEL_HEY_MUSE    "wn10_himuse"
/* No "Hey Watcher" model exists; the name stays for the fallback log. */
#define WW_MODEL_HEY_WATCHER "wn9_hey_watcher"

/* ---- state ---- */
static bool ready_ = false;
static bool enabled_ = false;
static bool suspended_ = false;
static wakeword_cb_t cb_ = NULL;
static uint32_t detections_ = 0;

/* v0.4.44: dynamic level tracking for the triggered take's silence
 * stop. The feed meters every 1024-sample read: a noise-floor tracker
 * (fast attack down, slow rise — the classic ambient estimator)
 * follows the room's RMS floor, and a trailing peak holds the
 * loudest recent chunk RMS. At detection both are frozen so PTT can
 * derive the take's silence threshold from the actual environment
 * and the actual level of the "Hey Muse" that triggered it. */
static float ww_ambient_rms_ = 0.0f;
static float ww_peak_rms_ = 0.0f;
static int64_t ww_peak_rms_us_ = 0;
static float ww_detect_ambient_ = 0.0f;
static float ww_detect_speech_ = 0.0f;
static wakeword_phrase_t phrase_ = WAKEWORD_PHRASE_BOTH;

/* v0.4.38: feed and fetch are SEPARATE tasks (see the task section).
 * Both run on PSRAM stacks (v0.4.6); handles are kept for the stop /
 * reap protocol. ww_running_ is the cooperative stop flag; the
 * exited flags let the stopper distinguish a task that left its loop
 * (reap by freeing its handle directly) from one still inside a
 * blocking read (delete it). */
static volatile bool ww_running_ = false;
static volatile bool ww_feed_exited_ = true;
static volatile bool ww_fetch_exited_ = true;
static TaskHandle_t ww_feed_task_ = NULL;
static TaskHandle_t ww_fetch_task_ = NULL;
static task_psram_handle_t *ww_feed_psram_ = NULL;
static task_psram_handle_t *ww_fetch_psram_ = NULL;

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

/* ptt owns the mic (codec + I2S0); declared extern, same pattern as
 * the seal helpers above. Defined in ptt.c. */
extern esp_err_t ptt_mic_acquire(void);
extern void ptt_mic_release(void);
extern int ptt_mic_read(void *data, int len);
/* v0.4.38 diagnostics, defined in ptt.c. */
extern void ptt_mic_stats(uint32_t *ok, uint32_t *err, int *last_ret,
                          int32_t *peak);

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

/* ---- AFE ---- */

/* Resolve the primary wakenet model name for the current phrase
 * setting. "Hey Muse" shipped in 0.4.37 (wn10_himuse); "Hey Watcher"
 * was never trained and falls back to himuse with a warning. BOTH
 * mode adds hiesp as the second model (see afe_init). */
static const char *ww_model_for_phrase(void)
{
    switch (phrase_) {
    case WAKEWORD_PHRASE_HEY_WATCHER:
        ESP_LOGW(TAG, "hey_watcher model was never trained, using %s",
                 WW_MODEL_HEY_MUSE);
        return WW_MODEL_HEY_MUSE;
    case WAKEWORD_PHRASE_HEY_MUSE:
    case WAKEWORD_PHRASE_BOTH:
    default:
        return WW_MODEL_HEY_MUSE;
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
    /* BOTH mode: "Hi, ESP" rides along as the second model. */
    afe_config_->wakenet_model_name_2 =
        (phrase_ == WAKEWORD_PHRASE_BOTH) ? (char *)WW_MODEL_HIESP : NULL;
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

/* ---- detection tasks (v0.4.38) ----
 * Feed and fetch are separate tasks, the canonical ESP-SR structure.
 * The pre-0.4.38 single loop called the BLOCKING fetch() after every
 * 10 ms feed chunk, so the AFE was fed at a fraction of real time and
 * its fetch side logged "Ringbuffer of AFE is empty, Please use
 * feed() to write data" every ~204 ms for the whole session. The feed
 * task now does nothing but read + feed (in 1024-sample blocks from
 * the codec, sliced to the AFE's feed chunksize), and the fetch task
 * does nothing but fetch + detect.
 *
 * Stop protocol: ww_tasks_stop() clears ww_running_ and waits for
 * both tasks to leave their loops (feed reads time out at 1 s worst
 * case; fetch returns on its own ~204 ms cadence) BEFORE the caller
 * frees the codec — the old code deleted the task wherever it
 * happened to be, potentially inside esp_codec_dev_read against a
 * channel about to be torn down. A task that exited on its own is
 * reaped by freeing its psram handle directly (task_psram_delete
 * would vTaskDelete a stale handle); a task still stuck after the
 * wait is deleted the old way. */
#define WW_READ_SAMPLES 1024

/* v0.4.45: ww_reap is defined below ww_tasks_start; the detection
 * path (ww_fetch_fn) and ww_tasks_start both need it. */
static void ww_reap(task_psram_handle_t **slot, TaskHandle_t *tslot,
                    volatile bool *exited);

static void ww_feed_fn(void *arg)
{
    (void)arg;
    int chunksize = afe_iface_->get_feed_chunksize(afe_data_);
    int16_t *buf = heap_caps_malloc(WW_READ_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no mem for feed buffer");
        ww_feed_exited_ = true;
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "feed task up (chunksize=%d)", chunksize);
    int64_t last_log = 0;
    while (ww_running_) {
        int ret = ptt_mic_read(buf, WW_READ_SAMPLES * sizeof(int16_t));
        if (ret == 0) {
            /* v0.4.44: meter this read — ambient floor + speech
             * reference for the dynamic silence threshold. */
            uint64_t ss = 0;
            for (int i = 0; i < WW_READ_SAMPLES; i++) {
                int32_t v = buf[i];
                ss += (uint64_t)(v * v);
            }
            float rms = sqrtf((float)((double)ss / WW_READ_SAMPLES));
            int64_t now_us = esp_timer_get_time();
            if (ww_ambient_rms_ <= 0.0f) {
                ww_ambient_rms_ = rms;
            } else if (rms < ww_ambient_rms_) {
                ww_ambient_rms_ += (rms - ww_ambient_rms_) * 0.10f;
            } else {
                ww_ambient_rms_ += (rms - ww_ambient_rms_) * 0.002f;
            }
            if (rms >= ww_peak_rms_ ||
                now_us - ww_peak_rms_us_ > 1500000) {
                ww_peak_rms_ = rms;
                ww_peak_rms_us_ = now_us;
            }
            for (int off = 0; off + chunksize <= WW_READ_SAMPLES;
                 off += chunksize) {
                afe_iface_->feed(afe_data_, buf + off);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        int64_t now = esp_timer_get_time();
        if (now - last_log > 5000000) {
            last_log = now;
            uint32_t ok, err;
            int lr;
            int32_t pk;
            ptt_mic_stats(&ok, &err, &lr, &pk);
            ESP_LOGI(TAG,
                     "mic: reads ok=%lu err=%lu last_ret=%d peak=%ld",
                     (unsigned long)ok, (unsigned long)err, lr,
                     (long)pk);
        }
    }
    heap_caps_free(buf);
    ww_feed_exited_ = true;
    vTaskDelete(NULL);
}

static void ww_fetch_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "listening (fetch task up)");
    while (ww_running_) {
        afe_fetch_result_t *res = afe_iface_->fetch(afe_data_);
        if (res && res->wakeup_state == WAKENET_DETECTED) {
            detections_++;
            /* v0.4.44: freeze the level references for the take —
             * the room's floor and the wake phrase's own level. */
            ww_detect_ambient_ = ww_ambient_rms_;
            ww_detect_speech_ = ww_peak_rms_;
            ESP_LOGI(TAG, "WAKE WORD detected (model %d, count %lu)",
                     res->wakenet_model_index,
                     (unsigned long)detections_);
            ESP_LOGI(TAG, "levels at detection: ambient=%.0f speech=%.0f",
                     ww_detect_ambient_, ww_detect_speech_);
            /* Hand the mic to the callback (PTT): stop the feed task
             * first and wait for it to leave its read, then release —
             * so ptt_start_external() -> rec_start() ->
             * wakeword_stop() sees suspended_ and no-ops. Our own
             * psram handle leaks by design (a task cannot free the
             * stack it is running on), bounded at 16 KB per detection. */
            ww_running_ = false;
            int waited = 0;
            while (!ww_feed_exited_ && waited < 1500) {
                vTaskDelay(pdMS_TO_TICKS(20));
                waited += 20;
            }
            /* v0.4.45: reap the feed task's corpse. Leaving its
             * handle non-NULL made the resume's ww_tasks_start()
             * guard conclude "already running" and start nothing —
             * the listener never re-armed after a detection
             * (diagnosed live 2026-11-08 on 0.4.44; button takes
             * were unaffected because wakeword_stop() reaps both
             * handles through ww_tasks_stop()). */
            ww_reap(&ww_feed_psram_, &ww_feed_task_, &ww_feed_exited_);
            ptt_mic_release();
            suspended_ = true;
            wakeword_cb_t cb = cb_;
            ww_fetch_task_ = NULL;
            ww_fetch_psram_ = NULL;
            ww_fetch_exited_ = true;
            if (cb) {
                cb(res->wakenet_model_index);
            }
            vTaskDelete(NULL);
            /* NOT REACHED */
        }
    }
    ww_fetch_exited_ = true;
    vTaskDelete(NULL);
}

static esp_err_t ww_tasks_start(void)
{
    /* v0.4.45: never trust a handle whose task has already exited —
     * reap corpses first so the guard below only sees live tasks. */
    if (ww_feed_task_ && ww_feed_exited_) {
        ww_reap(&ww_feed_psram_, &ww_feed_task_, &ww_feed_exited_);
    }
    if (ww_fetch_task_ && ww_fetch_exited_) {
        ww_reap(&ww_fetch_psram_, &ww_fetch_task_, &ww_fetch_exited_);
    }
    if (ww_feed_task_ || ww_fetch_task_) {
        return ESP_OK;
    }
    ww_running_ = true;
    ww_feed_exited_ = false;
    ww_fetch_exited_ = false;
    ww_feed_psram_ = task_psram_create(ww_feed_fn, "ww_feed", 4096 * 4,
                                       NULL, 5, tskNO_AFFINITY);
    if (!ww_feed_psram_) {
        ww_running_ = false;
        return ESP_FAIL;
    }
    ww_feed_task_ = ww_feed_psram_->task;
    ww_fetch_psram_ = task_psram_create(ww_fetch_fn, "ww_fetch",
                                        4096 * 4, NULL, 5,
                                        tskNO_AFFINITY);
    if (!ww_fetch_psram_) {
        ww_running_ = false;
        return ESP_FAIL;
    }
    ww_fetch_task_ = ww_fetch_psram_->task;
    return ESP_OK;
}

/* Reap one task handle after a stop: a task that already exited is
 * freed directly (its TCB/stack are ours; vTaskDelete on its stale
 * handle would corrupt the scheduler), a task still alive is deleted
 * via the helper. The 50 ms grace after an exit flag covers the gap
 * between the task setting the flag and its vTaskDelete(NULL)
 * completing on the stack we are about to free. */
static void ww_reap(task_psram_handle_t **slot, TaskHandle_t *tslot,
                    volatile bool *exited)
{
    task_psram_handle_t *h = *slot;
    if (!h) {
        return;
    }
    *slot = NULL;
    *tslot = NULL;
    if (*exited) {
        vTaskDelay(pdMS_TO_TICKS(50));
        heap_caps_free(h->stack);
        heap_caps_free(h);
    } else if (xTaskGetCurrentTaskHandle() == h->task) {
        vTaskDelete(NULL);
        /* NOTREACHED */
    } else {
        task_psram_delete(h, h->task);
    }
}

static void ww_tasks_stop(void)
{
    ww_running_ = false;
    if (xTaskGetCurrentTaskHandle() != ww_feed_task_ &&
        xTaskGetCurrentTaskHandle() != ww_fetch_task_) {
        int waited = 0;
        while ((!ww_feed_exited_ || !ww_fetch_exited_) &&
               waited < 2500) {
            vTaskDelay(pdMS_TO_TICKS(20));
            waited += 20;
        }
    }
    ww_reap(&ww_feed_psram_, &ww_feed_task_, &ww_feed_exited_);
    ww_reap(&ww_fetch_psram_, &ww_fetch_task_, &ww_fetch_exited_);
}

/* ---- internal start/stop ---- */

/* Bring up the full listening pipeline: I2S -> AFE -> task.
 * Assumes no other component owns I2S0. */
static esp_err_t ww_listen_start(void)
{
    ESP_RETURN_ON_ERROR(ptt_mic_acquire(), TAG, "mic acquire failed");
    ESP_RETURN_ON_ERROR(afe_init(), TAG, "afe init failed");
    /* v0.4.39: the boot/listen-start mic probe is GONE (John's
     * ruling: too invasive to trigger without permission — it also
     * consumed whatever was said during its ~1.2 s window). The
     * PTT-take-end probe remains: it rides on the user's own button
     * press. */
    /* v0.4.40: flush any AFE ringbuffer audio left over from before a
     * suspend (e.g. the tail of a PTT take) so a resumed listener can
     * never "detect" stale audio. The AFE instance persists across
     * suspend/resume; its buffer does not get a free pass. */
    if (afe_iface_ && afe_iface_->reset_buffer) {
        afe_iface_->reset_buffer(afe_data_);
    }
    ESP_RETURN_ON_ERROR(ww_tasks_start(), TAG, "task start failed");
    suspended_ = false;
    return ESP_OK;
}

/* Tear down the listening pipeline but keep the AFE instance (fast
 * resume). Called by wakeword_stop() for the PTT handoff. */
static void ww_listen_stop(void)
{
    ww_tasks_stop();
    ptt_mic_release();
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

void wakeword_detect_levels(float *ambient_rms, float *speech_rms)
{
    if (ambient_rms) {
        *ambient_rms = ww_detect_ambient_;
    }
    if (speech_rms) {
        *speech_rms = ww_detect_speech_;
    }
}

float wakeword_ambient_rms(void)
{
    return ww_ambient_rms_;
}

bool wakeword_listening(void)
{
    return ww_running_ && ww_feed_task_ != NULL &&
           ww_fetch_task_ != NULL;
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
