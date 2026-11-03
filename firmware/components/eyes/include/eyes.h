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

/** Capture a RAW8 image from the Himax over the SPI bulk path
 *  (v0.4.14 P1). AT+SPIDUMP captures a fresh frame on the Himax; the frame
 *  is drained with per-chunk CRC16 and verified against the Himax-reported
 *  SHA-256. buf must hold the frame (1296x972 = 1,259,712 bytes at the
 *  default sensor opt, or the active AT+CROP window up to the same byte
 *  budget); out_len receives the actual byte count (NULL ok), out_w/out_h
 *  receive the frame geometry from the SPIDUMP reply (NULL ok).
 *  Synchronous; takes several seconds. Returns ESP_OK on a verified frame,
 *  ESP_ERR_INVALID_STATE if the link isn't up, ESP_ERR_INVALID_SIZE if
 *  buf is too small, ESP_ERR_INVALID_CRC on hash mismatch. */
esp_err_t eyes_raw_pull(uint8_t *buf, size_t buf_len, size_t *out_len,
                        uint32_t *out_w, uint32_t *out_h);

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

/** Analysis of one captured PNG frame (no Wi-Fi needed).
 * v0.4.18.37: Migrated from JPEG to PNG (S3 ROM miniz encoder). */
typedef struct {
    int bytes;          /* PNG size in bytes */
    int ms;             /* capture time */
    bool soi;           /* starts with PNG signature 89504E47 */
    bool eoi;           /* ends with IEND chunk (AE426082) */
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
 *  `mid` is the originating command's _mid for correlation.
 *  v0.4.18.84: Returns false to abort the transfer (e.g. transport stuck).
 *  A dropped chunk corrupts the PNG, so abort is the only safe option. */
typedef bool (*eyes_snaptest_chunk_cb_t)(int id, const char *mid, int seq,
                                         int total, const char *b64);
void eyes_set_snaptest_chunk_cb(eyes_snaptest_chunk_cb_t cb);

/** Chunked base64 streaming of a captured PNG (see eyes_capture_png).
 *  seq runs 0..total-1.
 *  v0.4.18.84: Returns false to abort (see above). */
typedef bool (*eyes_png_chunk_cb_t)(int seq, int total, const char *b64);
void eyes_set_png_chunk_cb(eyes_png_chunk_cb_t cb);

/** Capture a full-resolution frame via AT+SPIDUMP + SPI bulk read,
 *  encode it as PNG (BGGR, SUB filter), and stream the PNG as base64
 *  chunks through the png chunk callback. Returns ESP_OK on success.
 *  v0.4.15 (P4): if an AT+CROP window is active on the Himax, the window
 *  is captured and encoded at its native geometry (from the SPIDUMP
 *  reply); otherwise the standard 1296x972 frame. */
esp_err_t eyes_capture_png(void);

/** Capture a full-resolution frame via AT+SPIDUMP + SPI bulk read,
 *  encode it as PNG (BGGR, SUB filter), and stream the PNG as base64
 *  chunks through the png chunk callback. Returns ESP_OK on success.
 *  v0.4.15 (P4): if an AT+CROP window is active on the Himax, the window
 *  is captured and encoded at its native geometry (from the SPIDUMP
 *  reply); otherwise the standard 1296x972 frame. */

/** Chunked base64 streaming of one P5 strip (see eyes_capture_strips).
 *  strip_idx runs 0..strip_count-1; y0/h describe the strip's window in
 *  the 2592x1944 full-res frame; seq runs 0..total-1 within the strip. */
typedef void (*eyes_strip_chunk_cb_t)(uint32_t strip_idx, uint32_t strip_count,
                                      uint32_t y0, uint32_t h,
                                      uint32_t seq, uint32_t total,
                                      const char *b64);
void eyes_set_strip_chunk_cb(eyes_strip_chunk_cb_t cb);

/** Set / clear the Himax full-res crop window (v0.4.15 P4, AT+CROP).
 *  eyes_set_crop programs the 2592x1944 sensor mode with a 1:1 window;
 *  geometry must be even and fit the byte budget (see AT+CROP docs).
 *  eyes_clear_crop restores the 1296x972 raw preset. Both are synchronous
 *  (the set re-inits the sensor, ~1 s). Returns the Himax reply status. */
esp_err_t eyes_set_crop(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
esp_err_t eyes_clear_crop(void);

/** Strip capture (v0.4.15 P5): tile the 2592x1944 full-res frame into
 *  2592x486 strips (byte-identical to the 1296x972 frame budget) and
 *  stream each strip as base64 chunks through the strip chunk callback.
 *  overlap_rows (must be even, default 0) adds Bayer-safe overlap between
 *  adjacent strips for motion QA; the strip count follows
 *  n = ceil((1944-ov)/(486-ov)) (4 at ov=0, 5 at ov=16). The crop window
 *  is cleared on exit, leaving the 1296x972 mode. Synchronous; takes
 *  ~10-20 s for the full set. */
esp_err_t eyes_capture_strips(uint16_t overlap_rows);

/* v0.4.18.11: SPI channel self-test. Runs 16 test patterns (4 patterns x
 * 4 sizes), verifies CRC and data integrity. Results is an optional
 * cJSON array that receives per-test results. Returns ESP_OK if all pass. */
esp_err_t eyes_spi_test(cJSON *results, cJSON *params);

#ifdef __cplusplus
}
#endif
