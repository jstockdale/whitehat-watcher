/* SPDX-License-Identifier: MIT
 * WMS2 envelope verifier (Phase 2). C port of mqtt_seal.py semantics.
 *
 * Envelope: HEADER(40) || BODY || AUTH, little-endian.
 *   magic "WMS2" 4 | kind u8 1 | flags u8 1 | dev_id char[16] |
 *   ctr u64 | ts_ms u64 | rsv u16
 * AUTH covers: u16le(len(topic)) || topic || header || body
 *   kind=1: tag[16] = BLAKE2b-keyed(key, msg)
 *   kind=2: sig[64] = Ed25519(RFC 8032, SHA-512) over the same bytes
 *
 * D1b: kind=2 is STANDARD Ed25519 (RFC 8032), verified with
 * crypto_ed25519_check from upstream Monocypher's optional/
 * monocypher-ed25519.{c,h}. NEVER the vendored core's
 * crypto_eddsa_* (EdDSA-BLAKE2b) — different algorithm, does not
 * interop with libsodium/PyNaCl/noble.
 *
 * Pure C99, no ESP-IDF dependencies — must host-compile with gcc
 * for the cross-verification harness. No large stack buffers:
 * callers size a scratch via wms2_auth_bytes_needed() (D6).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WMS2_MAGIC        "WMS2"
#define WMS2_HDR_LEN      40
#define WMS2_TAG_LEN      16
#define WMS2_SIG_LEN      64
#define WMS2_DEVID_LEN    16
#define WMS2_KIND_TELEM   1
#define WMS2_KIND_CTRL    2
#define WMS2_FLAG_ENC     0x01
#define WMS2_MAX_TOPIC    128

typedef enum {
    WMS2_OK = 0,
    WMS2_OK_DUP,          /* exact repeat of last ctr: QoS echo, silent */
    WMS2_OK_GRACE,        /* unauthenticated input honored under grace  */
    WMS2_E_SHORT  = -1,
    WMS2_E_MAGIC  = -2,
    WMS2_E_KIND   = -3,
    WMS2_E_BADTAG = -4,   /* ALWAYS fatal */
    WMS2_E_BADSIG = -5,   /* ALWAYS fatal */
    WMS2_E_REPLAY = -6,
    WMS2_E_STALE  = -7,
    WMS2_E_NOKEY  = -8,
    WMS2_E_UNSEAL = -9,
    WMS2_E_SPACE  = -10,
    WMS2_E_NOAEAD = -11,
} wms2_rc;

typedef struct {
    uint8_t  kind, flags;
    char     dev_id[WMS2_DEVID_LEN + 1];
    uint64_t ctr, ts_ms;
} wms2_hdr;

typedef struct {
    uint64_t high_water;
} wms2_replay;

typedef struct {
    bool     strict;
    uint64_t ts_window_ms;      /* 0 = disable timestamp check (device) */
    uint32_t grace_warn_every;
    uint32_t grace_warns;
} wms2_policy;

/* Scratch bytes needed to assemble the auth input
 * u16le(len) || topic || header || body. */
size_t wms2_auth_bytes_needed(size_t topic_len, size_t body_len);

/* Sender side (host tests + device telemetry signing).
 * ed_sk: 64-byte RFC 8032 secret key (seed||pubkey layout as produced
 * by crypto_ed25519_key_pair). Returns bytes written or <0. */
int wms2_seal_control(uint8_t *out, size_t cap,
                      const char *topic,
                      const char *dev_id,
                      uint64_t ctr, uint64_t ts_ms,
                      const uint8_t *body, size_t body_len,
                      const uint8_t ed_sk[64]);

/* Verifier. Order: magic/kind/len -> tag/sig (fatal) -> ctr replay
 * (dup silent, rollback loud) -> ts window. high_water adopted only
 * after ALL gates pass. The body pointer and length out-params point
 * into buf on success.
 * scratch/scratch_len: caller buffer >= wms2_auth_bytes_needed().
 * scratch MUST be writable: the verifier assembles the auth input
 * (u16le(len)||topic||header||body) in it (D6). Passing a const or
 * read-only buffer is a contract violation.
 * On WMS2_OK_GRACE the input was unauthenticated (grace policy). */
wms2_rc wms2_verify(const uint8_t *buf, size_t len,
                    const char *topic,
                    const uint8_t key32[32],   /* kind-1 MAC key, NULL=none */
                    const uint8_t ed_pk[32],   /* kind-2 pinned pk, NULL=none */
                    uint8_t *scratch, size_t scratch_len,
                    uint64_t now_ms,
                    wms2_replay *rp, wms2_policy *pol,
                    wms2_hdr *h,
                    const uint8_t **body, size_t *body_len);

const char *wms2_rc_str(wms2_rc rc);

#ifdef __cplusplus
}
#endif
