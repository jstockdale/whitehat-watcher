#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_io_expander.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the Himax camera path (I2C expander + SPI2 + SSCMA client).
 *  Best-effort: returns ESP_FAIL if the camera isn't reachable, but the rest
 *  of the firmware keeps working. */
esp_err_t eyes_init(void);

bool eyes_ready(void);
/* v0.4.3: true once the Himax AT handshake completed (on_connect fired).
 * Distinct from eyes_ready() (init sequence ok). Snapshots require
 * link_up, not just ready. */
bool eyes_link_up(void);

/** IO expander handle created by eyes_init() (NULL before init or when the
 *  expander isn't reachable). Shared with the display component, which uses
 *  it to switch the LCD power rail. */
esp_io_expander_handle_t eyes_io_expander(void);

/** Asynchronous result callback, fired from the snapshot task context.
 *  `mid` is the originating command's _mid (may be ""), echoed so the
 *  host can correlate the completion with the command. */
typedef void (*eyes_emit_cb_t)(const char *type, int id, const char *mid,
                               int bytes, int ms, const char *err);
void eyes_set_emit(eyes_emit_cb_t cb);

/** Capture one still via SSCMA INVOKE and HTTP-POST the JPEG to
 *  http://host:port/still?id=<id>. Runs asynchronously; completion is
 *  reported as {"t":"still_sent","id":N,"bytes":B,"ms":T} or
 *  {"t":"err","cmd":"snap","msg":...}, both carrying the _mid.
 *  Returns ESP_ERR_INVALID_STATE if Wi-Fi isn't up, ESP_ERR_INVALID_STATE
 *  (camera) if eyes aren't ready, ESP_ERR_INVALID_ARG on bad host.
 *  fresh=true (default): primer + real capture, POSTs a new image.
 *  fresh=false: bare INVOKE, POSTs whatever frame is currently buffered
 *  (the "old" image) without triggering a new capture first.
 *  `mid` is the command's _mid for correlation (NULL ok). */
esp_err_t eyes_snap(const char *host, int port, int id, bool fresh,
                    const char *mid);

/** Full camera recovery: RST pulse (+ optional Himax rail power-cycle),
 *  then AT re-handshake. The caller must guarantee no snapshot is in
 *  flight (the auto path runs inside the snap worker that owns the slot;
 *  eyes_reset refuses while snap_busy_). Serial-only callers. Takes
 *  ~2-4 s. Returns ESP_OK if the Himax is answering again. */
esp_err_t eyes_recover_camera(bool cycle_rail);

/** True while a snapshot worker owns the in-flight slot. */
bool eyes_snap_busy(void);

/* ------------------------------------------------------------------ */
/* Himax debug tooling (v0.2.8)                                        */
/* ------------------------------------------------------------------ */

/** Fill o with live Himax status: ready/link flags, AT+ID/NAME/VER query
 *  results, model slot info, sensor state, and last-snapshot stats.
 *  Best-effort: a Himax that doesn't answer a query leaves that field
 *  null/false instead of failing the whole report. */
void eyes_status(cJSON *o);

/** Send one raw AT command (e.g. "AT+INFO?") to the Himax and copy the
 *  reply payload JSON into out (truncated to out_len - 1 chars).
 *  Synchronous; waits up to ~3 s for the reply. */
esp_err_t eyes_at(const char *cmd, char *out, size_t out_len);

/** Invoke without JPEG (results-only). Tests whether the Himax can produce
 *  frames at the current sensor resolution when image encoding is skipped.
 *  Synchronous; waits up to ~10s. Returns ESP_OK if the Himax replied
 *  (even with zero boxes), or an error (e.g. timeout) if frames are dropped.
 *  If this succeeds on opt 5 while snapshots fail, the issue is 100% the
 *  Himax JPEG encode buffer, not the sensor pipeline. */
esp_err_t eyes_invoke_noimg(void);

/** Set the Himax sensor resolution opt at runtime (no reboot needed).
 *  opt_id follows the SSCMA AT+SENSOR numbering; the new mode is verified
 *  by re-querying the sensor and reported in the returned opt_detail.
 *  Returns ESP_OK and fills *applied_opt with the opt_id the Himax actually
 *  switched to (may differ from opt_id if the Himax clamps it).
 *  Pass opt_id < 0 to query only (no change). */
esp_err_t eyes_set_sensor_opt(int opt_id, int *applied_opt,
                              char *detail_out, size_t detail_len);

/** Analysis of one captured JPEG frame (no Wi-Fi needed). */
typedef struct {
    int bytes;          /* JPEG size in bytes */
    int ms;             /* capture time */
    bool soi;           /* starts with FFD8 */
    bool eoi;           /* ends with FFD9 */
    uint32_t fnv1a;     /* checksum of the frame */
    uint8_t min_b;      /* byte stats: flat image => min==max */
    uint8_t max_b;
    uint8_t mean_b;
    char head_hex[33];  /* first 16 bytes as hex */
    char tail_hex[17];  /* last 8 bytes as hex */
    bool fresh;         /* fnv1a differs from the previous snapshot's frame */
    const char *err;    /* NULL on success */
} eyes_snaptest_result_t;

/** Completion callback for the async snapshot test.
 *  `mid` is the originating command's _mid (may be ""), echoed so the
 *  host can correlate the completion with the command. */
typedef void (*eyes_snaptest_cb_t)(int id, const char *mid,
                                   const eyes_snaptest_result_t *res);
void eyes_set_snaptest_cb(eyes_snaptest_cb_t cb);

/** Capture one frame via SSCMA INVOKE, analyze the JPEG in PSRAM, and
 *  report through the snaptest callback. No Wi-Fi needed. If send is true,
 *  the frame is also streamed as base64 chunks through the chunk callback
 *  (one call per 768 input bytes -> 1024 base64 chars). Asynchronous
 *  (capture can take several seconds); returns ESP_ERR_INVALID_STATE if
 *  eyes aren't ready or a snapshot is already in flight.
 *  fresh=true (default): primer + real capture, analyzes a new image.
 *  fresh=false: bare INVOKE, analyzes whatever frame is currently buffered
 *  (the "old" image) without triggering a new capture first.
 *  `mid` is the command's _mid for correlation (NULL ok). */
esp_err_t eyes_snap_test_async(int id, bool send, bool fresh, const char *mid);

/** Chunked base64 streaming of the captured frame. seq runs 0..total-1.
 *  `mid` is the originating command's _mid for correlation. */
typedef void (*eyes_snaptest_chunk_cb_t)(int id, const char *mid, int seq,
                                         int total, const char *b64);
void eyes_set_snaptest_chunk_cb(eyes_snaptest_chunk_cb_t cb);

#ifdef __cplusplus
}
#endif
