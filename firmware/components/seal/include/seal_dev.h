/* SPDX-License-Identifier: MIT
 * Phase 2 device-side seal: NVS-pinned commander key, wear-aware
 * replay high-water (D4 zero-budget variant), sealed_fwd handling.
 *
 * D4: persist high-water every SEAL_HW_PERSIST_EVERY accepts; on boot
 * adopt floor = stored + SEAL_HW_PERSIST_EVERY. The persist invariant
 * guarantees executed <= stored + N - 1 at all times, so the floor
 * clears everything already executed: replay budget after crash = 0.
 * The floor is surfaced in status; the commander resyncs with
 * next = max(own_counter, floor) + 1, capped (anti-oracle: reject
 * floor > own + 2^20, or trust the floor only from authenticated
 * status).
 *
 * All NVS access goes through the single nvs task (fleet
 * ESP-target lesson). ts_window_ms = 0 on device (no SNTP).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "wms2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEAL_NVS_NS            "seal"
#define SEAL_HW_PERSIST_EVERY  32u   /* D4: persist every Nth accept */

/* Max sealed command body the device will verify (D6 static scratch
 * is sized from this). Sealed firmware chunks exceed this: in Phase-2
 * bridged mode they stay page-verified; chunked sealed_fwd is future. */
#define SEAL_MAX_CMD_BODY      2048

esp_err_t seal_dev_init(void);   /* call once at boot */
bool      seal_dev_has_commander_key(void);
/* Boot floor for status (0 when nothing persisted yet). */
uint64_t  seal_dev_floor(void);
uint64_t  seal_dev_high_water(void);
uint32_t  seal_dev_verified_count(void);
uint32_t  seal_dev_rejected_count(void);
bool      seal_dev_strict(void);

/* Verify a WMS2 kind-2 envelope against the CLAIMED topic (topic
 * binding: rebuild S with the claimed topic, never a local default).
 * body_out/body_len_out point into seal_dev's static scratch, valid
 * until the next call. Named rejection events are counted for SECLOG.
 * Returns the wms2_rc. */
wms2_rc seal_dev_verify_envelope(const uint8_t *env, size_t env_len,
                                 const char *claimed_topic,
                                 const uint8_t **body_out,
                                 size_t *body_len_out,
                                 wms2_hdr *hdr_out);

/* Serial provisioning handlers (bench = trusted physical channel).
 * Each prints its own ok/err reply via link_send and returns 0/-1. */
int seal_dev_cmd_provision(const char *pk_hex64);  /* {"t":"seal_provision","pk":"…"} */
int seal_dev_cmd_unpin(void);                       /* {"t":"seal_unpin"} */
int seal_dev_cmd_strict(bool on);                   /* {"t":"seal_strict","on":bool} */
int seal_dev_cmd_tel_seed(const char *seed_hex64);  /* {"t":"seal_tel_seed","seed":"…"} */

/* Telemetry keys derived from the provisioned seed with the WMS2
 * labeled KDF (same labels as the page: "WMS2-tel-mac-v1",
 * "WMS2-tel-aead-v1"), so page and device derive identical keys
 * from one seed. Returns false when no seed is provisioned. */
bool seal_dev_tel_keys(uint8_t k_mac[32], uint8_t k_aead[32]);

/* NVS task interface: callable from any task (including PSRAM-stack
 * tasks); the dedicated nvs task (internal stack) performs the actual
 * NVS operation. The task is the ONLY code that calls nvs_open.
 *
 * v0.4.7 BLOCKING/SERIALIZATION CONTRACT (John's requirements —
 * future developers: these are load-bearing guarantees, do not weaken):
 *
 * 1. Single FIFO queue, strictly ordered. Every NVS operation (read
 *    or write) goes through one FreeRTOS queue (depth 32, see
 *    NVS_TASK_QDEPTH in seal_dev.c for the sizing math). The broker
 *    task processes them one at a time, in enqueue order. No
 *    concurrent NVS access, ever.
 *
 * 2. Callers block consistently. Every API below is synchronous: the
 *    caller blocks until the broker completes the operation and
 *    returns the result/ACK. No fire-and-forget, no polling. The
 *    timeout_ms is a dead-broker detector, not a normal return path.
 *
 * 3. No out-of-order writes. FIFO guarantees this: if task A queues
 *    write1 then task B queues write2, write1 commits before write2.
 *
 * 4. No stale reads. The broker NEVER caches NVS values. Every read
 *    goes directly to NVS. Read-after-write consistency is guaranteed
 *    by FIFO ordering: a read queued after a write sees the written
 *    value.
 *
 * 5. Never drop, never reject. If the queue would fill (practically
 *    unreachable at 4.5x headroom), the caller PANICS — a full queue
 *    means the broker is deadlocked or NVS is hung, both system-fatal.
 *    If the broker doesn't respond within timeout_ms, the caller
 *    PANICS. Queue management is never a failure source.
 *
 * 6. Only NVS hardware errors propagate. ESP_ERR_NVS_NOT_FOUND,
 *    flash errors, etc. are returned to the caller. Everything else
 *    (queue full, broker hung, task not running) is a panic.
 *
 * ns="" selects the seal namespace. timeout_ms: generous values only
 * (5000 recommended); NVS ops complete in <10ms typical. */
esp_err_t seal_dev_nvs_write_u64_sync(const char *ns, const char *key,
                                      uint64_t val, uint32_t timeout_ms);
esp_err_t seal_dev_nvs_write_u8_sync(const char *ns, const char *key,
                                     uint8_t val, uint32_t timeout_ms);
esp_err_t seal_dev_nvs_write_blob_sync(const char *ns, const char *key,
                                       const void *data, size_t len,
                                       uint32_t timeout_ms);
esp_err_t seal_dev_nvs_write_str_sync(const char *ns, const char *key,
                                      const char *val, uint32_t timeout_ms);
esp_err_t seal_dev_nvs_delete_sync(const char *ns, const char *key,
                                   uint32_t timeout_ms);
esp_err_t seal_dev_nvs_read_u8_sync(const char *ns, const char *key,
                                    uint8_t *out, uint32_t timeout_ms);
esp_err_t seal_dev_nvs_read_u64_sync(const char *ns, const char *key,
                                     uint64_t *out, uint32_t timeout_ms);
esp_err_t seal_dev_nvs_read_str_sync(const char *ns, const char *key,
                                     char *out, size_t *inout_len,
                                     uint32_t timeout_ms);
esp_err_t seal_dev_nvs_read_blob_sync(const char *ns, const char *key,
                                      void *out, size_t *inout_len,
                                      uint32_t timeout_ms);

/* NVS stress test (John's FIFO reliability verification). Spawns
 * PSRAM-stack workers doing interleaved reads/writes; verifies
 * ordering (write1->write2->read==write2), no stale reads, zero
 * drops, and FIFO serialization. Returns 0 on pass, -1 on fail.
 * Serial console only (takes ~1s, hammers NVS). */
int seal_dev_nvs_stress_test(void);

/* Named SECLOG counters summary for status/heartbeat ("seal_seclog").
 * Writes a compact summary into out (always NUL-terminated). */
void seal_dev_seclog_str(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
