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
#include "esp_log.h"
#include "esp_system.h"
#include "hal/wdt_hal.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "link.h"
#include "net.h"
#include "hid_ble.h"
#include "eyes.h"
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
#define FW_VERSION "0.4.13"

static void on_ble_event(bool connected)
{
    display_set_ble(connected);
    cJSON *o = link_new_event("ble");
    cJSON_AddStringToObject(o, "state", connected ? "connected" : "disconnected");
    link_send(o);
}

static void on_snaptest_chunk(int id, const char *mid, int seq, int total,
                              const char *b64)
{
    cJSON *o = link_new_event("snaptest_chunk");
    cJSON_AddNumberToObject(o, "id", id);
    if (mid && mid[0]) {
        cJSON_AddStringToObject(o, "_mid", mid);
    }
    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddBoolToObject(o, "last", seq == total - 1);
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
    hid_ble_set_event_cb(on_ble_event);
    net_set_event_cb(on_net_event);
    eyes_set_emit(on_eyes_emit);
    eyes_set_snaptest_cb(on_snaptest_done);
    eyes_set_snaptest_chunk_cb(on_snaptest_chunk);

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
