/**
 * png_stream.c — Streaming Bayer RAW8 -> RGB -> PNG encoder.
 *
 * Pipeline per row: bilinear debayer (3-row window over the PSRAM-resident
 * Bayer frame) -> PNG row filter -> ROM miniz tdefl (streaming callback) ->
 * IDAT chunk accumulator -> sink.
 *
 * Verified against ESP-IDF v5.3 ROM miniz (S3 ROM: tdefl_compress_buffer at
 * fixed ROM address; MINIZ_NO_ZLIB_APIS set, so only the low-level tdefl API
 * is available; mz_crc32 is #defined to the ROM/hardware CRC32).
 */

#include "png_stream.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"

static const char *TAG = "png_stream";

#define PNG_DEFAULT_IDAT_CHUNK 8192u
#define PNG_DEFAULT_PROBES     128u
#define PNG_YIELD_EVERY_ROWS   64u

/* ------------------------------------------------------------------ */
/* Bayer / CFA helpers                                                 */
/* ------------------------------------------------------------------ */

enum { PC_R = 0, PC_G = 1, PC_B = 2 };

/* 2x2 CFA tile per pattern; index = (y&1)*2 + (x&1). */
static const uint8_t s_cfa_tiles[4][4] = {
    [PNG_CFA_BGGR] = { PC_B, PC_G,  PC_G, PC_R },
    [PNG_CFA_GBRG] = { PC_G, PC_B,  PC_R, PC_G },
    [PNG_CFA_GRBG] = { PC_G, PC_R,  PC_B, PC_G },
    [PNG_CFA_RGGB] = { PC_R, PC_G,  PC_G, PC_B },
};

/* Clamped Bayer sample (edges replicate, which also keeps the CFA phase
 * consistent at the borders). */
static inline uint8_t bayer_px(const uint8_t *bayer, uint32_t w, uint32_t h,
                               int x, int y) {
    if (x < 0) {
        x = 0;
    } else if ((uint32_t)x >= w) {
        x = (int)w - 1;
    }
    if (y < 0) {
        y = 0;
    } else if ((uint32_t)y >= h) {
        y = (int)h - 1;
    }
    return bayer[(uint32_t)y * w + (uint32_t)x];
}

/* Bilinear debayer of one row into RGB888 (3*w bytes at rgb_out).
 * For each pixel, R/B come from the pixel itself when on-color, else from
 * bilinear interpolation of the nearest same-color neighbors; G is the pixel
 * itself when on green, else the mean of the 4 orthogonal neighbors. */
static void debayer_row_bilinear(const uint8_t *bayer, uint32_t w, uint32_t h,
                                 uint32_t y, const uint8_t *tile,
                                 uint8_t *rgb_out) {
    const int yi = (int)y;
    for (uint32_t x = 0; x < w; x++) {
        const int xi = (int)x;
        const uint8_t c = tile[((yi & 1) << 1) | (xi & 1)];

        /* 3x3 neighborhood, clamped. p[1][1] is the center pixel. */
        uint8_t p[3][3];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                p[dy + 1][dx + 1] = bayer_px(bayer, w, h, xi + dx, yi + dy);
            }
        }

        uint8_t r, g, b;
        if (c == PC_R) {
            r = p[1][1];
            g = (uint8_t)((p[0][1] + p[2][1] + p[1][0] + p[1][2] + 2) >> 2);
            b = (uint8_t)((p[0][0] + p[0][2] + p[2][0] + p[2][2] + 2) >> 2);
        } else if (c == PC_B) {
            b = p[1][1];
            g = (uint8_t)((p[0][1] + p[2][1] + p[1][0] + p[1][2] + 2) >> 2);
            r = (uint8_t)((p[0][0] + p[0][2] + p[2][0] + p[2][2] + 2) >> 2);
        } else {
            g = p[1][1];
            /* Among the 4 orthogonal neighbors exactly two are red and two
             * are blue (Bayer property); average each pair. Colors are read
             * at clamped coordinates so edges stay phase-consistent. */
            int rsum = 0, bsum = 0;
            const int dxs[4] = { 0, 0, -1, 1 };
            const int dys[4] = { -1, 1, 0, 0 };
            const uint8_t vs[4] = { p[0][1], p[2][1], p[1][0], p[1][2] };
            for (int k = 0; k < 4; k++) {
                int nx = xi + dxs[k], ny = yi + dys[k];
                if (nx < 0) {
                    nx = 0;
                } else if ((uint32_t)nx >= w) {
                    nx = (int)w - 1;
                }
                if (ny < 0) {
                    ny = 0;
                } else if ((uint32_t)ny >= h) {
                    ny = (int)h - 1;
                }
                if (tile[((ny & 1) << 1) | (nx & 1)] == PC_R) {
                    rsum += vs[k];
                } else {
                    bsum += vs[k];
                }
            }
            r = (uint8_t)((rsum + 1) >> 1);
            b = (uint8_t)((bsum + 1) >> 1);
        }

        rgb_out[3 * x + 0] = r;
        rgb_out[3 * x + 1] = g;
        rgb_out[3 * x + 2] = b;
    }
}

/* ------------------------------------------------------------------ */
/* PNG row filters                                                     */
/* ------------------------------------------------------------------ */

static inline uint8_t paeth_predictor(int a, int b, int c) {
    const int p = a + b - c;
    int pa = p - a;
    if (pa < 0) {
        pa = -pa;
    }
    int pb = p - b;
    if (pb < 0) {
        pb = -pb;
    }
    int pc = p - c;
    if (pc < 0) {
        pc = -pc;
    }
    if (pa <= pb && pa <= pc) {
        return (uint8_t)a;
    }
    return (uint8_t)((pb <= pc) ? b : c);
}

/* Filter one RGB row. raw = 3*w unfiltered bytes, prev = previous raw row
 * (or zeros for row 0). out = 1 + 3*w bytes; out[0] is the filter type. */
static void filter_row(const uint8_t *raw, const uint8_t *prev, uint32_t w,
                       png_filter_t filter, uint8_t *out) {
    const uint32_t n = w * 3u;
    out[0] = (uint8_t)filter;
    uint8_t *d = out + 1;
    switch (filter) {
        case PNG_FILTER_NONE:
            memcpy(d, raw, n);
            break;
        case PNG_FILTER_SUB:
            d[0] = raw[0];
            d[1] = raw[1];
            d[2] = raw[2];
            for (uint32_t i = 3; i < n; i++) {
                d[i] = (uint8_t)(raw[i] - raw[i - 3]);
            }
            break;
        case PNG_FILTER_UP:
            for (uint32_t i = 0; i < n; i++) {
                d[i] = (uint8_t)(raw[i] - prev[i]);
            }
            break;
        case PNG_FILTER_PAETH:
            for (uint32_t i = 0; i < n; i++) {
                const int a = (i >= 3) ? raw[i - 3] : 0;
                const int b = prev[i];
                const int c = (i >= 3) ? prev[i - 3] : 0;
                d[i] = (uint8_t)(raw[i] - paeth_predictor(a, b, c));
            }
            break;
        default: /* unknown -> None (never emit an illegal filter type) */
            out[0] = (uint8_t)PNG_FILTER_NONE;
            memcpy(d, raw, n);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* PNG framing + streaming deflate                                     */
/* ------------------------------------------------------------------ */

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct {
    png_stream_sink_t sink;
    void *user;
    uint8_t *idat_buf;
    size_t idat_cap;
    size_t idat_len;
    uint32_t total_out;   /* every byte handed to sink */
    uint32_t idat_bytes;  /* compressed payload bytes only */
    bool ok;
    /* v0.4.18.86: progress reporting (row-based, 0-100) */
    void (*progress_cb)(int pct, void *user);
    void *progress_user;
    int last_pct;
    /* v0.4.20 (Step 0.3b): deflate-vs-transport split timers.
     * t_sink_acc: total time inside tdefl_sink_cb (accumulate + emit).
     * t_cb_acc:   time inside the user's sink callback (b64 + JSON +
     *             ring/transport wait) — the transport-dominated part.
     * Pure deflate CPU ~= t_deflate - t_sink. */
    int64_t t_sink_acc;
    int64_t t_cb_acc;
} enc_state_t;

/* PNG chunk CRC: mz_crc32 is the ROM/hardware CRC32; init with
 * MZ_CRC32_INIT and chain type+data (verified against esp_rom_crc32_le
 * semantics: crc=~crc per call makes chaining exact). */
static uint32_t chunk_crc(const char *type, const uint8_t *data, size_t len) {
    uint32_t crc = mz_crc32(MZ_CRC32_INIT, NULL, 0);
    crc = mz_crc32(crc, (const mz_uint8 *)type, 4);
    if (len > 0) {
        crc = mz_crc32(crc, data, len);
    }
    return crc;
}

/* Emit one complete non-IDAT chunk (length+type+data+crc) as a SINGLE
 * sink call. v0.4.18.82 (John 2026-10-08): Previously made 3 separate sink
 * calls (header, data, CRC), causing the "8192, 4, 8, 8192" fragmentation
 * pattern and 3x flow-control overhead per PNG chunk. Now assembles the
 * complete chunk in a temp buffer first. */
static bool emit_chunk(enc_state_t *st, const char *type, const uint8_t *data,
                       uint32_t len) {
    /* Chunk = 8-byte header + len-byte data + 4-byte CRC. Max IDAT is
     * 8192+12=8204 bytes. Use stack for small chunks, PSRAM for large. */
    uint8_t hdr[8];
    uint8_t crcb[4];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    be32(crcb, chunk_crc(type, data, len));
    st->total_out += 8 + len + 4;

    if (len <= 64) {
        /* Small chunk (IHDR=13, IEND=0): stack buffer. */
        uint8_t buf[64 + 12];
        memcpy(buf, hdr, 8);
        if (len > 0) memcpy(buf + 8, data, len);
        memcpy(buf + 8 + len, crcb, 4);
        int64_t cb_t0 = esp_timer_get_time();
        bool ok = st->sink(buf, 8 + len + 4, st->user);
        st->t_cb_acc += esp_timer_get_time() - cb_t0;
        return ok;
    } else {
        /* Large chunk (IDAT): the data is already in st->idat_buf which we
         * can't modify in place. Allocate a temp PSRAM buffer. */
        uint8_t *buf = (uint8_t *)heap_caps_malloc(12 + len, MALLOC_CAP_SPIRAM);
        if (!buf) {
            return false;
        }
        memcpy(buf, hdr, 8);
        memcpy(buf + 8, data, len);
        memcpy(buf + 8 + len, crcb, 4);
        int64_t cb_t0 = esp_timer_get_time();
        bool ok = st->sink(buf, 8 + len + 4, st->user);
        st->t_cb_acc += esp_timer_get_time() - cb_t0;
        heap_caps_free(buf);
        return ok;
    }
}

/* Flush the IDAT accumulator as one IDAT chunk (no-op when empty). */
static bool emit_idat(enc_state_t *st) {
    if (st->idat_len == 0) {
        return true;
    }
    bool ok = emit_chunk(st, "IDAT", st->idat_buf, (uint32_t)st->idat_len);
    st->idat_bytes += (uint32_t)st->idat_len;
    st->idat_len = 0;
    return ok;
}

/* tdefl output callback: accumulate compressed bytes, flushing full IDAT
 * chunks to the sink. Returning MZ_FALSE aborts the compressor. */
static mz_bool tdefl_sink_cb(const void *buf, int len, void *user) {
    enc_state_t *st = (enc_state_t *)user;
    const uint8_t *p = (const uint8_t *)buf;
    int64_t sink_t0 = esp_timer_get_time();
    while (len > 0 && st->ok) {
        size_t room = st->idat_cap - st->idat_len;
        size_t n = (size_t)len < room ? (size_t)len : room;
        memcpy(st->idat_buf + st->idat_len, p, n);
        st->idat_len += n;
        p += n;
        len -= (int)n;
        if (st->idat_len == st->idat_cap && !emit_idat(st)) {
            st->ok = false;
        }
    }
    st->t_sink_acc += esp_timer_get_time() - sink_t0;
    return st->ok ? MZ_TRUE : MZ_FALSE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t png_stream_encode(const uint8_t *bayer, const png_stream_cfg_t *cfg,
                            png_stream_sink_t sink, void *user,
                            png_stream_result_t *res) {
    /* v0.4.18.70 (Claude F6c): Initialize result struct on entry. Prevents
     * stale stack garbage if caller reads it after failure. */
    if (res) {
        memset(res, 0, sizeof(*res));
    }
    ESP_LOGI(TAG, "png_stream_encode: entry, bayer=%p w=%lu h=%lu",
             bayer, (unsigned long)(cfg ? cfg->width : 0), (unsigned long)(cfg ? cfg->height : 0));
    if (!bayer || !cfg || !sink || cfg->width < 2 || cfg->height < 2 ||
        (uint32_t)cfg->cfa > (uint32_t)PNG_CFA_RGGB) {
        ESP_LOGE(TAG, "png_stream_encode: invalid args");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "png_stream_encode: args valid");
    const uint32_t w = cfg->width;
    const uint32_t h = cfg->height;
    const uint8_t *tile = s_cfa_tiles[cfg->cfa];
    const png_filter_t filter = (cfg->filter == PNG_FILTER_SUB ||
                                 cfg->filter == PNG_FILTER_UP ||
                                 cfg->filter == PNG_FILTER_PAETH)
                                    ? cfg->filter
                                    : PNG_FILTER_NONE;
    const uint32_t probes = cfg->tdefl_probes;
    /* v0.4.18.70 (Claude F5): 0 means 0 (fastest), not default. The old
     * ternary (0 ? default) was wrong; the config sets 0 intending fastest.
     * If default 128 is wanted, set explicitly. */
    const size_t idat_cap = cfg->idat_chunk ? cfg->idat_chunk : PNG_DEFAULT_IDAT_CHUNK;

    const int64_t t0 = esp_timer_get_time();
    /* v0.4.19 (Step 0.3): Stage timers for speed optimization */
    int64_t t_debayer_acc = 0, t_filter_acc = 0, t_deflate_acc = 0;
    int64_t t_stage_start = 0;
    ESP_LOGI(TAG, "png_stream_encode: allocating comp (%u bytes PSRAM)", (unsigned)sizeof(tdefl_compressor));

    /* Large, randomly-accessed compressor state lives in PSRAM; small
     * streaming buffers stay internal for speed. */
    tdefl_compressor *comp =
        (tdefl_compressor *)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "png_stream_encode: comp=%p", comp);
    /* v0.4.18.71: All streaming buffers in PSRAM, not internal RAM.
     * Internal SRAM is critically low (~700 bytes free on 0.4.18.70).
     * The 19KB of row buffers must not use internal. Speed difference
     * is negligible vs. not working at all. */
    uint8_t *raw_a = (uint8_t *)heap_caps_malloc((size_t)w * 3, MALLOC_CAP_SPIRAM); /* current raw row */
    ESP_LOGI(TAG, "png_stream_encode: raw_a=%p", raw_a);
    uint8_t *raw_b = (uint8_t *)heap_caps_calloc(1, (size_t)w * 3, MALLOC_CAP_SPIRAM); /* prev raw row (row 0 = zeros) */
    ESP_LOGI(TAG, "png_stream_encode: raw_b=%p", raw_b);
    uint8_t *frow = (uint8_t *)heap_caps_malloc(1 + (size_t)w * 3, MALLOC_CAP_SPIRAM); /* filtered row */
    ESP_LOGI(TAG, "png_stream_encode: frow=%p", frow);
    uint8_t *idat_buf = (uint8_t *)heap_caps_malloc(idat_cap, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "png_stream_encode: idat_buf=%p", idat_buf);
    if (!comp || !raw_a || !raw_b || !frow || !idat_buf) {
        ESP_LOGE(TAG, "OOM: comp=%p rows=%p/%p frow=%p idat=%p", comp, raw_a, raw_b, frow,
                 idat_buf);
        heap_caps_free(comp);
        heap_caps_free(raw_a);
        heap_caps_free(raw_b);
        heap_caps_free(frow);
        heap_caps_free(idat_buf);
        return ESP_ERR_NO_MEM;
    }

    enc_state_t st = {
        .sink = sink, .user = user, .idat_buf = idat_buf, .idat_cap = idat_cap,
        .idat_len = 0, .total_out = 0, .idat_bytes = 0, .ok = true,
    };

    esp_err_t ret = ESP_OK;

    /* PNG signature. */
    static const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47,
                                        0x0D, 0x0A, 0x1A, 0x0A };
    if (!sink(png_sig, 8, user)) {
        ret = ESP_FAIL;
        goto done;
    }
    st.total_out += 8;

    /* IHDR: 8-bit truecolor, non-interlaced. */
    uint8_t ihdr[13];
    be32(ihdr + 0, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* color type: truecolor RGB */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter method: adaptive (we pick per config) */
    ihdr[12] = 0; /* interlace: none */
    if (!emit_chunk(&st, "IHDR", ihdr, sizeof(ihdr))) {
        ret = ESP_FAIL;
        goto done;
    }

    /* Streaming deflate: TDEFL_WRITE_ZLIB_HEADER adds the zlib wrapper PNG
     * requires. Multiple IDAT chunks are emitted as the accumulator fills,
     * so no seek-back/length patching is needed. */
    int tdefl_flags = (int)(TDEFL_WRITE_ZLIB_HEADER | (probes & TDEFL_MAX_PROBES_MASK));
    ESP_LOGI(TAG, "png_stream_encode: tdefl_init flags=0x%x (probes=%lu)", tdefl_flags, (unsigned long)probes);
    tdefl_status tst = tdefl_init(comp, tdefl_sink_cb, &st, tdefl_flags);
    if (tst != TDEFL_STATUS_OKAY) {
        ESP_LOGE(TAG, "tdefl_init failed: %d", (int)tst);
        ret = ESP_FAIL;
        goto done;
    }

    for (uint32_t y = 0; y < h; y++) {
        t_stage_start = esp_timer_get_time();
        debayer_row_bilinear(bayer, w, h, y, tile, raw_a);
        t_debayer_acc += esp_timer_get_time() - t_stage_start;

        t_stage_start = esp_timer_get_time();
        filter_row(raw_a, raw_b, w, filter, frow);
        t_filter_acc += esp_timer_get_time() - t_stage_start;

        /* v0.4.18.86: report progress every 5% */
        if (st.progress_cb) {
            int pct = (int)((y * 100) / h);
            if (pct != st.last_pct && pct % 5 == 0) {
                st.last_pct = pct;
                st.progress_cb(pct, st.progress_user);
            }
        }

        t_stage_start = esp_timer_get_time();
        tst = tdefl_compress_buffer(comp, frow, 1 + (size_t)w * 3,
                                    (y == h - 1) ? TDEFL_FINISH : TDEFL_NO_FLUSH);
        t_deflate_acc += esp_timer_get_time() - t_stage_start;
        if (tst < TDEFL_STATUS_OKAY || !st.ok) {
            ESP_LOGE(TAG, "deflate error at row %lu: tst=%d sink_ok=%d", (unsigned long)y,
                     (int)tst, (int)st.ok);
            ret = ESP_FAIL;
            break;
        }

        /* Swap raw rows (filtered row keeps its own buffer). */
        uint8_t *tmp = raw_a;
        raw_a = raw_b;
        raw_b = tmp;

        if ((y % PNG_YIELD_EVERY_ROWS) == (PNG_YIELD_EVERY_ROWS - 1)) {
            vTaskDelay(1); /* keep Wi-Fi/task watchdog happy */
        }
    }

    if (ret == ESP_OK) {
        if (!emit_idat(&st)) {
            ret = ESP_FAIL;
        } else if (!emit_chunk(&st, "IEND", NULL, 0)) {
            ret = ESP_FAIL;
        }
    }

done:
    heap_caps_free(comp);
    /* raw_a/raw_b were swapped; free both (whichever they point at now). */
    heap_caps_free(raw_a);
    heap_caps_free(raw_b);
    heap_caps_free(frow);
    heap_caps_free(idat_buf);

    if (ret == ESP_OK && res) {
        res->png_bytes = st.total_out;
        res->idat_bytes = st.idat_bytes;
        res->ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        /* v0.4.19 (Step 0.3): Stage timers */
        res->t_debayer_us = (uint32_t)t_debayer_acc;
        res->t_filter_us = (uint32_t)t_filter_acc;
        res->t_deflate_us = (uint32_t)t_deflate_acc;
        res->t_sink_us = (uint32_t)st.t_sink_acc;
        res->t_cb_us = (uint32_t)st.t_cb_acc;
        res->tdefl_flags = (uint32_t)tdefl_flags;
        ESP_LOGI(TAG, "encoded %lux%lu CFA=%d filter=%d: %lu PNG bytes (%lu IDAT) in %lu ms",
                 (unsigned long)w, (unsigned long)h, (int)cfg->cfa, (int)filter,
                 (unsigned long)res->png_bytes, (unsigned long)res->idat_bytes,
                 (unsigned long)res->ms);
        /* v0.4.19 (Step 0.3): Stage timers for speed optimization */
        ESP_LOGI(TAG, "png_stages: debayer=%lu us, filter=%lu us, deflate=%lu us, tdefl_flags=0x%lx",
                 (unsigned long)res->t_debayer_us, (unsigned long)res->t_filter_us,
                 (unsigned long)res->t_deflate_us, (unsigned long)res->tdefl_flags);
        /* v0.4.20: deflate split — sink is inside deflate; cb (transport)
         * is inside sink. Pure deflate CPU ~= deflate - sink. */
        ESP_LOGI(TAG, "png_deflate_split: sink=%lu us, cb_transport=%lu us, deflate_cpu~=%lu us",
                 (unsigned long)res->t_sink_us, (unsigned long)res->t_cb_us,
                 (unsigned long)(res->t_deflate_us - res->t_sink_us));
    }
    return ret;
}
