/**
 * png_stream.h — Streaming Bayer RAW8 -> RGB -> PNG encoder for the ESP32-S3.
 *
 * Consumes a full-resolution Bayer RAW8 frame (e.g. 1296x972 from the Himax
 * sensor over SPI, resident in PSRAM) and emits a standard PNG (8-bit
 * truecolor, color type 2) through a caller-supplied sink callback.
 *
 * The pipeline is fully streaming:
 *   - Debayer (bilinear) needs only a 3-row window of the input frame, which
 *     stays in PSRAM; rows are produced one at a time.
 *   - Each RGB row is PNG-filtered and fed directly into the ROM miniz
 *     tdefl compressor (S3 ROM, no flash/RAM cost for the codec itself).
 *   - Compressed output is accumulated into small IDAT chunks (default 8 KiB)
 *     and pushed to the sink as PNG framing is completed. Multiple IDAT
 *     chunks are spec-legal, so no seeking/length patching is required.
 *
 * Peak extra RAM: ~175 KiB tdefl_compressor (PSRAM) + ~12 KiB row/IDAT
 * buffers (internal). The 3.8 MiB RGB frame is never materialized.
 *
 * CFA pattern note: the sensor's native readout is BGGR, but the sensor
 * mirror/flip registers (OV5647 0x3821=0x07 in our driver) permute the
 * effective pattern. The pattern is a runtime parameter; verify empirically
 * against a captured frame (see tools/debayer_check.py on the host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bayer CFA pattern of the input frame (2x2 tile, top-left origin).
 *  BGGR tile:          GBRG tile:
 *      B G                 G B
 *      G R                 R G            (etc.) */
typedef enum {
    PNG_CFA_BGGR = 0, /**< Sensor native readout (verify on capture path) */
    PNG_CFA_GBRG = 1, /**< BGGR with horizontal flip */
    PNG_CFA_GRBG = 2, /**< BGGR with vertical flip */
    PNG_CFA_RGGB = 3, /**< BGGR with both flips */
} png_cfa_pattern_t;

/** PNG row filter, applied uniformly to every row. */
typedef enum {
    PNG_FILTER_NONE  = 0,
    PNG_FILTER_SUB   = 1,
    PNG_FILTER_UP    = 2,
    PNG_FILTER_PAETH = 4,
} png_filter_t;

/** Output sink: receives consecutive PNG byte runs in emission order.
 *  Return false to abort the encode (png_stream_encode returns ESP_FAIL). */
typedef bool (*png_stream_sink_t)(const uint8_t *data, size_t len,
                                  void *user);

typedef struct {
    uint32_t width;         /**< Frame width in pixels (>= 2). */
    uint32_t height;        /**< Frame height in pixels (>= 2). */
    png_cfa_pattern_t cfa;  /**< Bayer pattern of the input frame. */
    png_filter_t filter;    /**< Row filter; PNG_FILTER_SUB is a good default
                                 for camera shots of screens. */
    uint32_t tdefl_probes;  /**< LZ hash probes (compression effort).
                                 0 = fastest (no search), 128 = default
                                 (~zlib level 6). Range 0..4095.
                                 v0.4.18.70: 0 means 0, not default. */
    uint32_t idat_chunk;    /**< Max bytes per IDAT chunk, 0 = default 8192. */
} png_stream_cfg_t;

typedef struct {
    uint32_t png_bytes;     /**< Total PNG bytes delivered to the sink. */
    uint32_t idat_bytes;    /**< Compressed IDAT payload bytes (excl. framing). */
    uint32_t ms;            /**< Wall time for debayer+filter+deflate. */
    /* v0.4.19 (Step 0.3): Stage timers for speed optimization (microseconds) */
    uint32_t t_debayer_us;  /**< Time in debayer stage. */
    uint32_t t_filter_us;   /**< Time in filter stage. */
    uint32_t t_deflate_us;  /**< Time in deflate (miniz) stage. */
    /* v0.4.20 (Step 0.3b): deflate-vs-transport split. t_sink_us is a
     * subset of t_deflate_us (miniz output callback); t_cb_us is a
     * subset of t_sink_us (caller chunk callback: base64 + JSON + ring
     * wait). Pure deflate CPU ~= t_deflate_us - t_sink_us. */
    uint32_t t_sink_us;     /**< Time in miniz output callback. */
    uint32_t t_cb_us;       /**< Time in caller chunk callback (transport). */
    uint32_t tdefl_flags;   /**< Flags passed to tdefl_init (debug LZ status). */
} png_stream_result_t;

/**
 * Encode one Bayer RAW8 frame as PNG, streaming through sink.
 *
 * @param bayer  width*height bytes of Bayer RAW8 (must outlive the call).
 * @param cfg    Encode configuration (see above).
 * @param sink   Called with consecutive PNG byte runs; must not retain the
 *               buffer after returning.
 * @param user   Opaque pointer passed through to sink.
 * @param res    Optional result stats (NULL ok).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad args,
 *         ESP_ERR_NO_MEM on allocation failure, ESP_FAIL if the sink
 *         aborted or the compressor errored.
 *
 * Runs on the caller's task; yields to the RTOS every 64 rows so the Wi-Fi
 * stack and task watchdog stay happy during the ~1-2 s encode. Safe to run
 * in a PSRAM task (see task_psram component).
 */
esp_err_t png_stream_encode(const uint8_t *bayer, const png_stream_cfg_t *cfg,
                            png_stream_sink_t sink, void *user,
                            png_stream_result_t *res);

#ifdef __cplusplus
}
#endif
