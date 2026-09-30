/* SPDX-License-Identifier: MIT
 * Phase 2 device-side seal: NVS-pinned commander key, wear-aware replay
 * high-water (D4 zero-budget variant), WMS2 envelope verification.
 *
 * NVS discipline (fleet ESP-target lesson): ONE dedicated nvs_task
 * performs EVERY NVS operation — reads and writes. All other task
 * contexts (link worker, MQTT task, etc.) enqueue requests through
 * the synchronous seal_dev_nvs_*_sync() APIs; they never call nvs_*
 * directly. The task is the ONLY code that calls nvs_open.
 *
 * Why: the SPI flash write disables the CPU cache, and a PSRAM stack
 * behind the disabled cache panics the CPU ("Cache disabled but
 * cached memory region accessed"). v0.4.6 proved reads must go
 * through the task too (mqtt_native/ww_feed on PSRAM stacks
 * panicked on direct nvs_get_*). See RAM_OPTIMIZATION_v0.3.9.md
 * and the v0.4.1 set_name crash.
 *
 * D4 zero-budget high-water: every accepted kind-2 command advances a
 * RAM high-water. The persisted value ("cmd_hw") is written on every
 * 32nd accept, or immediately when the high-water runs 32 past the
 * last persisted value (covers ctr gap-jumps). Invariant: executed
 * ctr <= stored + 31 at all times, so boot floor = stored + 32 clears
 * everything already executed: replay budget after a crash is zero.
 * The commander resyncs with next = max(own_ctr, floor) + 1.
 *
 * D6: no large stack buffers. The verify scratch (2+128+40+2048) and
 * the base64/NVS queues are static or heap. A mutex serializes
 * verify_envelope callers (link worker + MQTT task share the scratch).
 */

#include "seal_dev.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "esp_attr.h"
#include "esp_memory_utils.h" /* v0.4.8: esp_ptr_internal() boot check */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"  /* MALLOC_CAP_INTERNAL for done structs */
#include "task_psram.h"     /* stress-test tasks run on PSRAM stacks */
#include "nvs_flash.h"
#include "nvs.h"

#include "monocypher.h"

/* link.h is deliberately NOT #included here: link.c #includes the seal
 * headers, so including link.h would create a build-level include cycle.
 * The two reply helpers are declared extern instead; the link-time
 * dependency seal->link is one-directional. */
extern esp_err_t link_respond_ok(const char *cmd);
extern esp_err_t link_respond_err(const char *cmd, const char *msg);

static const char *TAG = "seal_dev";

/* NVS keys (namespace "seal", all <= 15 chars). */
#define NVS_KEY_CMD_PK   "cmd_pk"    /* blob 32: pinned commander Ed25519 pk */
#define NVS_KEY_TEL_SEED "tel_seed"  /* blob 32: telemetry seed */
#define NVS_KEY_STRICT   "strict"    /* u8: 1 = strict, 0 = grace */
#define NVS_KEY_CMD_HW   "cmd_hw"    /* u64: last persisted high-water */

/* ------------------------------------------------------------------ */
/* Persistent + runtime state                                          */
/* ------------------------------------------------------------------ */

static bool     inited_ = false;
static bool     has_key_ = false;
static uint8_t  cmd_pk_[32];
static bool     has_tel_seed_ = false;
static uint8_t  tel_seed_[32];
static bool     strict_ = false;          /* default 0 = grace */

/* D4 high-water state. */
static uint64_t floor_ = 0;               /* boot floor, surfaced in status */
static uint64_t high_water_ = 0;          /* RAM high-water (accepted ctr) */
static uint64_t last_persisted_ = 0;      /* value last written to NVS */
static bool     hw_valid_ = false;        /* "cmd_hw" was ever stored */
static uint32_t accepted_since_persist_ = 0;

static uint32_t verified_count_ = 0;
static uint32_t rejected_count_ = 0;

static wms2_replay replay_;
static wms2_policy policy_;

/* D6 verify scratch: 2 + topic(<=128) + header(40) + body(<=2048).
 * Shared by all verify callers under verify_lock_; body_out points
 * into it, valid until the next seal_dev_verify_envelope call.
 * v0.4.3 (R2): moved to PSRAM — CPU-only scratch (no DMA, no ISR,
 * never touched during flash writes), saves 2218 B of internal DRAM. */
static EXT_RAM_BSS_ATTR uint8_t verify_scratch_[2 + WMS2_MAX_TOPIC +
                                                 WMS2_HDR_LEN +
                                                 SEAL_MAX_CMD_BODY];
static SemaphoreHandle_t verify_lock_ = NULL;

/* ------------------------------------------------------------------ */
/* SECLOG ring: named rejection events (code + count).                 */
/* ------------------------------------------------------------------ */

#define SEAL_SECLOG_CAP 12
typedef struct {
    char     code[16];
    uint32_t count;
} seal_seclog_e_t;

static seal_seclog_e_t seclog_[SEAL_SECLOG_CAP];
static size_t seclog_used_ = 0;

static void seclog_add(const char *code)
{
    if (!code || !*code) {
        code = "?";
    }
    for (size_t i = 0; i < seclog_used_; i++) {
        if (strncmp(seclog_[i].code, code, sizeof(seclog_[i].code)) == 0) {
            seclog_[i].count++;
            return;
        }
    }
    if (seclog_used_ < SEAL_SECLOG_CAP) {
        strncpy(seclog_[seclog_used_].code, code,
                sizeof(seclog_[seclog_used_].code) - 1);
        seclog_[seclog_used_].code[sizeof(seclog_[seclog_used_].code) - 1] = '\0';
        seclog_[seclog_used_].count = 1;
        seclog_used_++;
        return;
    }
    /* Full: evict the oldest entry, shift the rest down. */
    memmove(&seclog_[0], &seclog_[1],
            sizeof(seclog_[0]) * (SEAL_SECLOG_CAP - 1));
    strncpy(seclog_[SEAL_SECLOG_CAP - 1].code, code,
            sizeof(seclog_[SEAL_SECLOG_CAP - 1].code) - 1);
    seclog_[SEAL_SECLOG_CAP - 1].code[sizeof(seclog_[SEAL_SECLOG_CAP - 1].code) - 1] = '\0';
    seclog_[SEAL_SECLOG_CAP - 1].count = 1;
}

/* Contract gap: seal_dev.h has no SECLOG accessor yet (only the
 * verified/rejected counters). Declared extern in link.c so the
 * heartbeat can carry the ring; the coordinator should promote this
 * to seal_dev.h in the next contract revision. */
void seal_dev_seclog_str(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < seclog_used_; i++) {
        int n = snprintf(out + pos, out_len - pos, "%s%s=%lu",
                         i ? "," : "", seclog_[i].code,
                         (unsigned long)seclog_[i].count);
        if (n < 0 || (size_t)n >= out_len - pos) {
            break;
        }
        pos += (size_t)n;
    }
}

/* ------------------------------------------------------------------ */
/* NVS task: the ONLY task that touches NVS/flash (reads AND writes). */
/* ------------------------------------------------------------------ */

/* Queue depth: 32.
 *
 * Sizing rationale (John's requirement: >=4x worst-case in-flight):
 * Tasks that can issue NVS ops: main/init (boot only), link_worker,
 * mqtt_native, net, hid_ble, wakeword, wia1. Each issues at most 1
 * op at a time (all APIs are synchronous; a task blocks until its
 * op completes before issuing another). Worst realistic in-flight:
 * 7 tasks x 1 op = 7. 32 / 7 = 4.5x headroom.
 *
 * Why it can't fill in practice: NVS ops complete in <10ms typical,
 * <100ms worst (page compaction). At 7 in-flight, the broker drains
 * the queue in <1s. A full queue persisting for seconds means the
 * broker is deadlocked or NVS/flash is hung — both are system-fatal,
 * so we PANIC (see nvs_op_sync), never silently drop.
 *
 * Memory cost: 32 x sizeof(nvs_task_req_t) (~200B) = ~6.4KB from
 * the FreeRTOS heap. Acceptable for the guarantee. */
#define NVS_TASK_QDEPTH 32

/* Panic timeouts (John: queue management must never be a failure
 * source; these fire only if the broker is dead or NVS is hung). */
#define NVS_ENQUEUE_TIMEOUT_MS  5000  /* xQueueSend before PANIC */
#define NVS_RESPONSE_TIMEOUT_MS 10000 /* xSemaphoreTake before PANIC */

/* v0.4.3: string writes + cross-namespace writes + synchronous
 * completion. v0.4.7: reads also go through the task (John's call —
 * the task is now reader/writer agnostic). The task remains the ONLY
 * task that touches NVS — now for reads too, across non-seal
 * namespaces (e.g. "dev" for the device name, the net component's
 * WiFi creds, mqtt_native's broker creds, wakeword's settings).
 * Callers on PSRAM-stack tasks must NEVER call nvs_* directly: the
 * SPI flash write disables the CPU cache, and a PSRAM stack behind
 * the disabled cache panics the CPU ("Cache disabled but cached
 * memory region accessed"). See RAM_OPTIMIZATION_v0.3.9.md and the
 * v0.4.1 set_name crash. v0.4.6 proved reads must go through the task
 * too (mqtt_native/ww_feed on PSRAM stacks panicked). */

typedef enum {
    NVS_TASK_U64,
    NVS_TASK_U8,
    NVS_TASK_BLOB,
    NVS_TASK_STR,    /* v0.4.3: NUL-terminated string value */
    NVS_TASK_DEL,
    /* v0.4.7: synchronous reads. The task fills done->result. */
    NVS_TASK_READ_U64,
    NVS_TASK_READ_U8,
    NVS_TASK_READ_BLOB,
    NVS_TASK_READ_STR,
} nvs_task_op_t;

/* Synchronous completion.
 *
 * v0.4.7 contract (John's requirements):
 * - Single FIFO queue, strictly ordered. The broker processes one
 *   op at a time, in enqueue order. No concurrent NVS access, ever.
 * - Callers block until completion. Every API is synchronous; the
 *   caller blocks on done->sem until the broker signals. No
 *   fire-and-forget, no polling.
 * - Never drop, never reject. If the queue is full (practically
 *   unreachable — see NVS_TASK_QDEPTH), we PANIC, not drop. If the
 *   broker doesn't respond within NVS_RESPONSE_TIMEOUT_MS, we PANIC,
 *   not timeout-return. Queue management is never a failure source.
 * - Only NVS hardware errors propagate. ESP_ERR_NVS_*, flash errors,
 *   etc. are returned to the caller in done->err. Everything else
 *   (queue full, broker hung) is a system-fatal panic.
 * - done is allocated in INTERNAL RAM (heap_caps, MALLOC_CAP_INTERNAL).
 *   The broker touches done->result while flash/cache ops occur; if
 *   done were in PSRAM, we'd reintroduce the exact cache-disabled
 *   crash this task exists to prevent.
 *
 * Lifecycle: caller allocates (internal RAM) -> enqueues -> blocks
 * on sem -> broker fills err/result -> broker gives sem -> caller
 * takes sem, copies result, deletes sem, frees done. The broker
 * NEVER frees done; the caller NEVER touches done after a panic
 * (we abort, so there's no "after").
 *
 * v0.4.7: for reads, the broker fills done->result. */
typedef struct {
    SemaphoreHandle_t sem;
    esp_err_t         err;
    union {
        uint64_t u64;
        uint8_t  u8;
        struct {
            uint8_t buf[128];
            size_t  len;
        } blob;
        struct {
            char  str[96];
            size_t len;
        } str;
    } result;   /* v0.4.7: read result (READ_* ops only) */
} nvs_task_done_t;

typedef struct {
    nvs_task_op_t op;
    char          ns[16];    /* v0.4.3: NVS namespace; "" = SEAL_NVS_NS */
    char          key[16];
    union {
        uint64_t u64;
        uint8_t  u8;
        struct {
            uint8_t buf[128];
            size_t  len;
        } blob;
        char str[96];        /* v0.4.3: NVS_TASK_STR value (SSID/pass/name) */
    } v;
    nvs_task_done_t *done;   /* v0.4.3: NULL = fire-and-forget */
} nvs_task_req_t;

static QueueHandle_t nvs_q_ = NULL;

/* v0.4.8: the nvs task MUST run on an internal-RAM stack. Bare
 * xTaskCreate() allocates the stack via pvPortMalloc(MALLOC_CAP_DEFAULT);
 * with SPIRAM_USE_MALLOC=y + ALWAYSINTERNAL=16384, a 4096-word
 * (16384-byte) stack is NOT < 16384, so the allocator placed it in PSRAM
 * and the first NVS->flash->cache-disable panicked
 * (esp_task_stack_is_sane_cache_disabled, v0.4.7 boot loop). A static
 * DRAM_ATTR stack is linker-guaranteed in DRAM — no allocator roulette. */
#define NVS_TASK_STACK_WORDS 4096
static DRAM_ATTR StackType_t nvs_task_stack_[NVS_TASK_STACK_WORDS];
static StaticTask_t nvs_task_tcb_;

static void nvs_task_fn(void *arg)
{
    (void)arg;
    /* v0.4.8: hard guarantee — fail fast at boot if the broker ever runs
     * on a non-internal stack again. The static DRAM_ATTR stack above is
     * linker-guaranteed; this assert catches any future regression (e.g.
     * someone switching back to xTaskCreate). */
    {
        int stack_probe;
        assert(esp_ptr_internal(&stack_probe));
    }
    ESP_LOGI(TAG, "nvs task: static stack @%p (%u words, internal DRAM)",
             (void *)nvs_task_stack_, (unsigned)NVS_TASK_STACK_WORDS);
    nvs_handle_t h;
    esp_err_t err = nvs_open(SEAL_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open '%s' failed: %s", SEAL_NVS_NS,
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        nvs_task_req_t r;
        if (xQueueReceive(nvs_q_, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* v0.4.3: cross-namespace requests open their namespace for
         * this request only. Rare (device name, wifi creds), so the
         * open/close cost is irrelevant. */
        nvs_handle_t wh = h;
        bool close_after = false;
        if (r.ns[0] != '\0' && strcmp(r.ns, SEAL_NVS_NS) != 0) {
            if (nvs_open(r.ns, NVS_READWRITE, &wh) != ESP_OK) {
                ESP_LOGE(TAG, "nvs open '%s' failed", r.ns);
                err = ESP_FAIL;
                goto complete;
            }
            close_after = true;
        }
        err = ESP_OK;
        switch (r.op) {
        case NVS_TASK_U64:
            err = nvs_set_u64(wh, r.key, r.v.u64);
            break;
        case NVS_TASK_U8:
            err = nvs_set_u8(wh, r.key, r.v.u8);
            break;
        case NVS_TASK_BLOB:
            err = nvs_set_blob(wh, r.key, r.v.blob.buf, r.v.blob.len);
            break;
        case NVS_TASK_STR:
            err = nvs_set_str(wh, r.key, r.v.str);
            break;
        case NVS_TASK_DEL:
            err = nvs_erase_key(wh, r.key);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK; /* unpin of nothing is fine */
            }
            break;
        /* v0.4.7: reads. Fill done->result; no commit needed. */
        case NVS_TASK_READ_U64:
            if (r.done) {
                err = nvs_get_u64(wh, r.key, &r.done->result.u64);
            } else {
                err = ESP_ERR_INVALID_STATE;
            }
            break;
        case NVS_TASK_READ_U8:
            if (r.done) {
                err = nvs_get_u8(wh, r.key, &r.done->result.u8);
            } else {
                err = ESP_ERR_INVALID_STATE;
            }
            break;
        case NVS_TASK_READ_BLOB:
            if (r.done) {
                size_t blen = sizeof(r.done->result.blob.buf);
                err = nvs_get_blob(wh, r.key, r.done->result.blob.buf,
                                   &blen);
                if (err == ESP_OK) {
                    r.done->result.blob.len = blen;
                }
            } else {
                err = ESP_ERR_INVALID_STATE;
            }
            break;
        case NVS_TASK_READ_STR:
            if (r.done) {
                size_t slen = sizeof(r.done->result.str.str);
                err = nvs_get_str(wh, r.key, r.done->result.str.str,
                                  &slen);
                if (err == ESP_OK) {
                    r.done->result.str.len = slen;
                }
            } else {
                err = ESP_ERR_INVALID_STATE;
            }
            break;
        }
        /* Reads skip the commit; writes commit on success. */
        if (err == ESP_OK && r.op <= NVS_TASK_DEL) {
            err = nvs_commit(wh);
        }
        if (err != ESP_OK) {
            /* v0.4.7: NOT_FOUND on reads is normal (optional keys
             * absent at first boot) — don't create error noise. */
            if (err == ESP_ERR_NVS_NOT_FOUND && r.op > NVS_TASK_DEL) {
                ESP_LOGD(TAG, "nvs read '%s/%s': not found",
                         r.ns, r.key);
            } else {
                ESP_LOGE(TAG, "nvs %s '%s/%s' failed: %s",
                         r.op <= NVS_TASK_DEL ? "write" : "read",
                         r.ns, r.key, esp_err_to_name(err));
            }
        }
        if (close_after) {
            nvs_close(wh);
        }
    complete:
        /* Response path (John: must be equally lossless).
         * The broker NEVER drops a response. done is always non-NULL
         * for sync ops (we have no fire-and-forget APIs). We fill
         * err and signal; the caller is blocked on the semaphore
         * (or has panicked, in which case we're aborting too).
         * xSemaphoreGive cannot fail here: the semaphore was created
         * empty and is given exactly once per request. If it does,
         * something is corrupt — panic. */
        if (r.done) {
            r.done->err = err;
            if (xSemaphoreGive(r.done->sem) != pdTRUE) {
                ESP_LOGE(TAG, "FATAL: nvs response semaphore give failed "
                         "for '%s/%s'; aborting", r.ns, r.key);
                abort();
            }
            /* Caller owns done cleanup after xSemaphoreTake. We never
             * free it here — no UAF, no leak (unless we panicked). */
        }
    }
}

/* v0.4.7: ALL NVS operations are synchronous. No fire-and-forget.
 *
 * John's FIFO reliability contract:
 * - Single FIFO queue, strictly ordered. The broker processes one
 *   op at a time, in enqueue order. No concurrent NVS access, ever.
 * - Callers block until completion. The caller blocks on done->sem
 *   until the broker signals. No fire-and-forget, no polling.
 * - NEVER drop, NEVER reject. If xQueueSend would fail (queue full),
 *   we PANIC — a full queue means the broker is deadlocked or NVS
 *   is hung, both system-fatal. We never return an error the caller
 *   might ignore.
 * - Response path is equally lossless. If the broker doesn't signal
 *   within timeout_ms, we PANIC — the broker accepted our request
 *   but never completed it, so it's dead.
 * - ONLY NVS hardware errors propagate to the caller (ESP_ERR_NVS_*,
 *   flash errors). Queue management is never a failure source.
 *
 * done is allocated in INTERNAL RAM. The broker writes done->result
 * while flash/cache operations occur; PSRAM-backed done would
 * reintroduce the cache-disabled crash.
 *
 * Unified plumbing for all sync ops (reads and writes). */
static esp_err_t nvs_op_sync(nvs_task_op_t op, const char *ns,
                             const char *key, const void *val,
                             size_t val_len, nvs_task_done_t **done_out,
                             uint32_t timeout_ms)
{
    if (!key || !key[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nvs_q_) {
        /* Task not created yet — this is a programming error (init
         * order), not a runtime condition. Panic: silently failing
         * an NVS op at boot would corrupt state. */
        ESP_LOGE(TAG, "FATAL: nvs task not running; op '%s' cannot "
                 "proceed; aborting", key);
        abort();
    }
    /* Internal RAM: see contract above. */
    nvs_task_done_t *done = (nvs_task_done_t *)heap_caps_calloc(
        1, sizeof(*done), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!done) {
        return ESP_ERR_NO_MEM;
    }
    done->sem = xSemaphoreCreateBinary();
    if (!done->sem) {
        heap_caps_free(done);
        return ESP_ERR_NO_MEM;
    }
    done->err = ESP_ERR_TIMEOUT; /* default; broker overwrites */

    nvs_task_req_t r;
    memset(&r, 0, sizeof(r));
    r.op = op;
    strncpy(r.ns, ns ? ns : "", sizeof(r.ns) - 1);
    strncpy(r.key, key, sizeof(r.key) - 1);
    switch (op) {
    case NVS_TASK_U64:
        if (val) {
            r.v.u64 = *(const uint64_t *)val;
        }
        break;
    case NVS_TASK_U8:
        if (val) {
            r.v.u8 = *(const uint8_t *)val;
        }
        break;
    case NVS_TASK_BLOB:
        if (val && val_len > 0) {
            if (val_len > sizeof(r.v.blob.buf)) {
                vSemaphoreDelete(done->sem);
                heap_caps_free(done);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(r.v.blob.buf, val, val_len);
            r.v.blob.len = val_len;
        }
        break;
    case NVS_TASK_STR:
        if (val) {
            size_t slen = strlen((const char *)val);
            if (slen >= sizeof(r.v.str)) {
                vSemaphoreDelete(done->sem);
                heap_caps_free(done);
                return ESP_ERR_INVALID_SIZE;
            }
            strcpy(r.v.str, (const char *)val);
        }
        break;
    case NVS_TASK_DEL:
        /* No value. */
        break;
    default:
        /* Reads: no request value to pack. */
        break;
    }
    r.done = done;

    /* Enqueue: BLOCK up to NVS_ENQUEUE_TIMEOUT_MS. If the queue is
     * still full, the broker is dead — PANIC (John: never drop). */
    if (xQueueSend(nvs_q_, &r,
                   pdMS_TO_TICKS(NVS_ENQUEUE_TIMEOUT_MS)) != pdPASS) {
        ESP_LOGE(TAG, "FATAL: nvs queue full after %d ms; broker "
                 "deadlocked or NVS hung; aborting",
                 NVS_ENQUEUE_TIMEOUT_MS);
        /* done leaks, but we're aborting. */
        abort();
    }

    /* Wait for the broker. If it doesn't respond within timeout_ms,
     * the broker is dead — PANIC (John: response path lossless). */
    if (xSemaphoreTake(done->sem,
                       pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "FATAL: nvs task did not complete '%s/%s' within "
                 "%lu ms; broker deadlocked or NVS hung; aborting",
                 r.ns, r.key, (unsigned long)timeout_ms);
        /* done leaks, but we're aborting. */
        abort();
    }

    esp_err_t err = done->err;
    if (done_out) {
        /* Caller wants the result (reads); it owns cleanup. */
        *done_out = done;
    } else {
        vSemaphoreDelete(done->sem);
        heap_caps_free(done);
    }
    return err;
}

/* Helper: run a sync op and discard done (writes). */
static esp_err_t nvs_write_op(nvs_task_op_t op, const char *ns,
                              const char *key, const void *val,
                              size_t val_len, uint32_t timeout_ms)
{
    return nvs_op_sync(op, ns, key, val, val_len, NULL, timeout_ms);
}

esp_err_t seal_dev_nvs_write_u64_sync(const char *ns, const char *key,
                                      uint64_t val, uint32_t timeout_ms)
{
    return nvs_write_op(NVS_TASK_U64, ns, key, &val, sizeof(val),
                        timeout_ms);
}

esp_err_t seal_dev_nvs_write_u8_sync(const char *ns, const char *key,
                                     uint8_t val, uint32_t timeout_ms)
{
    return nvs_write_op(NVS_TASK_U8, ns, key, &val, sizeof(val),
                        timeout_ms);
}

esp_err_t seal_dev_nvs_write_blob_sync(const char *ns, const char *key,
                                       const void *data, size_t len,
                                       uint32_t timeout_ms)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_write_op(NVS_TASK_BLOB, ns, key, data, len, timeout_ms);
}

esp_err_t seal_dev_nvs_write_str_sync(const char *ns, const char *key,
                                      const char *val, uint32_t timeout_ms)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_write_op(NVS_TASK_STR, ns, key, val, strlen(val) + 1,
                        timeout_ms);
}

esp_err_t seal_dev_nvs_delete_sync(const char *ns, const char *key,
                                   uint32_t timeout_ms)
{
    return nvs_write_op(NVS_TASK_DEL, ns, key, NULL, 0, timeout_ms);
}

esp_err_t seal_dev_nvs_read_u8_sync(const char *ns, const char *key,
                                    uint8_t *out, uint32_t timeout_ms)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_task_done_t *done = NULL;
    esp_err_t err = nvs_op_sync(NVS_TASK_READ_U8, ns, key, NULL, 0,
                               &done, timeout_ms);
    if (err == ESP_OK && done) {
        *out = done->result.u8;
    }
    if (done) {
        vSemaphoreDelete(done->sem);
        heap_caps_free(done);
    }
    return err;
}

esp_err_t seal_dev_nvs_read_u64_sync(const char *ns, const char *key,
                                     uint64_t *out, uint32_t timeout_ms)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_task_done_t *done = NULL;
    esp_err_t err = nvs_op_sync(NVS_TASK_READ_U64, ns, key, NULL, 0,
                               &done, timeout_ms);
    if (err == ESP_OK && done) {
        *out = done->result.u64;
    }
    if (done) {
        vSemaphoreDelete(done->sem);
        heap_caps_free(done);
    }
    return err;
}

esp_err_t seal_dev_nvs_read_str_sync(const char *ns, const char *key,
                                     char *out, size_t *inout_len,
                                     uint32_t timeout_ms)
{
    if (!out || !inout_len || *inout_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_task_done_t *done = NULL;
    esp_err_t err = nvs_op_sync(NVS_TASK_READ_STR, ns, key, NULL, 0,
                               &done, timeout_ms);
    if (err == ESP_OK && done) {
        size_t n = done->result.str.len;
        /* done->result.str.len includes NUL (nvs_get_str convention).
         * Clamp to the caller's buffer. */
        if (n > *inout_len) {
            n = *inout_len;
        }
        memcpy(out, done->result.str.str, n);
        if (n == *inout_len) {
            out[n - 1] = '\0';
        }
        *inout_len = n;
    }
    if (done) {
        vSemaphoreDelete(done->sem);
        heap_caps_free(done);
    }
    return err;
}

esp_err_t seal_dev_nvs_read_blob_sync(const char *ns, const char *key,
                                      void *out, size_t *inout_len,
                                      uint32_t timeout_ms)
{
    if (!out || !inout_len || *inout_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_task_done_t *done = NULL;
    esp_err_t err = nvs_op_sync(NVS_TASK_READ_BLOB, ns, key, NULL, 0,
                               &done, timeout_ms);
    if (err == ESP_OK && done) {
        size_t n = done->result.blob.len;
        if (n > *inout_len) {
            /* Caller's buffer too small. This is a caller bug for
             * our fixed-size keys; return the size so they can
             * retry with a bigger buffer. */
            *inout_len = n;
            vSemaphoreDelete(done->sem);
            heap_caps_free(done);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, done->result.blob.buf, n);
        *inout_len = n;
    }
    if (done) {
        vSemaphoreDelete(done->sem);
        heap_caps_free(done);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Stress test (John: verify FIFO, no drops, no stale reads)           */
/* ------------------------------------------------------------------ */

/* NVS stress test. Verifies the v0.4.7 contract under concurrent load:
 * - Phase 1 (ordering): write1 -> write2 -> read == write2. Proves
 *   FIFO ordering and no stale reads (single task).
 * - Phase 2 (concurrent, isolated keys): N PSRAM-stack tasks x K
 *   iterations, each writing/reading its own key. Proves zero drops,
 *   zero hangs, all callers unblock under true concurrency.
 * - Phase 3 (concurrent, shared key): N tasks x K iterations on ONE
 *   key, with a mutex around each task's write->read pair (so the
 *   pair is adjacent in the queue). Proves FIFO serialization: each
 *   task's read sees its own write, so no op was dropped or
 *   reordered between them.
 *
 * Test tasks run on PSRAM stacks (task_psram_create) — the whole
 * point is that PSRAM-stack tasks can safely use NVS via the broker.
 * Runs on the caller's task for Phase 1, then spawns workers.
 * Cleans up all test keys. Returns 0 on pass, -1 on fail.
 * Invoke via {"t":"dbg","cmd":"nvs_stress"} (serial console only). */

#define NVS_STRESS_TASKS 4
#define NVS_STRESS_ITERS 10

typedef struct {
    int         id;
    int         iters;
    volatile int *failures;
    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t pair_lock; /* Phase 3 only; NULL for Phase 2 */
} nvs_stress_arg_t;

static void nvs_stress_worker(void *arg)
{
    nvs_stress_arg_t *a = (nvs_stress_arg_t *)arg;
    char key[16];
    int local_fails = 0;

    for (int k = 0; k < a->iters; k++) {
        uint64_t val = ((uint64_t)a->id << 32) | (uint64_t)k;
        uint64_t got = 0;
        esp_err_t err;

        if (a->pair_lock) {
            /* Phase 3: shared key, mutex makes write->read adjacent
             * in the queue. */
            xSemaphoreTake(a->pair_lock, portMAX_DELAY);
            err = seal_dev_nvs_write_u64_sync("", "nvs_shr", val, 5000);
            if (err == ESP_OK) {
                err = seal_dev_nvs_read_u64_sync("", "nvs_shr", &got,
                                                 5000);
            }
            xSemaphoreGive(a->pair_lock);
            if (err != ESP_OK || got != val) {
                ESP_LOGE(TAG, "stress phase3 task %d iter %d: "
                         "err=%s got=0x%llx want=0x%llx",
                         a->id, k, esp_err_to_name(err),
                         (unsigned long long)got,
                         (unsigned long long)val);
                local_fails++;
            }
        } else {
            /* Phase 2: own key, no locking — true concurrency. */
            snprintf(key, sizeof(key), "nvs_t%d", a->id);
            err = seal_dev_nvs_write_u64_sync("", key, val, 5000);
            if (err == ESP_OK) {
                err = seal_dev_nvs_read_u64_sync("", key, &got, 5000);
            }
            if (err != ESP_OK || got != val) {
                ESP_LOGE(TAG, "stress phase2 task %d iter %d: "
                         "err=%s got=0x%llx want=0x%llx",
                         a->id, k, esp_err_to_name(err),
                         (unsigned long long)got,
                         (unsigned long long)val);
                local_fails++;
            }
        }
    }

    if (local_fails) {
        /* Atomic increment (workers run concurrently). */
        __atomic_fetch_add(a->failures, local_fails, __ATOMIC_SEQ_CST);
    }
    xSemaphoreGive(a->done_sem);
    vTaskDelete(NULL);
}

int seal_dev_nvs_stress_test(void)
{
    esp_err_t err;
    uint64_t got = 0;

    ESP_LOGI(TAG, "nvs stress: phase 1 (ordering, single task)");
    /* Phase 1: write1 -> write2 -> read must return write2. */
    err = seal_dev_nvs_write_u64_sync("", "nvs_tst", 0x1111111111111111ULL,
                                      5000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stress phase1: write1 failed: %s",
                 esp_err_to_name(err));
        return -1;
    }
    err = seal_dev_nvs_write_u64_sync("", "nvs_tst", 0x2222222222222222ULL,
                                      5000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stress phase1: write2 failed: %s",
                 esp_err_to_name(err));
        return -1;
    }
    err = seal_dev_nvs_read_u64_sync("", "nvs_tst", &got, 5000);
    if (err != ESP_OK || got != 0x2222222222222222ULL) {
        ESP_LOGE(TAG, "stress phase1: ORDERING FAIL: err=%s "
                 "got=0x%llx want=0x2222222222222222",
                 esp_err_to_name(err), (unsigned long long)got);
        return -1;
    }
    /* No stale reads: write -> immediate read sees the new value. */
    err = seal_dev_nvs_write_u64_sync("", "nvs_tst", 0x3333333333333333ULL,
                                      5000);
    if (err == ESP_OK) {
        err = seal_dev_nvs_read_u64_sync("", "nvs_tst", &got, 5000);
    }
    if (err != ESP_OK || got != 0x3333333333333333ULL) {
        ESP_LOGE(TAG, "stress phase1: STALE READ: err=%s got=0x%llx",
                 esp_err_to_name(err), (unsigned long long)got);
        return -1;
    }
    ESP_LOGI(TAG, "nvs stress: phase 1 PASS");

    /* Phases 2 and 3: spawn PSRAM-stack workers. */
    for (int phase = 2; phase <= 3; phase++) {
        ESP_LOGI(TAG, "nvs stress: phase %d (%d tasks x %d iters, "
                 "PSRAM stacks)", phase, NVS_STRESS_TASKS,
                 NVS_STRESS_ITERS);
        SemaphoreHandle_t pair_lock = NULL;
        if (phase == 3) {
            pair_lock = xSemaphoreCreateMutex();
            if (!pair_lock) {
                return -1;
            }
        }
        volatile int phase_failures = 0;
        nvs_stress_arg_t args[NVS_STRESS_TASKS];
        SemaphoreHandle_t done_sems[NVS_STRESS_TASKS];
        task_psram_handle_t *handles[NVS_STRESS_TASKS];

        for (int i = 0; i < NVS_STRESS_TASKS; i++) {
            done_sems[i] = xSemaphoreCreateBinary();
            if (!done_sems[i]) {
                return -1;
            }
            args[i].id = i;
            args[i].iters = NVS_STRESS_ITERS;
            args[i].failures = (volatile int *)&phase_failures;
            args[i].done_sem = done_sems[i];
            args[i].pair_lock = pair_lock;
            char name[16];
            snprintf(name, sizeof(name), "nvs_st%d", i);
            handles[i] = task_psram_create(nvs_stress_worker, name,
                                           4096, &args[i], 5,
                                           tskNO_AFFINITY);
            if (!handles[i]) {
                ESP_LOGE(TAG, "stress phase%d: task create failed", phase);
                return -1;
            }
        }
        /* Wait for all workers. If any hangs (dropped op), we block
         * here — the task watchdog will fire, which is the correct
         * loud failure (John: never silently drop). */
        for (int i = 0; i < NVS_STRESS_TASKS; i++) {
            xSemaphoreTake(done_sems[i], portMAX_DELAY);
            vSemaphoreDelete(done_sems[i]);
        }
        if (pair_lock) {
            vSemaphoreDelete(pair_lock);
        }
        if (phase_failures != 0) {
            ESP_LOGE(TAG, "stress phase%d: FAIL (%d mismatches)",
                     phase, phase_failures);
            return -1;
        }
        ESP_LOGI(TAG, "nvs stress: phase %d PASS", phase);
    }

    /* Cleanup test keys. */
    seal_dev_nvs_delete_sync("", "nvs_tst", 5000);
    seal_dev_nvs_delete_sync("", "nvs_shr", 5000);
    for (int i = 0; i < NVS_STRESS_TASKS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "nvs_t%d", i);
        seal_dev_nvs_delete_sync("", key, 5000);
    }

    ESP_LOGI(TAG, "nvs stress: ALL PASS");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t seal_dev_init(void)
{
    if (inited_) {
        return ESP_OK;
    }
    nvs_q_ = xQueueCreate(NVS_TASK_QDEPTH, sizeof(nvs_task_req_t));
    if (!nvs_q_) {
        return ESP_ERR_NO_MEM;
    }
    verify_lock_ = xSemaphoreCreateMutex();
    if (!verify_lock_) {
        return ESP_ERR_NO_MEM;
    }

    /* v0.4.7: create the nvs task BEFORE loading persisted state, so
     * the init reads also go through the task (the ONLY task that
     * touches NVS). */
    /* v0.4.8: xTaskCreateStatic with the DRAM_ATTR static stack above.
     * Bare xTaskCreate is UNSAFE for flash-touching tasks: with
     * SPIRAM_USE_MALLOC the allocator may place the stack in PSRAM
     * (the v0.4.7 16384-byte threshold edge), and NVS->flash->
     * cache-disable then panics. The audit (tools/audit_psram_flash.py)
     * now enforces this pattern for every flash-touching task. */
    if (xTaskCreateStatic(nvs_task_fn, "nvs_task", NVS_TASK_STACK_WORDS,
                          NULL, 5, nvs_task_stack_,
                          &nvs_task_tcb_) == NULL) {
        return ESP_FAIL;
    }

    /* Load persisted state via the nvs task. This runs on the app_main
     * task during link_init, after nvs_flash_init. */
    size_t len = sizeof(cmd_pk_);
    if (seal_dev_nvs_read_blob_sync("", NVS_KEY_CMD_PK, cmd_pk_, &len,
                                    5000) == ESP_OK &&
        len == sizeof(cmd_pk_)) {
        has_key_ = true;
    }
    len = sizeof(tel_seed_);
    if (seal_dev_nvs_read_blob_sync("", NVS_KEY_TEL_SEED, tel_seed_, &len,
                                    5000) == ESP_OK &&
        len == sizeof(tel_seed_)) {
        has_tel_seed_ = true;
    }
    uint8_t s8 = 0;
    if (seal_dev_nvs_read_u8_sync("", NVS_KEY_STRICT, &s8, 5000) == ESP_OK) {
        strict_ = (s8 != 0);
    }
    uint64_t hw = 0;
    if (seal_dev_nvs_read_u64_sync("", NVS_KEY_CMD_HW, &hw, 5000) == ESP_OK) {
        last_persisted_ = hw;
        hw_valid_ = true;
    }

    /* D4: boot floor = stored + 32 (0 if never stored). */
    floor_ = hw_valid_ ? last_persisted_ + SEAL_HW_PERSIST_EVERY : 0;
    high_water_ = floor_;
    replay_.high_water = floor_;
    policy_.strict = strict_;
    policy_.ts_window_ms = 0; /* no SNTP on the S3 */
    policy_.grace_warn_every = 10;
    policy_.grace_warns = 0;

    inited_ = true;
    ESP_LOGI(TAG,
             "init: key=%d tel=%d strict=%d floor=%llu (stored %s)",
             has_key_ ? 1 : 0, has_tel_seed_ ? 1 : 0,
             strict_ ? 1 : 0, (unsigned long long)floor_,
             hw_valid_ ? "yes" : "no");
    return ESP_OK;
}

bool seal_dev_has_commander_key(void)
{
    return has_key_;
}

uint64_t seal_dev_floor(void)
{
    return floor_;
}

uint64_t seal_dev_high_water(void)
{
    return high_water_;
}

uint32_t seal_dev_verified_count(void)
{
    return verified_count_;
}

uint32_t seal_dev_rejected_count(void)
{
    return rejected_count_;
}

bool seal_dev_strict(void)
{
    return strict_;
}

/* ------------------------------------------------------------------ */
/* Envelope verification (control plane, kind-2).                       */
/* ------------------------------------------------------------------ */

wms2_rc seal_dev_verify_envelope(const uint8_t *env, size_t env_len,
                                 const char *claimed_topic,
                                 const uint8_t **body_out,
                                 size_t *body_len_out,
                                 wms2_hdr *hdr_out)
{
    if (!env || !claimed_topic || !body_out || !body_len_out || !hdr_out) {
        return WMS2_E_SHORT;
    }
    size_t tlen = strlen(claimed_topic);

    /* E_SHORT *before* any crypto: bound the topic and the body first.
     * Topic binding uses exactly the claimed string, never a local
     * default. */
    if (tlen == 0 || tlen > WMS2_MAX_TOPIC) {
        return WMS2_E_SHORT;
    }
    if (env_len < WMS2_HDR_LEN + WMS2_TAG_LEN) {
        return WMS2_E_SHORT;
    }
    if (memcmp(env, WMS2_MAGIC, 4) != 0) {
        return WMS2_E_MAGIC;
    }
    uint8_t kind = env[4];
    size_t alen = (kind == WMS2_KIND_CTRL) ? WMS2_SIG_LEN :
                  (kind == WMS2_KIND_TELEM) ? WMS2_TAG_LEN : 0;
    if (alen == 0) {
        return WMS2_E_KIND;
    }
    size_t blen = env_len - WMS2_HDR_LEN - alen;
    if (blen > SEAL_MAX_CMD_BODY) {
        return WMS2_E_SHORT;
    }
    if (wms2_auth_bytes_needed(tlen, blen) > sizeof(verify_scratch_)) {
        return WMS2_E_SHORT; /* unreachable given the bounds above */
    }

    if (xSemaphoreTake(verify_lock_, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "verify lock timeout");
        return WMS2_E_SHORT;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint8_t *ed_pk = has_key_ ? cmd_pk_ : NULL;
    wms2_rc rc = wms2_verify(env, env_len, claimed_topic,
                             NULL /* kind-1 key: control plane is kind-2 */,
                             ed_pk,
                             verify_scratch_, sizeof(verify_scratch_),
                             now_ms, &replay_, &policy_, hdr_out,
                             body_out, body_len_out);
    if (rc == WMS2_OK) {
        verified_count_++;
        high_water_ = replay_.high_water; /* adopted after all gates */
        /* D4 persist: every 32nd accept, or as soon as the high-water
         * runs 32 past the last persisted value (ctr gap-jumps).
         * v0.4.7: synchronous — if the persist fails, do NOT advance
         * last_persisted_; we'll retry on the next accept. (With the
         * old fire-and-forget we couldn't know.) */
        accepted_since_persist_++;
        if (accepted_since_persist_ >= SEAL_HW_PERSIST_EVERY ||
            high_water_ >= last_persisted_ + SEAL_HW_PERSIST_EVERY) {
            if (seal_dev_nvs_write_u64_sync("", NVS_KEY_CMD_HW,
                                            high_water_,
                                            5000) == ESP_OK) {
                last_persisted_ = high_water_;
                hw_valid_ = true;
                accepted_since_persist_ = 0;
            } else {
                ESP_LOGE(TAG, "high-water persist failed; will retry");
            }
        }
    } else if (rc == WMS2_OK_GRACE || rc == WMS2_OK_DUP) {
        /* Honored without advancing the high-water. */
        verified_count_++;
    } else {
        rejected_count_++;
        seclog_add(wms2_rc_str(rc));
    }

    /* Contract: body_out points into OUR static scratch (valid until
     * the next call), not into the caller's envelope buffer. The auth
     * assembly is done with the scratch by now, so reuse it. */
    if (rc >= 0 && *body_len_out <= sizeof(verify_scratch_)) {
        memmove(verify_scratch_, *body_out, *body_len_out);
        *body_out = verify_scratch_;
    }
    xSemaphoreGive(verify_lock_);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Telemetry KDF: labeled BLAKE2b-keyed, byte-identical to the host's  */
/* mqtt_seal.py derive_tel_keys and the page's derivation.             */
/* ------------------------------------------------------------------ */

bool seal_dev_tel_keys(uint8_t k_mac[32], uint8_t k_aead[32])
{
    if (!has_tel_seed_ || !k_mac || !k_aead) {
        return false;
    }
    crypto_blake2b_keyed(k_mac, 32, tel_seed_, sizeof(tel_seed_),
                         (const uint8_t *)"WMS2-tel-mac-v1",
                         sizeof("WMS2-tel-mac-v1") - 1);
    crypto_blake2b_keyed(k_aead, 32, tel_seed_, sizeof(tel_seed_),
                         (const uint8_t *)"WMS2-tel-aead-v1",
                         sizeof("WMS2-tel-aead-v1") - 1);
    return true;
}

/* ------------------------------------------------------------------ */
/* Serial provisioning (bench = trusted physical channel).             */
/* ------------------------------------------------------------------ */

static unsigned hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned)(c - 'a' + 10);
    }
    return (unsigned)(c - 'A' + 10);
}

static bool parse_hex64(const char *hex, uint8_t out[32])
{
    if (!hex) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)hex[i])) {
            return false;
        }
    }
    if (hex[64] != '\0') {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        out[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
    }
    return true;
}

int seal_dev_cmd_provision(const char *pk_hex64)
{
    uint8_t pk[32];
    if (!parse_hex64(pk_hex64, pk)) {
        link_respond_err("seal_provision", "pk must be 64 hex chars");
        return -1;
    }
    memcpy(cmd_pk_, pk, sizeof(cmd_pk_));
    crypto_wipe(pk, sizeof(pk));
    has_key_ = true;
    if (seal_dev_nvs_write_blob_sync("", NVS_KEY_CMD_PK, cmd_pk_,
                                     sizeof(cmd_pk_), 5000) != ESP_OK) {
        link_respond_err("seal_provision", "nvs persist failed");
        return -1;
    }
    ESP_LOGI(TAG, "commander key pinned (verify path armed)");
    link_respond_ok("seal_provision");
    return 0;
}

int seal_dev_cmd_unpin(void)
{
    crypto_wipe(cmd_pk_, sizeof(cmd_pk_));
    has_key_ = false;
    if (seal_dev_nvs_delete_sync("", NVS_KEY_CMD_PK, 5000) != ESP_OK) {
        link_respond_err("seal_unpin", "nvs delete failed");
        return -1;
    }
    ESP_LOGI(TAG, "commander key unpinned (verify path disarmed)");
    link_respond_ok("seal_unpin");
    return 0;
}

int seal_dev_cmd_strict(bool on)
{
    strict_ = on;
    if (xSemaphoreTake(verify_lock_, pdMS_TO_TICKS(1000)) == pdTRUE) {
        policy_.strict = on;
        xSemaphoreGive(verify_lock_);
    }
    if (seal_dev_nvs_write_u8_sync("", NVS_KEY_STRICT, on ? 1 : 0,
                                     5000) != ESP_OK) {
        link_respond_err("seal_strict", "nvs persist failed");
        return -1;
    }
    ESP_LOGI(TAG, "seal strict %s", on ? "ON" : "OFF");
    link_respond_ok("seal_strict");
    return 0;
}

int seal_dev_cmd_tel_seed(const char *seed_hex64)
{
    uint8_t seed[32];
    if (!parse_hex64(seed_hex64, seed)) {
        link_respond_err("seal_tel_seed", "seed must be 64 hex chars");
        return -1;
    }
    memcpy(tel_seed_, seed, sizeof(tel_seed_));
    crypto_wipe(seed, sizeof(seed));
    has_tel_seed_ = true;
    if (seal_dev_nvs_write_blob_sync("", NVS_KEY_TEL_SEED, tel_seed_,
                                     sizeof(tel_seed_), 5000) != ESP_OK) {
        link_respond_err("seal_tel_seed", "nvs persist failed");
        return -1;
    }
    /* The fingerprint identifies the seed without exposing it; the host
     * compares it against the page's fingerprint to confirm both sides
     * derived from the same seed. */
    uint8_t fp32[32];
    crypto_blake2b_keyed(fp32, sizeof(fp32), tel_seed_, sizeof(tel_seed_),
                         (const uint8_t *)"WMS2-tel-fp-v1",
                         sizeof("WMS2-tel-fp-v1") - 1);
    char fp[17];
    for (int i = 0; i < 8; i++) {
        snprintf(fp + 2 * i, 3, "%02x", fp32[i]);
    }
    fp[16] = '\0';
    crypto_wipe(fp32, sizeof(fp32));
    ESP_LOGI(TAG, "telemetry seed stored, fp=%s", fp);
    link_respond_ok("seal_tel_seed");
    return 0;
}
