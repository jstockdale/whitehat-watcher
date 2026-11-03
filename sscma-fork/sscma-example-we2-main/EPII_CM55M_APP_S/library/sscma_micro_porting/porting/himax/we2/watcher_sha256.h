/* Minimal SHA-256 (FIPS 180-4), public-domain style implementation for the
 * Himax WE2 porting layer. Used by AT+SPIDUMP to hash the captured frame
 * for the S3's end-to-end verification. No dynamic allocation. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t total_len;   /* total message bytes hashed so far */
    uint8_t  buf[64];
    size_t   buf_len;
} watcher_sha256_ctx_t;

void watcher_sha256_init(watcher_sha256_ctx_t *ctx);
void watcher_sha256_update(watcher_sha256_ctx_t *ctx, const uint8_t *data,
                           size_t len);
/* Finalize; writes the 32-byte digest to out. */
void watcher_sha256_final(watcher_sha256_ctx_t *ctx, uint8_t out[32]);

/* One-shot convenience. */
static inline void watcher_sha256(const uint8_t *data, size_t len,
                                  uint8_t out[32])
{
    watcher_sha256_ctx_t ctx;
    watcher_sha256_init(&ctx);
    watcher_sha256_update(&ctx, data, len);
    watcher_sha256_final(&ctx, out);
}

#ifdef __cplusplus
}
#endif
