/* SPDX-License-Identifier: MIT
 * WMS2 envelope verifier (Phase 2). C port of mqtt_seal.py semantics.
 *
 * Wire: HEADER(40) || BODY || AUTH, little-endian.
 * AUTH covers: u16le(len(topic)) || topic || header || body
 *   kind=1: tag[16] = BLAKE2b-keyed(key, msg)
 *   kind=2: sig[64] = Ed25519 (RFC 8032, SHA-512) over the same bytes
 *
 * D1b: kind=2 is STANDARD Ed25519 via crypto_ed25519_check from
 * upstream Monocypher's optional/monocypher-ed25519.{c,h}. NEVER the
 * core crypto_eddsa_* (EdDSA-BLAKE2b) — different algorithm.
 *
 * Verify order: magic/kind/len -> tag/sig (ALWAYS fatal) -> ctr
 * replay (dup silent, rollback loud) -> ts window (0 = disabled).
 * high_water is adopted only after ALL gates pass.
 *
 * Pure C99, no ESP-IDF includes. No malloc in the verify path; the
 * kind-2 message is assembled in the caller's scratch buffer.
 */
#include "wms2.h"
#include "monocypher.h"
#include "optional/monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian writers (wire is LE regardless of host) ---- */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t r64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

size_t wms2_auth_bytes_needed(size_t topic_len, size_t body_len)
{
    return 2 + topic_len + WMS2_HDR_LEN + body_len;
}

static int hdr_build(uint8_t out[WMS2_HDR_LEN], uint8_t kind,
                     const char *dev_id, uint64_t ctr, uint64_t ts_ms)
{
    size_t dl = strlen(dev_id);
    if (dl > WMS2_DEVID_LEN) return -1;
    memcpy(out, WMS2_MAGIC, 4);
    out[4] = kind;
    out[5] = 0;
    memset(out + 6, 0, WMS2_DEVID_LEN);
    memcpy(out + 6, dev_id, dl);
    w64(out + 22, ctr);
    w64(out + 30, ts_ms);
    out[38] = out[39] = 0;
    return 0;
}

/* The ONE byte-string every auth covers:
 * u16le(len(topic)) || topic || header || body. */
static size_t auth_bytes_assemble(uint8_t *scratch, size_t cap,
                                  const char *topic,
                                  const uint8_t *hdr,
                                  const uint8_t *body, size_t body_len)
{
    size_t tlen = strlen(topic);
    size_t need = wms2_auth_bytes_needed(tlen, body_len);
    if (need > cap) return 0;
    w16(scratch, (uint16_t)tlen);
    memcpy(scratch + 2, topic, tlen);
    memcpy(scratch + 2 + tlen, hdr, WMS2_HDR_LEN);
    memcpy(scratch + 2 + tlen + WMS2_HDR_LEN, body, body_len);
    return need;
}

/* kind=1 tag, streaming BLAKE2b so the verify path needs no scratch. */
static void auth_tag(uint8_t tag[WMS2_TAG_LEN], const uint8_t key[32],
                     const char *topic,
                     const uint8_t *hdr,
                     const uint8_t *body, size_t body_len)
{
    crypto_blake2b_ctx c;
    uint8_t tl[2];
    size_t tlen = strlen(topic);
    crypto_blake2b_keyed_init(&c, WMS2_TAG_LEN, key, 32);
    w16(tl, (uint16_t)tlen);
    crypto_blake2b_update(&c, tl, 2);
    crypto_blake2b_update(&c, (const uint8_t *)topic, tlen);
    crypto_blake2b_update(&c, hdr, WMS2_HDR_LEN);
    crypto_blake2b_update(&c, body, body_len);
    crypto_blake2b_final(&c, tag);
}

/* ---- grace-mode rate-limited warning (matches mqtt_seal.py: the
 * first unsigned message warns, then every grace_warn_every-th) ---- */
static void grace_warn(wms2_policy *pol, const char *topic)
{
    uint32_t every = pol->grace_warn_every ? pol->grace_warn_every : 64;
    pol->grace_warns++;
    if ((pol->grace_warns % every) == 1)
        fprintf(stderr, "SEAL: unauthenticated msg on '%s' honored (grace)\n",
                topic);
}

int wms2_seal_control(uint8_t *out, size_t cap,
                      const char *topic,
                      const char *dev_id,
                      uint64_t ctr, uint64_t ts_ms,
                      const uint8_t *body, size_t body_len,
                      const uint8_t ed_sk[64])
{
    size_t tlen;
    size_t mlen;
    uint8_t *scratch;
    if (!topic) topic = "";
    tlen = strlen(topic);
    if (tlen > 65535) return WMS2_E_SPACE;
    if (cap < WMS2_HDR_LEN + body_len + WMS2_SIG_LEN) return WMS2_E_SPACE;
    if (hdr_build(out, WMS2_KIND_CTRL, dev_id, ctr, ts_ms))
        return WMS2_E_SPACE;   /* dev_id too long to fit the header */
    memcpy(out + WMS2_HDR_LEN, body, body_len);
    /* Seal path may allocate; the verify path never does. */
    scratch = (uint8_t *)malloc(wms2_auth_bytes_needed(tlen, body_len));
    if (!scratch) return WMS2_E_SPACE;
    mlen = auth_bytes_assemble(scratch,
                               wms2_auth_bytes_needed(tlen, body_len),
                               topic, out, body, body_len);
    if (!mlen) {
        free(scratch);
        return WMS2_E_SPACE;
    }
    crypto_ed25519_sign(out + WMS2_HDR_LEN + body_len, ed_sk,
                        scratch, mlen);
    crypto_wipe(scratch, mlen);
    free(scratch);
    return (int)(WMS2_HDR_LEN + body_len + WMS2_SIG_LEN);
}

wms2_rc wms2_verify(const uint8_t *buf, size_t len,
                    const char *topic,
                    const uint8_t key32[32],
                    const uint8_t ed_pk[32],
                    uint8_t *scratch, size_t scratch_len,
                    uint64_t now_ms,
                    wms2_replay *rp, wms2_policy *pol,
                    wms2_hdr *h,
                    const uint8_t **body, size_t *body_len)
{
    uint8_t kind;
    size_t alen;
    const uint8_t *auth;
    bool framed;

    if (!topic) topic = "";
    framed = len >= WMS2_HDR_LEN && memcmp(buf, WMS2_MAGIC, 4) == 0;

    /* -- unauthenticated input: the grace/strict gate (fleet law) -- */
    if (!framed) {
        if (pol->strict) {
            fprintf(stderr, "SEAL: UNSEALED msg on '%s' - DROPPED\n",
                    topic);
            return WMS2_E_UNSEAL;
        }
        grace_warn(pol, topic);
        h->kind = 0; h->flags = 0; h->dev_id[0] = 0;
        h->ctr = 0; h->ts_ms = 0;
        *body = buf; *body_len = len;
        return WMS2_OK_GRACE;
    }

    /* -- magic/kind/len gate -------------------------------------- */
    kind = buf[4];
    alen = kind == WMS2_KIND_TELEM ? WMS2_TAG_LEN :
           kind == WMS2_KIND_CTRL  ? WMS2_SIG_LEN  : 0;
    if (!alen) {
        fprintf(stderr, "SEAL: bad kind %u on '%s' - DROPPED\n",
                kind, topic);
        return WMS2_E_KIND;
    }
    if (len < WMS2_HDR_LEN + alen) {
        fprintf(stderr, "SEAL: SHORT msg on '%s' - DROPPED\n", topic);
        return WMS2_E_SHORT;
    }

    /* -- parse header --------------------------------------------- */
    h->kind = kind;
    h->flags = buf[5];
    memcpy(h->dev_id, buf + 6, WMS2_DEVID_LEN);
    h->dev_id[WMS2_DEVID_LEN] = 0;
    h->ctr = r64(buf + 22);
    h->ts_ms = r64(buf + 30);
    *body = buf + WMS2_HDR_LEN;
    *body_len = len - WMS2_HDR_LEN - alen;
    auth = buf + len - alen;

    /* -- auth first; a bad tag/sig is ALWAYS fatal (fleet law) ---- */
    if (kind == WMS2_KIND_TELEM) {
        uint8_t t[WMS2_TAG_LEN];
        if (!key32) {
            if (pol->strict) {
                fprintf(stderr, "SEAL: NO KEY for %s on '%s' - DROPPED\n",
                        h->dev_id, topic);
                return WMS2_E_NOKEY;
            }
            fprintf(stderr, "SEAL: no key for %s, honored (grace)\n",
                    h->dev_id);
            return WMS2_OK_GRACE;
        }
        auth_tag(t, key32, topic, buf, *body, *body_len);
        if (crypto_verify16(auth, t) != 0) {
            fprintf(stderr, "SEAL: BAD TAG from %s on '%s' - DROPPED\n",
                    h->dev_id, topic);
            return WMS2_E_BADTAG;
        }
        crypto_wipe(t, sizeof t);
    } else {
        size_t mlen;
        if (!ed_pk) {
            if (pol->strict) {
                fprintf(stderr, "SEAL: NO PIN for %s on '%s' - DROPPED\n",
                        h->dev_id, topic);
                return WMS2_E_NOKEY;
            }
            fprintf(stderr, "SEAL: no pin for %s, honored (grace)\n",
                    h->dev_id);
            return WMS2_OK_GRACE;
        }
        mlen = auth_bytes_assemble((uint8_t *)scratch, scratch_len,
                                   topic, buf, *body, *body_len);
        if (!mlen) {
            fprintf(stderr, "SEAL: auth scratch too small on '%s' - DROPPED\n",
                    topic);
            return WMS2_E_SPACE;
        }
        if (crypto_ed25519_check(auth, ed_pk, scratch, mlen) != 0) {
            fprintf(stderr, "SEAL: BAD SIGNATURE from %s on '%s' - DROPPED\n",
                    h->dev_id, topic);
            return WMS2_E_BADSIG;
        }
    }

    /* -- replay: counter is primary ------------------------------- */
    if (rp) {
        if (h->ctr == rp->high_water && h->ctr != 0)
            return WMS2_OK_DUP;   /* QoS echo: silent by law */
        if (h->ctr <= rp->high_water) {
            fprintf(stderr, "SEAL: REPLAY ctr %llu <= %llu from %s - DROPPED\n",
                    (unsigned long long)h->ctr,
                    (unsigned long long)rp->high_water, h->dev_id);
            return WMS2_E_REPLAY;
        }
    }

    /* -- freshness: timestamp is secondary; 0 disables (device) ---- */
    if (pol->ts_window_ms) {
        uint64_t d = now_ms > h->ts_ms ? now_ms - h->ts_ms
                                       : h->ts_ms - now_ms;
        if (d > pol->ts_window_ms) {
            fprintf(stderr, "SEAL: STALE ts (|dt|=%llums) from %s - DROPPED\n",
                    (unsigned long long)d, h->dev_id);
            return WMS2_E_STALE;
        }
    }

    /* -- kind-1 ENC: this verifier has no AEAD key input. The outer
     * MAC above already authenticated the ciphertext; refusing to
     * carry undecryptable media is a hard gate, placed before the
     * high-water adoption so it never advances. */
    if (kind == WMS2_KIND_TELEM && (h->flags & WMS2_FLAG_ENC)) {
        fprintf(stderr, "SEAL: NO AEAD KEY for %s on '%s' - DROPPED\n",
                h->dev_id, topic);
        return WMS2_E_NOAEAD;
    }

    if (rp) rp->high_water = h->ctr;  /* adopt only after ALL gates */
    return WMS2_OK;
}

const char *wms2_rc_str(wms2_rc rc)
{
    switch (rc) {
    case WMS2_OK:       return "OK";
    case WMS2_OK_DUP:   return "OK-DUP";
    case WMS2_OK_GRACE: return "OK-GRACE";
    case WMS2_E_SHORT:  return "SHORT";
    case WMS2_E_MAGIC:  return "MAGIC";
    case WMS2_E_KIND:   return "KIND";
    case WMS2_E_BADTAG: return "BAD-TAG";
    case WMS2_E_BADSIG: return "BAD-SIG";
    case WMS2_E_REPLAY: return "REPLAY";
    case WMS2_E_STALE:  return "STALE";
    case WMS2_E_NOKEY:  return "NO-KEY";
    case WMS2_E_UNSEAL: return "UNSEALED";
    case WMS2_E_SPACE:  return "SPACE";
    case WMS2_E_NOAEAD: return "NO-AEAD-KEY";
    }
    return "?";
}
