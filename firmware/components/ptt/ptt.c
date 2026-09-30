/* Push-to-talk: wheel-button long-press (>500ms) captures mic audio,
 * IMA-ADPCM encodes it, and ships it to the host as base64 JSON chunks.
 *
 * Hardware: ES7243/ES7243E mic ADC on I2C0 (0x13/0x14), I2S0 RX on
 *   MCLK=GPIO10, BCLK=GPIO11, WS=GPIO12, DIN=GPIO15, 16kHz/16-bit/mono.
 * Requires eyes_init() (owns I2C0) and wheel_init() to have run first.
 *
 * Button coordination with the input component: a hold crossing 500ms is
 * claimed by PTT. input.c checks ptt_button_claimed() to suppress its
 * short-press action while PTT owns the button.
 *
 * Mode-cycle is push+scroll: button held while the wheel rotates. If
 * rotation is seen during the hold window (via wheel_pending(), which does
 * not consume detents), PTT suppresses itself. input.c also calls
 * ptt_note_mode_cycle() when it cycles the mode ring, covering the case
 * where it consumed the detents first.
 */

#include "ptt.h"
#include "task_psram.h"
#include "wheel.h"
#include "link.h"
#include "led.h"
#include "wakeword.h"
#include "cJSON.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7243_adc.h"
#include "es7243e_adc.h"

static const char *TAG = "ptt";

/* ---- config ---- */
#define PTT_SAMPLE_RATE     16000
#define PTT_I2S_PORT        I2S_NUM_0
#define PTT_PIN_MCLK        GPIO_NUM_10
#define PTT_PIN_BCLK        GPIO_NUM_11
#define PTT_PIN_WS          GPIO_NUM_12
#define PTT_PIN_DIN         GPIO_NUM_15
#define PTT_I2C_PORT        I2C_NUM_0
#define PTT_ES7243_ADDR     0x13
#define PTT_ES7243E_ADDR    0x14

#define PTT_HOLD_MS         500   /* hold this long -> PTT takes over */
#define PTT_DEBOUNCE_MS     30
#define PTT_POLL_MS         10
#define PTT_MIN_MS          300   /* shorter captures are discarded */
#define PTT_MAX_MS          30000 /* safety cap */
#define PTT_PCM_CHUNK       1024  /* samples per I2S read (64ms) */
#define PTT_ADPCM_CHUNK     1536  /* ADPCM bytes per MQTT chunk (~0.19s,
                                   ~2KB base64 JSON per line — reliable
                                   over MQTT/WebSerial) */

/* ---- IMA-ADPCM encoder ---- */
static const int adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767
};
static const int adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int32_t predictor;
    int step_index;
} adpcm_state_t;

static uint8_t adpcm_nibble(adpcm_state_t *st, int16_t sample)
{
    int diff = (int)sample - st->predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int step = adpcm_step_table[st->step_index];
    int diffq = step >> 3;
    if (diff >= step)         { code |= 4; diff -= step;        diffq += step; }
    if (diff >= (step >> 1))  { code |= 2; diff -= (step >> 1); diffq += (step >> 1); }
    if (diff >= (step >> 2))  { code |= 1;                      diffq += (step >> 2); }
    if (code & 8) st->predictor -= diffq;
    else          st->predictor += diffq;
    if (st->predictor > 32767)  st->predictor = 32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->step_index += adpcm_index_table[code & 0x0F];
    if (st->step_index < 0)  st->step_index = 0;
    if (st->step_index > 88) st->step_index = 88;
    return code & 0x0F;
}

/* ---- tiny base64 (n must be multiple of 3; out needs 4*n/3+1) ---- */
static void b64_encode(const uint8_t *in, size_t n, char *out)
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

/* ---- state ---- */
static bool ready_ = false;
/* PSRAM task handle: ptt_task does I2S audio + base64, no flash ops.
 * Stack in PSRAM; audio DMA buffers stay internal (allocated separately). */
static task_psram_handle_t *task_psram_ = NULL;
static volatile bool claimed_ = false;
static volatile bool recording_ = false;
/* Set when push+scroll is detected (wheel rotated while the button is
 * held). Suppresses the PTT claim for the rest of this press — the press
 * belongs to mode-cycle, not PTT. Written from ptt_task and from
 * ptt_note_mode_cycle() (input task); bool R/W is atomic on the S3. */
static volatile bool suppress_ = false;
/* External trigger (wake-word auto-PTT): set by ptt_start_external(),
 * consumed by ptt_task. ext_active_ tracks an in-progress external
 * recording so the stop conditions (timeout, wheel-click, explicit stop)
 * can find it. */
static volatile bool ext_start_ = false;
static volatile bool ext_stop_ = false;
static volatile bool ext_active_ = false;
static i2s_chan_handle_t rx_chan_ = NULL;
/* Codec I2C control (created once, persists across I2S acquire/release). */
static const audio_codec_ctrl_if_t *codec_ctrl_ = NULL;
static const audio_codec_if_t *codec_iface_ = NULL;
static uint8_t codec_addr7_ = 0;
/* Per-I2S-acquire: data interface + device handle. Recreated each time
 * the I2S channel is (re)created so no stale channel handle survives.
 * esp_codec_dev_delete() does NOT free the data_if; we delete it
 * ourselves in i2s_release() via audio_codec_delete_data_if(). */
static const audio_codec_data_if_t *data_if_ = NULL;
static esp_codec_dev_handle_t codec_dev_ = NULL;

static uint8_t *adpcm_buf_ = NULL;   /* PSRAM, PTT_MAX_MS worth */
static size_t adpcm_len_ = 0;
static adpcm_state_t adpcm_st_;
static int64_t rec_start_us_ = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* probe for an ACK at 7-bit addr on I2C0 */
static bool i2c_probe(uint8_t addr7)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr7 << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(PTT_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r == ESP_OK;
}

/* Probe the mic codec on I2C0 and create the control/codec interfaces.
 * Called once at ptt_init(). Does NOT touch I2S — the channel is
 * acquired per-recording via i2s_acquire() so it can be time-shared
 * with the wake-word component. */
static esp_err_t codec_probe(void)
{
    /* ES7243 (0x13) or ES7243E (0x14)? Probe the bus. */
    uint8_t addr7 = 0;
    if (i2c_probe(PTT_ES7243_ADDR)) {
        addr7 = PTT_ES7243_ADDR;
        ESP_LOGI(TAG, "found ES7243 @ 0x%02x", addr7);
    } else if (i2c_probe(PTT_ES7243E_ADDR)) {
        addr7 = PTT_ES7243E_ADDR;
        ESP_LOGI(TAG, "found ES7243E @ 0x%02x", addr7);
    } else {
        ESP_LOGE(TAG, "no mic codec on I2C0 (probed 0x13, 0x14)");
        return ESP_ERR_NOT_FOUND;
    }
    codec_addr7_ = addr7;

    /* NOTE: esp_codec_dev's i2c ctrl does (addr >> 1) internally, so it
     * wants the 8-bit address form here. */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = PTT_I2C_PORT,
        .addr = (uint8_t)(addr7 << 1),
    };
    codec_ctrl_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(codec_ctrl_, ESP_FAIL, TAG, "i2c ctrl");

    if (addr7 == PTT_ES7243_ADDR) {
        es7243_codec_cfg_t ccfg = { .ctrl_if = codec_ctrl_ };
        codec_iface_ = es7243_codec_new(&ccfg);
    } else {
        es7243e_codec_cfg_t ccfg = { .ctrl_if = codec_ctrl_ };
        codec_iface_ = es7243e_codec_new(&ccfg);
    }
    ESP_RETURN_ON_FALSE(codec_iface_, ESP_FAIL, TAG, "codec new");
    return ESP_OK;
}

/* Acquire I2S0 for PTT: create the RX channel, wire it to a fresh
 * codec device, and open the codec (needs the I2S clock running).
 * Safe to call when already acquired (no-op). Returns ESP_OK on
 * success. The caller must have ensured no other component (wake-word)
 * currently owns I2S0. */
static void i2s_release(void); /* defined below; acquire calls it on fail */
static esp_err_t i2s_acquire(void)
{
    if (rx_chan_) {
        return ESP_OK;
    }
    /* I2S RX channel (S3 is I2S master; codec is slave). */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(PTT_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx_chan_),
                        TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PTT_SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            /* Vendor BSP uses SLOT_BOTH for mono mic capture. The ES7243
             * is a stereo ADC; the single mic is on the LEFT channel.
             * Sampling RIGHT only (as before) yields all zeros. */
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = PTT_PIN_MCLK,
            .bclk = PTT_PIN_BCLK,
            .ws = PTT_PIN_WS,
            .dout = GPIO_NUM_NC,
            .din = PTT_PIN_DIN,
            .invert_flags = {0},
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(rx_chan_, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s std init: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_enable(rx_chan_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable: %s", esp_err_to_name(err));
        goto fail;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = PTT_I2S_PORT,
        .rx_handle = rx_chan_,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "i2s data if");
        err = ESP_FAIL;
        goto fail;
    }
    data_if_ = data_if;
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_iface_,
        .data_if = data_if_,
    };
    codec_dev_ = esp_codec_dev_new(&dev_cfg);
    if (!codec_dev_) {
        ESP_LOGE(TAG, "codec dev new");
        err = ESP_FAIL;
        goto fail;
    }

    /* es7243 header: call after I2S clock ready (it is — channel enabled). */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = PTT_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(codec_dev_, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open");
        err = ESP_FAIL;
        goto fail;
    }
    return ESP_OK;

fail:
    i2s_release();
    return err;
}

/* Release I2S0: tear down the codec device and delete the RX channel.
 * Safe to call when not acquired (no-op). After this returns, another
 * component (wake-word) may acquire I2S0. */
static void i2s_release(void)
{
    if (codec_dev_) {
        esp_codec_dev_close(codec_dev_);
        esp_codec_dev_delete(codec_dev_);
        codec_dev_ = NULL;
    }
    /* The codec device doesn't own the data interface — delete it
     * ourselves so repeated acquire/release cycles don't leak. */
    if (data_if_) {
        audio_codec_delete_data_if(data_if_);
        data_if_ = NULL;
    }
    if (rx_chan_) {
        i2s_channel_disable(rx_chan_);
        i2s_del_channel(rx_chan_);
        rx_chan_ = NULL;
    }
}

/* ---- recording ---- */

static void rec_start(void)
{
    /* Take I2S0 from the wake-word component if it's listening. The AFE
     * is suspended and its I2S channel deleted; we then create our own.
     * Only one owner at a time — no conflicts, no leaks. */
    wakeword_stop();
    if (i2s_acquire() != ESP_OK) {
        ESP_LOGE(TAG, "i2s acquire failed, cannot record");
        wakeword_start(); /* give the mic back */
        return;
    }
    size_t max_adpcm = (PTT_MAX_MS * PTT_SAMPLE_RATE / 1000) / 2;
    adpcm_buf_ = heap_caps_malloc(max_adpcm, MALLOC_CAP_SPIRAM);
    if (!adpcm_buf_) {
        ESP_LOGE(TAG, "no PSRAM for audio buffer");
        i2s_release();
        wakeword_start();
        return;
    }
    adpcm_len_ = 0;
    adpcm_st_.predictor = 0;
    adpcm_st_.step_index = 0;
    rec_start_us_ = esp_timer_get_time();
    recording_ = true;
    led_set_state(LED_STATE_RECORDING);
    ESP_LOGI(TAG, "recording...");
}

static void rec_feed(void)
{
    /* One I2S read = 64ms of audio; encode and append. */
    static int16_t pcm[PTT_PCM_CHUNK];
    size_t n = 0;
    if (i2s_channel_read(rx_chan_, pcm, sizeof(pcm), &n,
                         pdMS_TO_TICKS(100)) != ESP_OK || n == 0) {
        return;
    }
    size_t samples = n / 2;
    size_t max_adpcm = (PTT_MAX_MS * PTT_SAMPLE_RATE / 1000) / 2;
    for (size_t i = 0; i + 1 < samples && adpcm_len_ + 1 < max_adpcm; i += 2) {
        uint8_t lo = adpcm_nibble(&adpcm_st_, pcm[i]);
        uint8_t hi = adpcm_nibble(&adpcm_st_, pcm[i + 1]);
        adpcm_buf_[adpcm_len_++] = (uint8_t)((hi << 4) | lo);
    }
}

static void rec_stop_and_send(void)
{
    recording_ = false;
    led_clear_state(LED_STATE_RECORDING);

    int dur_ms = (int)((esp_timer_get_time() - rec_start_us_) / 1000);
    size_t len = adpcm_len_;
    uint8_t *buf = adpcm_buf_;
    adpcm_buf_ = NULL;
    adpcm_len_ = 0;

    if (dur_ms < PTT_MIN_MS || len == 0) {
        ESP_LOGI(TAG, "discarded %dms tap", dur_ms);
        heap_caps_free(buf);
        /* Still release I2S and resume wake-word on the discard path. */
        i2s_release();
        wakeword_start();
        return;
    }

    /* Encoding + shipping the audio is device work: breathe green until
     * done, then a single green pulse as confirmation. */
    led_set_state(LED_STATE_WORKING);

    /* Ship as base64 chunks. PTT_ADPCM_CHUNK is a multiple of 3. */
    size_t total = (len + PTT_ADPCM_CHUNK - 1) / PTT_ADPCM_CHUNK;
    /* v0.4.6: PSRAM for base64 buffer (~5.5KB, was internal). PTT runs on PSRAM stack. */
    char *b64 = heap_caps_malloc(PTT_ADPCM_CHUNK * 4 / 3 + 1, MALLOC_CAP_SPIRAM);
    if (!b64) {
        ESP_LOGE(TAG, "no mem for b64");
        heap_caps_free(buf);
        led_clear_state(LED_STATE_WORKING);
        i2s_release();
        wakeword_start();
        return;
    }
    int64_t id = rec_start_us_ / 1000;
    for (size_t s = 0; s < total; s++) {
        size_t off = s * PTT_ADPCM_CHUNK;
        size_t n = len - off > PTT_ADPCM_CHUNK ? PTT_ADPCM_CHUNK : len - off;
        /* pad to multiple of 3 for the encoder */
        size_t npad = (3 - (n % 3)) % 3;
        uint8_t tail[3] = {0, 0, 0};
        size_t nfull = n - (n % 3);
        b64_encode(buf + off, nfull, b64);
        if (n % 3) {
            memcpy(tail, buf + off + nfull, n % 3);
            char tmp[8];
            b64_encode(tail, 3, tmp);
            size_t bl = strlen(b64);
            memcpy(b64 + bl, tmp, 4 - npad);
            for (size_t p = 0; p < npad; p++) b64[bl + 4 - npad + p] = '=';
            b64[bl + 4] = '\0';
        }
        cJSON *obj = link_new_event("audio");
        if (!obj) { break; }
        cJSON_AddNumberToObject(obj, "id", (double)id);
        cJSON_AddNumberToObject(obj, "seq", (double)s);
        cJSON_AddNumberToObject(obj, "total", (double)total);
        cJSON_AddNumberToObject(obj, "ms", (double)dur_ms);
        cJSON_AddStringToObject(obj, "data", b64);
        link_send(obj); /* takes ownership */
    }
    free(b64);

    cJSON *end = link_new_event("audio_end");
    if (end) {
        cJSON_AddNumberToObject(end, "id", (double)id);
        cJSON_AddNumberToObject(end, "chunks", (double)total);
        cJSON_AddNumberToObject(end, "ms", (double)dur_ms);
        link_send(end);
    }
    heap_caps_free(buf);
    ESP_LOGI(TAG, "sent %dms audio in %d chunks", dur_ms, (int)total);
    led_clear_state(LED_STATE_WORKING);
    led_flash_success();
    /* The host is about to transcribe/process this audio: show purple
     * "AI thinking" until it responds (or 60s timeout). The success pulse
     * above is a transient, so it plays first, then purple breathes. */
    link_ai_thinking_start();
    /* Give I2S0 back: release our channel, then let the wake-word
     * component resume listening (no-op if it was never enabled). */
    i2s_release();
    wakeword_start();
}

/* ---- button monitor task ---- */

static void ptt_task(void *arg)
{
    (void)arg;
    bool down = false;          /* debounced state */
    bool raw_last = false;
    int64_t raw_edge_ms = 0;
    int64_t press_start_ms = 0;

    for (;;) {
        bool raw = wheel_button_down();
        int64_t t = now_ms();

        /* debounce: require stable for PTT_DEBOUNCE_MS */
        if (raw != raw_last) {
            raw_last = raw;
            raw_edge_ms = t;
        }
        bool stable = (t - raw_edge_ms) >= PTT_DEBOUNCE_MS;
        bool deb = stable ? raw : down;

        if (deb != down) {
            down = deb;
            if (down) {
                press_start_ms = t; /* press edge */
                suppress_ = false;  /* fresh press, clear any stale flag */
            } else if (claimed_) {
                /* release edge */
                rec_stop_and_send();
                claimed_ = false;
                suppress_ = false;
            } else {
                /* release without ever claiming: clear suppress too */
                suppress_ = false;
            }
        }

        /* push+scroll = mode-cycle, not PTT. If the wheel rotates while
         * the button is held (before the claim threshold), suppress the
         * claim for the rest of this press. wheel_pending() peeks without
         * consuming, so input.c still sees the detents. */
        if (down && !claimed_ && !suppress_ && wheel_pending() != 0) {
            suppress_ = true;
            ESP_LOGI(TAG, "push+scroll detected, PTT suppressed");
        }

        /* hold crossing the PTT threshold -> claim the button */
        if (down && !claimed_ && !suppress_ &&
            (t - press_start_ms) >= PTT_HOLD_MS) {
            claimed_ = true;
            rec_start();
            if (!recording_) {
                /* rec_start failed (no PSRAM) — release the claim so the
                 * input component can still use the button. */
                claimed_ = false;
            }
        }

        /* while recording, pump the I2S -> ADPCM pipeline */
        if ((claimed_ || ext_active_) && recording_) {
            rec_feed();
            /* v0.4.3: explicit yield every recording iteration. The old
             * comment claimed rec_feed "already consumed ~64ms", but
             * i2s_channel_read returns as soon as DMA data is ready —
             * it does not block the full chunk period — so this loop
             * spun without ever hitting the vTaskDelay() below and
             * starved the idle task: TWDT fired "CPU 1: ptt" on an 11 s
             * recording (00:58:38). 1 ms preserves the audio cadence
             * (64 ms chunks) while letting idle run. */
            vTaskDelay(pdMS_TO_TICKS(1));
            /* safety cap: auto-release at PTT_MAX_MS */
            if ((esp_timer_get_time() - rec_start_us_) / 1000 >= PTT_MAX_MS) {
                rec_stop_and_send();
                claimed_ = false;
                ext_active_ = false;
            } else if (ext_active_ && ext_stop_) {
                /* Wake-word auto-PTT: explicit stop (wheel-click or host). */
                ext_stop_ = false;
                rec_stop_and_send();
                ext_active_ = false;
            }
            continue;
        }

        /* External trigger (wake-word auto-PTT): start recording without
         * the button. rec_start() takes I2S from the wake-word component. */
        if (ext_start_ && !recording_) {
            ext_start_ = false;
            ESP_LOGI(TAG, "external trigger, starting recording");
            rec_start();
            if (recording_) {
                ext_active_ = true;
            } else {
                ESP_LOGW(TAG, "external rec_start failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PTT_POLL_MS));
    }
}

/* ---- public API ---- */

esp_err_t ptt_init(void)
{
    if (ready_) return ESP_OK;

    ESP_RETURN_ON_ERROR(codec_probe(), TAG, "codec probe failed");

    /* Stack in PSRAM: ptt_task does I2S + JSON + base64, never touches flash.
     * I2S DMA buffers are allocated separately with MALLOC_CAP_DMA. */
    task_psram_ = task_psram_create(ptt_task, "ptt", 4096, NULL, 5, tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(task_psram_ != NULL, ESP_FAIL, TAG, "task create");

    ready_ = true;
    ESP_LOGI(TAG, "ready (hold wheel >%dms to talk)", PTT_HOLD_MS);
    return ESP_OK;
}

bool ptt_ready(void) { return ready_; }
bool ptt_is_recording(void) { return ready_ && recording_; }
bool ptt_button_claimed(void) { return ready_ && claimed_; }

/** Start a PTT recording programmatically (wake-word auto-PTT).
 *  The ptt_task picks this up, takes I2S from the wake-word component,
 *  and records until ptt_stop_external(), the safety timeout, or a
 *  wheel-click (via input.c). Safe to call when not initialized. */
void ptt_start_external(void)
{
    if (!ready_ || recording_) {
        return;
    }
    ext_start_ = true;
}

/** End an external (wake-word triggered) recording early. No-op if no
 *  external recording is active. */
void ptt_stop_external(void)
{
    if (ext_active_) {
        ext_stop_ = true;
    }
}

/** True while an external (wake-word triggered) recording is active. */
bool ptt_is_external(void)
{
    return ready_ && ext_active_ && recording_;
}

void ptt_note_mode_cycle(void)
{
    /* input.c saw push+scroll and cycled the mode ring: this press is
     * mode-cycle's, not PTT's. Suppress the claim until release. */
    suppress_ = true;
}
