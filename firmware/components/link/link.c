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
#include "driver/uart.h"
#include "driver/gpio.h" /* v0.4.31: TX pad drive-strength experiment */
#include "hal/uart_ll.h" /* v0.4.26: uart_ll_is_tx_idle (driverless TX drain) */
#include "hal/uart_hal.h" /* v0.4.27 diag: uart_hal_get_sclk */
#include "cJSON.h"
#include "link.h"
#include "hid_ble.h"
#include "eyes.h"
#include "wakeword.h"
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
/* v0.4.18.68: worker_psram_ removed — link_worker now uses internal RAM stack
 * via xTaskCreate (PSRAM stack + NVS/flash ops = crash). */
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
/* Retained log ring: every ESP_LOG line is kept so the host can pull recent */
/* history with {"t":"logs"}. The console_tx_task drains to USB via UART.  */
/* Cold path: lives in PSRAM (v0.4.18.70: moved back from internal to save  */
/* SRAM; only link_worker needs internal).                                 */
/* ------------------------------------------------------------------ */

#define LOG_BUF_SIZE 32768
/* v0.4.21 (chunk-loss RCA 2026-11-02): headroom kept free after chunk
 * pushes (see link_wait_for_ring_space) so uncontrolled log writes
 * can't lap the TX reader into the middle of a chunk. */
#define LINK_RING_HEADROOM_BYTES 8192
static char *log_ring_ = NULL;
static size_t log_head_ = 0;
static size_t log_valid_ = 0;
/* v0.4.18.70 (Claude F2): Monotonic counters for retained-history ring.
 * Writer increments log_written_ by bytes written. TX task tracks tx_read_
 * (bytes drained). Ring is retained-history only; TX task is a chasing
 * reader that consumes nothing. */
static volatile uint64_t log_written_ = 0;  /* total bytes ever written to ring */
static volatile uint64_t tx_read_ = 0;      /* total bytes ever drained by TX task */
static uint32_t log_dropped_ = 0;  /* bytes dropped due to writer lapping TX */
static uint32_t log_laps_ = 0;     /* v0.4.21: TX-reader lap (resync) events */

/* v0.4.24: switchable console baud (docs/baud-negotiation-design-2026-11-03.md).
 * Boots at 115200 ALWAYS (known connect speed); a sealed {"t":"baud"} command
 * negotiates up the ladder 230400/460800/921600. After a switch, the revert
 * watchdog is armed: if no valid line arrives within 5 s, the monitor task
 * falls back to 115200 so a bad switch can never strand the link. */
#define LINK_BAUD_DEFAULT 115200
static volatile uint32_t link_baud_ = LINK_BAUD_DEFAULT;
/* v0.4.25: baud transitions strand junk in the RX path — bytes clocked
 * in mid-switch (framing errors, port open/close transients) sit in the
 * line assembly with no newline and poison the next host line (a valid
 * resync ping was answered "unparseable line" in 0.4.24 field testing).
 * The baud handler and the revert path request a flush here; rx_task
 * performs it (it owns the ring and the drain) at the top of its loop. */
static volatile bool rx_flush_requested_ = false;
static void link_rx_flush(void) { rx_flush_requested_ = true; }
/* v0.4.27 DIAGNOSTIC (921600 garbling investigation): log the UART0
 * clock source, its frequency, and the raw CLKDIV after boot and after
 * every switch, so the ACHIEVED baud is arithmetic, not opinion. If the
 * mux sits on REF_TICK (1 MHz), the 1/16-step divider gives 941176 at
 * the 921600 rung (+2.12% — over the framing edge) while 115200 stays
 * at -0.08% — matching the observed error gradient across rungs. */
static void baud_diag_log(const char *when, uint32_t target)
{
    uart_hal_context_t hal = { .dev = &UART0 };
    soc_module_clk_t src = 0;
    uart_hal_get_sclk(&hal, &src);
    uint32_t freq = 0;
    (void)uart_get_sclk_freq((uart_sclk_t)src, &freq);
    uint32_t div_int = UART0.clkdiv.clkdiv;
    uint32_t div_frag = UART0.clkdiv.clkdiv_frag;
    /* v0.4.34: integer math, same reasoning as baud_actual_now. */
    uint32_t tot16 = div_int * 16u + div_frag;
    uint32_t actual = (freq && tot16)
                          ? (uint32_t)(((uint64_t)freq * 16u) / tot16)
                          : 0;
    ESP_LOGI(TAG, "baud diag %s: target=%u sclk=%d freq=%u clkdiv=%u+%u/16 actual=%u",
             when, (unsigned)target, (int)src, (unsigned)freq,
             (unsigned)div_int, (unsigned)div_frag, (unsigned)actual);
}
/* v0.4.35: ack'd chunk streaming state (see link.h). Written by the
 * rx task (ack/nack lines are consumed in the line parser, like
 * baud_ok), polled by the chunk sender in main.c. */
static volatile int chunk_ack_id_ = -1;      /* armed stream id, -1 = off */
static volatile int chunk_ack_last_ = -1;    /* highest acked seq */
static volatile int chunk_nack_seq_ = -1;    /* latest nacked seq */
static volatile uint32_t chunk_nack_count_ = 0;
static volatile uint32_t chunk_nack_seen_ = 0;

void link_chunk_ack_arm(int id)
{
    chunk_ack_id_ = id;
    chunk_ack_last_ = -1;
    chunk_nack_seq_ = -1;
    chunk_nack_count_ = 0;
    chunk_nack_seen_ = 0;
}

void link_chunk_ack_disarm(void)
{
    chunk_ack_id_ = -1;
}

bool link_chunk_ack_active(void)
{
    return chunk_ack_id_ >= 0;
}

void link_chunk_ack_note_send(void)
{
    chunk_nack_seen_ = chunk_nack_count_;
}

bool link_chunk_ack_wait(int id, int seq, size_t chunk_chars)
{
    if (chunk_ack_id_ != id) {
        return true; /* not armed for this stream: do not gate */
    }
    /* Baud-aware timeout (see link.h): wire time + slack, 1 s floor. */
    uint32_t timeout_ms = 600 + (uint32_t)((chunk_chars * 10000ULL) /
                                           (link_baud_ ? link_baud_ : 115200));
    if (timeout_ms < 1000) {
        timeout_ms = 1000;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (chunk_ack_last_ >= seq) {
            return true;
        }
        if (chunk_ack_id_ != id) {
            return true; /* disarmed mid-wait: lift the gate */
        }
        if (chunk_nack_count_ != chunk_nack_seen_ &&
            chunk_nack_seq_ == seq) {
            return false; /* host has it but it failed validation */
        }
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static volatile int64_t baud_revert_deadline_us_ = 0; /* 0 = disarmed */
/* v0.4.28: revert to the last CONFIRMED rate, not always 115200
 * (John's ladder: a failed 921600 should fall back to the proven
 * 460800). rx_task records the current rate here whenever a valid
 * line disarms the watchdog — that rate has demonstrably carried
 * traffic. The monitor reverts to it first; if it stays silent too,
 * a second stage falls to 115200. Stage2 flag marks that second leg. */
static volatile uint32_t baud_last_good_ = LINK_BAUD_DEFAULT;
static volatile bool baud_revert_stage2_ = false;
/* v0.4.28: RX FIFO overflow events (polled from the UART raw
 * interrupt status by rx_task) — the page→device loss counter this
 * link has never had. The 128-byte FIFO fills in ~1.4 ms at 921600,
 * so any rx_task stall (PSRAM cache-off windows, scheduling) shows
 * up here. Reported in the heartbeat. */
static volatile uint32_t rx_ovf_ = 0;
static uint32_t baud_actual_now(void)
{
    uart_hal_context_t hal = { .dev = &UART0 };
    soc_module_clk_t src = 0;
    uart_hal_get_sclk(&hal, &src);
    uint32_t freq = 0;
    (void)uart_get_sclk_freq((uart_sclk_t)src, &freq);
    /* v0.4.34: integer math (was soft-emulated doubles -- this
     * runs in baud log paths, incl. the small-stack monitor). */
    uint32_t tot16 = (uint32_t)UART0.clkdiv.clkdiv * 16u +
                     (uint32_t)UART0.clkdiv.clkdiv_frag;
    return (freq && tot16)
               ? (uint32_t)(((uint64_t)freq * 16u) / tot16)
               : 0;
}
static SemaphoreHandle_t log_lock_ = NULL;
static vprintf_like_t log_orig_vprintf_ = NULL;
/* v0.4.18.70 (Claude F6a): Deleted dead log_ring_write (no callers, old blocking semantics). */

/* v0.4.18.70 (Claude F1/F2): Ring push. Always writes (overwrite-oldest
 * for retention), advances head, saturates valid, increments
 * log_written_. Returns true if written, false if the lock could not be
 * taken within wait_ticks.
 * v0.4.22 (chunk-loss RCA follow-up): the wait is parameterized. The log
 * path (vprintf) must never block -> wait 0 (ring_push_nb). The data
 * path (link_send) MUST NOT drop an 11KB chunk line on instantaneous
 * lock contention — 0.4.21 verification run 1 aborted at chunk 42 on
 * exactly that dice roll. log_lock_ is a leaf lock and every holder is
 * a bounded byte-copy (worst case the 32KB logs-replay snapshot, ~ms),
 * so a 250ms take cannot deadlock and effectively never times out
 * unless the ring is genuinely wedged — which is when failing loud is
 * correct. */
static bool ring_push_w(const char *data, size_t len, TickType_t wait_ticks)
{
    if (!log_ring_ || !log_lock_ || !data || len == 0) {
        return false;
    }
    if (xSemaphoreTake(log_lock_, wait_ticks) != pdTRUE) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        log_ring_[log_head_] = data[i];
        log_head_ = (log_head_ + 1) % LOG_BUF_SIZE;
    }
    if (log_valid_ + len <= LOG_BUF_SIZE) {
        log_valid_ += len;
    } else {
        log_valid_ = LOG_BUF_SIZE;
    }
    log_written_ += len;
    xSemaphoreGive(log_lock_);
    return true;
}

/* Non-blocking variant for the log capture path: logs must never block. */
static bool ring_push_nb(const char *data, size_t len)
{
    return ring_push_w(data, len, 0);
}

/* Blocking variant for the link_send data path (see ring_push_w). */
static bool ring_push_blocking(const char *data, size_t len)
{
    return ring_push_w(data, len, pdMS_TO_TICKS(250));
}

/* v0.4.18.81: Ring backlog for PNG chunk flow control (John 2026-10-08).
 * Returns 0-100 (percent of ring size backed up). Measures the actual
 * TX backlog (log_written_ - tx_read_), NOT log_valid_ which is retained
 * history that never drains.
 * v0.4.18.84 (John 2026-10-08): Lock-free read. The counters are volatile;
 * taking log_lock_ here deadlocked the snaptest task (0.4.18.82). For flow
 * control, an approximate reading is fine — we just need to know if the
 * TX task is keeping up. */
int link_ring_fill_pct(void)
{
    if (!log_ring_) {
        return 100;  /* conservative: assume full if not initialized */
    }
    /* Lock-free: volatile reads. May race with TX task updates, but the
     * values are monotonic counters — a torn read just gives a slightly
     * stale percentage, which is fine for backpressure. */
    uint64_t written = log_written_;
    uint64_t read = tx_read_;
    uint64_t backlog = (written >= read) ? (written - read) : 0;
    if (backlog > LOG_BUF_SIZE) backlog = LOG_BUF_SIZE;
    return (int)((backlog * 100) / LOG_BUF_SIZE);
}

size_t link_ring_free_bytes(void)
{
    if (!log_ring_) {
        return 0;
    }
    uint64_t written = log_written_;
    uint64_t read = tx_read_;
    uint64_t backlog = (written >= read) ? (written - read) : 0;
    if (backlog > LOG_BUF_SIZE) backlog = LOG_BUF_SIZE;
    return LOG_BUF_SIZE - (size_t)backlog;
}

/* v0.4.18.81: Wait until the ring has room for a chunk. Blocks (with
 * watchdog-friendly delays) until fill drops below the low watermark.
 * v0.4.18.84 (John 2026-10-08): Proper blocking backpressure. The producer
 * (PNG encoder) waits for the consumer (USB TX task) instead of dropping
 * data. This is the correct flow control — blocking here is the ONLY case
 * where blocking is appropriate. Watchdog is fed by the caller's periodic
 * yields. */
bool link_wait_for_ring_space(uint32_t timeout_ms, size_t required_bytes)
{
    uint32_t waited = 0;
    /* v0.4.18.88 (2026-10-08): Wait for EXACT required space, not just 50%.
     * The old 50% watermark (6KB free) was insufficient for 11KB chunk JSONs.
     * ring_push_nb overwrites when full, corrupting the data. Now we wait
     * until required_bytes are actually free.
     * v0.4.21 (chunk-loss RCA 2026-11-02): HEADROOM. Exact space left the
     * ring at 100% after a chunk push; any uncontrolled log write then
     * lapped the chasing TX reader, which silently deleted a mid-stream
     * span (observed: a chunk's base64 arriving 342 chars short). Wait
     * for required + headroom so post-push backlog stays <= 24K and
     * stray log bursts are absorbed instead of deleting chunk bytes. */
    size_t target = required_bytes + LINK_RING_HEADROOM_BYTES;
    if (target > LOG_BUF_SIZE) {
        target = LOG_BUF_SIZE;
    }
    while (link_ring_free_bytes() < target) {
        if (waited >= timeout_ms) {
            return false;
        }
        /* Poll delay sized to drain ~500 bytes at the CURRENT baud
         * (v0.4.24: baud-derived — ~43ms at 115200, ~5ms at 921600),
         * clamped to [5, 50] ms. Longer than the 20ms in 0.4.18.81 to
         * reduce CPU spin. */
        uint32_t drain_ms = 5000000u / link_baud_;
        if (drain_ms < 5) {
            drain_ms = 5;
        }
        if (drain_ms > 50) {
            drain_ms = 50;
        }
        vTaskDelay(pdMS_TO_TICKS(drain_ms));
        waited += drain_ms;
    }
    return true;
}

static int log_capture_vprintf(const char *fmt, va_list args)
{
    /* v0.4.18.70 (Claude F2): Writers never block. Format into stack
     * buffer, push to ring via ring_push_nb (overwrite-oldest), return.
     * Do NOT call log_orig_vprintf_ here — that would do synchronous USB
     * I/O in the caller's task context. The console_tx_task drains. */
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n > 0) {
        size_t len = (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
        if (!ring_push_nb(buf, len)) {
            /* Lock unavailable: count as dropped (benignly racy, no lock). */
            log_dropped_ += (uint32_t)len;
        }
    }
    return n;
}

static void log_capture_init(void)
{
    /* v0.4.18.70: Ring BACK to PSRAM. It's just a data buffer, no need for
     * internal RAM. The 0.4.18.68/69 internal allocation exhausted SRAM. */
    log_ring_ = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    log_lock_ = xSemaphoreCreateMutex();
    if (log_ring_ && log_lock_) {
        log_orig_vprintf_ = esp_log_set_vprintf(log_capture_vprintf);
        ESP_LOGI(TAG, "log ring ready (%d bytes, PSRAM)", LOG_BUF_SIZE);
    } else {
        ESP_LOGW(TAG, "log ring unavailable");
    }
}

/* v0.4.18.68 (Claude 2026-10-08): Dedicated console TX task. Drains the log
 * ring to USB at low priority (3). If USB blocks (50ms/byte spin), only this
 * task stalls — nimble_host, link_worker, and snaptest are unaffected. */
/* v0.4.18.70 (Claude F2): Dedicated console TX task. Chasing reader that
 * consumes nothing — the ring is retained-history only. Uses monotonic
 * counters: lag = log_written_ - tx_read_. If lag > LOG_BUF_SIZE, the writer
 * lapped us; resync to oldest retained and count dropped bytes. */
static void console_tx_task(void *arg)
{
    (void)arg;
    char chunk[256];
    while (1) {
        size_t to_send = 0;
        if (xSemaphoreTake(log_lock_, pdMS_TO_TICKS(10)) == pdTRUE) {
            uint64_t written = log_written_;
            uint64_t read = tx_read_;
            uint64_t lag = (written >= read) ? (written - read) : 0;
            if (lag > LOG_BUF_SIZE) {
                /* Writer lapped us: count dropped, resync to oldest retained. */
                log_dropped_ += (uint32_t)(lag - LOG_BUF_SIZE);
                log_laps_++;
                tx_read_ = written - LOG_BUF_SIZE;
                lag = LOG_BUF_SIZE;
                read = tx_read_;
            }
            if (lag > 0) {
                /* Copy up to 256 bytes starting at (head - lag). */
                to_send = (lag < sizeof(chunk)) ? (size_t)lag : sizeof(chunk);
                size_t head = log_head_;
                /* Start position: head - lag (oldest unread byte). */
                size_t start = (head + LOG_BUF_SIZE - (size_t)(lag % LOG_BUF_SIZE)) % LOG_BUF_SIZE;
                for (size_t i = 0; i < to_send; i++) {
                    chunk[i] = log_ring_[(start + i) % LOG_BUF_SIZE];
                }
                tx_read_ += to_send;
            }
            xSemaphoreGive(log_lock_);
        }
        /* Send via fwrite to USB. This may spin 50ms/byte if host isn't
         * draining, but we're priority 3 — nobody cares. */
        if (to_send > 0) {
            fwrite(chunk, 1, to_send, stdout);
            fflush(stdout);
            /* v0.4.18.70 (Claude Q7): Yield after each burst so lower/equal
             * priority tasks (link_mon pri 3, link_hb pri 2) always breathe,
             * even during USB stall spin. */
            vTaskDelay(1);
        } else {
            /* No data: yield to avoid busy loop. */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
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
    /* v0.4.18.70 (Claude F1): Restore USB output via ring (non-blocking).
     * Push JSON line to the shared log ring; console_tx_task drains to USB.
     * v0.4.18.81 (John 2026-10-08): PNG chunks flow through USB again, but
     * with watermark flow control. The PNG sink callbacks (eyes.c) call
     * link_wait_for_ring_space() before each chunk, pacing the encoder to
     * the USB drain rate. This avoids both the 0.4.18.79 wedge (firehose)
     * and the 0.4.18.80 blackout (drop everything). */
    esp_err_t push_err = ESP_OK;
    {
        size_t slen = strlen(s);
        /* v0.4.18.87 (2026-10-08): Atomic single push. The JSON + newline
         * MUST go in one ring_push_nb call. Two separate pushes allow a
         * log line from another task to interleave between them, corrupting
         * the JSON (page sees JSON without newline, misclassifies as log).
         * This was dropping ~6/188 snaptest chunks. */
        char *line = (char *)malloc(slen + 2);
        if (line) {
            memcpy(line, s, slen);
            line[slen] = '\n';
            line[slen + 1] = '\0';
            if (!ring_push_blocking(line, slen + 1)) {
                /* v0.4.21 (chunk-loss RCA 2026-11-02): a failed push
                 * drops the whole line — count it here and report
                 * failure so chunk senders abort instead of silently
                 * shipping a holed image.
                 * v0.4.22: the push waits up to 250ms for log_lock_
                 * (ring_push_blocking), so reaching this branch means
                 * the ring is genuinely wedged, not a contention dice
                 * roll — failing loud is the correct outcome. */
                log_dropped_ += (uint32_t)(slen + 1);
                push_err = ESP_FAIL;
                ESP_LOGE(TAG, "link_send: ring push failed — line dropped");
            }
            free(line);
        } else {
            log_dropped_ += (uint32_t)(slen + 1);
            push_err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "link_send: line buffer alloc failed — dropped");
        }
    }
    /* Native-MQTT TX mirror: offer the line to the broker while native
     * mode is up. Non-blocking by contract; the line buffer is still
     * valid here. Offered even when the serial ring push failed — the
     * native path is independent of ring state. */
    if (tx_mirror_cb_) {
        tx_mirror_cb_(s, strlen(s));
    }
    free(s);
    xSemaphoreGive(tx_lock_);
    return push_err;
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
            /* v0.4.50 (WI-1): nested Himax link-health object. state is
             * "up" (answering), "down" (gate failing) or "recovering"
             * (a ladder rung in progress); streak is the io gate-failure
             * streak; gen the Himax session generation; rec_* are the
             * per-rung recovery counters since boot; last_rung the rung
             * credited with the most recent recovery.
             * v0.4.51 (D3): emitted IMMEDIATELY after "fw" on purpose.
             * The bridge truncates long telemetry lines at ~290 chars;
             * at its old position (after stack_hwm) this object ended
             * near char 370 and was truncated out of every heartbeat
             * in the 6-hour flap log — the audit found it had literally
             * never been observed. Here the object ends at ~char 194 of
             * the serialized heartbeat, inside the truncation budget
             * with ~96 chars to spare. Do not move it back down. */
            {
                eyes_himax_link_info_t li;
                eyes_himax_link_info(&li);
                cJSON *hl = cJSON_CreateObject();
                if (hl) {
                    cJSON_AddStringToObject(hl, "state", li.state);
                    cJSON_AddNumberToObject(hl, "streak", (double)li.streak);
                    cJSON_AddNumberToObject(hl, "gen", (double)li.gen);
                    cJSON_AddNumberToObject(hl, "rec_drain", (double)li.rec_drain);
                    cJSON_AddNumberToObject(hl, "rec_reset", (double)li.rec_reset);
                    cJSON_AddNumberToObject(hl, "rec_rst", (double)li.rec_rst);
                    cJSON_AddNumberToObject(hl, "rec_rail", (double)li.rec_rail);
                    cJSON_AddStringToObject(hl, "last_rung", li.last_rung);
                    cJSON_AddItemToObject(o, "himax_link", hl);
                }
            }
            cJSON_AddNumberToObject(o, "baud", (double)link_baud_);
            /* v0.4.28: achieved baud from the live CLKDIV + RX FIFO
             * overflow count — the UART path's vital signs. */
            cJSON_AddNumberToObject(o, "baud_actual",
                (double)baud_actual_now());
            cJSON_AddNumberToObject(o, "rx_ovf", (double)rx_ovf_);
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
            /* v0.4.21 (chunk-loss RCA 2026-11-02): ring drop observability.
             * log_dropped = bytes lost (TX-reader laps + whole lines
             * dropped on ring-push lock contention); log_laps = lap
             * events. Deltas across a snaptest convict or clear the ring
             * as the chunk-loss source. */
            cJSON_AddNumberToObject(o, "log_dropped", (double)log_dropped_);
            cJSON_AddNumberToObject(o, "log_laps", (double)log_laps_);
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
                    /* v0.4.18.70 (Claude F6g): Track console_tx HWM. */
                    "console_tx",
                    /* v0.4.50: link-health supervisor (ladder owner). */
                    "eyes_link_hlth",
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
        /* v0.4.24 baud auto-revert (armed by the "baud" handler). */
        if (baud_revert_deadline_us_ &&
            esp_timer_get_time() > baud_revert_deadline_us_) {
            /* v0.4.28: two-stage fallback — last confirmed rate first,
             * 115200 only if that stays silent too. */
            uint32_t was = link_baud_;
            uint32_t target;
            if (baud_revert_stage2_) {
                target = LINK_BAUD_DEFAULT;
                baud_revert_stage2_ = false;
                baud_revert_deadline_us_ = 0;
            } else if (baud_last_good_ != LINK_BAUD_DEFAULT &&
                       baud_last_good_ != was) {
                target = baud_last_good_;
                baud_revert_stage2_ = true;
                baud_revert_deadline_us_ = esp_timer_get_time() + 8000000;
            } else {
                target = LINK_BAUD_DEFAULT;
                baud_revert_deadline_us_ = 0;
            }
            if (uart_set_baudrate(UART_NUM_0, target) == ESP_OK) {
                link_baud_ = target;
                link_rx_flush(); /* v0.4.25: drop transition junk */
            }
            ESP_LOGW(TAG, "baud: no valid traffic at %u — reverted to %u%s",
                     (unsigned)was, (unsigned)target,
                     baud_revert_stage2_ ? " (last confirmed)" : "");
            baud_diag_log("revert", target);
        }
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
        /* v0.4.50 (§3.3): force-tag the origin on the inner command, so
         * the recursion sees the same origin facts as the native-MQTT
         * path (link_sealed_dispatch). Before this fix the recursion
         * carried NEITHER tag, and the gates below key on _via_mqtt —
         * so serial-only commands refused on the native path executed
         * when the identical sealed command arrived bridged (the
         * sealed_fwd handler dispatches from above the gates).
         * _sealed is set only when the envelope cryptographically
         * verified against the pinned commander key (rc == WMS2_OK);
         * a grace-accepted envelope (WMS2_OK_GRACE, unauthenticated)
         * gets _via_mqtt but NOT _sealed. Both tags are forced,
         * replacing any host-supplied values. */
        cJSON_DeleteItemFromObject(inner, "_via_mqtt");
        cJSON_AddTrueToObject(inner, "_via_mqtt");
        cJSON_DeleteItemFromObject(inner, "_sealed");
        if (rc == WMS2_OK) {
            cJSON_AddTrueToObject(inner, "_sealed");
        }
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
    if (strcmp(t, "baud") == 0) {
        /* v0.4.24: negotiate the console UART rate (see the design note).
         * Ack FIRST at the current baud, let it physically drain, then
         * switch and arm the revert watchdog; the host confirms the
         * ROUND TRIP by sending {"t":"baud_ok"} once it has parsed a
         * reply at the new rate (v0.4.30 — rx_task disarms on that
         * line only). */
        int rate = opt_int(cmd, "rate", 0);
        /* v0.4.32: mid-ladder candidates added for the rate scan --
         * 460800 is proven clean and 921600 is beyond this link's
         * edge quality at every divider offset (host sweep, 2026-
         * 11-04); find the highest clean rung instead. S3 clkdiv
         * quantization at these rates is <0.05%. */
        if (rate != 115200 && rate != 230400 && rate != 460800 &&
            rate != 576000 && rate != 691200 && rate != 750000 &&
            rate != 806400 && rate != 921600) {
            link_respond_err(t, "unsupported rate");
            return;
        }
        /* v0.4.33: optional clkdiv trim, in 1/16-divider steps
         * (positive = larger divider = slower). Clamped to a sane
         * bench range; applied after the switch below. */
        int frag = opt_int(cmd, "frag", 0);
        if (frag < -24) frag = -24;
        if (frag > 24) frag = 24;
        cJSON *o = link_new_event("baud_ack");
        if (o) {
            cJSON_AddNumberToObject(o, "rate", rate);
            cJSON_AddNumberToObject(o, "from", (double)link_baud_);
            cJSON_AddNumberToObject(o, "frag", frag);
            if (cmd_mid_[0]) {
                cJSON_AddStringToObject(o, "_mid", cmd_mid_);
            }
            link_send_term(o);
        }
        /* v0.4.26: wait until the ack is really ON THE WIRE before
         * switching. The old uart_wait_tx_done call never waited at
         * all: it is a driver-object API and the console runs
         * driverless through the VFS (p_uart_obj[0] is NULL), so it
         * failed instantly with "uart driver error". And even a
         * working FIFO wait would miss the real backlog — the ack
         * queues in the shared log ring behind whatever console_tx
         * hasn't drained. Sample the monotonic written-counter (the
         * ack enqueue above already advanced it past the ack) and
         * wait for the drained-counter to cover it, then for the TX
         * FSM to go idle (last byte actually on the wire). */
        {
            uint64_t ack_end = log_written_;
            int64_t wait_start = esp_timer_get_time();
            while (tx_read_ < ack_end &&
                   esp_timer_get_time() - wait_start < 2000000) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (tx_read_ < ack_end) {
                ESP_LOGW(TAG,
                         "baud: ring still draining after 2s — switching anyway");
            }
            wait_start = esp_timer_get_time();
            while (!uart_ll_is_tx_idle(&UART0) &&
                   esp_timer_get_time() - wait_start < 200000) {
                vTaskDelay(1);
            }
        }
        if (uart_set_baudrate(UART_NUM_0, (uint32_t)rate) == ESP_OK) {
            link_baud_ = (uint32_t)rate;
            /* v0.4.33: apply the clkdiv trim. The 2026-11-04
             * forensics show the CH9102 receiver losing bit-phase
             * coherence across a burst at 921600 (clean edges,
             * corruption growing mid-line, displaced-regrouping
             * reproduces the captured garbage) -- a sampling-rate
             * mismatch. Trimming from THIS side is exact (1/16
             * steps, readback via baud_actual_now) while the host
             * sweep through the bridge's own divider mapping was
             * erratic. The revert path calls plain
             * uart_set_baudrate, restoring the nominal divider,
             * so a bad trim can never stick. */
            if (frag != 0) {
                uint32_t tot16 = (uint32_t)UART0.clkdiv.clkdiv * 16u
                               + (uint32_t)UART0.clkdiv.clkdiv_frag;
                int32_t nt = (int32_t)tot16 + frag;
                if (nt < 16 * 8) nt = 16 * 8;
                UART0.clkdiv.clkdiv = (uint32_t)nt / 16u;
                UART0.clkdiv.clkdiv_frag = (uint32_t)nt % 16u;
            }
            link_rx_flush(); /* v0.4.25: drop transition junk */
            /* v0.4.30: the armed window is 8 s (was 5): the host's
             * slowest success path (lost ack → probe at the new rate
             * → baud_ok) can take ~4.5 s; margin matters more than
             * revert speed now that the disarm is round-trip-proof. */
            baud_revert_deadline_us_ =
                (rate == LINK_BAUD_DEFAULT)
                    ? 0
                    : esp_timer_get_time() + 8000000;
            baud_revert_stage2_ = false; /* v0.4.28: fresh attempt */
            ESP_LOGI(TAG, "baud: switched to %d (revert %s)", rate,
                     rate == LINK_BAUD_DEFAULT ? "disarmed" : "armed 8s");
            baud_diag_log("switch", (uint32_t)rate);
            /* v0.4.33: announce the achieved rate at the NEW rate;
             * the host's trim sweep reads its ground truth here. */
            {
                cJSON *rdy = link_new_event("baud_ready");
                if (rdy) {
                    cJSON_AddNumberToObject(rdy, "rate", rate);
                    cJSON_AddNumberToObject(rdy, "frag", frag);
                    cJSON_AddNumberToObject(rdy, "actual",
                                            (double)baud_actual_now());
                    link_send_term(rdy);
                }
            }
        } else {
            ESP_LOGE(TAG, "baud: uart_set_baudrate(%d) failed", rate);
        }
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
    if (strcmp(t, "reply") == 0) {
        /* {"t":"reply","text":"..."} — the host's short reply to a
         * voice take (v0.4.45): end the AI-thinking LED and show the
         * text on the device screen, paced at reading speed by the
         * display component. The full reply lives in the chat; this
         * is the glanceable essence. */
        const char *text = opt_str(cmd, "text");
        if (!text || !*text) {
            link_respond_err(t, "missing text");
            return;
        }
        link_ai_thinking_stop();
        display_show_message(text);
        link_respond_ok(t);
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
        if (strcmp(sub, "png") == 0) {
            /* v0.4.14 (P2): capture a full frame via AT+SPIDUMP + SPI bulk
             * read, encode as PNG, and stream it as {t:"png_chunk"} events
             * (base64). Read op — safe over MQTT, not serial-gated. Runs
             * synchronously; the terminal {t:"dbg",cmd:"png"} reply lands
             * after the last chunk. */
            /* v0.4.35: same ack opt-in as snaptest; the capture is
             * synchronous here, so disarm right after it returns. */
            if (opt_int(cmd, "ack", 0) != 0) {
                link_chunk_ack_arm(LINK_PNG_STREAM_ID);
            } else {
                link_chunk_ack_disarm();
            }
            esp_err_t err = eyes_capture_png();
            link_chunk_ack_disarm();
            cJSON *o = dbg_new("png");
            cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
            }
            link_send_term(o);
            return;
        }
        /* v0.4.18.69: Report stack high-water marks for SRAM optimization.
         * Helps right-size task stacks (Claude Q4). */
        if (strcmp(sub, "stacks") == 0) {
            cJSON *o = dbg_new("stacks");
            cJSON *tasks = cJSON_CreateArray();
            /* Get HWM for current task (link_worker). */
            {
                UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
                cJSON *t = cJSON_CreateObject();
                cJSON_AddStringToObject(t, "name", "link_worker");
                cJSON_AddNumberToObject(t, "hwm_words", (double)hwm);
                cJSON_AddNumberToObject(t, "hwm_bytes", (double)(hwm * sizeof(StackType_t)));
                cJSON_AddItemToArray(tasks, t);
            }
            cJSON_AddItemToObject(o, "tasks", tasks);
            cJSON_AddNumberToObject(o, "dropped_logs", (double)log_dropped_);
            link_send(o);
            return;
        }
        if (strcmp(sub, "rings") == 0) {
            /* Visible-area diagnostic (v0.4.46): concentric rings.
             * Toggle — show if hidden, hide if showing. */
            if (display_ring_test_visible()) {
                display_hide_ring_test();
            } else {
                display_show_ring_test();
            }
            link_respond_ok(t);
            return;
        }
        if (strcmp(sub, "spitest") == 0) {
            /* v0.4.18.11: SPI channel self-test. Runs 16 test patterns
             * (4 patterns x 4 sizes) through CMD_TEST_PATTERN, verifies
             * CRC and data integrity. Isolates SPI transport from camera.
             * Requires Himax 0.4.20.11+ (CMD_TEST_PATTERN support).
             * v0.4.18.19: Accepts optional params: gap_ms, pattern, len,
             * iterations. Passed through to eyes_spi_test(). */
            cJSON *o = dbg_new("spitest");
            cJSON *results = cJSON_CreateArray();
            /* The dbg command JSON itself contains the params */
            esp_err_t err = eyes_spi_test(results, cmd);
            cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
            cJSON_AddItemToObject(o, "tests", results);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
            }
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "crop") == 0) {
            /* v0.4.15 (P4): set/clear/query the Himax full-res crop window.
             * {"t":"dbg","cmd":"crop","args":"x,y,w,h"} -> AT+CROP (enters
             *   2592x1944 1:1 mode);
             * {"t":"dbg","cmd":"crop","args":"clear"} -> AT+CROP=0,0,0,0;
             * {"t":"dbg","cmd":"crop"} -> AT+CROP? query.
             * A set is followed by {"t":"dbg","cmd":"png"} to capture. */
            cJSON *o = dbg_new("crop");
            esp_err_t err = ESP_OK;
            const char *args = opt_str(cmd, "args");
            if (args && *args) {
                if (strcmp(args, "clear") == 0) {
                    /* v0.4.50 (§2.5): re-issue once if the command timed
                     * out across a link generation change (it may have
                     * died with the old Himax session). */
                    uint32_t gen0 = eyes_link_gen();
                    err = eyes_clear_crop();
                    if (err == ESP_ERR_TIMEOUT && eyes_link_gen() != gen0) {
                        ESP_LOGW(TAG, "dbg crop clear lost with Himax "
                                      "session (gen %u->%u); re-issuing once",
                                 (unsigned)gen0, (unsigned)eyes_link_gen());
                        err = eyes_clear_crop();
                    }
                } else {
                    unsigned x = 0, y = 0, w = 0, h = 0;
                    if (sscanf(args, "%u,%u,%u,%u", &x, &y, &w, &h) == 4) {
                        /* v0.4.50 (§2.5): same one-shot re-issue as the
                         * clear branch — AT+CROP is idempotent. */
                        uint32_t gen0 = eyes_link_gen();
                        err = eyes_set_crop((uint16_t)x, (uint16_t)y,
                                            (uint16_t)w, (uint16_t)h);
                        if (err == ESP_ERR_TIMEOUT &&
                            eyes_link_gen() != gen0) {
                            ESP_LOGW(TAG, "dbg crop set lost with Himax "
                                          "session (gen %u->%u); re-issuing once",
                                     (unsigned)gen0, (unsigned)eyes_link_gen());
                            err = eyes_set_crop((uint16_t)x, (uint16_t)y,
                                                (uint16_t)w, (uint16_t)h);
                        }
                    } else {
                        err = ESP_ERR_INVALID_ARG;
                    }
                }
            } else {
                char reply[256];
                err = eyes_at("AT+CROP?", reply, sizeof(reply));
                if (err == ESP_OK) {
                    cJSON_AddStringToObject(o, "crop", reply);
                }
            }
            cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
            }
            link_send_term(o);
            return;
        }
        if (strcmp(sub, "strip") == 0) {
            /* v0.4.15 (P5): tile the 2592x1944 full-res frame into 2592x486
             * strips and stream each as {t:"strip_chunk"} events (base64).
             * {"t":"dbg","cmd":"strip"} -> overlap 0 (4 strips);
             * {"t":"dbg","cmd":"strip","overlap":16} -> 5 strips.
             * Read op — safe over MQTT. The crop window is cleared on exit. */
            cJSON *ov = cJSON_GetObjectItem(cmd, "overlap");
            uint16_t overlap = (cJSON_IsNumber(ov) && ov->valuedouble >= 0)
                                   ? (uint16_t)ov->valuedouble : 0;
            esp_err_t err = eyes_capture_strips(overlap);
            cJSON *o = dbg_new("strip");
            cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
            cJSON_AddNumberToObject(o, "overlap", overlap);
            if (err != ESP_OK) {
                cJSON_AddStringToObject(o, "err", esp_err_to_name(err));
            }
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
             * + re-handshake. (v0.4.50: no longer serial-only — the
             * origin policy below governs remote callers.)
             * {"t":"dbg","cmd":"eyes_reset"} or
             * {"t":"dbg","cmd":"eyes_reset","rail":0} (warm-only).
             * v0.4.50 (§3.3): the blanket remote refusal is replaced by
             * an origin policy. Serial callers: unchanged (rail
             * defaults to 1). Remote callers (_via_mqtt): allowed ONLY
             * with _sealed set — the command's envelope cryptographically
             * verified against the pinned commander key. (The spec's
             * "highest seal scope" is realized as _sealed: the shipped
             * WMS2 verify path has a single pinned commander key and no
             * per-key scope tiers — reduced-scope keys are a deferred
             * design; if tiers land, the tier test goes here.) For
             * remote callers the rail default flips to 0 (warm RST
             * recovery); a remote rail cycle requires an explicit
             * "rail":1 in the same sealed command. Rationale: the
             * link-health supervisor already performs unattended
             * RST/rail recovery on this device, so a sealed remote
             * reset adds no new unattended capability — while an
             * unsealed one would let anyone able to publish to the
             * broker yank the camera. */
            bool sealed = cJSON_IsTrue(cJSON_GetObjectItem(cmd, "_sealed"));
            if (via_mqtt && !sealed) {
                link_respond_err(t, "eyes_reset over MQTT requires a sealed command");
                return;
            }
            if (eyes_snap_busy()) {
                link_respond_err(t, "snapshot in progress");
                return;
            }
            /* v0.4.51 (G3): rail is 1 ONLY for the JSON number 1
             * (valuedouble == 1.0). The old opt_int parse silently
             * mapped a JSON true to the origin default — 1 on serial:
             * a warm-reset typo became a rail cycle. Absent key keeps
             * the origin defaults (serial 1 / remote 0); every other
             * value or type (true, 2, "1", 1.5) parses to 0, warm
             * only, and the line below records what was parsed, from
             * which origin, and the key's actual type — the exact
             * ambiguity the audit flagged, now visible in the log. */
            cJSON *rail_item = cJSON_GetObjectItem(cmd, "rail");
            const char *rail_kind = "absent";
            bool rail;
            if (!rail_item) {
                rail = !via_mqtt;
            } else if (cJSON_IsNumber(rail_item)) {
                rail_kind = "number";
                rail = (rail_item->valuedouble == 1.0);
            } else {
                rail_kind = "other";
                rail = false;
            }
            ESP_LOGI(TAG, "eyes_reset: rail=%d origin=%s key=%s",
                     rail ? 1 : 0, via_mqtt ? "mqtt" : "serial",
                     rail_kind);
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
            /* v0.4.14 (P0.3): the reply buffer is heap-allocated (PSRAM) at
             * 4 KiB. AT+RAWREAD returns ~2.8 KiB and was silently truncated
             * to 511 chars by the old 512-byte stack buffer. A 4 KiB stack
             * buffer is unsafe here — the link worker task has an 8 KiB
             * stack. */
            char *out = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
            if (!out) {
                link_respond_err(t, "no mem for at reply");
                return;
            }
            esp_err_t err = eyes_at(at, out, 4096);
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
            heap_caps_free(out);
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
        if (strcmp(sub, "wakeword") == 0) {
            /* {"t":"dbg","cmd":"wakeword","on":1|0} — enable/disable
             * wake-word listening (persisted in NVS). No "on" field =
             * status only. v0.4.36: this subcommand was documented in
             * the wakeword DESIGN.md since v0.3.4 but never wired into
             * the dispatch — the feature could not be turned on at all.
             * The live model is "Hi, ESP" (wn9_hiesp); the phrase field
             * reports the NVS selection, which falls back to it. */
            if (cJSON_HasObjectItem(cmd, "on")) {
                wakeword_set_enabled(opt_int(cmd, "on", 1) != 0);
            }
            cJSON *o = dbg_new(sub);
            cJSON_AddBoolToObject(o, "enabled", wakeword_enabled());
            cJSON_AddBoolToObject(o, "listening", wakeword_listening());
            cJSON_AddNumberToObject(o, "phrase",
                                    (double)wakeword_get_phrase());
            cJSON_AddNumberToObject(o, "detections",
                                    (double)wakeword_detection_count());
            cJSON_AddStringToObject(o, "model", "wn10_himuse");
            /* v0.4.38 mic instrumentation (extern, defined in ptt.c):
             * cumulative codec read outcomes + peak since last reset,
             * so capture health is queryable without the log stream. */
            {
                extern void ptt_mic_stats(uint32_t *, uint32_t *, int *,
                                          int32_t *);
                uint32_t ok, err;
                int lr;
                int32_t pk;
                ptt_mic_stats(&ok, &err, &lr, &pk);
                cJSON_AddNumberToObject(o, "mic_ok", (double)ok);
                cJSON_AddNumberToObject(o, "mic_err", (double)err);
                cJSON_AddNumberToObject(o, "mic_peak", (double)pk);
            }
            link_send(o);
            return;
        }
        if (strcmp(sub, "codec") == 0) {
            /* {"t":"dbg","cmd":"codec"} — v0.4.41 mic audit: live
             * ES7243E register dump (read-only I2C). The host diffs
             * "regs" against the intended state in
             * docs/mic-audit-2026-11-07.md. "acquired" says whether
             * the mic is currently open (wake-word listening or a
             * take) — a dump taken while released shows the
             * powered-down state instead. */
            extern esp_err_t ptt_codec_dump(bool *, char *, size_t);
            char regs[320];
            bool acq = false;
            esp_err_t r = ptt_codec_dump(&acq, regs, sizeof(regs));
            cJSON *o = dbg_new(sub);
            if (r == ESP_OK) {
                cJSON_AddStringToObject(o, "regs", regs);
                cJSON_AddBoolToObject(o, "acquired", acq);
            } else {
                cJSON_AddStringToObject(o, "error", esp_err_to_name(r));
            }
            link_send(o);
            return;
        }
        if (strcmp(sub, "loopback") == 0) {
            /* {"t":"dbg","cmd":"loopback"} — v0.4.42: phased ES8311
             * loopback / mic-power test (see ptt_loopback_test).
             * Blocks ~4 s in this task; results arrive as three
             * {"t":"loopback","phase":0|1|2} events. The ack here is
             * non-terminal. */
            extern esp_err_t ptt_loopback_test(void);
            esp_err_t r = ptt_loopback_test();
            cJSON *o = dbg_new(sub);
            cJSON_AddStringToObject(
                o, "result",
                (r == ESP_OK) ? "done" : esp_err_to_name(r));
            link_send(o);
            return;
        }
        if (strcmp(sub, "snaptest") == 0) {
            /* Async snapshot self-test: analyze a frame on-device, no
             * Wi-Fi needed. Completion arrives later via the snaptest
             * callback (terminal reply); the ack here is non-terminal. */
            int id = opt_int(cmd, "id", 0);
            bool send = opt_int(cmd, "send", 0) != 0;
            bool fresh = opt_int(cmd, "fresh", 1) != 0;
            /* v0.4.35: ack'd chunk streaming is opt-in per command —
             * a host that never acks must never stall the sender, so
             * the gate arms only when the command asks for it. */
            if (opt_int(cmd, "ack", 0) != 0) {
                link_chunk_ack_arm(id);
            } else {
                link_chunk_ack_disarm();
            }
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
 * provisioning commands are refused for it. v0.4.50: the command is
 * also tagged _sealed when `verified` (body cryptographically verified
 * by seal_dev); grace-mode plain commands (verified=false) never get
 * it, and any host-supplied _sealed is stripped. */
void link_sealed_dispatch(const char *json, size_t len, bool verified)
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
    /* v0.4.50: _sealed marks a body seal_dev cryptographically verified
     * against the pinned commander key (verified=true from
     * mqtt_native). Grace-mode plain commands arrive verified=false and
     * never carry it; any host-supplied claim is deleted first. */
    cJSON_DeleteItemFromObject(cmd, "_sealed");
    if (verified) {
        cJSON_AddTrueToObject(cmd, "_sealed");
    }
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
        /* v0.4.28: sample the sticky RX-FIFO-overflow flag (latches
         * until cleared, so polling here misses nothing). */
        if (UART0.int_raw.rxfifo_ovf_int_raw) {
            UART0.int_clr.rxfifo_ovf_int_clr = 1;
            rx_ovf_++;
        }
        if (rx_flush_requested_) {
            /* Baud transition — drop everything received so far so
             * junk can't poison the next line. rx_task owns the ring
             * and the drain, so it does the reset itself.
             * v0.4.26: the discard is driverless — uart_flush_input
             * needs the driver object this console never installs (it
             * failed with "uart driver error" and did nothing). Read
             * and drop whatever the VFS has buffered (this task is
             * the only stdin reader; the fd is already non-blocking),
             * then drop the ring contents. */
            rx_flush_requested_ = false;
            static char scratch[128];
            int guard = 64; /* ≤8 KB; the HW FIFO is 128 B — generous */
            while (guard-- > 0) {
                int n = read(STDIN_FILENO, scratch, sizeof(scratch));
                if (n <= 0) {
                    break;
                }
            }
            rx_rd_ = rx_wr_;
        }
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
            if (cmd) {
                /* v0.4.30: the ONLY line that disarms the revert
                 * watchdog is {"t":"baud_ok"} — the host sends it
                 * solely after it has parsed a reply at the new rate,
                 * i.e. proof the ROUND TRIP works. v0.4.24–0.4.28
                 * disarmed on ANY parseable line, but host→device is
                 * the clean direction on this link: the device parsed
                 * the confirm pings, disarmed, and stayed at 921600
                 * while the host heard only scrambled pongs and fell
                 * back — a permanent divergence (field, 2026-11-03).
                 * baud_ok is consumed here and never queued; receiving
                 * it also records the current rate as last-proven. */
                const char *t0 = opt_str(cmd, "t");
                if (t0 && strcmp(t0, "baud_ok") == 0) {
                    baud_revert_deadline_us_ = 0;
                    baud_last_good_ = link_baud_;
                    baud_revert_stage2_ = false;
                    cJSON_Delete(cmd);
                    continue;
                }
                if (t0 && strcmp(t0, "chunk_ack") == 0) {
                    /* v0.4.35: host validated chunk seq of the armed
                     * stream. Consumed here (never queued) so an ack
                     * can never be reordered behind worker jobs —
                     * the same rule as baud_ok. */
                    int aid = opt_int(cmd, "id", -1);
                    int aseq = opt_int(cmd, "seq", -1);
                    if (aid == chunk_ack_id_ && aseq > chunk_ack_last_) {
                        chunk_ack_last_ = aseq;
                    }
                    cJSON_Delete(cmd);
                    continue;
                }
                if (t0 && strcmp(t0, "chunk_nack") == 0) {
                    /* v0.4.35: host received the chunk but it failed
                     * validation — the sender resends it immediately
                     * instead of waiting out the ack timeout. Draws
                     * on the same bounded attempt budget. */
                    int aid = opt_int(cmd, "id", -1);
                    int aseq = opt_int(cmd, "seq", -1);
                    if (aid == chunk_ack_id_ && aseq >= 0) {
                        chunk_nack_seq_ = aseq;
                        chunk_nack_count_++;
                    }
                    cJSON_Delete(cmd);
                    continue;
                }
            }
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
            /* Idle: brief nap, baud-aware (v0.4.24). A burst is caught on
             * the next loop with ~no added latency (the loop re-drains
             * immediately while bytes keep arriving). At 115200 baud ~23
             * bytes arrive per 2ms — far below the UART driver's RX ring;
             * at 921600 the same 2ms would admit ~184 bytes, so the top
             * rungs nap only 1ms (~92 bytes). */
            vTaskDelay(pdMS_TO_TICKS(link_baud_ >= 460800 ? 1 : 2));
        }
    }
}

esp_err_t link_init(void)
{
    baud_diag_log("boot", LINK_BAUD_DEFAULT);
    /* v0.4.31 EXPERIMENT (single variable vs 0.4.30): raise the UART0
     * TX pad (GPIO43, the S3's IOMUX default for U0TXD) drive strength
     * to max. The 921600 failure is asymmetric: host→device arrives
     * clean (pings and commands parse), device→host scrambles —
     * consistent with the CH9102's receiver marginally sampling the
     * S3's TX edges. Sharper TX edges are the one analog lever
     * firmware has. A/B the device→page error rate against 0.4.30;
     * revert if it doesn't move. */
    {
        gpio_drive_cap_t cap_before = GPIO_DRIVE_CAP_0;
        (void)gpio_get_drive_capability(GPIO_NUM_43, &cap_before);
        (void)gpio_set_drive_capability(GPIO_NUM_43, GPIO_DRIVE_CAP_3);
        ESP_LOGI(TAG, "tx pad drive cap %d -> %d (GPIO43 experiment)",
                 (int)cap_before, (int)GPIO_DRIVE_CAP_3);
    }
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
    /* v0.4.18.70: console_tx uses PSRAM stack (not internal). It only does
     * USB writes, no flash ops. Internal RAM is precious (only 16KB free).
     * The 0.4.18.68 internal allocation contributed to SRAM exhaustion. */
    {
        TaskHandle_t tx_handle = NULL;
        /* Use task_psram_create for PSRAM stack. */
        task_psram_handle_t *tx_psram = task_psram_create(console_tx_task, "console_tx",
                                                          4096, NULL, 3, tskNO_AFFINITY);
        if (!tx_psram) {
            ESP_LOGE(TAG, "console TX task creation failed");
        }
        (void)tx_handle; /* unused with psram_create */
    }
    /* Worker executes commands; RX only frames/parses/enqueues so the
     * UART never overflows while a slow handler runs. RX runs one notch
     * above the worker so drain latency stays minimal. The monitor
     * enforces command deadlines at low priority. The AI-thinking LED
     * timer is created on first use.
     *
     * RAM: mon/hb stacks live in PSRAM (they do no flash ops and
     * are not timing-critical). link_rx stays internal — UART drain at
     * priority 9 cannot tolerate PSRAM cache-miss jitter.
     * v0.4.18.68 (Claude 2026-10-08): link_worker MUST use internal RAM stack,
     * not PSRAM. It does NVS/flash ops; PSRAM becomes inaccessible during
     * flash writes → crash. v0.4.18.70: worker is internal (16KB); mon/hb
     * remain PSRAM.
     * flash writes → crash. This is a crash-in-waiting. */
    TaskHandle_t worker_handle = NULL;
    BaseType_t ok = xTaskCreate(link_worker_task, "link_worker", 16384 / sizeof(StackType_t),
                                NULL, 8, &worker_handle);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }
    /* v0.4.6: PSRAM stack, 4096 words = 16384 bytes (was 16KB internal). */
    /* v0.4.18.68: Reverted to internal — PSRAM stack + flash ops = crash. */
    rx_psram_ = task_psram_create(rx_task, "link_rx", 4096 * 4, NULL, 9,
                                 tskNO_AFFINITY);
    if (!rx_psram_) {
        return ESP_FAIL;
    }
    /* v0.4.34: 2048 -> 4096. The 2048 stack overflowed on the
     * baud-revert path (ESP_LOGW + baud_diag_log: two full
     * esp_log traversals in the system's smallest task) --
     * panic + RTC_SW_CPU_RST at every unrecovered negotiation
     * since 0.4.24, proven by --watch-revert 2026-11-04 and a
     * matching error line in telemetry from the 0.4.176
     * "console freeze" incident. 4096 = parity with link_hb,
     * which does strictly heavier cJSON/log work; the margin
     * is verified via the heartbeat stack_hwm readout. */
    mon_psram_ = task_psram_create(link_monitor_task, "link_mon", 4096,
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
