/* JSON line protocol over the USB-serial console (stdin/stdout, 115200).
 *
 * Every line is one JSON object: {"t":"<command>", ...}.
 * Every accepted command gets exactly one terminal reply ("ok" or "err",
 * correlated with "_mid" when the command carried one). Async commands
 * (snap, snaptest) ack at dispatch and complete later; a monitor task
 * force-completes anything overdue so the host never waits forever.
 *
 * Concurrency: rx_task only drains the UART, frames lines, and enqueues
 * parsed commands. link_worker_task executes them. A dedicated monitor
 * task enforces deadlines. TX is serialized by tx_lock_.
 */
#include <stdio.h>
#include <string.h>
/* <sys/types.h> (via unistd.h/fcntl.h) defines the POSIX mode_t, which
 * collides with our mode component's mode_t enum. Rename the POSIX one
 * out of the way; nothing in this file uses it. */
#define mode_t posix_mode_t
#include <unistd.h>
#include <fcntl.h>
#undef mode_t
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/i2c.h"
#include "cJSON.h"
#include "link.h"
#include "hid_ble.h"
#include "eyes.h"
#include "net.h"
/* Phase 2/3 seal: device-side verify, native MQTT, WIA1 sessions. */
#include "seal_dev.h"
#include "mqtt_native.h"
#include "wia1.h"
#include "mbedtls/base64.h"
#include "input.h"
#include "wheel.h"
#include "touch.h"
#include "display.h"
#include "led.h"
#include "esp_io_expander.h"
#include "task_psram.h"

static const char *TAG = "link";
static SemaphoreHandle_t tx_lock_ = NULL;
/* PSRAM task handles (stacks in PSRAM, TCBs internal). link_rx stays on
 * internal RAM — UART drain at priority 9 is timing-critical. */
static task_psram_handle_t *worker_psram_ = NULL;
static task_psram_handle_t *mon_psram_ = NULL;
static task_psram_handle_t *hb_psram_ = NULL;
/* v0.4.6: link_rx runs on PSRAM stack (was 16KB internal). Audit found
 * PSRAM-safe: not ISR-context, no DMA. Priority 9 retained. */
static task_psram_handle_t *rx_psram_ = NULL;
static const char *fw_version_ = "unknown";

/* TX mirror callback, registered by mqtt_native_init(): every serial
 * TX line is offered to native MQTT while native mode is up. Invoked
 * with tx_lock_ held; the callback must be non-blocking and must
 * never call back into link_send. */
static void (*tx_mirror_cb_)(const char *line, size_t len) = NULL;

void link_set_tx_mirror(void (*cb)(const char *line, size_t len))
{
    tx_mirror_cb_ = cb;
}
/* Correlation ID of the command currently being executed by the worker
 * task ("" when the command carried none). Only touched by the worker. */
static char cmd_mid_[24] = {0};

/* ------------------------------------------------------------------ */
/* "AI thinking" LED: purple while set, auto-clears after 60s so a lost */
/* "ai off" can't leave it stuck on. Generation-guarded: the timer is  */
/* re-created on every start with the current generation as its arg,   */
/* so a stale fire can never clear a newer "thinking" session.         */
/* ------------------------------------------------------------------ */
static esp_timer_handle_t ai_timer_ = NULL;
static int ai_gen_ = 0;
static SemaphoreHandle_t ai_lock_ = NULL;

static void ai_timeout_cb(void *arg)
{
    int gen = (int)(intptr_t)arg;
    if (xSemaphoreTake(ai_lock_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    if (gen == ai_gen_) {
        led_clear_state(LED_STATE_AI_THINKING);
        ESP_LOGI(TAG, "ai thinking auto-cleared after 60s");
    }
    xSemaphoreGive(ai_lock_);
}

void link_ai_thinking_start(void)
{
    if (xSemaphoreTake(ai_lock_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    ai_gen_++;
    /* Re-create the single-shot timer carrying this generation, so only
     * this session's fire can clear the LED. */
    if (ai_timer_) {
        esp_timer_stop(ai_timer_);
        esp_timer_delete(ai_timer_);
        ai_timer_ = NULL;
    }
    led_set_state(LED_STATE_AI_THINKING);
    const esp_timer_create_args_t args = {
        .callback = ai_timeout_cb,
        .arg = (void *)(intptr_t)ai_gen_,
        .name = "ai_think",
    };
    if (esp_timer_create(&args, &ai_timer_) == ESP_OK) {
        esp_timer_start_once(ai_timer_, 60 * 1000000ULL);
    }
    xSemaphoreGive(ai_lock_);
}

void link_ai_thinking_stop(void)
{
    if (xSemaphoreTake(ai_lock_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    ai_gen_++;
    if (ai_timer_) {
        esp_timer_stop(ai_timer_);
        esp_timer_delete(ai_timer_);
        ai_timer_ = NULL;
    }
    led_clear_state(LED_STATE_AI_THINKING);
    xSemaphoreGive(ai_lock_);
}

void link_set_fw_version(const char *v)
{
    if (v && *v) {
        fw_version_ = v;
    }
}
/* ------------------------------------------------------------------ */
/* Retained log ring: every ESP_LOG line is kept (in addition to going */
/* to the UART) so the host can pull recent history with {"t":"logs"}. */
/* Cold path: lives in PSRAM.                                         */
/* ------------------------------------------------------------------ */

#define LOG_BUF_SIZE 12288
static char *log_ring_ = NULL;
static size_t log_head_ = 0;
static size_t log_valid_ = 0;
static SemaphoreHandle_t log_lock_ = NULL;
static vprintf_like_t log_orig_vprintf_ = NULL;

static void log_ring_write(const char *data, size_t len)
{
    if (!log_ring_ || !log_lock_) {
        return;
    }
    if (xSemaphoreTake(log_lock_, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        log_ring_[log_head_] = data[i];
        log_head_ = (log_head_ + 1) % LOG_BUF_SIZE;
        if (log_valid_ < LOG_BUF_SIZE) {
            log_valid_++;
        }
    }
    xSemaphoreGive(log_lock_);
}

static int log_capture_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        log_ring_write(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    }
    if (log_orig_vprintf_) {
        return log_orig_vprintf_(fmt, args);
    }
    return n;
}

static void log_capture_init(void)
{
    log_ring_ = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    log_lock_ = xSemaphoreCreateMutex();
    if (log_ring_ && log_lock_) {
        log_orig_vprintf_ = esp_log_set_vprintf(log_capture_vprintf);
        ESP_LOGI(TAG, "log ring ready (%d bytes, PSRAM)", LOG_BUF_SIZE);
    } else {
        ESP_LOGW(TAG, "log ring unavailable");
    }
}

/* ------------------------------------------------------------------ */
/* TX path: every object goes out as one newline-terminated line,     */
/* serialized under tx_lock_ so concurrent tasks can't interleave.    */
/* ------------------------------------------------------------------ */

/* Upsert: replace the key if present, else add. cJSON has no native
 * upsert; delete-then-add keeps exactly one copy. */
static void seal_upsert(cJSON *o, const char *key, cJSON *val)
{
    if (!val) {
        return;
    }
    cJSON_DeleteItemFromObject(o, key);
    cJSON_AddItemToObject(o, key, val);
}

/* Phase 2 seal: heartbeat, ready, and seal_status carry the seal/MQTT
 * arbitration fields. ready is built by main.c (outside this task's
 * touch list), so it is enriched on the way out; heartbeat is built
 * here already, upsert keeps a single copy either way. seal_status is
 * the on-demand diagnostic (dbg/seal_status) so the page need not wait
 * up to 60 s for the next heartbeat after a reload. Runs BEFORE
 * tx_lock_ is taken: seal_dev_floor() takes the verify lock, and the
 * lock order must stay verify -> tx, never nested. */
static void link_enrich_seal_status(cJSON *o)
{
    cJSON *titem = cJSON_GetObjectItem(o, "t");
    const char *t = (cJSON_IsString(titem) && titem->valuestring)
                        ? titem->valuestring : "";
    char sess_hex[17];
    char seclog[192];

    if (strcmp(t, "ready") != 0 && strcmp(t, "heartbeat") != 0 &&
        strcmp(t, "seal_status") != 0) {
        return;
    }
    seal_upsert(o, "mqtt_mode",
                cJSON_CreateString(seal_mqtt_mode()));
    seal_upsert(o, "seal_floor",
                cJSON_CreateNumber((double)seal_dev_floor()));
    seal_upsert(o, "seal_armed",
                cJSON_CreateBool(wia1_is_armed()));
    seal_upsert(o, "seal_dev_verify",
                cJSON_CreateBool(seal_dev_has_commander_key()));
    wia1_session_id_hex(sess_hex);
    seal_upsert(o, "seal_session",
                sess_hex[0] ? cJSON_CreateString(sess_hex)
                            : cJSON_CreateNull());
    /* Named rejection counters; omitted when none recorded. */
    seal_dev_seclog_str(seclog, sizeof(seclog));
    if (seclog[0]) {
        seal_upsert(o, "seal_seclog", cJSON_CreateString(seclog));
    }
}

esp_err_t link_send(cJSON *obj)
{
    if (!obj) {
        ESP_LOGE(TAG, "link_send: NULL object — dropped (never silent)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!tx_lock_) {
        ESP_LOGE(TAG, "link_send: no tx lock — dropped (never silent)");
        cJSON_Delete(obj);
        return ESP_ERR_INVALID_STATE;
    }
    /* Seal-status enrichment for ready/heartbeat (see above). */
    link_enrich_seal_status(obj);
    /* 100ms to serialize: the UART at 115200 drains ~1.4KB in that time,
     * far more than any reply. A wedged holder fails loud, not silent. */
    if (xSemaphoreTake(tx_lock_, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "link_send: tx lock timeout — reply dropped");
        cJSON_Delete(obj);
        return ESP_ERR_TIMEOUT;
    }
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        xSemaphoreGive(tx_lock_);
        ESP_LOGE(TAG, "link_send: serialization failed");
        return ESP_ERR_NO_MEM;
    }
    printf("%s\n", s);
    fflush(stdout);
    /* Native-MQTT TX mirror: offer the line to the broker while native
     * mode is up. Non-blocking by contract; the line buffer is still
     * valid here. */
    if (tx_mirror_cb_) {
        tx_mirror_cb_(s, strlen(s));
    }
    free(s);
    xSemaphoreGive(tx_lock_);
    return ESP_OK;
}

/* Forward declaration: the terminal gate lives in the registry section. */
static bool link_pending_term(const char *mid);

esp_err_t link_respond_ok(const char *cmd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "ok");
    cJSON_AddStringToObject(o, "cmd", cmd ? cmd : "");
    if (cmd_mid_[0]) {
        cJSON_AddStringToObject(o, "_mid", cmd_mid_);
    }
    if (!link_pending_term(cmd_mid_)) {
        /* Monitor already timed this command out: the timeout err was
         * the terminal reply. Drop the late duplicate. */
        cJSON_Delete(o);
        return ESP_OK;
    }
    return link_send(o);
}

esp_err_t link_respond_err(const char *cmd, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "err");
    cJSON_AddStringToObject(o, "cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(o, "msg", msg ? msg : "");
    if (cmd_mid_[0]) {
        cJSON_AddStringToObject(o, "_mid", cmd_mid_);
    }
    if (!link_pending_term(cmd_mid_)) {
        cJSON_Delete(o);
        return ESP_OK;
    }
    return link_send(o);
}

/* Explicit-mid variant for contexts where cmd_mid_ doesn't apply —
 * rx_task admission errors (nothing registered yet) and the monitor's
 * own timeout errs (entry already marked timed_out by the caller).
 * Ungated by design: the caller manages the registry entry itself. */
static esp_err_t link_respond_err_mid(const char *cmd, const char *msg,
                                      const char *mid)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "err");
    cJSON_AddStringToObject(o, "cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(o, "msg", msg ? msg : "");
    if (mid && mid[0]) {
        cJSON_AddStringToObject(o, "_mid", mid);
    }
    return link_send(o);
}

/* dbg response constructor: {"t":"dbg","cmd":"<sub>"} + _mid. */
static cJSON *dbg_new(const char *sub)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "dbg");
    cJSON_AddStringToObject(o, "cmd", sub ? sub : "");
    if (cmd_mid_[0]) {
        cJSON_AddStringToObject(o, "_mid", cmd_mid_);
    }
    return o;
}

/* HID command responder: honest errors. ESP_ERR_NO_MEM means the BLE
 * TX path is overloaded (mbuf pool exhausted), not "not connected" —
 * say so, so the host retries instead of re-pairing. */
static esp_err_t link_respond_hid(const char *cmd, esp_err_t err)
{
    if (err == ESP_OK) {
        return link_respond_ok(cmd);
    }
    if (err == ESP_ERR_NO_MEM) {
        return link_respond_err(cmd, "ble tx overloaded, retry");
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%s", esp_err_to_name(err));
    return link_respond_err(cmd, msg);
}

cJSON *link_new_event(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", type);
    return o;
}
/* ------------------------------------------------------------------ */
/* Pending registry: every accepted command with a _mid gets an entry  */
/* {mid, t, deadline}. The monitor task force-completes overdue       */
/* entries with err "timeout" so the host never waits forever.         */
/*                                                                     */
/* Exactly-once terminal replies: an entry is active until its         */
/* terminal reply is emitted. After the monitor fires the timeout,     */
/* the entry becomes a tombstone: late completions are suppressed,     */
/* and the tombstone is garbage-collected after LINK_TOMBSTONE_MS.     */
/* link_pending_done/term distinguish three cases:                     */
/*   active     -> clear the entry, allow the terminal reply;          */
/*   timed out  -> suppress the late reply (timeout already sent);     */
/*   unknown    -> allow (never registered: no mid, or registry was    */
/*                 full at admission — never silently drop).           */
/*                                                                     */
/* Timeouts: 45s for snap/snaptest (camera + network), 15s for         */
/* everything else. Sync handlers all complete well inside 15s        */
/* (mouse_glide caps at 10s, dbg pins clamps at 10s); a sync command   */
/* that overruns its deadline is wedged, and the timeout at least      */
/* tells the host instead of hanging silently.                        */
/* ------------------------------------------------------------------ */

#define LINK_MAX_PENDING 8
#define LINK_ASYNC_TIMEOUT_MS 45000
#define LINK_SYNC_TIMEOUT_MS 15000
#define LINK_TOMBSTONE_MS 120000
#define LINK_MONITOR_PERIOD_MS 500

typedef struct {
    bool used;
    bool timed_out;        /* deadline fired; terminal err already sent */
    char mid[24];
    char t[24];
    TickType_t deadline;   /* admission tick + timeout */
    TickType_t tombstone_at; /* when timed_out was set (for GC) */
} link_pending_t;

static link_pending_t pending_[LINK_MAX_PENDING];
static SemaphoreHandle_t pending_lock_ = NULL;

/* Register an admitted command. Untrackable without a mid (the host
 * couldn't correlate a timeout err anyway); the command still runs and
 * its replies still go out normally. */
static void link_pending_register(const char *t, const char *mid,
                                  uint32_t timeout_ms)
{
    if (!pending_lock_ || !mid || !mid[0]) {
        return;
    }
    if (xSemaphoreTake(pending_lock_, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "pending registry lock busy; cmd '%s' untracked",
                 t ? t : "?");
        return;
    }
    int slot = -1;
    int oldest_tomb = -1;
    for (int i = 0; i < LINK_MAX_PENDING; i++) {
        if (!pending_[i].used) {
            slot = i;
            break;
        }
        if (pending_[i].timed_out &&
            (oldest_tomb < 0 ||
             (int32_t)(pending_[i].tombstone_at -
                       pending_[oldest_tomb].tombstone_at) < 0)) {
            oldest_tomb = i;
        }
    }
    if (slot < 0) {
        /* Full: evict the oldest tombstone rather than leave the new
         * command untracked; if every slot is live, leave untracked
         * (completions still emitted — never silently drop). */
        slot = oldest_tomb;
    }
    if (slot >= 0) {
        pending_[slot].used = true;
        pending_[slot].timed_out = false;
        strncpy(pending_[slot].t, t ? t : "", sizeof(pending_[slot].t) - 1);
        pending_[slot].t[sizeof(pending_[slot].t) - 1] = '\0';
        strncpy(pending_[slot].mid, mid, sizeof(pending_[slot].mid) - 1);
        pending_[slot].mid[sizeof(pending_[slot].mid) - 1] = '\0';
        pending_[slot].deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    } else {
        ESP_LOGW(TAG, "pending registry full; cmd '%s' untracked",
                 t ? t : "?");
    }
    xSemaphoreGive(pending_lock_);
}

/* Terminal-reply gate: exactly one terminal reply per _mid.
 * Active entry -> clear it, allow the reply. Timed-out entry ->
 * suppress (the monitor already sent the terminal err). No entry ->
 * allow (never registered: no mid, or registry was full at admission). */
static bool link_pending_term(const char *mid)
{
    if (!mid || !mid[0] || !pending_lock_) {
        return true;
    }
    if (xSemaphoreTake(pending_lock_, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* Can't confirm; emit rather than risk swallowing a reply. */
        return true;
    }
    bool allow = true;
    for (int i = 0; i < LINK_MAX_PENDING; i++) {
        if (pending_[i].used && strcmp(pending_[i].mid, mid) == 0) {
            if (pending_[i].timed_out) {
                allow = false; /* timeout already sent the terminal reply */
            } else {
                pending_[i].used = false;
            }
            break;
        }
    }
    xSemaphoreGive(pending_lock_);
    return allow;
}

/* Completion gate for async completions (snap/snaptest callbacks in
 * main.c). Same three cases as link_pending_term. */
bool link_pending_done(const char *mid)
{
    return link_pending_term(mid);
}

/* Send a terminal reply for the current command (cmd_mid_). If the
 * monitor already timed this command out, the late reply is dropped so
 * the host sees exactly one terminal event per _mid. */
static esp_err_t link_send_term(cJSON *o)
{
    if (!link_pending_term(cmd_mid_)) {
        cJSON_Delete(o);
        return ESP_OK;
    }
    return link_send(o);
}

/* Heartbeat task: emits {"t":"heartbeat",...} over serial every 60s.
 * The console page forwards these to <prefix>/heartbeat on MQTT.
 * Includes uptime, heap stats, and fw version for basic telemetry. */
static void link_heartbeat_task(void *arg)
{
    (void)arg;
    /* Wait for boot to settle before the first heartbeat. */
    vTaskDelay(pdMS_TO_TICKS(10000));
    for (;;) {
        cJSON *o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "t", "heartbeat");
            cJSON_AddNumberToObject(o, "uptime_s",
                (double)(esp_timer_get_time() / 1000000));
            cJSON_AddStringToObject(o, "fw", fw_version_);
            cJSON_AddNumberToObject(o, "heap_free",
                heap_caps_get_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(o, "internal_free",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            /* v0.4.3 (R5): largest internal block — free bytes alone
             * hide fragmentation; the DMA allocator needs contiguous. */
            cJSON_AddNumberToObject(o, "internal_largest",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(o, "psram_free",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            cJSON_AddNumberToObject(o, "tasks", uxTaskGetNumberOfTasks());
            /* v0.4.3 (R5): per-task stack high-water marks (bytes) for
             * the internal-stack tasks, so the next RAM pass can trim
             * stacks from measured data instead of guesses. Only tasks
             * currently alive are reported (mqtt_native/snap/snaptest
             * come and go). */
            {
                static const char *const hwm_tasks[] = {
                    "link_rx", "hid_keepalive", "input", "ww_feed",
                    /* v0.4.9: sscma tasks renamed to fit FreeRTOS's
                     * 15-char task-name limit (xTaskGetHandle asserts
                     * strlen < 16; "sscma_client_process"/"sscma_client_monitor"
                     * were 20 chars and panicked at boot). */
                    "nvs_task", "sscma_client",
                    "sscma_monitor", "mqtt_native",
                    "snap", "snaptest",
                };
                cJSON *hwm = cJSON_CreateObject();
                for (size_t i = 0;
                     i < sizeof(hwm_tasks) / sizeof(hwm_tasks[0]); i++) {
                    TaskHandle_t th =
                        xTaskGetHandle(hwm_tasks[i]);
                    if (th) {
                        /* High-water mark is in words; report bytes. */
                        cJSON_AddNumberToObject(hwm, hwm_tasks[i],
                            (double)(uxTaskGetStackHighWaterMark(th) *
                                     sizeof(StackType_t)));
                    }
                }
                cJSON_AddItemToObject(o, "stack_hwm", hwm);
            }
            /* Phase 2/3 seal + MQTT mode for the page's arbitration:
             * seal_dev_verify=true means the device re-verifies every
             * inbound envelope itself (page forwards opaquely); false
             * means the page verifies in grace and dispatches locally. */
            cJSON_AddStringToObject(o, "mqtt_mode", seal_mqtt_mode());
            cJSON_AddNumberToObject(o, "seal_floor",
                                    (double)seal_dev_floor());
            cJSON_AddBoolToObject(o, "seal_armed", wia1_is_armed());
            cJSON_AddBoolToObject(o, "seal_dev_verify",
                                  seal_dev_has_commander_key());
            char sess_hex[17];
            wia1_session_id_hex(sess_hex);
            if (sess_hex[0]) {
                cJSON_AddStringToObject(o, "seal_session", sess_hex);
            } else {
                cJSON_AddNullToObject(o, "seal_session");
            }
            /* Named rejection counters, "CODE=n" pairs; omitted when
             * no rejections have been recorded. */
            char seclog[192];
            seal_dev_seclog_str(seclog, sizeof(seclog));
            if (seclog[0]) {
                cJSON_AddStringToObject(o, "seal_seclog", seclog);
            }
            link_send(o);
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* Monitor task: force-complete overdue commands and garbage-collect
 * old tombstones. Low priority; never holds pending_lock_ while
 * sending (link_send takes tx_lock_ — keep lock ordering flat). */
static void link_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINK_MONITOR_PERIOD_MS));
        if (!pending_lock_) {
            continue;
        }
        if (xSemaphoreTake(pending_lock_, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        bool held = true;
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < LINK_MAX_PENDING && held; i++) {
            if (!pending_[i].used) {
                continue;
            }
            if (pending_[i].timed_out) {
                /* GC tombstones whose late completions never arrived. */
                if ((int32_t)(now - pending_[i].tombstone_at) >=
                    (int32_t)pdMS_TO_TICKS(LINK_TOMBSTONE_MS)) {
                    pending_[i].used = false;
                }
                continue;
            }
            if ((int32_t)(now - pending_[i].deadline) < 0) {
                continue;
            }
            char mid[24], t[24];
            strncpy(mid, pending_[i].mid, sizeof(mid) - 1);
            mid[sizeof(mid) - 1] = '\0';
            strncpy(t, pending_[i].t, sizeof(t) - 1);
            t[sizeof(t) - 1] = '\0';
            pending_[i].timed_out = true;
            pending_[i].tombstone_at = now;
            xSemaphoreGive(pending_lock_);
            ESP_LOGW(TAG, "cmd '%s' timed out; host notified", t);
            /* Ungated send: this IS the terminal reply; the entry is
             * already marked timed_out so a racing completion is
             * suppressed by the gate. */
            link_respond_err_mid(t, "timeout", mid[0] ? mid : NULL);
            if (xSemaphoreTake(pending_lock_, pdMS_TO_TICKS(100)) != pdTRUE) {
                held = false; /* lock not held: skip the give below */
                break;
            }
        }
        if (held) {
            xSemaphoreGive(pending_lock_);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Dispatcher queue: rx_task only frames/parses/enqueues; the worker  */
/* task executes. UART drain latency stays tiny, so slow handlers can  */
/* no longer cause RX-ring overflow drops.                            */
/* ------------------------------------------------------------------ */

#define LINK_CMD_QUEUE_DEPTH 8
static QueueHandle_t cmd_queue_ = NULL;

/* Forward declaration: defined below. */
static void handle_command(cJSON *cmd);

/* JSON arg helpers. */
static const char *opt_str(cJSON *cmd, const char *key)
{
    cJSON *o = cJSON_GetObjectItem(cmd, key);
    return (cJSON_IsString(o) && o->valuestring) ? o->valuestring : NULL;
}

static int opt_int(cJSON *cmd, const char *key, int dflt)
{
    cJSON *o = cJSON_GetObjectItem(cmd, key);
    if (cJSON_IsNumber(o)) {
        return o->valueint;
    }
    return dflt;
}

/* JSON booleans are not numbers in cJSON (opt_int would miss them);
 * accept true/false and 1/0. */
static bool opt_bool(cJSON *cmd, const char *key, bool dflt)
{
    cJSON *o = cJSON_GetObjectItem(cmd, key);
    if (cJSON_IsTrue(o)) {
        return true;
    }
    if (cJSON_IsFalse(o)) {
        return false;
    }
    if (cJSON_IsNumber(o)) {
        return o->valueint != 0;
    }
    return dflt;
}

/* Worker task: executes commands dequeued by rx_task. 8KB stack covers
 * the deepest handlers (i2c scan, at, invoke_noimg all run here).
 * Registration happens at admission — before handle_command runs — so
 * an async command's completion can never race the registry entry. */
static void link_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "link worker ready");
    for (;;) {
        cJSON *cmd = NULL;
        if (xQueueReceive(cmd_queue_, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!cmd) {
            continue;
        }
        const char *t = opt_str(cmd, "t");
        const char *mid = opt_str(cmd, "_mid");
        /* v0.4.3: dbg/snaptest is an async camera operation too. The old
         * check only matched top-level t=snap/snaptest, so the console
         * self-test {"t":"dbg","cmd":"snaptest"} got the 15 s sync
         * deadline while the capture can take ~32 s — the monitor fired
         * early and the terminal reply arrived after the entry was
         * gone. */
        const char *sub = (t && strcmp(t, "dbg") == 0)
                          ? opt_str(cmd, "cmd") : NULL;
        uint32_t timeout = (t && (strcmp(t, "snap") == 0 ||
                                 strcmp(t, "snaptest") == 0 ||
                                 (sub && strcmp(sub, "snaptest") == 0)))
                           ? LINK_ASYNC_TIMEOUT_MS
                           : LINK_SYNC_TIMEOUT_MS;
        link_pending_register(t, mid, timeout);
        handle_command(cmd);
        cJSON_Delete(cmd);
    }
}
/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static void handle_command(cJSON *cmd)
{
    const char *t = opt_str(cmd, "t");
    if (!t) {
        return;
    }

    /* Capture the correlation ID (injected by the console page from the
     * MQTT envelope) so the ack can be matched to this command. */
    const char *mid = opt_str(cmd, "_mid");
    if (mid && mid[0]) {
        strncpy(cmd_mid_, mid, sizeof(cmd_mid_) - 1);
        cmd_mid_[sizeof(cmd_mid_) - 1] = '\0';
    } else {
        cmd_mid_[0] = '\0';
    }

    /* ------------------------------------------------------------------
     * Phase 2/3 seal commands.
     *
     * Commands tagged _via_mqtt arrived over native MQTT (verified by
     * seal_dev, or plain in grace mode). Credential provisioning and
     * seal_grace are USB-serial-console ONLY and are refused for them:
     * seal_grace must never be exposed over MQTT.
     * ------------------------------------------------------------------ */
    bool via_mqtt = cJSON_IsTrue(cJSON_GetObjectItem(cmd, "_via_mqtt"));

    if (strcmp(t, "sealed_fwd") == 0) {
        /* Bridged-mode sealed command: the page forwards the opaque
         * WMS2 envelope over serial; the device re-verifies it against
         * the CLAIMED topic. Binding uses exactly the claimed string,
         * never a local default. */
        const char *topic = opt_str(cmd, "topic");
        const char *b64 = opt_str(cmd, "b64");
        if (!topic || !topic[0] || !b64 || !b64[0]) {
            link_respond_err(t, "missing topic/b64");
            return;
        }
        /* D6: static decode scratch (envelope <= 40+2048+64).
         * v0.4.3 (R2): PSRAM — CPU-only, saves 2152 B internal. */
        static EXT_RAM_BSS_ATTR uint8_t
            fwd_env_[WMS2_HDR_LEN + SEAL_MAX_CMD_BODY + WMS2_SIG_LEN];
        size_t b64len = strlen(b64);
        size_t olen = 0;
        if (b64len > 4096 ||
            mbedtls_base64_decode(fwd_env_, sizeof(fwd_env_), &olen,
                                  (const unsigned char *)b64,
                                  b64len) != 0) {
            link_respond_err(t, "bad b64");
            return;
        }
        const uint8_t *body = NULL;
        size_t body_len = 0;
        wms2_hdr hdr;
        wms2_rc rc = seal_dev_verify_envelope(fwd_env_, olen, topic,
                                              &body, &body_len, &hdr);
        if (rc < 0) {
            /* Named rejection; SECLOG counted inside seal_dev. */
            char msg[48];
            snprintf(msg, sizeof(msg), "seal reject: %s",
                     wms2_rc_str(rc));
            link_respond_err(t, msg);
            return;
        }
        if (rc == WMS2_OK_DUP) {
            /* QoS redelivery of the last command: already executed.
             * Ack the forward without re-dispatching (fleet law: silent). */
            link_pending_term(cmd_mid_);
            link_respond_ok(t);
            return;
        }
        /* Verified body: {"id","cmd"}. Dispatch the inner command with
         * the outer pending entry consumed: the single terminal reply
         * belongs to the inner id, exactly once. */
        /* v0.4.6: PSRAM for JSON buffer (was internal). */
        char *js = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM);
        if (!js) {
            link_respond_err(t, "no mem");
            return;
        }
        memcpy(js, body, body_len);
        js[body_len] = '\0';
        cJSON *outer = cJSON_Parse(js);
        free(js);
        if (!outer) {
            link_respond_err(t, "sealed body not JSON");
            return;
        }
        cJSON *id = cJSON_DetachItemFromObject(outer, "id");
        cJSON *inner = cJSON_DetachItemFromObject(outer, "cmd");
        cJSON_Delete(outer);
        if (!cJSON_IsObject(inner)) {
            cJSON_Delete(inner);
            cJSON_Delete(id);
            link_respond_err(t, "sealed body: no cmd object");
            return;
        }
        if (cJSON_IsString(id) && id->valuestring) {
            cJSON_AddStringToObject(inner, "_mid", id->valuestring);
        } else if (cJSON_IsNumber(id)) {
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "%d", id->valueint);
            cJSON_AddStringToObject(inner, "_mid", tmp);
        }
        cJSON_Delete(id);
        link_pending_term(cmd_mid_);
        handle_command(inner); /* recursion: inner handler sends the reply */
        cJSON_Delete(inner);
        return;
    }
    if (strcmp(t, "seal_cert") == 0) {
        /* WIA1 kind-3 certificate ingest (arm ceremony). Accepted over
         * serial and MQTT alike: arming is gated by the root signature
         * and the physical tap, not by the transport. */
        const char *b64 = opt_str(cmd, "b64");
        if (!b64 || !b64[0]) {
            link_respond_err(t, "missing b64");
            return;
        }
        /* WIA1 envelope: 64 + 135 + 64 = 263 bytes; static scratch. */
        static uint8_t cert_env_[512];
        size_t b64len = strlen(b64);
        size_t olen = 0;
        if (b64len > 1024 ||
            mbedtls_base64_decode(cert_env_, sizeof(cert_env_), &olen,
                                  (const unsigned char *)b64,
                                  b64len) != 0) {
            link_respond_err(t, "bad b64");
            return;
        }
        int arc = wia1_ingest_cert(cert_env_, olen);
        if (arc < 0) {
            char msg[48];
            snprintf(msg, sizeof(msg), "cert reject: %s",
                     wia1_rc_str((wia1_rc)arc));
            link_respond_err(t, msg);
            return;
        }
        cJSON *o = link_new_event("ok");
        cJSON_AddStringToObject(o, "cmd", t);
        cJSON_AddBoolToObject(o, "armed", arc == 1);
        cJSON_AddBoolToObject(o, "pending_tap", wia1_needs_accept_tap());
        if (cmd_mid_[0]) {
            cJSON_AddStringToObject(o, "_mid", cmd_mid_);
        }
        link_send_term(o);
        return;
    }
    /* Serial-console-only provisioning. via_mqtt commands (native MQTT,
     * even sealed ones) are refused: credentials and the commander key
     * are provisioned over the trusted physical channel only. */
    if (strcmp(t, "seal_provision") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *pk = opt_str(cmd, "pk");
        if (!pk) {
            link_respond_err(t, "missing pk");
            return;
        }
        seal_dev_cmd_provision(pk); /* replies itself */
        return;
    }
    if (strcmp(t, "seal_unpin") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        seal_dev_cmd_unpin(); /* replies itself */
        return;
    }
    if (strcmp(t, "seal_strict") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        seal_dev_cmd_strict(opt_bool(cmd, "on", true)); /* replies itself */
        return;
    }
    if (strcmp(t, "seal_tel_seed") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *seed = opt_str(cmd, "seed");
        if (!seed) {
            link_respond_err(t, "missing seed");
            return;
        }
        seal_dev_cmd_tel_seed(seed); /* replies itself */
        return;
    }
    if (strcmp(t, "mqtt_cred") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *user = opt_str(cmd, "user");
        const char *pass = opt_str(cmd, "pass");
        if (!user || !pass) {
            link_respond_err(t, "missing user/pass");
            return;
        }
        if (mqtt_native_cmd_cred(user, pass) == 0) {
            link_respond_ok(t);
        } else {
            link_respond_err(t, "bad user/pass");
        }
        return;
    }
    if (strcmp(t, "mqtt_broker") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *url = opt_str(cmd, "url");
        if (!url) {
            link_respond_err(t, "missing url");
            return;
        }
        if (mqtt_native_cmd_broker(url) == 0) {
            link_respond_ok(t);
        } else {
            link_respond_err(t, "bad url (mqtts://host:port)");
        }
        return;
    }
    if (strcmp(t, "mqtt_topic") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *prefix = opt_str(cmd, "prefix");
        if (!prefix) {
            link_respond_err(t, "missing prefix");
            return;
        }
        if (mqtt_native_cmd_topic(prefix) == 0) {
            link_respond_ok(t);
        } else {
            link_respond_err(t, "bad prefix (1-32 of A-Za-z0-9_-)");
        }
        return;
    }
    if (strcmp(t, "wifi_cred") == 0) {
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *ssid = opt_str(cmd, "ssid");
        const char *pass = opt_str(cmd, "pass");
        if (!ssid) {
            link_respond_err(t, "missing ssid");
            return;
        }
        if (mqtt_native_cmd_wifi(ssid, pass ? pass : "") == 0) {
            link_respond_ok(t);
        } else {
            link_respond_err(t, "wifi setup failed");
        }
        return;
    }
    if (strcmp(t, "set_name") == 0) {
        /* Serial console only: renaming changes the Bluetooth identity
         * other hosts see, so it stays on the trusted physical channel. */
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        const char *name = opt_str(cmd, "name");
        if (!name) {
            link_respond_err(t, "missing name");
            return;
        }
        if (hid_ble_set_device_name(name) != ESP_OK) {
            link_respond_err(t, "bad name (1-32 printable ASCII chars)");
            return;
        }
        cJSON *o = link_new_event("ok");
        cJSON_AddStringToObject(o, "cmd", t);
        cJSON_AddStringToObject(o, "name", hid_ble_device_name());
        if (cmd_mid_[0]) {
            cJSON_AddStringToObject(o, "_mid", cmd_mid_);
        }
        link_send_term(o);
        return;
    }
    if (strcmp(t, "seal_grace") == 0) {
        /* USB-serial console ONLY: grace disables authentication, so it
         * must never be reachable over MQTT. (It isn't: the via_mqtt
         * gate above refuses it.) */
        if (via_mqtt) {
            link_respond_err(t, "serial console only");
            return;
        }
        bool on = opt_bool(cmd, "on", true);
        if (wia1_grace_set(on) != 0) {
            link_respond_err(t, "grace set failed");
            return;
        }
        cJSON *o = link_new_event("ok");
        cJSON_AddStringToObject(o, "cmd", t);
        cJSON_AddBoolToObject(o, "grace", wia1_grace_on());
        if (cmd_mid_[0]) {
            cJSON_AddStringToObject(o, "_mid", cmd_mid_);
        }
        link_send_term(o);
        return;
    }

    if (strcmp(t, "ping") == 0) {
        cJSON *o = link_new_event("pong");
        if (cmd_mid_[0]) {
            cJSON_AddStringToObject(o, "_mid", cmd_mid_);
        }
        link_send_term(o);
        return;
    }
    if (strcmp(t, "type") == 0) {
        const char *text = opt_str(cmd, "text");
        if (!text) {
            link_respond_err(t, "missing text");
            return;
        }
        link_respond_hid(t, hid_ble_type_text(text));
        return;
    }
    if (strcmp(t, "key") == 0) {
        const char *code = opt_str(cmd, "code");
        if (!code) {
            link_respond_err(t, "missing code");
            return;
        }
        link_respond_hid(t, hid_ble_key(code, opt_str(cmd, "mod")));
        return;
    }
    if (strcmp(t, "mouse_move") == 0) {
        int dx = opt_int(cmd, "dx", 0), dy = opt_int(cmd, "dy", 0);
        link_respond_hid(t, hid_ble_mouse_move(dx, dy));
        return;
    }
    if (strcmp(t, "mouse_glide") == 0) {
        // Waypoint form: {"t":"mouse_glide","waypoints":[[dx,dy,dur_ms],...]}
        // Simple form: {"t":"mouse_glide","dx":N,"dy":N,"dur_ms":N}
        // NOTE: Runs synchronously in the link worker task (blocks it, but
        // the RX task keeps draining the UART via the command queue, so no
        // commands are dropped while a glide runs). Abort is unsupported.
        cJSON *wps = cJSON_GetObjectItem(cmd, "waypoints");
        esp_err_t err = ESP_OK;
        if (cJSON_IsArray(wps)) {
            int n = cJSON_GetArraySize(wps);
            if (n < 1 || n > 16) {
                link_respond_err(t, "waypoints: 1..16");
                return;
            }
            /* Pre-validate: total duration must fit the 15s sync deadline
             * (with margin), else the worker would wedge past its timeout. */
            int total_ms = 0;
            for (int i = 0; i < n; i++) {
                cJSON *wp = cJSON_GetArrayItem(wps, i);
                if (!cJSON_IsArray(wp) || cJSON_GetArraySize(wp) < 3) {
                    link_respond_err(t, "waypoint must be [dx,dy,dur_ms]");
                    return;
                }
                total_ms += cJSON_GetArrayItem(wp, 2)->valueint;
            }
            if (total_ms > 12000) {
                link_respond_err(t, "waypoints total dur_ms > 12000");
                return;
            }
            for (int i = 0; i < n && err == ESP_OK; i++) {
                cJSON *wp = cJSON_GetArrayItem(wps, i);
                int dx = cJSON_GetArrayItem(wp, 0)->valueint;
                int dy = cJSON_GetArrayItem(wp, 1)->valueint;
                int dur = cJSON_GetArrayItem(wp, 2)->valueint;
                err = hid_ble_mouse_glide(dx, dy, dur);
            }
        } else {
            int dx = opt_int(cmd, "dx", 0);
            int dy = opt_int(cmd, "dy", 0);
            int dur = opt_int(cmd, "dur_ms", 1000);
            err = hid_ble_mouse_glide(dx, dy, dur);
        }
        link_respond_hid(t, err);
        return;
    }
    if (strcmp(t, "mouse_abort") == 0) {
        link_respond_err(t,
            "glide is synchronous; abort not supported in this build");
        return;
    }
    if (strcmp(t, "mouse_click") == 0) {
        const char *button = opt_str(cmd, "button");
        link_respond_hid(t, hid_ble_mouse_click(button ? button : "left"));
        return;
    }
    if (strcmp(t, "mouse_dblclick") == 0) {
        const char *button = opt_str(cmd, "button");
        link_respond_hid(t,
                         hid_ble_mouse_dblclick(button ? button : "left"));
        return;
    }
    if (strcmp(t, "scroll") == 0) {
        int delta = opt_int(cmd, "delta", 0);
        link_respond_hid(t, hid_ble_mouse_wheel(delta));
        return;
    }
    if (strcmp(t, "consumer") == 0) {
        const char *code = opt_str(cmd, "code");
        if (!code) {
            link_respond_err(t, "missing code");
            return;
        }
        link_respond_hid(t, hid_ble_consumer(code));
        return;
    }
    if (strcmp(t, "hid_status") == 0) {
        cJSON *o = link_new_event("hid_status");
        cJSON_AddBoolToObject(o, "connected", hid_ble_connected());
        cJSON_AddBoolToObject(o, "advertising", hid_ble_advertising());
        cJSON_AddBoolToObject(o, "keepalive", hid_ble_keepalive_enabled());
        cJSON_AddNumberToObject(o, "keepalive_interval_s",
                               hid_ble_keepalive_get_interval());
        cJSON_AddBoolToObject(o, "pointer", input_pointer_enabled());
        if (cmd_mid_[0]) {
            cJSON_AddStringToObject(o, "_mid", cmd_mid_);
        }
        link_send_term(o);
        return;
    }
    if (strcmp(t, "keepalive") == 0) {
        /* {"t":"keepalive","interval_s":N} — 0 disables. Legacy
         * {"t":"keepalive","on":1|0} maps to 60/0. */
        if (cJSON_HasObjectItem(cmd, "interval_s")) {
            hid_ble_keepalive_set_interval(opt_int(cmd, "interval_s", 60));
        } else {
            bool on = opt_int(cmd, "on", 1) != 0;
            hid_ble_keepalive_set_interval(on ? 60 : 0);
        }
        link_respond_ok(t);
        return;
    }
    if (strcmp(t, "pointer") == 0) {
        bool on = opt_int(cmd, "on", 1) != 0;
        input_set_pointer_enabled(on);
        link_respond_ok(t);
        return;
    }
    if (strcmp(t, "wifi") == 0) {
        const char *ssid = opt_str(cmd, "ssid");
        const char *pass = opt_str(cmd, "pass");
        if (!ssid) {
            link_respond_err(t, "missing ssid");
            return;
        }
        esp_err_t err = net_set_wifi(ssid, pass ? pass : "");
        if (err == ESP_OK) {
            link_respond_ok(t);
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "wifi setup failed: %s",
                     esp_err_to_name(err));
            link_respond_err(t, msg);
        }
        return;
    }
    if (strcmp(t, "ble") == 0) {
        /* {"t":"ble","advertise":1} starts advertising, 0 stops it. */
        bool start = opt_int(cmd, "advertise", 1) != 0;
        esp_err_t err = hid_ble_advertise(start);
        if (err == ESP_OK) {
            link_respond_ok(t);
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "advertise failed: %s",
                     esp_err_to_name(err));
            link_respond_err(t, msg);
        }
        return;
    }
    if (strcmp(t, "unbond") == 0) {
        esp_err_t err = hid_ble_unbond();
        if (err == ESP_OK) {
            link_respond_ok(t);
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "unbond failed: %s",
                     esp_err_to_name(err));
            link_respond_err(t, msg);
        }
        return;
    }
    if (strcmp(t, "snap") == 0) {
        const char *host = opt_str(cmd, "host");
        int port = opt_int(cmd, "port", 8099);
        int id = opt_int(cmd, "id", 0);
        /* fresh=0 returns the buffered ("old") frame without a new capture;
         * default 1 takes a new image. */
        bool fresh = opt_int(cmd, "fresh", 1) != 0;
        if (!host) {
            link_respond_err(t, "missing host");
            return;
        }
        if (!net_is_up()) {
            link_respond_err(t, "wifi not connected");
            return;
        }
        /* Registered at admission (worker task), before the camera task
         * exists — no registration race. The start ack below is
         * non-terminal (raw link_send): the still_sent/err completion
         * arriving later from the snap task is the terminal reply. */
        esp_err_t err = eyes_snap(host, port, id, fresh,
                                  cmd_mid_[0] ? cmd_mid_ : NULL);
        if (err == ESP_OK) {
            cJSON *o = link_new_event("ok");
            cJSON_AddStringToObject(o, "cmd", t);
            cJSON_AddNumberToObject(o, "id", id);
            cJSON_AddBoolToObject(o, "fresh", fresh);
            if (cmd_mid_[0]) {
                cJSON_AddStringToObject(o, "_mid", cmd_mid_);
            }
            link_send(o);
        } else if (err == ESP_ERR_INVALID_STATE) {
            link_respond_err(t, "snapshot already in progress");
        } else {
            link_respond_err(t, "camera not ready");
        }
        return;
    }
    if (strcmp(t, "logs") == 0) {
        int n = opt_int(cmd, "n", 40);
        if (n < 1) {
            n = 1;
        }
        if (n > 200) {
            n = 200;
        }
        /* Copy the ring under lock, then release before doing any
         * JSON/link work (avoids lock-order issues with tx_lock_). */
        char *snap = heap_caps_malloc(LOG_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
        if (!snap || log_ring_ == NULL) {
            free(snap);
            link_respond_err(t, "no mem");
            return;
        }
        size_t valid, start;
        xSemaphoreTake(log_lock_, portMAX_DELAY);
        valid = log_valid_;
        start = (log_head_ + LOG_BUF_SIZE - valid) % LOG_BUF_SIZE;
        for (size_t i = 0; i < valid; i++) {
            snap[i] = log_ring_[(start + i) % LOG_BUF_SIZE];
        }
        xSemaphoreGive(log_lock_);
        snap[valid] = '\0';

        /* Find the start of the last n lines. */
        size_t s = valid;
        int lines = 0;
        while (s > 0 && lines < n) {
            s--;
            if (snap[s] == '\n') {
                lines++;
            }
        }
        if (s < valid && snap[s] == '\n') {
            s++;
        }
        cJSON *o = link_new_event("logs");
        cJSON *arr = cJSON_CreateArray();
        size_t ls = s;
        for (size_t i = s; i <= valid; i++) {
            if (i == valid || snap[i] == '\n') {
                size_t e = i;
                while (e > ls && (snap[e - 1] == '\r' || snap[e - 1] == '\n')) {
                    e--;
                }
                snap[e] = '\0';
                if (e > ls) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(snap + ls));
                }
                ls = i + 1;
            }
        }
        cJSON_AddItemToObject(o, "lines", arr);
        if (cmd_mid_[0]) {
            cJSON_AddStringToObject(o, "_mid", cmd_mid_);
        }
        free(snap);
        link_send_term(o);
        return;
    }
    if (strcmp(t, "dbg") == 0) {
        const char *sub = opt_str(cmd, "cmd");
        if (!sub) {
            link_respond_err(t, "missing cmd");
            return;
        }
        if (strcmp(sub, "version") == 0) {
            cJSON *o = dbg_new("version");
            cJSON_AddStringToObject(o, "fw", fw_version_);
            cJSON_AddStringToObject(o, "idf", esp_get_idf_version());
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "seal_status") == 0) {
            /* v0.4.3: on-demand seal posture. The page's provisioning
             * pills sit at UNKNOWN for up to 60 s after a reload +
             * serial connect because seal state only ships on
             * heartbeat/ready; this brings the page's (dead)
             * t:"seal_status" handler to life. Top-level t (not
             * t:"dbg") so the page's existing handler matches.
             * Read-only, no hardware effects — safe over MQTT. */
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "t", "seal_status");
            if (cmd_mid_[0]) {
                cJSON_AddStringToObject(o, "_mid", cmd_mid_);
            }
            /* link_send_term -> link_send runs the seal enricher,
             * which adds mqtt_mode/seal_floor/seal_armed/
             * seal_dev_verify/seal_session/seal_seclog. */
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "heap") == 0) {
            cJSON *o = dbg_new("heap");
            cJSON_AddNumberToObject(o, "free",
                heap_caps_get_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(o, "internal_free",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(o, "psram_free",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            cJSON_AddNumberToObject(o, "largest_internal",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(o, "min_free",
                heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "mem") == 0) {
            /* Alias of heap (the console's memory button uses "mem"). */
            cJSON *o = dbg_new("mem");
            cJSON_AddNumberToObject(o, "free",
                heap_caps_get_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(o, "internal_free",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(o, "psram_free",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            cJSON_AddNumberToObject(o, "largest_internal",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "tasks") == 0) {
            cJSON *o = dbg_new("tasks");
            cJSON_AddNumberToObject(o, "count", uxTaskGetNumberOfTasks());
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "pwr") == 0) {
            /* Dump all 16 IO-expander pin levels (pins 8-12/14/15 are the
             * power rails: SDCARD/LCD/SYSTEM/AI_CHIP/CODEC_PA/GROVE/BAT). */
            esp_io_expander_handle_t exp = eyes_io_expander();
            if (!exp) {
                link_respond_err(t, "no expander");
                return;
            }
            uint32_t val = 0;
            cJSON *o = dbg_new("pwr");
            if (esp_io_expander_get_level(exp, 0xFFFFu, &val) == ESP_OK) {
                cJSON_AddNumberToObject(o, "levels", val);
            } else {
                cJSON_AddStringToObject(o, "err", "read failed");
            }
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "pins") == 0) {
            /* Wheel encoder pin probe: sample GPIO41/42 for a few seconds
             * while the wheel is turned. Reports transitions per pin plus
             * the knob driver's own count, so we can tell a dead encoder
             * from a deaf driver. {"t":"dbg","cmd":"pins","secs":5}
             * The first reply is an explicit non-terminal progress event
             * (phase=start); the second (phase=done) is the terminal reply. */
            int secs = opt_int(cmd, "secs", 5);
            if (secs < 1) {
                secs = 1;
            }
            if (secs > 10) {
                secs = 10; /* keep well inside the 15s sync deadline */
            }
            cJSON *ack = dbg_new("pins");
            cJSON_AddStringToObject(ack, "phase", "start");
            cJSON_AddStringToObject(ack, "msg", "sampling, turn the wheel");
            link_send(ack);
            int pa, pb;
            wheel_gpio_levels(&pa, &pb);
            int trans_a = 0, trans_b = 0;
            for (int i = 0; i < secs * 20; i++) {
                vTaskDelay(pdMS_TO_TICKS(50));
                int a, b;
                wheel_gpio_levels(&a, &b);
                if (a != pa) {
                    trans_a++;
                    pa = a;
                }
                if (b != pb) {
                    trans_b++;
                    pb = b;
                }
            }
            cJSON *o = dbg_new("pins");
            cJSON_AddStringToObject(o, "phase", "done");
            cJSON_AddNumberToObject(o, "secs", secs);
            cJSON_AddNumberToObject(o, "trans_a", trans_a);
            cJSON_AddNumberToObject(o, "trans_b", trans_b);
            cJSON_AddNumberToObject(o, "gpio_a", pa);
            cJSON_AddNumberToObject(o, "gpio_b", pb);
            cJSON_AddNumberToObject(o, "knob_count", wheel_count());
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "trace") == 0) {
            /* {"t":"dbg","cmd":"trace","on":1} streams input-task events
             * (gesture transitions, wheel detents, HID calls) to the log. */
            if (cJSON_HasObjectItem(cmd, "on")) {
                input_set_trace(opt_int(cmd, "on", 1) != 0);
            }
            cJSON *o = dbg_new("trace");
            cJSON_AddBoolToObject(o, "on", input_trace());
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "touch") == 0) {
            uint16_t x = 0, y = 0, strength = 0;
            int n = touch_read(&x, &y, &strength);
            cJSON *o = dbg_new("touch");
            cJSON_AddBoolToObject(o, "ready", touch_ready());
            cJSON_AddNumberToObject(o, "touches", n);
            cJSON_AddNumberToObject(o, "x", x);
            cJSON_AddNumberToObject(o, "y", y);
            cJSON_AddNumberToObject(o, "strength", strength);
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "wheel") == 0) {
            cJSON *o = dbg_new("wheel");
            cJSON_AddBoolToObject(o, "ready", wheel_ready());
            cJSON_AddNumberToObject(o, "count", wheel_count());
            cJSON_AddNumberToObject(o, "pending", wheel_pending());
            cJSON_AddBoolToObject(o, "button_down", wheel_button_down());
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "pointer") == 0) {
            if (cJSON_HasObjectItem(cmd, "on")) {
                input_set_pointer_enabled(opt_int(cmd, "on", 1) != 0);
            }
            cJSON *o = dbg_new("pointer");
            cJSON_AddBoolToObject(o, "on", input_pointer_enabled());
            cJSON_AddStringToObject(o, "gesture",
                                    input_gesture_state_name());
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "reboot") == 0) {
            cJSON *o = dbg_new("reboot");
            cJSON_AddStringToObject(o, "msg", "restarting");
            link_send_term(o);
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
            return; /* not reached */
        }
        if (strcmp(sub, "i2c") == 0) {
            /* Scan I2C0 (IO expander bus) for responding addresses. */
            cJSON *o = dbg_new("i2c");
            cJSON *arr = cJSON_CreateArray();
            for (int addr = 0x08; addr < 0x78; addr++) {
                i2c_cmd_handle_t ch = i2c_cmd_link_create();
                i2c_master_start(ch);
                i2c_master_write_byte(ch, (addr << 1) | I2C_MASTER_WRITE,
                                     true);
                i2c_master_stop(ch);
                esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, ch,
                    pdMS_TO_TICKS(20));
                i2c_cmd_link_delete(ch);
                if (err == ESP_OK) {
                    cJSON_AddItemToArray(arr, cJSON_CreateNumber(addr));
                }
            }
            cJSON_AddItemToObject(o, "found", arr);
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "fill") == 0) {
            /* LCD fill test: {"t":"dbg","cmd":"fill","color":12345} */
            int color = opt_int(cmd, "color", 0xF800);
            esp_err_t err = display_fill((uint16_t)color);
            if (err == ESP_OK) {
                link_respond_ok(t);
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "fill failed: %s",
                         esp_err_to_name(err));
                link_respond_err(t, msg);
            }
            return;
        }
        if (strcmp(sub, "bl") == 0) {
            /* Backlight: {"t":"dbg","cmd":"bl","pct":80} */
            int pct = opt_int(cmd, "pct", 100);
            if (pct < 0) {
                pct = 0;
            }
            if (pct > 100) {
                pct = 100;
            }
            esp_err_t err = display_set_backlight(pct);
            if (err == ESP_OK) {
                link_respond_ok(t);
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "backlight failed: %s",
                         esp_err_to_name(err));
                link_respond_err(t, msg);
            }
            return;
        }
        if (strcmp(sub, "spitest") == 0) {
            /* Full-frame LCD write test (expected to fail: 340KB DMA can
             * never fit the S3's internal RAM — documents the limit). */
            esp_err_t err = display_test_fullframe();
            cJSON *o = dbg_new("spitest");
            cJSON_AddStringToObject(o, "result", esp_err_to_name(err));
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "eyes") == 0) {
            cJSON *o = dbg_new("eyes");
            eyes_status(o);
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "eyes_reset") == 0) {
            /* v0.4.3: moved from top-level {"t":"eyes_reset"} for
             * consistency with dbg/reboot. Himax rail power-cycle + RST
             * + re-handshake. Serial console only: this yanks the camera
             * power rail, which a remote caller must never do
             * unattended. {"t":"dbg","cmd":"eyes_reset"} or
             * {"t":"dbg","cmd":"eyes_reset","rail":0} (warm-only). */
            if (via_mqtt) {
                link_respond_err(t, "serial console only");
                return;
            }
            if (eyes_snap_busy()) {
                link_respond_err(t, "snapshot in progress");
                return;
            }
            bool rail = opt_int(cmd, "rail", 1) != 0;
            esp_err_t rerr = eyes_recover_camera(rail);
            if (rerr == ESP_OK) {
                link_respond_ok(t);
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "recovery failed: %s",
                         esp_err_to_name(rerr));
                link_respond_err(t, msg);
            }
            return;
        }
        if (strcmp(sub, "nvs_stress") == 0) {
            /* v0.4.7: NVS task FIFO reliability stress test. Serial
             * console only: hammers NVS with concurrent PSRAM-stack
             * workers (~1s). Verifies ordering, no stale reads, zero
             * drops, FIFO serialization. */
            if (via_mqtt) {
                link_respond_err(t, "serial console only");
                return;
            }
            int r = seal_dev_nvs_stress_test();
            if (r == 0) {
                link_respond_ok(t);
            } else {
                link_respond_err(t, "stress test FAILED (see log)");
            }
            return;
        }
        if (strcmp(sub, "at") == 0) {
            /* Raw AT passthrough to the Himax.
             * Dangerous verbs (reset/ota/erase/invoke/...) are blocklisted below.
             * Commands containing '=' are allowed — the blocklist is the safety net. */
            const char *at = opt_str(cmd, "at");
            if (!at || !*at) {
                link_respond_err(t, "missing at");
                return;
            }
            static const char *blocked[] = {
                "RESET", "REBOOT", "OTA", "UPGRADE", "ERASE", "FORMAT",
                "INVOKE", "YMODEM", "BOOT", NULL
            };
            char up[64];
            size_t ul = strlen(at);
            if (ul >= sizeof(up)) {
                ul = sizeof(up) - 1;
            }
            for (size_t i = 0; i < ul; i++) {
                char c = at[i];
                up[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
            }
            up[ul] = '\0';
            for (int b = 0; blocked[b]; b++) {
                size_t bl = strlen(blocked[b]);
                for (size_t j = 0; j + bl <= ul; j++) {
                    size_t k = 0;
                    while (k < bl && up[j + k] == blocked[b][k]) {
                        k++;
                    }
                    if (k == bl) {
                        link_respond_err(t, "blocked AT verb");
                        return;
                    }
                }
            }
            char out[512];
            esp_err_t err = eyes_at(at, out, sizeof(out));
            if (err == ESP_OK) {
                cJSON *o = dbg_new("at");
                cJSON_AddStringToObject(o, "reply", out);
                link_send_term(o);
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "at failed: %s",
                         esp_err_to_name(err));
                link_respond_err(t, msg);
            }
            return;
        }
        if (strcmp(sub, "ai") == 0) {
            /* {"t":"dbg","cmd":"ai","on":1} — purple "AI thinking" LED. */
            bool on = opt_int(cmd, "on", 1) != 0;
            if (on) {
                link_ai_thinking_start();
            } else {
                link_ai_thinking_stop();
            }
            link_respond_ok(t);
            return;
        }
        if (strcmp(sub, "snaptest") == 0) {
            /* Async snapshot self-test: analyze a frame on-device, no
             * Wi-Fi needed. Completion arrives later via the snaptest
             * callback (terminal reply); the ack here is non-terminal. */
            int id = opt_int(cmd, "id", 0);
            bool send = opt_int(cmd, "send", 0) != 0;
            bool fresh = opt_int(cmd, "fresh", 1) != 0;
            esp_err_t err = eyes_snap_test_async(id, send, fresh,
                                                 cmd_mid_[0] ? cmd_mid_ : NULL);
            if (err == ESP_OK) {
                cJSON *o = dbg_new("snaptest");
                cJSON_AddStringToObject(o, "phase", "start");
                cJSON_AddNumberToObject(o, "id", id);
                link_send(o);
            } else if (err == ESP_ERR_INVALID_STATE) {
                link_respond_err(t, "snapshot already in progress");
            } else {
                link_respond_err(t, "camera not ready");
            }
            return;
        }
        link_respond_err(t, "unknown dbg cmd");
        return;
    }

    link_respond_err(t, "unknown command");
}

/* Parse a JSON command line and enqueue it on the worker path, exactly
 * as if it arrived over the USB-serial console. Used for verified
 * native-MQTT commands (see mqtt_native.c). The command is tagged
 * _via_mqtt — forced on, replacing any host-supplied value, so a
 * sealed command can never claim serial origin and serial-only
 * provisioning commands are refused for it. */
void link_sealed_dispatch(const char *json, size_t len)
{
    if (!json || len == 0 || !cmd_queue_) {
        return;
    }
    /* v0.4.6: PSRAM for command copy (was internal). */
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!copy) {
        ESP_LOGE(TAG, "sealed_dispatch: out of memory");
        return;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';
    cJSON *cmd = cJSON_Parse(copy);
    free(copy);
    if (!cmd) {
        ESP_LOGW(TAG, "sealed_dispatch: unparseable JSON");
        return;
    }
    const char *t = opt_str(cmd, "t");
    if (!t) {
        ESP_LOGW(TAG, "sealed_dispatch: missing t");
        cJSON_Delete(cmd);
        return;
    }
    cJSON_DeleteItemFromObject(cmd, "_via_mqtt");
    cJSON_AddTrueToObject(cmd, "_via_mqtt");
    if (xQueueSend(cmd_queue_, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "sealed_dispatch: worker busy, dropped '%s'", t);
        link_respond_err_mid(t, "busy", opt_str(cmd, "_mid"));
        cJSON_Delete(cmd);
        return;
    }
    /* Ownership transferred to the worker task. */
}
/* ------------------------------------------------------------------ */
/* RX ring buffer: line assembly between the non-blocking UART drain  */
/* and the line extractor.                                            */
/*                                                                     */
/* - Power-of-two size, separate reader/writer indices, single writer */
/*   (drain side) / single reader (line extractor): no locks needed.  */
/*   Both currently live in rx_task; the indices are volatile with    */
/*   the standard SPSC ordering — data is written before the write    */
/*   index advances, data is read before the read index advances — so  */
/*   the design stays correct if drain/extract are ever split across  */
/*   tasks. (On Xtensa a 16-bit aligned store is single-copy atomic.) */
/* - Lives in internal DRAM (.bss): small and hot, every inbound byte */
/*   passes through it; PSRAM latency has no business here. (The 12KB */
/*   log ring correctly stays in PSRAM — cold path.)                  */
/* - Buffer-full (line longer than the extractor can hold) is a       */
/*   protocol error, not silent corruption: emit err "line too long"  */
/*   and resync to the next newline.                                 */
/* ------------------------------------------------------------------ */

#define RX_RING_BITS 12
#define RX_RING_SIZE (1u << RX_RING_BITS)
#define RX_RING_MASK (RX_RING_SIZE - 1)
/* Max assembled line, including the NUL terminator. */
#define RX_LINE_MAX 4096

static uint8_t rx_ring_[RX_RING_SIZE];      /* .bss -> internal DRAM */
static volatile uint16_t rx_wr_ = 0;        /* next write pos (drain) */
static volatile uint16_t rx_rd_ = 0;        /* next read pos (extractor) */

/* Drain all currently-available bytes from stdin into the ring.
 * Non-blocking: returns as soon as the UART driver's RX ring is empty.
 * Keeps the driver's small ring from ever overflowing during bursts. */
static void rx_drain(void)
{
    for (;;) {
        uint16_t wr = rx_wr_;
        uint16_t used = (uint16_t)(wr - rx_rd_);
        uint16_t free = (uint16_t)(RX_RING_SIZE - used);
        if (free == 0) {
            break; /* extractor will make room; never spin here */
        }
        /* One read never wraps: clamp to the contiguous free run. */
        uint16_t wpos = (uint16_t)(wr & RX_RING_MASK);
        uint16_t chunk = (uint16_t)(RX_RING_SIZE - wpos);
        if (chunk > free) {
            chunk = free;
        }
        if (chunk > 256) {
            chunk = 256; /* bound a single read; loop for the rest */
        }
        ssize_t n = read(STDIN_FILENO, &rx_ring_[wpos], chunk);
        if (n <= 0) {
            break; /* EAGAIN (no data) or error: nothing more to drain */
        }
        /* Publish the bytes only after they are written. */
        rx_wr_ = (uint16_t)(wr + n);
    }
}

/* Extract the next complete line from the ring.
 * Returns 1 and NUL-terminates out (CR/LF stripped) when a full line is
 * available; 0 when the line is incomplete (wait for more bytes); -1
 * when the line exceeds the buffer — the ring is resynced to just past
 * the next newline (or emptied) and the caller must emit "line too long".
 * An embedded NUL truncates the extracted C string at that point (the
 * JSON parse then rejects the line with an acked error — never silent). */
static int rx_getline(char *out, size_t cap)
{
    uint16_t wr = rx_wr_;
    uint16_t rd = rx_rd_;
    uint16_t rpos = (uint16_t)(rd & RX_RING_MASK);
    uint16_t avail = (uint16_t)(wr - rd);

    /* Scan at most cap bytes for the newline: a line whose content needs
     * more than cap-1 chars can never fit in out (cap-1 chars + NUL). */
    uint16_t scan = avail < cap ? avail : (uint16_t)cap;
    uint16_t i;
    for (i = 0; i < scan; i++) {
        if (rx_ring_[(uint16_t)((rpos + i) & RX_RING_MASK)] == '\n') {
            break;
        }
    }
    if (i == scan) {
        if (avail >= cap) {
            /* Definitely overlong: discard through the next newline (or
             * everything if none arrived yet) and report it. */
            uint16_t j;
            for (j = 0; j < avail; j++) {
                if (rx_ring_[(uint16_t)((rpos + j) & RX_RING_MASK)] == '\n') {
                    j++; /* consume the newline too */
                    break;
                }
            }
            rx_rd_ = (uint16_t)(rd + j);
            return -1;
        }
        return 0; /* incomplete line: wait for more bytes */
    }
    /* Newline at offset i: copy i bytes, strip one trailing CR. */
    uint16_t len = i;
    if (len > 0 && rx_ring_[(uint16_t)((rpos + len - 1) & RX_RING_MASK)] == '\r') {
        len--;
    }
    for (uint16_t k = 0; k < len; k++) {
        uint8_t b = rx_ring_[(uint16_t)((rpos + k) & RX_RING_MASK)];
        if (b == '\0') {
            len = k; /* embedded NUL truncates the C string */
            break;
        }
        out[k] = (char)b;
    }
    out[len] = '\0';
    rx_rd_ = (uint16_t)(rd + i + 1); /* consume through the newline */
    return 1;
}

/* RX task: non-blocking drain -> extract complete lines -> validate ->
 * enqueue. Never executes a command (the worker task does that) and
 * never blocks waiting for a newline, so UART drain latency stays tiny
 * and the driver's RX ring can't overflow. Every admission failure
 * produces a correlated err, never silence. */
static void rx_task(void *arg)
{
    (void)arg;
    /* Non-blocking stdin: the drain side must never wait for bytes. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    static char line[RX_LINE_MAX]; /* .bss, not task stack */
    ESP_LOGI(TAG, "link ready on USB-serial console");
    for (;;) {
        rx_drain();
        bool busy = false;
        int r;
        while ((r = rx_getline(line, sizeof(line))) != 0) {
            busy = true;
            if (r < 0) {
                /* Can't recover the _mid of an overlong line: best-effort. */
                link_respond_err_mid("", "line too long (max 4095 chars)",
                                     NULL);
                continue;
            }
            if (line[0] == '\0') {
                continue; /* blank line */
            }
            cJSON *cmd = cJSON_Parse(line);
            if (!cmd) {
                /* Not JSON (e.g. pasted log line): say so instead of going
                 * silent, so the host can tell noise apart from drops. */
                link_respond_err_mid("", "unparseable line", NULL);
                continue;
            }
            const char *t = opt_str(cmd, "t");
            const char *mid = opt_str(cmd, "_mid");
            if (!t) {
                link_respond_err_mid("", "missing t", mid);
                cJSON_Delete(cmd);
                continue;
            }
            /* Hand off to the worker; if it's backed up, fail fast with a
             * correlated "busy" instead of dropping the command silently. */
            if (xQueueSend(cmd_queue_, &cmd, 0) != pdPASS) {
                link_respond_err_mid(t, "busy", mid);
                cJSON_Delete(cmd);
                continue;
            }
            /* Ownership of cmd transferred to the worker task. */
        }
        if (!busy) {
            /* Idle: brief nap. A burst is caught on the next loop with
             * ~no added latency (the loop re-drains immediately while
             * bytes keep arriving). At 115200 baud ~23 bytes arrive per
             * 2ms — far below the UART driver's RX ring. */
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

esp_err_t link_init(void)
{
    tx_lock_ = xSemaphoreCreateMutex();
    if (!tx_lock_) {
        return ESP_ERR_NO_MEM;
    }
    ai_lock_ = xSemaphoreCreateMutex();
    if (!ai_lock_) {
        return ESP_ERR_NO_MEM;
    }
    pending_lock_ = xSemaphoreCreateMutex();
    if (!pending_lock_) {
        return ESP_ERR_NO_MEM;
    }
    cmd_queue_ = xQueueCreate(LINK_CMD_QUEUE_DEPTH, sizeof(cJSON *));
    if (!cmd_queue_) {
        return ESP_ERR_NO_MEM;
    }
    /* Retain ESP_LOG history for the {"t":"logs"} debug command. */
    log_capture_init();
    /* Worker executes commands; RX only frames/parses/enqueues so the
     * UART never overflows while a slow handler runs. RX runs one notch
     * above the worker so drain latency stays minimal. The monitor
     * enforces command deadlines at low priority. The AI-thinking LED
     * timer is created on first use.
     *
     * RAM: worker/mon/hb stacks live in PSRAM (they do no flash ops and
     * are not timing-critical). link_rx stays internal — UART drain at
     * priority 9 cannot tolerate PSRAM cache-miss jitter. */
    worker_psram_ = task_psram_create(link_worker_task, "link_worker", 4096,
                                      NULL, 8, tskNO_AFFINITY);
    if (!worker_psram_) {
        return ESP_FAIL;
    }
    /* v0.4.6: PSRAM stack, 4096 words = 16384 bytes (was 16KB internal). */
    rx_psram_ = task_psram_create(rx_task, "link_rx", 4096 * 4, NULL, 9,
                                 tskNO_AFFINITY);
    if (!rx_psram_) {
        return ESP_FAIL;
    }
    mon_psram_ = task_psram_create(link_monitor_task, "link_mon", 2048,
                                   NULL, 3, tskNO_AFFINITY);
    if (!mon_psram_) {
        return ESP_FAIL;
    }
    hb_psram_ = task_psram_create(link_heartbeat_task, "link_hb", 4096,
                                  NULL, 2, tskNO_AFFINITY);
    if (!hb_psram_) {
        return ESP_FAIL;
    }
    /* Phase 2 seal: NVS writer task + persisted key/floor state. Never
     * blocks; NVS reads only, safe before net_init. */
    if (seal_dev_init() != ESP_OK) {
        ESP_LOGW(TAG, "seal unavailable (continuing unverified)");
    }
    /* Phase 2 native MQTT: stays dormant (bridged) without WiFi or
     * broker creds; never blocks boot or crash-loops. */
    if (mqtt_native_init() != ESP_OK) {
        ESP_LOGW(TAG, "native mqtt unavailable (continuing bridged)");
    }
    /* Phase 3 WIA1 session machine: RAM-only state, strict by default,
     * disarmed at boot. Never blocks boot. */
    if (wia1_init() != ESP_OK) {
        ESP_LOGW(TAG, "wia1 unavailable (sessions disabled)");
    }
    return ESP_OK;
}
