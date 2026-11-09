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
#include "es8311_codec.h"
#include <math.h>

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

/* v0.4.44: dynamic silence stop for wake-word (external) takes. No
 * fixed threshold: wakeword.c tracks the room's ambient RMS floor
 * while listening and freezes it, plus the wake phrase's own RMS,
 * at detection. The take's threshold interpolates between them:
 *   thr = ambient + VAD_THR_FRAC * (speech - ambient), clamped to
 *   [max(VAD_THR_MIN, 2*ambient), VAD_THR_MAX].
 * (Calibrated against a live triggered take 2026-11-08 — quiet
 * chunks RMS ~50, speech median ~1370 in John's room — but the
 * device measures ITS environment at trigger time; these constants
 * only shape the interpolation, so the take adapts anywhere.) */
#define VAD_THR_FRAC        0.18f
#define VAD_THR_MIN         120.0f
#define VAD_THR_MAX         1200.0f
#define VAD_ARM_MULT        1.3f   /* chunk >= thr*this counts as speech
                                    * (low on purpose: arming latches on
                                    * any single chunk and only enables
                                    * the silence stop — a quiet talker
                                    * must still arm; false arms are
                                    * cheap, the 4 s rule ends them) */
#define VAD_CUE_MS          2000   /* silence at which the LED switches
                                    * to REC_ENDING (blinking red) so
                                    * the device feels responsive well
                                    * before the take actually ends */
#define VAD_SILENCE_MS      4000   /* trailing quiet that ends the take
                                    * (John 2026-11-08: 4 s — slower
                                    * speech cadences need the room) */
#define VAD_NOSPEECH_MS     8000   /* no speech at all: false trigger */
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

/* v0.4.44 VAD state for the current take (external takes only;
 * button takes end on release, as always). stop_reason_ is set by
 * whichever path ends the take and ships in audio_end. */
static bool vad_ext_ = false;
static float vad_thr_ = 0.0f, vad_amb_ = 0.0f, vad_spk_ = 0.0f;
static bool vad_armed_ = false;
static bool vad_cue_ = false;   /* REC_ENDING LED currently shown */
static int vad_silence_ms_ = 0;
static volatile bool vad_stop_ = false;
static const char *stop_reason_ = "release";
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
            .slot_mode = I2S_SLOT_MODE_STEREO,
            /* Start STEREO/BOTH exactly like the vendor BSP's
             * bsp_audio_init; esp_codec_dev re-stamps this channel
             * from the fs at open (MONO + the masked slot). v0.4.38:
             * the raw start state no longer pre-empts that reconfig
             * (the old MONO/RIGHT pre-config was the one structural
             * deviation from the vendor flow left). */
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

    /* es7243 header: call after I2S clock ready (it is — channel enabled).
     * v0.4.36: the mic is on the RIGHT slot. Open as a 2-channel stream
     * with the channel mask on channel 1, exactly as the vendor BSP does
     * (bsp_codec_set_fs). The old open (channel=1, no mask) read the
     * empty LEFT slot — that was the "records perfect silence" bug,
     * present since the first PTT build: flawless transport of zeros.
     * esp_codec_dev_read() returns clean mono from the masked channel.
     * Input gain matches the vendor's DRV_AUDIO_MIC_GAIN (27 dB); it is
     * set before the open, the vendor's order. */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = PTT_SAMPLE_RATE,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .bits_per_sample = 16,
    };
    bool gain_ok = (esp_codec_dev_set_in_gain(codec_dev_, 27.0f) == ESP_CODEC_DEV_OK);
    if (esp_codec_dev_open(codec_dev_, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open");
        err = ESP_FAIL;
        goto fail;
    }
    if (!gain_ok &&
        esp_codec_dev_set_in_gain(codec_dev_, 27.0f) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set_in_gain failed before and after open");
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
    /* v0.4.37: closing the codec device already disables the I2S
     * channel (its data-if teardown does), so our own disable below is
     * only for the no-codec path (acquire failed before the device was
     * created). Skipping it when a codec device existed removes the
     * harmless but noisy "the channel has not been enabled yet" error
     * the driver logged on every release. */
    bool had_codec = (codec_dev_ != NULL);
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
        if (!had_codec) {
            i2s_channel_disable(rx_chan_);
        }
        i2s_del_channel(rx_chan_);
        rx_chan_ = NULL;
    }
}

/* ---- shared mic access (v0.4.36) ----
 * The wake-word component used to run its own raw I2S channel and
 * never configured the codec: the ES7243E's ADC is only enabled (and
 * its clocks/gain only set) inside esp_codec_dev_open, so the wake
 * feed was reading an unconfigured chip. The mic now has ONE owner —
 * this component — and wakeword borrows it through these wrappers:
 * acquire configures the codec (RIGHT channel + gain), reads come
 * back as clean mono via esp_codec_dev_read, release frees I2S0. */
esp_err_t ptt_mic_acquire(void)
{
    return i2s_acquire();
}

void ptt_mic_release(void)
{
    i2s_release();
}

/* ---- mic diagnostics (v0.4.38) ----
 * The capture path has never verifiably carried the mic (every PTT
 * take since 2026-09-24 decodes to the empty channel's hiss, and the
 * wakeword feed starved silently). These counters make every read
 * observable: who read, whether it worked, and the peak level of what
 * came back. ptt_mic_probe() goes further and measures each candidate
 * capture path back to back — the answer rides out as a structured
 * {"t":"mic_probe"} event instead of the lossy log stream. */
static volatile uint32_t mic_reads_ok_ = 0;
static volatile uint32_t mic_reads_err_ = 0;
static volatile int mic_last_ret_ = 0;
static volatile int32_t mic_peak_win_ = 0;

static int32_t peak_of(const int16_t *pcm, size_t n, size_t stride,
                       size_t off)
{
    int32_t pk = 0;
    for (size_t i = off; i < n; i += stride) {
        int32_t v = pcm[i];
        if (v < 0) {
            v = -v;
        }
        if (v > pk) {
            pk = v;
        }
    }
    return pk;
}

static void mic_account(int ret, const int16_t *pcm, int bytes)
{
    if (ret == 0) {
        mic_reads_ok_++;
        if (pcm && bytes > 0) {
            int32_t pk = peak_of(pcm, (size_t)bytes / sizeof(int16_t),
                                 1, 0);
            if (pk > mic_peak_win_) {
                mic_peak_win_ = pk;
            }
        }
    } else {
        mic_reads_err_++;
        mic_last_ret_ = ret;
    }
}

/* v0.4.41 (mic audit 2026-11-07): live ES7243E register dump. Reads
 * the codec's actual register state back over I2C so the host can
 * diff it against the datasheet-derived intended state
 * (docs/mic-audit-2026-11-07.md). Reads go through the control
 * interface directly — the es7243e driver's get_reg is broken
 * upstream (it passes &value where value is already int*, so the
 * byte lands in the pointer variable, never in the caller's int).
 * I2C reads only; safe while the codec streams. Output format:
 * "00:80,01:3a,..." for regs 0x00-0x21 plus 0xF9/0xFC/0xFD/0xFE
 * (0xFC bit0 = automute flag; 0xFD/0xFE = chip ID 0x7A/0x43). */
esp_err_t ptt_codec_dump(bool *acquired, char *buf, size_t cap)
{
    if (acquired) {
        *acquired = (codec_dev_ != NULL);
    }
    if (!codec_ctrl_ || !codec_ctrl_->read_reg || cap < 16) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint8_t extra[] = {0xF9, 0xFC, 0xFD, 0xFE};
    size_t off = 0;
    buf[0] = '\0';
    for (int pass = 0; pass < 2; pass++) {
        int count = (pass == 0) ? 0x22 : (int)sizeof(extra);
        for (int i = 0; i < count; i++) {
            int reg = (pass == 0) ? i : extra[i];
            uint8_t val = 0;
            int r = codec_ctrl_->read_reg(codec_ctrl_, reg, 1, &val, 1);
            char entry[8];
            if (r == 0) {
                snprintf(entry, sizeof(entry), "%02x:%02x", reg, val);
            } else {
                snprintf(entry, sizeof(entry), "%02x:??", reg);
            }
            size_t el = strlen(entry);
            if (off + el + 2 > cap) {
                return ESP_ERR_NO_MEM;
            }
            if (off) {
                buf[off++] = ',';
            }
            memcpy(buf + off, entry, el + 1);
            off += el;
        }
    }
    return ESP_OK;
}

/* ---- v0.4.42: ES8311 loopback / mic-power test ----
 * Schematic finding (docs/mic-audit-2026-11-07.md): the mic's VDD is
 * fed from the ES8311's ADCVREF pin via R160 (the ADC_3V3 option R155
 * is DNP). Our firmware never initializes the ES8311, so the mic may
 * have been unpowered in every capture ever taken. This test splits
 * the question in three phases, each ~1.2 s of raw-stereo reads
 * (LEFT = DAC loopback input, RIGHT = mic):
 *   phase 0: baseline, ES8311 untouched;
 *   phase 1: ES8311 created + opened (powers its analog/reference
 *            section), no tone — if RIGHT comes alive, the mic was
 *            starved of ADCVREF and just got power;
 *   phase 2: 1 kHz tone written to the DAC — LEFT must show it
 *            (Goertzel), proving the shared analog/ADC path.
 * The DAC device and TX channel are created once and kept (closed
 * between runs) — a few hundred bytes for a reusable diagnostic. */
static i2s_chan_handle_t lb_tx_chan_;
static esp_codec_dev_handle_t lb_dac_dev_;
static bool lb_dac_open_;

typedef struct {
    int32_t l_peak, r_peak;
    int32_t l_rms, r_rms;
    int32_t l_dc, r_dc;
    int32_t l_g1k, r_g1k;
    int reads;
} lb_stats_t;

/* Reopen the record codec unmasked (raw stereo), as in the probe. */
static esp_err_t lb_open_raw(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = PTT_SAMPLE_RATE,
        .channel = 2,
        .channel_mask = 0,
        .bits_per_sample = 16,
    };
    esp_codec_dev_close(codec_dev_);
    esp_codec_dev_set_in_gain(codec_dev_, 27.0f);
    return (esp_codec_dev_open(codec_dev_, &fs) == ESP_CODEC_DEV_OK)
               ? ESP_OK
               : ESP_FAIL;
}

static void lb_measure(int16_t *buf, int chunks, bool with_tone,
                       lb_stats_t *st)
{
    /* Discard the first 3 chunks (~190 ms): a phase that follows a
     * codec reopen otherwise measures the power-up transient, not
     * the steady state (v0.4.42 phase 0: r_rms 201 from the reopen
     * transient alone). */
    for (int i = 0; i < 3; i++) {
        esp_codec_dev_read(codec_dev_, buf, 2048 * sizeof(int16_t));
    }
    /* 1 kHz Goertzel at 16 kHz: coeff = 2*cos(2*pi*1000/16000). */
    const float coeff = 1.847759f;
    float ls1 = 0, ls2 = 0, rs1 = 0, rs2 = 0;
    int64_t lsum = 0, rsum = 0;
    uint64_t lsq = 0, rsq = 0;
    int64_t n = 0;
    memset(st, 0, sizeof(*st));
    /* One period (16 samples) of a 1 kHz sine at amplitude 12000. */
    static const int16_t period[16] = {
        0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
        0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    };
    int16_t tone[1024];
    if (with_tone) {
        for (int i = 0; i < 1024; i++) {
            tone[i] = period[i & 15];
        }
    }
    for (int c = 0; c < chunks; c++) {
        if (with_tone && lb_dac_dev_) {
            esp_codec_dev_write(lb_dac_dev_, tone, sizeof(tone));
        }
        if (esp_codec_dev_read(codec_dev_, buf,
                               2048 * sizeof(int16_t)) !=
            ESP_CODEC_DEV_OK) {
            continue;
        }
        st->reads++;
        for (int i = 0; i < 2048; i += 2) {
            int32_t l = buf[i], r = buf[i + 1];
            int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            if (al > st->l_peak) st->l_peak = al;
            if (ar > st->r_peak) st->r_peak = ar;
            lsum += l;
            rsum += r;
            lsq += (uint64_t)(l * l);
            rsq += (uint64_t)(r * r);
            float s = (float)l + coeff * ls1 - ls2;
            ls2 = ls1; ls1 = s;
            s = (float)r + coeff * rs1 - rs2;
            rs2 = rs1; rs1 = s;
            n++;
        }
    }
    if (n > 0) {
        st->l_dc = (int32_t)(lsum / n);
        st->r_dc = (int32_t)(rsum / n);
        st->l_rms = (int32_t)sqrtf((float)((double)lsq / (double)n));
        st->r_rms = (int32_t)sqrtf((float)((double)rsq / (double)n));
        float lp = ls1 * ls1 + ls2 * ls2 - coeff * ls1 * ls2;
        float rp = rs1 * rs1 + rs2 * rs2 - coeff * rs1 * rs2;
        st->l_g1k = (int32_t)(2.0f * sqrtf(lp > 0 ? lp : 0) / (float)n);
        st->r_g1k = (int32_t)(2.0f * sqrtf(rp > 0 ? rp : 0) / (float)n);
    }
}

static void lb_report(int phase, const lb_stats_t *st)
{
    cJSON *o = link_new_event("loopback");
    if (o) {
        cJSON_AddNumberToObject(o, "phase", (double)phase);
        cJSON_AddNumberToObject(o, "l_peak", (double)st->l_peak);
        cJSON_AddNumberToObject(o, "r_peak", (double)st->r_peak);
        cJSON_AddNumberToObject(o, "l_rms", (double)st->l_rms);
        cJSON_AddNumberToObject(o, "r_rms", (double)st->r_rms);
        cJSON_AddNumberToObject(o, "l_dc", (double)st->l_dc);
        cJSON_AddNumberToObject(o, "r_dc", (double)st->r_dc);
        cJSON_AddNumberToObject(o, "l_g1k", (double)st->l_g1k);
        cJSON_AddNumberToObject(o, "r_g1k", (double)st->r_g1k);
        cJSON_AddNumberToObject(o, "reads", (double)st->reads);
        link_send(o);
    }
    ESP_LOGI(TAG,
             "loopback p%d: L pk=%ld rms=%ld dc=%ld g1k=%ld | "
             "R pk=%ld rms=%ld dc=%ld g1k=%ld reads=%d",
             phase, (long)st->l_peak, (long)st->l_rms, (long)st->l_dc,
             (long)st->l_g1k, (long)st->r_peak, (long)st->r_rms,
             (long)st->r_dc, (long)st->r_g1k, st->reads);
}

/* Create (once) + open the ES8311 DAC device on I2S0 TX. Opening it
 * powers the chip's analog/reference section — ADCVREF included. */
static esp_err_t lb_es8311_open(void)
{
    if (!lb_dac_dev_) {
        /* Small DMA footprint: internal RAM is tight (~12 KB free
         * with the AFE resident); the default TX config's DMA buffers
         * failed with ESP_ERR_NO_MEM in v0.4.42. */
        i2s_chan_config_t chan_cfg = {
            .id = PTT_I2S_PORT,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 3,
            .dma_frame_num = 128,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
        };
        esp_err_t cerr = i2s_new_channel(&chan_cfg, &lb_tx_chan_, NULL);
        if (cerr != ESP_OK) {
            ESP_LOGE(TAG, "loopback: tx channel: %s",
                     esp_err_to_name(cerr));
            return cerr;
        }
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PTT_SAMPLE_RATE),
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_MONO,
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
                .dout = GPIO_NUM_16,
                .din = GPIO_NUM_NC,
                .invert_flags = {0},
            },
        };
        esp_err_t err = i2s_channel_init_std_mode(lb_tx_chan_, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "loopback: tx std init: %s",
                     esp_err_to_name(err));
            i2s_del_channel(lb_tx_chan_);
            lb_tx_chan_ = NULL;
            return err;
        }
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port = PTT_I2C_PORT,
            .addr = ES8311_CODEC_DEFAULT_ADDR,
        };
        const audio_codec_ctrl_if_t *ctrl =
            audio_codec_new_i2c_ctrl(&i2c_cfg);
        const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
        es8311_codec_cfg_t ccfg = {
            .ctrl_if = ctrl,
            .gpio_if = gpio,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
            .pa_pin = GPIO_NUM_NC,
            .pa_reverted = false,
            .master_mode = false,
            .use_mclk = true,
            .digital_mic = false,
            .invert_mclk = false,
            .invert_sclk = false,
            .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        };
        const audio_codec_if_t *codec_if = es8311_codec_new(&ccfg);
        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = PTT_I2S_PORT,
            .rx_handle = NULL,
            .tx_handle = lb_tx_chan_,
        };
        const audio_codec_data_if_t *data_if =
            audio_codec_new_i2s_data(&i2s_cfg);
        if (!ctrl || !codec_if || !data_if) {
            return ESP_FAIL;
        }
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = codec_if,
            .data_if = data_if,
        };
        lb_dac_dev_ = esp_codec_dev_new(&dev_cfg);
        if (!lb_dac_dev_) {
            ESP_LOGE(TAG, "loopback: dac dev new failed");
            return ESP_FAIL;
        }
    }
    if (lb_dac_open_) {
        return ESP_OK;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = PTT_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(lb_dac_dev_, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "loopback: dac open failed");
        return ESP_FAIL;
    }
    lb_dac_open_ = true;
    esp_codec_dev_set_out_vol(lb_dac_dev_, 80);
    return ESP_OK;
}

esp_err_t ptt_loopback_test(void)
{
    if (recording_ || !codec_iface_) {
        return ESP_ERR_INVALID_STATE;
    }
    wakeword_stop();
    if (i2s_acquire() != ESP_OK) {
        wakeword_start();
        return ESP_FAIL;
    }
    int16_t *buf = heap_caps_malloc(2048 * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM);
    if (!buf) {
        i2s_release();
        wakeword_start();
        return ESP_ERR_NO_MEM;
    }
    lb_stats_t st;
    /* Phase 0: baseline, ES8311 untouched. */
    if (lb_open_raw() == ESP_OK) {
        lb_measure(buf, 19, false, &st);
        lb_report(0, &st);
    }
    /* Phase 1: ES8311 powered (ADCVREF live), no tone yet. */
    esp_err_t dac = lb_es8311_open();
    ESP_LOGI(TAG, "loopback: es8311 open %s", esp_err_to_name(dac));
    if (dac == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(400)); /* ADCVREF + mic bias settle */
        lb_measure(buf, 19, false, &st);
        lb_report(1, &st);
        /* Phase 2: 1 kHz tone into the DAC -> AINL loopback. */
        lb_measure(buf, 19, true, &st);
        lb_report(2, &st);
        /* v0.4.43: the DAC is NOT closed anymore — since 0.4.43 it
         * is opened at boot and kept powered for the mic's ADCVREF
         * supply. Closing it would starve the mic again. */
    }
    /* Restore the standard masked open, then hand the mic back. */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = PTT_SAMPLE_RATE,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .bits_per_sample = 16,
    };
    esp_codec_dev_close(codec_dev_);
    esp_codec_dev_set_in_gain(codec_dev_, 27.0f);
    esp_codec_dev_open(codec_dev_, &fs);
    heap_caps_free(buf);
    i2s_release();
    wakeword_start();
    return ESP_OK;
}

void ptt_mic_stats(uint32_t *ok, uint32_t *err, int *last_ret,
                   int32_t *peak)
{
    if (ok) {
        *ok = mic_reads_ok_;
    }
    if (err) {
        *err = mic_reads_err_;
    }
    if (last_ret) {
        *last_ret = mic_last_ret_;
    }
    if (peak) {
        *peak = mic_peak_win_;
    }
}

void ptt_mic_peak_reset(void)
{
    mic_peak_win_ = 0;
}

int ptt_mic_read(void *data, int len)
{
    if (!codec_dev_) {
        mic_account(-1, NULL, 0);
        return -1;
    }
    int ret = esp_codec_dev_read(codec_dev_, data, len);
    mic_account(ret, (const int16_t *)data, ret == 0 ? len : 0);
    return ret;
}

/* Measure every candidate capture path for ~380 ms each, in order:
 * A = the standard open (2ch, mask on channel 1 = RIGHT), B = mask on
 * channel 0 (LEFT), C = no mask (raw stereo interleave, peaks split
 * by slot). The standard open is restored before returning. Caller
 * must hold the mic (acquired) and be its only reader. */
esp_err_t ptt_mic_probe(const char *src)
{
    if (!codec_dev_) {
        return ESP_ERR_INVALID_STATE;
    }
    int16_t *buf = heap_caps_malloc(2048 * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int32_t pk_right = 0, pk_left = 0, pk_raw_l = 0, pk_raw_r = 0;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = PTT_SAMPLE_RATE,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .bits_per_sample = 16,
    };
    /* A: current open (RIGHT). */
    for (int i = 0; i < 6; i++) {
        if (esp_codec_dev_read(codec_dev_, buf,
                               1024 * sizeof(int16_t)) ==
            ESP_CODEC_DEV_OK) {
            int32_t pk = peak_of(buf, 1024, 1, 0);
            if (pk > pk_right) {
                pk_right = pk;
            }
        }
    }
    /* B: LEFT. */
    esp_codec_dev_close(codec_dev_);
    fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    esp_codec_dev_set_in_gain(codec_dev_, 27.0f);
    if (esp_codec_dev_open(codec_dev_, &fs) == ESP_CODEC_DEV_OK) {
        for (int i = 0; i < 6; i++) {
            if (esp_codec_dev_read(codec_dev_, buf,
                                   1024 * sizeof(int16_t)) ==
                ESP_CODEC_DEV_OK) {
                int32_t pk = peak_of(buf, 1024, 1, 0);
                if (pk > pk_left) {
                    pk_left = pk;
                }
            }
        }
        esp_codec_dev_close(codec_dev_);
    }
    /* C: no mask — raw stereo interleave. */
    fs.channel_mask = 0;
    esp_codec_dev_set_in_gain(codec_dev_, 27.0f);
    if (esp_codec_dev_open(codec_dev_, &fs) == ESP_CODEC_DEV_OK) {
        for (int i = 0; i < 6; i++) {
            if (esp_codec_dev_read(codec_dev_, buf,
                                   2048 * sizeof(int16_t)) ==
                ESP_CODEC_DEV_OK) {
                int32_t pl = peak_of(buf, 2048, 2, 0);
                int32_t pr = peak_of(buf, 2048, 2, 1);
                if (pl > pk_raw_l) {
                    pk_raw_l = pl;
                }
                if (pr > pk_raw_r) {
                    pk_raw_r = pr;
                }
            }
        }
        esp_codec_dev_close(codec_dev_);
    }
    /* Restore the standard open. */
    fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    esp_codec_dev_set_in_gain(codec_dev_, 27.0f);
    esp_err_t rerr = (esp_codec_dev_open(codec_dev_, &fs) ==
                      ESP_CODEC_DEV_OK)
                         ? ESP_OK
                         : ESP_FAIL;
    cJSON *o = link_new_event("mic_probe");
    if (o) {
        cJSON_AddStringToObject(o, "src", src ? src : "?");
        cJSON_AddNumberToObject(o, "right", (double)pk_right);
        cJSON_AddNumberToObject(o, "left", (double)pk_left);
        cJSON_AddNumberToObject(o, "raw_l", (double)pk_raw_l);
        cJSON_AddNumberToObject(o, "raw_r", (double)pk_raw_r);
        link_send(o);
    }
    ESP_LOGI(TAG, "mic_probe(%s): right=%ld left=%ld raw_l=%ld raw_r=%ld",
             src ? src : "?", (long)pk_right, (long)pk_left,
             (long)pk_raw_l, (long)pk_raw_r);
    heap_caps_free(buf);
    return rerr;
}

/* ---- recording ---- */

static void rec_start(bool ext)
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
    ptt_mic_peak_reset();
    /* v0.4.44: per-take VAD reset. External takes snapshot the level
     * references wakeword froze at detection and derive the dynamic
     * silence threshold from them; button takes never silence-stop. */
    vad_ext_ = ext;
    vad_armed_ = false;
    vad_cue_ = false;
    vad_silence_ms_ = 0;
    vad_stop_ = false;
    stop_reason_ = "release";
    vad_thr_ = vad_amb_ = vad_spk_ = 0.0f;
    if (ext) {
        float amb = 0.0f, spk = 0.0f;
        wakeword_detect_levels(&amb, &spk);
        vad_amb_ = amb;
        vad_spk_ = spk;
        float thr;
        if (spk > amb * 1.5f && spk > 150.0f) {
            thr = amb + VAD_THR_FRAC * (spk - amb);
        } else {
            /* No usable speech reference (levels degenerate): fall
             * back to a multiple of the measured floor. */
            thr = amb * 4.0f;
            if (thr < 200.0f) {
                thr = 200.0f;
            }
        }
        float min_thr = amb * 2.0f;
        if (min_thr < VAD_THR_MIN) {
            min_thr = VAD_THR_MIN;
        }
        if (thr < min_thr) {
            thr = min_thr;
        }
        if (thr > VAD_THR_MAX) {
            thr = VAD_THR_MAX;
        }
        vad_thr_ = thr;
        ESP_LOGI(TAG, "vad: amb=%.0f spk=%.0f thr=%.0f (silence %d ms)",
                 amb, spk, thr, VAD_SILENCE_MS);
    }
    rec_start_us_ = esp_timer_get_time();
    recording_ = true;
    led_set_state(LED_STATE_RECORDING);
    ESP_LOGI(TAG, "recording...");
}

static void rec_feed(void)
{
    /* One read = 64ms of audio; encode and append. v0.4.36: read
     * through the codec device — its open re-stamped the I2S channel
     * to MONO on the masked (RIGHT = mic) slot, so reads return clean
     * mono. v0.4.38: every read is metered (mic_account) so a take's
     * peak level ships in audio_end and silence is provable on-device,
     * not just at the bridge. */
    static int16_t pcm[PTT_PCM_CHUNK];
    int rr = esp_codec_dev_read(codec_dev_, pcm, sizeof(pcm));
    mic_account(rr, pcm, rr == ESP_CODEC_DEV_OK ? (int)sizeof(pcm) : 0);
    if (rr != ESP_CODEC_DEV_OK) {
        return;
    }
    size_t samples = PTT_PCM_CHUNK;
    size_t max_adpcm = (PTT_MAX_MS * PTT_SAMPLE_RATE / 1000) / 2;
    for (size_t i = 0; i + 1 < samples && adpcm_len_ + 1 < max_adpcm; i += 2) {
        uint8_t lo = adpcm_nibble(&adpcm_st_, pcm[i]);
        uint8_t hi = adpcm_nibble(&adpcm_st_, pcm[i + 1]);
        adpcm_buf_[adpcm_len_++] = (uint8_t)((hi << 4) | lo);
    }

    /* v0.4.44: dynamic silence detection (external takes only).
     * Per 64 ms chunk RMS: arm once speech-level energy appears
     * (>= ARM_MULT x threshold) so a slow start can't self-cancel;
     * then count consecutive sub-threshold chunks — VAD_SILENCE_MS
     * of them ends the take. Levels between threshold and the arm
     * level count as sound (speech tails never cut). A take with no
     * speech at all ends at VAD_NOSPEECH_MS (false trigger). The
     * flags are consumed by ptt_task beside the safety cap. */
    if (vad_ext_ && !vad_stop_) {
        uint64_t ss = 0;
        for (int i = 0; i < PTT_PCM_CHUNK; i++) {
            int32_t v = pcm[i];
            ss += (uint64_t)(v * v);
        }
        float rms = sqrtf((float)((double)ss / PTT_PCM_CHUNK));
        int64_t elapsed_ms = (esp_timer_get_time() - rec_start_us_) / 1000;
        if (rms >= vad_thr_ * VAD_ARM_MULT) {
            vad_armed_ = true;
            vad_silence_ms_ = 0;
        } else if (rms < vad_thr_) {
            if (vad_armed_) {
                vad_silence_ms_ += PTT_PCM_CHUNK * 1000 / PTT_SAMPLE_RATE;
                if (vad_silence_ms_ >= VAD_SILENCE_MS) {
                    stop_reason_ = "silence";
                    vad_stop_ = true;
                }
            }
        } else {
            vad_silence_ms_ = 0;
        }
        /* LED cue at VAD_CUE_MS of trailing silence (John 2026-11-08):
         * blinking red says "heard you finish, wrapping up" 2 s
         * before the take ends. Speech (or in-band sound) restores
         * steady red. */
        bool cue = vad_armed_ && vad_silence_ms_ >= VAD_CUE_MS;
        if (cue && !vad_cue_) {
            led_set_state(LED_STATE_REC_ENDING);
        } else if (!cue && vad_cue_) {
            led_clear_state(LED_STATE_REC_ENDING);
        }
        vad_cue_ = cue;
        if (!vad_armed_ && elapsed_ms >= VAD_NOSPEECH_MS) {
            stop_reason_ = "no_speech";
            vad_stop_ = true;
        }
    }
}

static void rec_stop_and_send(void)
{
    recording_ = false;
    led_clear_state(LED_STATE_RECORDING);
    led_clear_state(LED_STATE_REC_ENDING);

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
    int dropped = 0;
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
        /* v0.4.36: pace + verify, the discipline the image paths got in
         * 0.4.18.81. The old loop fired link_send unchecked: with the
         * ring busy, whole chunks dropped silently (42% of a take lost
         * at 115200, ~38% even at 750000). Wait for room, retry a
         * failed send (link_send owns the object either way, so it is
         * rebuilt per attempt), and count whatever still gets away. */
        size_t need = strlen(b64) + 128;
        bool sent = false;
        for (int attempt = 0; attempt < 3 && !sent; attempt++) {
            if (!link_wait_for_ring_space(2000, need)) {
                ESP_LOGW(TAG, "audio seq %u: ring still full", (unsigned)s);
                continue;
            }
            cJSON *obj = link_new_event("audio");
            if (!obj) { break; }
            cJSON_AddNumberToObject(obj, "id", (double)id);
            cJSON_AddNumberToObject(obj, "seq", (double)s);
            cJSON_AddNumberToObject(obj, "total", (double)total);
            cJSON_AddNumberToObject(obj, "ms", (double)dur_ms);
            cJSON_AddStringToObject(obj, "data", b64);
            sent = (link_send(obj) == ESP_OK); /* takes ownership */
        }
        if (!sent) {
            dropped++;
            ESP_LOGE(TAG, "audio seq %u dropped after retries", (unsigned)s);
        }
    }
    free(b64);

    cJSON *end = link_new_event("audio_end");
    if (end) {
        cJSON_AddNumberToObject(end, "id", (double)id);
        cJSON_AddNumberToObject(end, "chunks", (double)total);
        cJSON_AddNumberToObject(end, "ms", (double)dur_ms);
        cJSON_AddNumberToObject(end, "peak", (double)mic_peak_win_);
        /* v0.4.44: how the take ended, and — for wake-word takes —
         * the dynamic threshold inputs (ambient floor / speech
         * reference) and the derived threshold, so the silence stop
         * is verifiable from the bridge, not just on-device. */
        cJSON_AddStringToObject(end, "stop", stop_reason_);
        if (vad_ext_) {
            cJSON_AddNumberToObject(end, "thr", (double)vad_thr_);
            cJSON_AddNumberToObject(end, "amb", (double)vad_amb_);
            cJSON_AddNumberToObject(end, "spk", (double)vad_spk_);
        }
        if (dropped) {
            cJSON_AddNumberToObject(end, "dropped", (double)dropped);
        }
        link_send(end);
    }
    heap_caps_free(buf);
    ESP_LOGI(TAG, "sent %dms audio in %d chunks (%d dropped, stop=%s)",
             dur_ms, (int)total, dropped, stop_reason_);
    led_clear_state(LED_STATE_WORKING);
    led_flash_success();
    /* The host is about to transcribe/process this audio: show purple
     * "AI thinking" until it responds (or 60s timeout). The success pulse
     * above is a transient, so it plays first, then purple breathes. */
    link_ai_thinking_start();
    /* v0.4.43.1: the v0.4.38 take-end mic probe is GONE. Its rapid
     * close/open cycles on the record codec crash in esp_codec_dev's
     * paired-channel handling now that the ES8311 DAC is permanently
     * open (0.4.43): a close pends the RX disable (paired TX still
     * running), and a later reopen enables an RX channel whose DMA
     * state is gone — LoadProhibited in i2s_rx_channel_start,
     * 2026-11-08, first live PTT take on 0.4.43. The probe's hunt
     * (the silent-mic RCA) is finished anyway. ptt_mic_probe() stays
     * in the binary, uncalled, for manual resurrection if needed. */
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
            rec_start(false);
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
                stop_reason_ = "cap";
                rec_stop_and_send();
                claimed_ = false;
                ext_active_ = false;
            } else if (ext_active_ && vad_stop_) {
                /* v0.4.44: dynamic silence / no-speech stop —
                 * stop_reason_ was set by rec_feed ("silence" or
                 * "no_speech"). */
                rec_stop_and_send();
                claimed_ = false;
                ext_active_ = false;
            } else if (ext_active_ && ext_stop_) {
                /* Wake-word auto-PTT: explicit stop (wheel-click or host). */
                stop_reason_ = "stop";
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
            rec_start(true);
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

    /* v0.4.43: power the ES8311 speaker DAC and KEEP it powered.
     * Schematic sheet 8: the microphone's VDD is fed from the
     * ES8311's ADCVREF pin via R160 (the ADC_3V3 option R155 is
     * DNP), and that reference only exists while the ES8311's
     * analog section is powered by a driver open. Every firmware
     * whose mic works (factory, XiaoZhi) creates the speaker codec
     * at startup; ours never did — the mic was unpowered in every
     * capture from 2026-09 to 2026-11. Proven 2026-11-08 by the
     * loopback phases: ES8311 off + shouting = R rms 67 (floor);
     * ES8311 on + shouting = R rms 847, peak 10016.
     * docs/mic-audit-2026-11-07.md. Failure is non-fatal here so a
     * missing DAC can't take PTT down with it — but on this board
     * the mic will be deaf until it succeeds. */
    esp_err_t dac_err = lb_es8311_open();
    if (dac_err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed (%s) — mic has no ADCVREF power",
                 esp_err_to_name(dac_err));
    } else {
        ESP_LOGI(TAG, "ES8311 powered (mic ADCVREF supply live)");
    }

    /* Stack in PSRAM: ptt_task does I2S + JSON + base64, never touches flash.
     * I2S DMA buffers are allocated separately with MALLOC_CAP_DMA. */
    task_psram_ = task_psram_create(ptt_task, "ptt", 4096, NULL, 5, tskNO_AFFINITY);
    ESP_RETURN_ON_FALSE(task_psram_ != NULL, ESP_FAIL, TAG, "task create");

    ready_ = true;
    ESP_LOGI(TAG, "ready (hold wheel >%dms to talk)", PTT_HOLD_MS);
    ESP_LOGI(TAG, "mic ready @ %dHz (codec 0x%02x)", PTT_SAMPLE_RATE, codec_addr7_);
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
