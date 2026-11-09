/* Watcher input-assistant PoC (Phase 0).
 *
 * Boots: NVS -> link (USB-serial JSON) -> Wi-Fi -> BLE HID -> eyes (Himax).
 * The device is a BLE keyboard/mouse the host pairs with; the host agent
 * drives it over the serial JSON link and receives camera stills over Wi-Fi.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/wdt_hal.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "link.h"
#include "net.h"
#include "hid_ble.h"
#include "eyes.h"
#include "png_crash_detector.h"
#include "display.h"
#include "touch.h"
#include "wheel.h"
#include "input.h"
#include "mode.h"
#include "timer.h"
#include "led.h"
#include "ptt.h"
#include "wakeword.h"

static const char *TAG = "main";
#define FW_VERSION "0.4.49"

/* v0.4.18.72 (Claude btmon 2026-10-08): BLE GAP callbacks run in the NimBLE
 * host task context. Doing I/O there (display, link_send, MQTT) blocks the
 * host task, causing SMP Pairing Requests to go unanswered → 30s timeout →
 * disconnect. This queue makes on_ble_event fully async: the callback just
 * queues (non-blocking) and returns; ble_evt_task does the work.
 *
 * v0.4.18.75 (NCP-1): Extended from bool to struct to carry pairing prompt
 * events (PAIR_SHOW/PAIR_HIDE) alongside connect/disconnect. */
typedef enum {
    BLE_EVT_CONN,
    BLE_EVT_DISC,
    BLE_EVT_PAIR_NUMCMP,
    BLE_EVT_PAIR_PASSKEY,
    BLE_EVT_PAIR_CONSENT,
    BLE_EVT_PAIR_DISMISS,
} ble_evt_kind_t;

typedef struct {
    ble_evt_kind_t kind;
    uint32_t gen;   /* pairing generation (for PAIR_SHOW/HIDE) */
    uint32_t num;   /* 6-digit comparison number (for PAIR_SHOW) */
} ble_evt_t;

static QueueHandle_t ble_evt_q_ = NULL;

/* NCP-1 revC: Pairing timeout kinds.
 * NC prompt: 25s (< 30s SMP budget) → auto-reject.
 * Passkey prompt: 35s dismiss-only (SMP handles timeout; we just hide stale UI).
 * Consent prompt: 60s (no protocol deadline) → auto-reject. */
typedef enum {
    PAIR_TIMEOUT_NC,
    PAIR_TIMEOUT_PASSKEY,
    PAIR_TIMEOUT_CONSENT,
} pair_timeout_kind_t;

/* NCP-1: Pairing timeout (one-shot esp_timer). Generation-guarded. */
static esp_timer_handle_t pair_timer_ = NULL;
static uint32_t pair_timer_gen_ = 0;
static pair_timeout_kind_t pair_timer_kind_ = PAIR_TIMEOUT_NC;

/* NCP-1 revB: Pairing input via touchscreen buttons (see display.c).
 * Wheel arming removed — John prefers touchscreen Yes/No for the ceremony. */

static void pair_timeout_cb(void *arg)
{
    (void)arg;
    /* Use the generation and kind captured when the timer was started. */
    uint32_t gen = pair_timer_gen_;
    if (pair_timer_kind_ == PAIR_TIMEOUT_PASSKEY) {
        /* NCP-1 revC: Passkey timeout is dismiss-only. The SMP 30s window
         * handles the actual failure; we just hide a stale modal. */
        ESP_LOGW("BLE", "passkey modal timeout (gen=%u), dismissing",
                 (unsigned)gen);
        if (ble_evt_q_) {
            ble_evt_t e = { .kind = BLE_EVT_PAIR_DISMISS, .gen = gen, .num = 0 };
            xQueueSend(ble_evt_q_, &e, 0);
        }
    } else {
        const char *kind_str = (pair_timer_kind_ == PAIR_TIMEOUT_CONSENT)
                               ? "consent" : "numcmp";
        ESP_LOGW("BLE", "%s timeout (gen=%u), auto-rejecting",
                 kind_str, (unsigned)gen);
        hid_ble_pair_confirm(gen, false);
        /* Dismiss UI via queue. */
        if (ble_evt_q_) {
            ble_evt_t e = { .kind = BLE_EVT_PAIR_DISMISS, .gen = gen, .num = 0 };
            xQueueSend(ble_evt_q_, &e, 0);
        }
        /* Telemetry: timeout (number never sent). */
        cJSON *o = link_new_event("ble_pair");
        cJSON_AddStringToObject(o, "kind", kind_str);
        cJSON_AddStringToObject(o, "state", "timeout");
        cJSON_AddNumberToObject(o, "gen", gen);
        link_send(o);
    }
}

static void ble_evt_task(void *arg)
{
    (void)arg;
    ble_evt_t evt;
    while (1) {
        if (xQueueReceive(ble_evt_q_, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.kind) {
            case BLE_EVT_CONN:
                /* v0.4.18.73: LED updates moved here from gap_event_cb.
                 * They use portMAX_DELAY locks which would block the NimBLE
                 * host task. Safe here in our own task context. */
                led_clear_state(LED_STATE_BLE_ADVERTISING);
                led_flash_green();
                display_set_ble(true);
                {
                    cJSON *o = link_new_event("ble");
                    cJSON_AddStringToObject(o, "state", "connected");
                    link_send(o);
                }
                break;
            case BLE_EVT_DISC:
                display_set_ble(false);
                {
                    cJSON *o = link_new_event("ble");
                    cJSON_AddStringToObject(o, "state", "disconnected");
                    link_send(o);
                }
                break;
            case BLE_EVT_PAIR_NUMCMP:
                /* NCP-1 revB: Show NC modal with 6-digit number, 25s timeout.
                 * Touchscreen Accept/Reject buttons (no wheel needed). */
                display_pair_show(DISPLAY_PAIR_NUMCMP, evt.gen, evt.num);
                pair_timer_gen_ = evt.gen;
                pair_timer_kind_ = PAIR_TIMEOUT_NC;
                esp_timer_start_once(pair_timer_, 25000000);
                {
                    cJSON *o = link_new_event("ble_pair");
                    cJSON_AddStringToObject(o, "kind", "numcmp");
                    cJSON_AddStringToObject(o, "state", "prompt");
                    cJSON_AddNumberToObject(o, "gen", evt.gen);
                    link_send(o);
                }
                break;
            case BLE_EVT_PAIR_PASSKEY:
                /* NCP-1 revC: Show passkey (host types it), no local confirm.
                 * 35s dismiss-only fallback timer (SMP 30s handles failure). */
                display_pair_show(DISPLAY_PAIR_PASSKEY, evt.gen, evt.num);
                /* No input arm — display-only role. Dismissed by ENC_CHANGE. */
                pair_timer_gen_ = evt.gen;
                pair_timer_kind_ = PAIR_TIMEOUT_PASSKEY;
                esp_timer_start_once(pair_timer_, 35000000);
                {
                    cJSON *o = link_new_event("ble_pair");
                    cJSON_AddStringToObject(o, "kind", "passkey");
                    cJSON_AddStringToObject(o, "state", "prompt");
                    cJSON_AddNumberToObject(o, "gen", evt.gen);
                    /* Never send the passkey number (revC §0 hard rule). */
                    link_send(o);
                }
                break;
            case BLE_EVT_PAIR_CONSENT:
                /* NCP-1 revB: Show consent modal (amber, no number), 60s timeout.
                 * Touchscreen Accept/Reject buttons. */
                display_pair_show(DISPLAY_PAIR_CONSENT, evt.gen, 0);
                pair_timer_gen_ = evt.gen;
                pair_timer_kind_ = PAIR_TIMEOUT_CONSENT;
                esp_timer_start_once(pair_timer_, 60000000);
                {
                    cJSON *o = link_new_event("ble_pair");
                    cJSON_AddStringToObject(o, "kind", "consent");
                    cJSON_AddStringToObject(o, "state", "prompt");
                    cJSON_AddNumberToObject(o, "gen", evt.gen);
                    link_send(o);
                }
                break;
            case BLE_EVT_PAIR_DISMISS:
                /* NCP-1 revB: Dismiss any pairing modal. */
                esp_timer_stop(pair_timer_);
                display_pair_hide();
                break;
            }
        }
    }
}

static void on_ble_event(bool connected)
{
    /* v0.4.18.72: Async only. Queue the event (non-blocking, drop if full)
     * and return immediately. Never do I/O in the NimBLE host task. */
    if (ble_evt_q_) {
        ble_evt_t e = {
            .kind = connected ? BLE_EVT_CONN : BLE_EVT_DISC,
            .gen = 0,
            .num = 0,
        };
        xQueueSend(ble_evt_q_, &e, 0);
    }
}

/* NCP-1 revB: Pairing UI callback from hid_ble (runs in NimBLE host task).
 * Queue-only, never block. */
static void on_ble_pair_ui(uint32_t gen, hid_ble_pair_ui_t what, uint32_t number)
{
    if (ble_evt_q_) {
        ble_evt_kind_t kind;
        switch (what) {
        case HID_BLE_PAIR_UI_NUMCMP:  kind = BLE_EVT_PAIR_NUMCMP;  break;
        case HID_BLE_PAIR_UI_PASSKEY: kind = BLE_EVT_PAIR_PASSKEY; break;
        case HID_BLE_PAIR_UI_CONSENT: kind = BLE_EVT_PAIR_CONSENT; break;
        case HID_BLE_PAIR_UI_DISMISS:
        default:                      kind = BLE_EVT_PAIR_DISMISS; break;
        }
        ble_evt_t e = { .kind = kind, .gen = gen, .num = number };
        if (xQueueSend(ble_evt_q_, &e, 0) != pdTRUE) {
            ESP_LOGW("BLE", "pair UI event dropped (queue full)");
        }
    }
}

static bool on_snaptest_chunk(int id, const char *mid, int seq, int total,
                              const char *b64, uint32_t crc)
{
    /* v0.4.18.81 (John 2026-10-08): Watermark flow control. Block until
     * ring has space — paces the PNG encoder to the USB drain rate.
     * v0.4.18.84: 60s timeout per chunk (generous; 800KB at 115200 takes
     * ~45s total). Blocking is correct here — this is the one place where
     * backpressure should apply.
     * v0.4.18.84 (John 2026-10-08): Return false on timeout to ABORT the
     * entire PNG transfer. A dropped chunk corrupts the image — abort is
     * the only safe option (fatal for the function, not a panic).
     * v0.4.18.88 (2026-10-08): Pass exact required bytes. The old 50%
     * watermark (6KB free) was insufficient for 11KB chunk JSONs — the
     * ring_push_nb overwrote, truncating the base64. */
    size_t need = strlen(b64) + 160;  /* b64 + JSON overhead (+crc v0.35) */
    if (!link_wait_for_ring_space(60000, need)) {
        ESP_LOGE("main", "snaptest: ring stuck full, ABORTING PNG transfer at chunk %d", seq);
        return false;
    }
    /* v0.4.35: ack'd streaming (opt-in via the snaptest command's
     * "ack":1; armed in link.c). Emit one chunk, then wait for the
     * host's chunk_ack before returning — the encoder cannot run
     * ahead of the receiver, so receive flooding (console 0.4.178
     * field failure: gaps widening from seq ~64) becomes impossible.
     * A nack (host got the chunk but it failed validation) or the
     * baud-aware timeout resends the SAME chunk; 5 sends total, then
     * abort the stream with a snaptest_chunk_error event. Legacy
     * hosts never arm the gate and see exactly the old behavior. */
    bool ack_mode = link_chunk_ack_active();
    char crcbuf[9];
    snprintf(crcbuf, sizeof(crcbuf), "%08x", (unsigned)crc);
    for (int attempt = 0; ; attempt++) {
        if (ack_mode) {
            link_chunk_ack_note_send();
        }
        cJSON *o = link_new_event("snaptest_chunk");
        cJSON_AddNumberToObject(o, "id", id);
        if (mid && mid[0]) {
            cJSON_AddStringToObject(o, "_mid", mid);
        }
        cJSON_AddNumberToObject(o, "seq", seq);
        cJSON_AddNumberToObject(o, "total", total);
        /* v0.4.18.70 (Claude F6e): Removed "last" field. In streaming mode
         * total=-1, so seq==total-1 is never true. Page uses IEND chunk. */
        /* v0.4.21: blen = expected b64 length, so receivers can pinpoint a
         * truncated chunk deterministically (chunk-loss RCA 2026-11-02). */
        cJSON_AddNumberToObject(o, "blen", (double)strlen(b64));
        /* v0.4.35: CRC32 of the raw chunk bytes so the host can verify
         * the chunk before acking (a bit-flipped b64 payload still
         * parses as JSON — blen alone can't catch that). */
        cJSON_AddStringToObject(o, "crc", crcbuf);
        cJSON_AddStringToObject(o, "b64", b64);
        /* v0.4.21: a failed send (ring push dropped the line) aborts the
         * encode — never silently ship a holed image. */
        if (link_send(o) != ESP_OK) {
            ESP_LOGE("main", "snaptest: chunk %d send failed, ABORTING PNG transfer", seq);
            return false;
        }
        if (!ack_mode) {
            return true;
        }
        if (link_chunk_ack_wait(id, seq, strlen(b64) + 128)) {
            return true;
        }
        if (attempt >= 4) {
            ESP_LOGE("main", "snaptest: chunk %d unacked after %d sends, ABORTING PNG transfer", seq, attempt + 1);
            cJSON *e = link_new_event("snaptest_chunk_error");
            cJSON_AddNumberToObject(e, "id", id);
            cJSON_AddNumberToObject(e, "seq", seq);
            cJSON_AddStringToObject(e, "msg",
                                    "chunk not acknowledged by host");
            link_send(e);
            return false;
        }
        ESP_LOGW("main", "snaptest: chunk %d unacked/nacked — resending (send %d)", seq, attempt + 2);
    }
}

/* v0.4.14 (P2): PNG capture chunks -> {t:"png_chunk"} events. */
static bool on_png_chunk(int seq, int total, const char *b64, uint32_t crc)
{
    /* v0.4.18.81: Watermark flow control (see on_snaptest_chunk).
     * v0.4.18.84: 60s timeout, blocking is correct.
     * v0.4.18.84 (John 2026-10-08): Return false on timeout to ABORT.
     * A dropped chunk corrupts the PNG.
     * v0.4.18.88: Pass exact required bytes (see on_snaptest_chunk). */
    size_t need = strlen(b64) + 160;
    if (!link_wait_for_ring_space(60000, need)) {
        ESP_LOGE("main", "dbg png: ring stuck full, ABORTING PNG transfer at chunk %d", seq);
        return false;
    }
    /* v0.4.35: ack'd streaming, same gate as on_snaptest_chunk but
     * under the reserved png stream id (see link.h). The event gains
     * "id" so the host can echo it in chunk_ack/chunk_nack. */
    bool ack_mode = link_chunk_ack_active();
    char crcbuf[9];
    snprintf(crcbuf, sizeof(crcbuf), "%08x", (unsigned)crc);
    for (int attempt = 0; ; attempt++) {
        if (ack_mode) {
            link_chunk_ack_note_send();
        }
        cJSON *o = link_new_event("png_chunk");
        cJSON_AddNumberToObject(o, "id", LINK_PNG_STREAM_ID);
        cJSON_AddNumberToObject(o, "seq", seq);
        cJSON_AddNumberToObject(o, "total", total);
        cJSON_AddBoolToObject(o, "last", seq == total - 1);
        /* v0.4.21: blen + send-failure abort (see on_snaptest_chunk). */
        cJSON_AddNumberToObject(o, "blen", (double)strlen(b64));
        /* v0.4.35: CRC32 of the raw chunk bytes (see on_snaptest_chunk). */
        cJSON_AddStringToObject(o, "crc", crcbuf);
        cJSON_AddStringToObject(o, "b64", b64);
        if (link_send(o) != ESP_OK) {
            ESP_LOGE("main", "dbg png: chunk %d send failed, ABORTING PNG transfer", seq);
            return false;
        }
        if (!ack_mode) {
            return true;
        }
        if (link_chunk_ack_wait(LINK_PNG_STREAM_ID, seq,
                                 strlen(b64) + 128)) {
            return true;
        }
        if (attempt >= 4) {
            ESP_LOGE("main", "dbg png: chunk %d unacked after %d sends, ABORTING PNG transfer", seq, attempt + 1);
            cJSON *e = link_new_event("png_chunk_error");
            cJSON_AddNumberToObject(e, "id", LINK_PNG_STREAM_ID);
            cJSON_AddNumberToObject(e, "seq", seq);
            cJSON_AddStringToObject(e, "msg",
                                    "chunk not acknowledged by host");
            link_send(e);
            return false;
        }
        ESP_LOGW("main", "dbg png: chunk %d unacked/nacked — resending (send %d)", seq, attempt + 2);
    }
}

/* v0.4.15 (P5): strip capture chunks -> {t:"strip_chunk"} events.
 * The host reassembles strips (in order) into the 2592x1944 frame. */
static void on_strip_chunk(uint32_t strip_idx, uint32_t strip_count,
                           uint32_t y0, uint32_t h,
                           uint32_t seq, uint32_t total,
                           const char *b64)
{
    cJSON *o = link_new_event("strip_chunk");
    cJSON_AddNumberToObject(o, "strip_idx", strip_idx);
    cJSON_AddNumberToObject(o, "strip_count", strip_count);
    cJSON_AddNumberToObject(o, "y0", y0);
    cJSON_AddNumberToObject(o, "h", h);
    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddBoolToObject(o, "last",
                          strip_idx == strip_count - 1 && seq == total - 1);
    cJSON_AddStringToObject(o, "b64", b64);
    link_send(o);
}

static void on_net_event(bool connected, const char *ip)
{
    display_set_wifi(connected, ip);
    cJSON *o = link_new_event("wifi");
    cJSON_AddStringToObject(o, "state", connected ? "connected" : "disconnected");
    if (connected && ip) {
        cJSON_AddStringToObject(o, "ip", ip);
    }
    link_send(o);
}

static void on_mode_change(mode_t m)
{
    display_set_mode(mode_name(m));
    bool is_timer = (m == MODE_TIMER);
    display_set_timer_visible(is_timer);
    if (is_timer) {
        char buf[16];
        timer_format(buf, sizeof(buf));
        display_set_timer(buf);
    }
    display_set_keyboard_visible(m == MODE_KEYS);
    display_set_media_visible(m == MODE_MEDIA);
    if (m == MODE_MEDIA) {
        /* Sync the UI with the tracked state (may have changed while in
         * another mode, or on first entry). */
        display_set_media_playing(input_media_playing());
        display_set_media_volume(input_media_volume());
    }
    cJSON *o = link_new_event("mode");
    cJSON_AddStringToObject(o, "mode", mode_name(m));
    link_send(o);
}

static void on_eyes_emit(const char *type, int id, const char *mid,
                         int bytes, int ms, const char *err)
{
    /* Exactly-once terminal replies: if the link layer already
     * force-completed this command via the timeout monitor, drop the
     * late completion instead of emitting a second terminal event. */
    if (!link_pending_done(mid)) {
        ESP_LOGW(TAG, "dropping late snap completion (already timed out)");
        return;
    }
    cJSON *o = link_new_event(type);
    if (mid && mid[0]) {
        cJSON_AddStringToObject(o, "_mid", mid);
    }
    if (strcmp(type, "err") == 0) {
        cJSON_AddStringToObject(o, "cmd", "snap");
        cJSON_AddStringToObject(o, "msg", err ? err : "");
    } else {
        cJSON_AddNumberToObject(o, "id", id);
        cJSON_AddNumberToObject(o, "bytes", bytes);
        cJSON_AddNumberToObject(o, "ms", ms);
    }
    link_send(o);
}

static void on_snaptest_done(int id, const char *mid,
                             const eyes_snaptest_result_t *res)
{
    /* v0.4.35: the stream is over either way — lift the ack gate. */
    link_chunk_ack_disarm();
    /* Exactly-once terminal replies (see on_eyes_emit). */
    if (!link_pending_done(mid)) {
        ESP_LOGW(TAG, "dropping late snaptest completion (already timed out)");
        return;
    }
    cJSON *o = link_new_event("snaptest");
    cJSON_AddNumberToObject(o, "id", id);
    if (mid && mid[0]) {
        cJSON_AddStringToObject(o, "_mid", mid);
    }
    if (res->err) {
        cJSON_AddStringToObject(o, "err", res->err);
    } else {
        cJSON_AddNumberToObject(o, "bytes", res->bytes);
        cJSON_AddNumberToObject(o, "ms", res->ms);
        cJSON_AddBoolToObject(o, "soi", res->soi);
        cJSON_AddBoolToObject(o, "eoi", res->eoi);
        char fnv[11];
        snprintf(fnv, sizeof(fnv), "%08x", (unsigned)res->fnv1a);
        cJSON_AddStringToObject(o, "fnv1a", fnv);
        cJSON_AddNumberToObject(o, "min", res->min_b);
        cJSON_AddNumberToObject(o, "max", res->max_b);
        cJSON_AddNumberToObject(o, "mean", res->mean_b);
        cJSON_AddStringToObject(o, "head", res->head_hex);
        cJSON_AddStringToObject(o, "tail", res->tail_hex);
        cJSON_AddBoolToObject(o, "fresh", res->fresh);
    }
    link_send(o);
}

/* Wake-word detected: auto-start a PTT recording for the user's command.
 * Runs in the wake-word task; ptt_start_external() defers to the PTT task. */
static void on_wakeword_detected(int index)
{
    ESP_LOGI(TAG, "wake-word detected (index %d): auto-starting PTT", index);
    /* v0.4.36: an explicit event too, so the console/MQTT can show the
     * detection (and the count) without parsing log lines. */
    cJSON *o = link_new_event("wakeword");
    if (o) {
        cJSON_AddNumberToObject(o, "index", index);
        cJSON_AddNumberToObject(o, "count",
                                (double)wakeword_detection_count());
        link_send(o);
    }
    ptt_start_external();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
        return;
    }

    /* LED first: the startup rainbow plays while the rest of boot runs.
     * If anything critical fails below, we flag STARTUP_ISSUE (yellow). */
    if (led_init() != ESP_OK) {
        ESP_LOGW(TAG, "LED not ready (continuing without status light)");
    }

    /* Link first: everything else reports through it. */
    if (link_init() != ESP_OK) {
        ESP_LOGE(TAG, "link init failed");
        led_set_state(LED_STATE_STARTUP_ISSUE);
        return;
    }
    link_set_fw_version(FW_VERSION);
    /* v0.4.18.65 (John 2026-10-08): Report PNG crash AFTER link_init so it
     * goes via MQTT telemetry, not just USB serial. The RTC flag survives
     * reboot; we need to see it even if USB wasn't connected. */
    png_crash_detector_report();
    /* v0.4.18.72: Create BLE event queue and task before registering callback.
     * The callback runs in NimBLE host task; it must not do I/O.
     * v0.4.18.75 (NCP-1): Queue carries ble_evt_t (conn/disc/pair events). */
    ble_evt_q_ = xQueueCreate(8, sizeof(ble_evt_t));
    if (ble_evt_q_) {
        xTaskCreate(ble_evt_task, "ble_evt", 4096, NULL, 5, NULL);
    }
    /* NCP-1: Pairing timeout timer (25s, one-shot). */
    {
        const esp_timer_create_args_t args = {
            .callback = pair_timeout_cb,
            .arg = NULL,
            .name = "pair_tmo",
        };
        esp_timer_create(&args, &pair_timer_);
    }
    hid_ble_set_event_cb(on_ble_event);
    hid_ble_set_pair_ui_cb(on_ble_pair_ui);
    net_set_event_cb(on_net_event);
    eyes_set_emit(on_eyes_emit);
    eyes_set_snaptest_cb(on_snaptest_done);
    eyes_set_snaptest_chunk_cb(on_snaptest_chunk);
    eyes_set_png_chunk_cb(on_png_chunk);
    eyes_set_strip_chunk_cb(on_strip_chunk);

    if (net_init() != ESP_OK) {
        ESP_LOGE(TAG, "network init failed (continuing without Wi-Fi)");
    }
    if (hid_ble_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID init failed");
    }
    if (eyes_init() != ESP_OK) {
        ESP_LOGW(TAG, "eyes not ready (continuing without camera)");
    }
    if (display_init() != ESP_OK) {
        ESP_LOGW(TAG, "display not ready (continuing headless)");
    }
    /* Sync BLE state with the display: if a bonded host reconnected
     * before display_init() finished, on_ble_event() already fired but
     * display_set_ble() dropped it (display not ready). Re-sync here so
     * the status screen doesn't stick on "waiting" while connected. */
    display_set_ble(hid_ble_connected());
    /* Input hardware needs the LCD power rail (touch) and the IO expander
     * (wheel-click button), so it comes after eyes + display. A transient
     * init failure no longer kills the pointer permanently: input_init()
     * starts the task with either device up and a background supervisor
     * retries the missing one with backoff. */
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch not ready (continuing without touchpad; retry in background)");
    }
    if (wheel_init() != ESP_OK) {
        ESP_LOGW(TAG, "wheel not ready (continuing without scroll wheel)");
    }
    /* PTT needs I2C0 (eyes) and the wheel button; warn but continue if the
     * mic codec is missing so a board without the codec still boots. */
    if (ptt_init() != ESP_OK) {
        ESP_LOGW(TAG, "PTT not ready (continuing without push-to-talk)");
        led_set_state(LED_STATE_STARTUP_ISSUE);
    }
    /* Timer/mode state before the input task: it reads both on every
     * poll, and starting them first removes any init-ordering question. */
    timer_init();
    mode_init();
    /* Wake-word (v0.3.4): full ESP-SR integration, default OFF (opt-in).
     * On detection the callback auto-starts a PTT recording; the host
     * transcribes it and sends the answer back via {"t":"msg"}. */
    if (wakeword_init() != ESP_OK) {
        ESP_LOGW(TAG, "wake-word not ready (PTT unaffected)");
    } else {
        wakeword_set_callback(on_wakeword_detected);
    }
    if (input_init() != ESP_OK) {
        ESP_LOGW(TAG, "pointer mode deferred; touch/wheel retry in background");
    }
    /* Mode ring: display follows mode changes (no-op until display ready). */
    mode_set_change_cb(on_mode_change);
    display_set_mode(mode_name(mode_get()));

    /* The v0.2.0 boot "ready" event was dropped once by a transient heap
     * squeeze inside link_send (which used to fail silently). Retry a few
     * times so the host always sees the boot; a duplicate ready is harmless
     * (the host resolves its boot wait on the first one). */
    for (int attempt = 1; attempt <= 3; attempt++) {
        cJSON *ready = link_new_event("ready");
        if (ready) {
            cJSON_AddStringToObject(ready, "fw", FW_VERSION);
            cJSON_AddStringToObject(ready, "name", hid_ble_device_name());
            cJSON_AddTrueToObject(ready, "ble");
            cJSON_AddBoolToObject(ready, "eyes", eyes_ready());
            cJSON_AddBoolToObject(ready, "display", display_ready());
            cJSON_AddBoolToObject(ready, "touch", touch_ready());
            cJSON_AddBoolToObject(ready, "wheel", wheel_ready());
            cJSON_AddBoolToObject(ready, "pointer", input_ready());
            cJSON_AddStringToObject(ready, "mode", mode_name(mode_get()));
            cJSON_AddBoolToObject(ready, "keepalive", hid_ble_keepalive_enabled());
            cJSON_AddNumberToObject(ready, "keepalive_interval_s",
                                    hid_ble_keepalive_get_interval());
            if (link_send(ready) == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "ready event send failed (attempt %d/3)", attempt);
        } else {
            ESP_LOGW(TAG, "ready event alloc failed (attempt %d/3)", attempt);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGI(TAG, "watcher input PoC " FW_VERSION " up");
    /* v0.4.5: log the reset reason so telemetry captures WHY we booted.
     * Critical for diagnosing the spontaneous reboot loop — the page
     * does not forward the ROM 'rst:' line. */
    ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());
    /* Boot is done: stop queueing LED transients (drop any still queued)
     * and restore normal preempt behavior. */
    led_boot_complete();
    for (;;) {
        /* v0.4.6: Feed the RTC watchdog every second. John's architecture:
         * watchdogs stay ACTIVE. If this loop hangs, the RTC WDT SHOULD
         * fire (reset reason 7) — that's the safety net working, not a bug.
         * We check in; we don't silence. */
        /* v0.4.6: Feed the RTC WDT (S3 RWDT) via the HAL. The bootloader
         * leaves it armed (CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y);
         * we feed it here so a hung main loop triggers a WDT reset (reason 7).
         * Note: rtc_wdt_feed() is ESP32/S2-only; S3 uses the RWDT HAL. */
        {
            wdt_hal_context_t rwdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
            wdt_hal_write_protect_disable(&rwdt_ctx);
            wdt_hal_feed(&rwdt_ctx);
            wdt_hal_write_protect_enable(&rwdt_ctx);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
