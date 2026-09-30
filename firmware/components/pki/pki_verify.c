/* SPDX-License-Identifier: MIT
 * Watcher PKI — device-side provisioning verification.
 *
 * STUB STATUS: the crypto verification in this file is REAL and complete.
 * What is stubbed: (1) the nonce store is RAM-only (full task persists
 * via the nvs_task broker); (2) pki_time_now() returns 0 — the device
 * has no SNTP, so the wall-clock window check awaits a time source;
 * (3) wiring into link.c's dispatcher and applying verified commands
 * (NVS writes for keys) is the follow-up firmware task.
 *
 * NVS DISCIPLINE: this file never calls nvs_* / spi_flash_* / esp_ota_*
 * directly. Verified-key persistence goes through seal_dev_nvs_*_sync()
 * (the nvs_task broker) in the follow-up task. The build-time audit
 * tools/audit_psram_flash.py must stay green.
 *
 * Crypto patterns reused from the existing tree:
 *   - Ed25519: crypto_ed25519_check() from optional/monocypher-ed25519.h,
 *     same call shape as seal/wms2.c (returns 0 on valid signature).
 *   - JSON: cJSON, same API as components/link/link.c.
 *   - ECDSA P-256: mbedTLS (MBEDTLS_ECDSA_C + SECP256R1 are enabled in
 *     sdkconfig). Signatures arrive as raw 64-byte r||s (the Lambda
 *     converts KMS DER -> raw64); we feed r/s as mbedTLS MPIs, so no
 *     DER parsing is needed on device.
 */
#include "pki_verify.h"

#ifdef PKI_HOST_TEST
/* Host unit-test build: stub ESP logging; the crypto backends
 * (pki_ecdsa_p256_verify / pki_ed25519_verify) are NOT compiled here —
 * the test harness (host_test/test_pki.c) provides stub definitions
 * that capture the canonical bytes for comparison against
 * Python-generated vectors. */
#define ESP_LOGE(tag, fmt, ...) do { (void)0; } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)0; } while (0)
#define ESP_LOGI(tag, fmt, ...) do { (void)0; } while (0)
#else
#include "esp_log.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/sha256.h"
#include "optional/monocypher-ed25519.h"
#endif

#include "tier2_keys.h"  /* GENERATED — bake_tier2_keys.py; see CMakeLists */
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "pki";

/* Magic prefixes (brief: canonical bytes, all integers big-endian). */
static const char PROV_MAGIC[]   = "WATCHER-PROVISION-v1";
static const char ROTATE_MAGIC[] = "WATCHER-TIER1-ROTATE-v1";
static const char PROV_FORMAT[]  = "watcher-provision-v1";

/* Device context (pki_set_device_context). */
static char s_device_id_[PKI_DEVICE_MAX];
static bool s_keyed_;
static bool s_ctx_set_;

/* RAM nonce store (STUB — the full task persists via nvs_task). */
static uint8_t s_nonces_[PKI_NONCE_STORE_N][PKI_NONCE_RAW_LEN];
static size_t  s_nonce_count_;

/* ------------------------------------------------------------------ */

const char *pki_rc_str(pki_rc_t rc)
{
    switch (rc) {
    case PKI_OK:         return "ok";
    case PKI_ERR_PARSE:  return "unparseable JSON";
    case PKI_ERR_SCHEMA: return "schema violation";
    case PKI_ERR_BADSIG: return "bad signature";
    case PKI_ERR_EXPIRED:return "expired";
    case PKI_ERR_NOTYET: return "not yet valid";
    case PKI_ERR_WINDOW: return "bad validity window";
    case PKI_ERR_NONCE:  return "nonce replayed/store full";
    case PKI_ERR_SCOPE:  return "device scope mismatch";
    case PKI_ERR_NOMEM:  return "scratch too small";
    case PKI_ERR_ARG:    return "bad argument";
    default:             return "?";
    }
}

void pki_set_device_context(const char *device_id, bool keyed)
{
    if (device_id && device_id[0]) {
        strncpy(s_device_id_, device_id, sizeof(s_device_id_) - 1);
        s_device_id_[sizeof(s_device_id_) - 1] = '\0';
    } else {
        s_device_id_[0] = '\0';
    }
    s_keyed_ = keyed;
    s_ctx_set_ = true;
}

uint64_t pki_time_now(void)
{
    /* No SNTP on device (ts_window=0): no wall clock. The full task
     * must wire a time source (SNTP once WiFi is up, or a provisioned
     * epoch) before the absolute window check is meaningful. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* ISO-8601 UTC ("YYYY-MM-DDTHH:MM:SSZ") -> unix seconds, strict.      */
/* ------------------------------------------------------------------ */

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

pki_rc_t pki_parse_iso8601_utc(const char *iso, uint64_t *out_unix)
{
    static const int mdays[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    int Y, M, D, h, m, s, i;
    uint64_t days;

    if (!iso || !out_unix) {
        return PKI_ERR_ARG;
    }
    /* Exact shape: 20 chars, fixed separators, trailing Z. */
    if (strlen(iso) != 20 || iso[4] != '-' || iso[7] != '-' ||
        iso[10] != 'T' || iso[13] != ':' || iso[16] != ':' ||
        iso[19] != 'Z') {
        return PKI_ERR_SCHEMA;
    }
    for (i = 0; i < 19; i++) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;
        }
        if (iso[i] < '0' || iso[i] > '9') {
            return PKI_ERR_SCHEMA;
        }
    }
    Y = (iso[0]-'0')*1000 + (iso[1]-'0')*100 + (iso[2]-'0')*10 + (iso[3]-'0');
    M = (iso[5]-'0')*10 + (iso[6]-'0');
    D = (iso[8]-'0')*10 + (iso[9]-'0');
    h = (iso[11]-'0')*10 + (iso[12]-'0');
    m = (iso[14]-'0')*10 + (iso[15]-'0');
    s = (iso[17]-'0')*10 + (iso[18]-'0');
    if (Y < 2020 || Y > 2100 || M < 1 || M > 12 || h > 23 || m > 59 ||
        s > 60 /* allow leap second, clamp below */) {
        return PKI_ERR_SCHEMA;
    }
    {
        int dim = mdays[M - 1];
        if (M == 2 && is_leap(Y)) {
            dim = 29;
        }
        if (D < 1 || D > dim) {
            return PKI_ERR_SCHEMA;
        }
    }
    if (s == 60) {
        s = 59; /* clamp leap second; never sign one anyway */
    }
    days = 0;
    for (i = 1970; i < Y; i++) {
        days += is_leap(i) ? 366 : 365;
    }
    for (i = 1; i < M; i++) {
        days += mdays[i - 1];
        if (i == 2 && is_leap(Y)) {
            days += 1;
        }
    }
    days += (uint64_t)(D - 1);
    *out_unix = days * 86400u + (uint64_t)h * 3600u +
                (uint64_t)m * 60u + (uint64_t)s;
    return PKI_OK;
}

/* ------------------------------------------------------------------ */

size_t pki_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    size_t i;
    if (!hex || !out) {
        return 0;
    }
    if (strlen(hex) != out_len * 2) {
        return 0;
    }
    for (i = 0; i < out_len; i++) {
        unsigned hi, lo;
        char a = hex[2 * i], b = hex[2 * i + 1];
        if (a >= '0' && a <= '9') {
            hi = (unsigned)(a - '0');
        } else if (a >= 'a' && a <= 'f') {
            hi = (unsigned)(a - 'a') + 10;
        } else if (a >= 'A' && a <= 'F') {
            hi = (unsigned)(a - 'A') + 10;
        } else {
            return 0;
        }
        if (b >= '0' && b <= '9') {
            lo = (unsigned)(b - '0');
        } else if (b >= 'a' && b <= 'f') {
            lo = (unsigned)(b - 'a') + 10;
        } else if (b >= 'A' && b <= 'F') {
            lo = (unsigned)(b - 'A') + 10;
        } else {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return out_len;
}

/* ------------------------------------------------------------------ */
/* Canonical JSON — byte-identical to Python's
 *   json.dumps(obj, sort_keys=True, separators=(",",":"),
 *              ensure_ascii=True)
 * Differential-tested against CPython (see host_test/).               */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t left;
    bool   oom; /* scratch exhausted */
    bool   bad; /* invalid UTF-8: schema violation, fail closed */
} canon_w_t;

static void cw_putc(canon_w_t *w, char c)
{
    if (w->left < 2) { /* 1 char + NUL */
        w->oom = true;
        return;
    }
    *w->p++ = c;
    w->left--;
}

static void cw_puts(canon_w_t *w, const char *s)
{
    while (*s) {
        cw_putc(w, *s++);
    }
}

static void cw_put_u16hex(canon_w_t *w, unsigned v)
{
    static const char hd[] = "0123456789abcdef";
    cw_puts(w, "\\u");
    cw_putc(w, hd[(v >> 12) & 0xf]);
    cw_putc(w, hd[(v >> 8) & 0xf]);
    cw_putc(w, hd[(v >> 4) & 0xf]);
    cw_putc(w, hd[v & 0xf]);
}

/* Decode one UTF-8 sequence. Returns bytes consumed (>0) or 0 on error;
 * code point in *cp. */
static int utf8_decode(const unsigned char *s, size_t avail, uint32_t *cp)
{
    unsigned char c0;
    if (avail == 0) {
        return 0;
    }
    c0 = s[0];
    if (c0 < 0x80) {
        *cp = c0;
        return 1;
    }
    if (c0 < 0xC2 || c0 > 0xF4) {
        return 0;
    }
    if (c0 < 0xE0) {
        if (avail < 2 || (s[1] & 0xC0) != 0x80) {
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x1F) << 6) | (s[1] & 0x3F);
        if (*cp < 0x80) {
            return 0; /* overlong */
        }
        return 2;
    }
    if (c0 < 0xF0) {
        if (avail < 3 || (s[1] & 0xC0) != 0x80 ||
            (s[2] & 0xC0) != 0x80) {
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (*cp < 0x800 ||
            (*cp >= 0xD800 && *cp <= 0xDFFF)) {
            return 0; /* overlong / surrogate */
        }
        return 3;
    }
    if (avail < 4 || (s[1] & 0xC0) != 0x80 ||
        (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
        return 0;
    }
    *cp = ((uint32_t)(c0 & 0x07) << 18) |
          ((uint32_t)(s[1] & 0x3F) << 12) |
          ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    if (*cp < 0x10000 || *cp > 0x10FFFF) {
        return 0;
    }
    return 4;
}

/* ensure_ascii=True string escaping. */
static void cw_put_escaped(canon_w_t *w, const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t avail = strlen(s);
    size_t i = 0;
    cw_putc(w, '"');
    while (i < avail && !w->oom) {
        uint32_t cp;
        int n = utf8_decode(u + i, avail - i, &cp);
        if (n == 0) {
            w->bad = true; /* invalid UTF-8: schema violation, no
                           * silent divergence from Python */
            return;
        }
        if (cp == '"' || cp == '\\') {
            cw_putc(w, '\\');
            cw_putc(w, (char)cp);
        } else if (cp == '\b') {
            cw_puts(w, "\\b");
        } else if (cp == '\f') {
            cw_puts(w, "\\f");
        } else if (cp == '\n') {
            cw_puts(w, "\\n");
        } else if (cp == '\r') {
            cw_puts(w, "\\r");
        } else if (cp == '\t') {
            cw_puts(w, "\\t");
        } else if (cp < 0x20 || cp == 0x7F) {
            cw_put_u16hex(w, cp);
        } else if (cp < 0x80) {
            cw_putc(w, (char)cp);
        } else if (cp < 0x10000) {
            cw_put_u16hex(w, cp);
        } else {
            /* surrogate pair, like CPython */
            uint32_t v = cp - 0x10000;
            cw_put_u16hex(w, 0xD800 + (v >> 10));
            cw_put_u16hex(w, 0xDC00 + (v & 0x3FF));
        }
        i += (size_t)n;
    }
    cw_putc(w, '"');
}

static pki_rc_t canon_value(canon_w_t *w, const cJSON *node);

static int cmp_strptr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Object keys, sorted byte-wise (== code-point order for valid UTF-8,
 * matching Python's sort_keys). */
static pki_rc_t canon_object(canon_w_t *w, const cJSON *obj)
{
    const cJSON *ch;
    size_t n = 0, i = 0;
    const char **keys;
    for (ch = obj->child; ch; ch = ch->next) {
        n++;
    }
    if (n > 64) {
        return PKI_ERR_SCHEMA; /* params objects are tiny; bound it */
    }
    keys = (const char **)malloc(n * sizeof(*keys));
    if (!keys && n) {
        return PKI_ERR_NOMEM;
    }
    for (ch = obj->child; ch; ch = ch->next) {
        keys[i++] = ch->string ? ch->string : "";
    }
    qsort(keys, n, sizeof(*keys), cmp_strptr);
    cw_putc(w, '{');
    for (i = 0; i < n && !w->oom; i++) {
        const cJSON *v;
        if (i) {
            cw_putc(w, ',');
        }
        cw_put_escaped(w, keys[i]);
        cw_putc(w, ':');
        /* re-find the child by pointer-stable key string */
        for (v = obj->child; v; v = v->next) {
            const char *ks = v->string ? v->string : "";
            if (ks == keys[i] || strcmp(ks, keys[i]) == 0) {
                pki_rc_t rc = canon_value(w, v);
                if (rc != PKI_OK) {
                    free(keys);
                    return rc;
                }
                break;
            }
        }
    }
    cw_putc(w, '}');
    free(keys);
    if (w->bad) {
        return PKI_ERR_SCHEMA;
    }
    return w->oom ? PKI_ERR_NOMEM : PKI_OK;
}

static pki_rc_t canon_value(canon_w_t *w, const cJSON *node)
{
    if (!node) {
        return PKI_ERR_SCHEMA;
    }
    if (cJSON_IsString(node)) {
        cw_put_escaped(w, node->valuestring ? node->valuestring : "");
    } else if (cJSON_IsNumber(node)) {
        double v = node->valuedouble;
        /* Schema has no floats: only exact integral doubles are
         * representable. Anything else is a schema violation, never
         * silently re-printed (%.17g could diverge from Python). */
        if (!isfinite(v) || v != floor(v) ||
            fabs(v) >= 9007199254740992.0 /* 2^53 */) {
            return PKI_ERR_SCHEMA;
        }
        {
            char nb[32];
            int n = snprintf(nb, sizeof(nb), "%lld",
                             (long long)v);
            if (n < 0 || (size_t)n >= sizeof(nb)) {
                return PKI_ERR_NOMEM;
            }
            cw_puts(w, nb);
        }
    } else if (cJSON_IsTrue(node)) {
        cw_puts(w, "true");
    } else if (cJSON_IsFalse(node)) {
        cw_puts(w, "false");
    } else if (cJSON_IsNull(node)) {
        cw_puts(w, "null");
    } else if (cJSON_IsArray(node)) {
        const cJSON *ch;
        bool first = true;
        cw_putc(w, '[');
        for (ch = node->child; ch && !w->oom; ch = ch->next) {
            if (!first) {
                cw_putc(w, ',');
            }
            first = false;
            {
                pki_rc_t rc = canon_value(w, ch);
                if (rc != PKI_OK) {
                    return rc;
                }
            }
        }
        cw_putc(w, ']');
    } else if (cJSON_IsObject(node)) {
        return canon_object(w, node);
    } else {
        return PKI_ERR_SCHEMA;
    }
    if (w->bad) {
        return PKI_ERR_SCHEMA;
    }
    return w->oom ? PKI_ERR_NOMEM : PKI_OK;
}

pki_rc_t pki_canon_json(const struct cJSON *node, char *out,
                        size_t out_cap, size_t *out_len)
{
    canon_w_t w;
    pki_rc_t rc;
    if (!node || !out || out_cap == 0) {
        return PKI_ERR_ARG;
    }
    w.p = out;
    w.left = out_cap;
    w.oom = false;
    w.bad = false;
    rc = canon_value(&w, (const cJSON *)node);
    if (rc != PKI_OK) {
        return rc;
    }
    if (w.bad) {
        return PKI_ERR_SCHEMA;
    }
    if (w.oom) {
        return PKI_ERR_NOMEM;
    }
    *w.p = '\0';
    if (out_len) {
        *out_len = (size_t)(w.p - out);
    }
    return PKI_OK;
}

/* ------------------------------------------------------------------ */
/* Canonical byte builders                                            */
/* ------------------------------------------------------------------ */

static void put_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_u64be(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

pki_rc_t pki_build_provision_canon(const char *cmd, const char *device,
                                   const uint8_t nonce[PKI_NONCE_RAW_LEN],
                                   uint64_t issued_at, uint64_t expires_at,
                                   const char *params_canon_json,
                                   uint8_t *out, size_t out_cap,
                                   size_t *out_len)
{
    size_t cl, dl, pl, need, o = 0;
    if (!cmd || !device || !nonce || !params_canon_json || !out) {
        return PKI_ERR_ARG;
    }
    cl = strlen(cmd);
    dl = strlen(device);
    pl = strlen(params_canon_json);
    if (cl == 0 || cl > 0xFFFF || dl == 0 || dl > 0xFFFF) {
        return PKI_ERR_SCHEMA;
    }
    need = sizeof(PROV_MAGIC) - 1 + 2 + cl + 2 + dl + PKI_NONCE_RAW_LEN +
           8 + 8 + pl;
    if (need > out_cap || need > PKI_CANON_MAX) {
        return PKI_ERR_NOMEM;
    }
    memcpy(out + o, PROV_MAGIC, sizeof(PROV_MAGIC) - 1);
    o += sizeof(PROV_MAGIC) - 1;
    put_u16be(out + o, (uint16_t)cl); o += 2;
    memcpy(out + o, cmd, cl);          o += cl;
    put_u16be(out + o, (uint16_t)dl); o += 2;
    memcpy(out + o, device, dl);      o += dl;
    memcpy(out + o, nonce, PKI_NONCE_RAW_LEN);
    o += PKI_NONCE_RAW_LEN;
    put_u64be(out + o, issued_at);    o += 8;
    put_u64be(out + o, expires_at);   o += 8;
    memcpy(out + o, params_canon_json, pl);
    o += pl;
    if (out_len) {
        *out_len = o;
    }
    return PKI_OK;
}

pki_rc_t pki_build_rotate_canon(const uint8_t new_pub65[65],
                                uint64_t expires_at, uint32_t serial,
                                uint8_t *out, size_t out_cap,
                                size_t *out_len)
{
    size_t need, o = 0;
    if (!new_pub65 || !out) {
        return PKI_ERR_ARG;
    }
    if (new_pub65[0] != 0x04) {
        return PKI_ERR_SCHEMA;
    }
    need = sizeof(ROTATE_MAGIC) - 1 + 65 + 8 + 4;
    if (need > out_cap) {
        return PKI_ERR_NOMEM;
    }
    memcpy(out + o, ROTATE_MAGIC, sizeof(ROTATE_MAGIC) - 1);
    o += sizeof(ROTATE_MAGIC) - 1;
    memcpy(out + o, new_pub65, 65);
    o += 65;
    put_u64be(out + o, expires_at);
    o += 8;
    put_u32be(out + o, serial);
    o += 4;
    if (out_len) {
        *out_len = o;
    }
    return PKI_OK;
}

/* ------------------------------------------------------------------ */
/* Crypto backends                                                    */
/* ------------------------------------------------------------------ */

#ifndef PKI_HOST_TEST
/* (Host unit-test builds provide their own pki_ecdsa_p256_verify /
 * pki_ed25519_verify stubs — see components/pki/host_test/. The real
 * definitions below are compiled out there.) */

int pki_ecdsa_p256_verify(const uint8_t pub_uncompressed65[65],
                          const uint8_t *msg, size_t msg_len,
                          const uint8_t sig_raw64[64])
{
    /* KMS signs ECDSA_SHA_256 = SHA-256 then ECDSA. The Lambda ships
     * raw 64-byte r||s, so we load r/s as MPIs — no DER parsing. */
    uint8_t hash[32];
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    int rc = -1;

    if (!pub_uncompressed65 || !msg || !sig_raw64 ||
        pub_uncompressed65[0] != 0x04) {
        return MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    }
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    /* SHA-256: this IDF's mbedTLS port exposes the multi-part API only
     * (implemented by the ESP32-S3 hardware SHA block). */
    {
        mbedtls_sha256_context sc;
        mbedtls_sha256_init(&sc);
        if (mbedtls_sha256_starts(&sc, 0) != 0 ||
            mbedtls_sha256_update(&sc, msg, msg_len) != 0 ||
            mbedtls_sha256_finish(&sc, hash) != 0) {
            mbedtls_sha256_free(&sc);
            rc = MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
            goto out;
        }
        mbedtls_sha256_free(&sc);
        rc = 0;
    }
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_ecp_point_read_binary(&grp, &Q,
                                       pub_uncompressed65, 65);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_ecp_check_pubkey(&grp, &Q);
    if (rc != 0) {
        goto out; /* not a valid curve point: reject */
    }
    rc = mbedtls_mpi_read_binary(&r, sig_raw64, 32);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_mpi_read_binary(&s, sig_raw64 + 32, 32);
    if (rc != 0) {
        goto out;
    }
    rc = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s);
    /* 0 = valid; MBEDTLS_ERR_ECP_VERIFY_FAILED = bad signature. */

out:
    mbedtls_ecp_group_free(&grp);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    {
        volatile uint8_t *vh = hash;
        size_t i;
        for (i = 0; i < sizeof(hash); i++) {
            vh[i] = 0;
        }
    }
    return rc;
}

int pki_ed25519_verify(const uint8_t pub32[32],
                       const uint8_t *msg, size_t msg_len,
                       const uint8_t sig64[64])
{
    /* Same call pattern as seal/wms2.c: crypto_ed25519_check() returns
     * 0 on a valid signature. */
    if (!pub32 || !sig64 || (!msg && msg_len)) {
        return -1;
    }
    return crypto_ed25519_check(sig64, pub32, msg, msg_len);
}

#endif /* !PKI_HOST_TEST */

/* The Tier 1 key actually used for verification. STUB: the baked key.
 * The full task upgrades this to "NVS-stored rotated key, falling back
 * to the baked key" (read via seal_dev_nvs_read_blob_sync("pki",
 * "tier1_pub", ...) through the nvs_task broker). */
static void pki_active_tier1_pubkey(uint8_t out65[65])
{
#ifndef PKI_HOST_TEST
    memcpy(out65, TIER1_P256_PUBKEY, 65);
#else
    memset(out65, 0, 65);
    out65[0] = 0x04;
#endif
}

/* ------------------------------------------------------------------ */
/* Nonce replay store — RAM STUB (full task: persist via nvs_task)     */
/* ------------------------------------------------------------------ */

bool pki_nonce_seen(const uint8_t nonce[PKI_NONCE_RAW_LEN])
{
    size_t i;
    if (!nonce) {
        return true; /* fail closed */
    }
    for (i = 0; i < s_nonce_count_; i++) {
        if (memcmp(s_nonces_[i], nonce, PKI_NONCE_RAW_LEN) == 0) {
            return true;
        }
    }
    return false;
}

pki_rc_t pki_nonce_record(const uint8_t nonce[PKI_NONCE_RAW_LEN])
{
    if (!nonce) {
        return PKI_ERR_ARG;
    }
    if (pki_nonce_seen(nonce)) {
        return PKI_ERR_NONCE;
    }
    if (s_nonce_count_ >= PKI_NONCE_STORE_N) {
        /* STUB eviction: drop the oldest. The NVS-backed full task
         * keeps a bounded recent set with the same policy. */
        memmove(s_nonces_[0], s_nonces_[1],
                (PKI_NONCE_STORE_N - 1) * PKI_NONCE_RAW_LEN);
        s_nonce_count_ = PKI_NONCE_STORE_N - 1;
    }
    memcpy(s_nonces_[s_nonce_count_], nonce, PKI_NONCE_RAW_LEN);
    s_nonce_count_++;
    return PKI_OK;
}

/* ------------------------------------------------------------------ */
/* Helpers for pki_verify_provision                                    */
/* ------------------------------------------------------------------ */

static const char *req_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(it) || !it->valuestring) {
        return NULL;
    }
    return it->valuestring;
}

static pki_cmd_kind_t cmd_kind_of(const char *cmd)
{
    if (strcmp(cmd, "install_commander_key") == 0) {
        return PKI_CMD_INSTALL_COMMANDER_KEY;
    }
    if (strcmp(cmd, "install_telemetry_seed") == 0) {
        return PKI_CMD_INSTALL_TELEMETRY_SEED;
    }
    if (strcmp(cmd, "rotate_tier1") == 0) {
        return PKI_CMD_ROTATE_TIER1;
    }
    if (strcmp(cmd, "factory_reset_keys") == 0) {
        return PKI_CMD_FACTORY_RESET_KEYS;
    }
    return PKI_CMD_UNKNOWN;
}

/* Per-cmd params schema validation (brief: "schema-validate params per
 * cmd"). Runs AFTER the signature check — it gates application, not
 * authenticity. */
static pki_rc_t validate_params(pki_cmd_kind_t kind, const cJSON *params)
{
    const cJSON *it;
    uint8_t tmp[65];

    if (!cJSON_IsObject(params)) {
        return PKI_ERR_SCHEMA;
    }
    switch (kind) {
    case PKI_CMD_INSTALL_COMMANDER_KEY:
        it = cJSON_GetObjectItem(params, "pubkey_hex");
        if (!cJSON_IsString(it) || !it->valuestring ||
            pki_hex_to_bytes(it->valuestring, tmp, 32) != 32) {
            return PKI_ERR_SCHEMA; /* 32-byte Ed25519 pubkey, hex */
        }
        return PKI_OK;

    case PKI_CMD_INSTALL_TELEMETRY_SEED:
        /* The seed itself is NEVER in this command — only its
         * fingerprint. The real seed travels over USB serial. */
        it = cJSON_GetObjectItem(params, "seed_fp");
        if (!cJSON_IsString(it) || !it->valuestring ||
            it->valuestring[0] == '\0' ||
            strlen(it->valuestring) > 128) {
            return PKI_ERR_SCHEMA;
        }
        return PKI_OK;

    case PKI_CMD_ROTATE_TIER1: {
        uint64_t new_exp;
        pki_rc_t rc;
        it = cJSON_GetObjectItem(params, "p256_pubkey_hex");
        if (!cJSON_IsString(it) || !it->valuestring ||
            pki_hex_to_bytes(it->valuestring, tmp, 65) != 65 ||
            tmp[0] != 0x04) {
            return PKI_ERR_SCHEMA; /* 65-byte uncompressed P-256 */
        }
        it = cJSON_GetObjectItem(params, "expires_at");
        if (!cJSON_IsString(it) || !it->valuestring) {
            return PKI_ERR_SCHEMA;
        }
        rc = pki_parse_iso8601_utc(it->valuestring, &new_exp);
        if (rc != PKI_OK) {
            return rc;
        }
        /* Rotation must never shorten the trust anchor's lifetime. */
        if (new_exp <= (uint64_t)TIER1_CERT_EXPIRES_AT) {
            ESP_LOGW(TAG, "rotate_tier1: new expiry not monotonic");
            return PKI_ERR_SCHEMA;
        }
        /* Tier 0 countersignature over the rotation payload — REQUIRED
         * (brief: a compromised Tier 1 must not rotate to its own key).
         * Carried in params as tier0_sig_hex (128 hex chars) + serial. */
        it = cJSON_GetObjectItem(params, "tier0_sig_hex");
        {
            const cJSON *ser = cJSON_GetObjectItem(params, "serial");
            uint8_t sig[64];
            if (!cJSON_IsString(it) || !it->valuestring ||
                pki_hex_to_bytes(it->valuestring, sig, 64) != 64 ||
                !cJSON_IsNumber(ser)) {
                ESP_LOGW(TAG, "rotate_tier1: missing/invalid Tier 0 "
                              "countersignature");
                return PKI_ERR_SCHEMA;
            }
            rc = pki_verify_rotate_tier1(tmp, new_exp,
                                         (uint32_t)ser->valuedouble, sig);
            if (rc != PKI_OK) {
                return rc;
            }
        }
        return PKI_OK;
    }

    case PKI_CMD_FACTORY_RESET_KEYS:
        it = cJSON_GetObjectItem(params, "confirm");
        if (!cJSON_IsTrue(it)) {
            return PKI_ERR_SCHEMA; /* explicit confirm:true required */
        }
        return PKI_OK;

    default:
        return PKI_ERR_SCHEMA;
    }
}

/* ------------------------------------------------------------------ */
/* pki_verify_provision                                                */
/* ------------------------------------------------------------------ */

pki_rc_t pki_verify_provision(const uint8_t *cmd_json, size_t len,
                              const uint8_t *sig_raw64,
                              pki_provision_cmd_t *out)
{
    char *tmp = NULL;
    cJSON *root = NULL;
    const cJSON *params;
    const char *fmt, *cmd, *device, *nonce_hex, *issued_s, *expires_s;
    uint8_t nonce[PKI_NONCE_RAW_LEN];
    uint64_t issued, expires, now;
    char params_canon[PKI_JSON_MAX];
    uint8_t canon[PKI_CANON_MAX];
    size_t canon_len = 0;
    uint8_t tier1[65];
    pki_cmd_kind_t kind;
    pki_rc_t rc;

    if (!cmd_json || !sig_raw64 || !out || len == 0 ||
        len > PKI_JSON_MAX) {
        return PKI_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));

    tmp = (char *)malloc(len + 1);
    if (!tmp) {
        return PKI_ERR_NOMEM;
    }
    memcpy(tmp, cmd_json, len);
    tmp[len] = '\0';
    root = cJSON_Parse(tmp);
    free(tmp);
    tmp = NULL;
    if (!root) {
        return PKI_ERR_PARSE;
    }

    /* ---- field extraction ---- */
    fmt = req_str(root, "format");
    cmd = req_str(root, "cmd");
    device = req_str(root, "device");
    nonce_hex = req_str(root, "nonce");
    issued_s = req_str(root, "issued_at");
    expires_s = req_str(root, "expires_at");
    params = cJSON_GetObjectItem(root, "params");
    if (!fmt || strcmp(fmt, PROV_FORMAT) != 0 || !cmd || !device ||
        !nonce_hex || !issued_s || !expires_s || !params) {
        rc = PKI_ERR_SCHEMA;
        goto done;
    }
    if (strlen(cmd) >= PKI_CMD_NAME_MAX ||
        strlen(device) >= PKI_DEVICE_MAX) {
        rc = PKI_ERR_SCHEMA;
        goto done;
    }
    kind = cmd_kind_of(cmd);
    if (kind == PKI_CMD_UNKNOWN) {
        rc = PKI_ERR_SCHEMA; /* Tier 1 signs provisioning cmds ONLY */
        goto done;
    }
    if (pki_hex_to_bytes(nonce_hex, nonce, PKI_NONCE_RAW_LEN) !=
        PKI_NONCE_RAW_LEN) {
        rc = PKI_ERR_SCHEMA;
        goto done;
    }
    rc = pki_parse_iso8601_utc(issued_s, &issued);
    if (rc != PKI_OK) {
        goto done;
    }
    rc = pki_parse_iso8601_utc(expires_s, &expires);
    if (rc != PKI_OK) {
        goto done;
    }

    /* ---- canonical bytes + Tier 1 signature ---- */
    rc = pki_canon_json(params, params_canon, sizeof(params_canon),
                        NULL);
    if (rc != PKI_OK) {
        goto done;
    }
    rc = pki_build_provision_canon(cmd, device, nonce, issued, expires,
                                   params_canon, canon, sizeof(canon),
                                   &canon_len);
    if (rc != PKI_OK) {
        goto done;
    }
    pki_active_tier1_pubkey(tier1);
    if (pki_ecdsa_p256_verify(tier1, canon, canon_len,
                              sig_raw64) != 0) {
        ESP_LOGW(TAG, "provision: Tier 1 signature INVALID (cmd=%s)", cmd);
        rc = PKI_ERR_BADSIG;
        goto done;
    }

    /* ---- validity window ---- */
    if (issued > expires) {
        rc = PKI_ERR_WINDOW;
        goto done;
    }
    if (expires - issued > PKI_PROV_WINDOW_S) {
        /* Brief: expiry window <= 15 min, enforced by the Lambda
         * pre-sign; the device re-checks so a misbehaving signer
         * cannot mint long-lived commands. */
        ESP_LOGW(TAG, "provision: window > 15 min");
        rc = PKI_ERR_WINDOW;
        goto done;
    }
    now = pki_time_now();
    if (now != 0) {
        if (now < issued) {
            rc = PKI_ERR_NOTYET;
            goto done;
        }
        if (now > expires) {
            rc = PKI_ERR_EXPIRED;
            goto done;
        }
    } else {
        /* No wall clock (no SNTP): the absolute check is deferred.
         * Logged so the gap is visible until the full task wires a
         * time source. */
        ESP_LOGW(TAG, "provision: no wall clock; absolute expiry "
                      "window not checked (issued<=expires + 15min "
                      "window enforced)");
    }

    /* ---- nonce replay ---- */
    if (pki_nonce_seen(nonce)) {
        ESP_LOGW(TAG, "provision: replayed nonce");
        rc = PKI_ERR_NONCE;
        goto done;
    }

    /* ---- device scope ---- */
    if (strcmp(device, "*") == 0) {
        if (s_keyed_ && s_ctx_set_) {
            /* Keyed devices ignore wildcard commands. */
            ESP_LOGW(TAG, "provision: wildcard refused (keyed)");
            rc = PKI_ERR_SCOPE;
            goto done;
        }
    } else if (!s_ctx_set_ || strcmp(device, s_device_id_) != 0) {
        rc = PKI_ERR_SCOPE;
        goto done;
    }

    /* ---- per-cmd params schema ---- */
    rc = validate_params(kind, params);
    if (rc != PKI_OK) {
        goto done;
    }

    /* ---- accept: fill out, record nonce ---- */
    rc = pki_nonce_record(nonce);
    if (rc != PKI_OK) {
        goto done;
    }
    out->kind = kind;
    strncpy(out->cmd, cmd, sizeof(out->cmd) - 1);
    strncpy(out->device, device, sizeof(out->device) - 1);
    memcpy(out->nonce, nonce, PKI_NONCE_RAW_LEN);
    out->issued_at = issued;
    out->expires_at = expires;
    strncpy(out->params_json, params_canon, sizeof(out->params_json) - 1);
    rc = PKI_OK;

done:
    if (root) {
        cJSON_Delete(root);
    }
    {
        volatile uint8_t *vc = canon;
        size_t i;
        for (i = 0; i < sizeof(canon); i++) {
            vc[i] = 0;
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* pki_verify_rotate_tier1                                             */
/* ------------------------------------------------------------------ */

pki_rc_t pki_verify_rotate_tier1(const uint8_t new_pub65[65],
                                 uint64_t expires_at, uint32_t serial,
                                 const uint8_t sig64[64])
{
    uint8_t canon[sizeof(ROTATE_MAGIC) - 1 + 65 + 8 + 4];
    size_t canon_len = 0;
    pki_rc_t rc;
    int vr;

    if (!new_pub65 || !sig64) {
        return PKI_ERR_ARG;
    }
    if (serial == 0) {
        return PKI_ERR_SCHEMA;
    }
    /* Anti-replay / anti-downgrade for the Tier-0-direct rotation path
     * (compromised Tier 1 cannot block or roll back its own rotation):
     * the new cert must strictly extend the baked trust anchor. */
    if (expires_at <= (uint64_t)TIER1_CERT_EXPIRES_AT) {
        ESP_LOGW(TAG, "rotate: expiry not monotonic");
        return PKI_ERR_SCHEMA;
    }
    if (serial <= (uint32_t)TIER1_CERT_SERIAL) {
        ESP_LOGW(TAG, "rotate: serial not monotonic");
        return PKI_ERR_SCHEMA;
    }
    rc = pki_build_rotate_canon(new_pub65, expires_at, serial,
                                canon, sizeof(canon), &canon_len);
    if (rc != PKI_OK) {
        return rc;
    }
    /* Host unit-test builds link their own pki_ed25519_verify stub. */
    vr = pki_ed25519_verify(TIER0_PUBKEY, canon, canon_len, sig64);
    {
        volatile uint8_t *vc = canon;
        size_t i;
        for (i = 0; i < sizeof(canon); i++) {
            vc[i] = 0;
        }
    }
    if (vr != 0) {
        ESP_LOGW(TAG, "rotate: Tier 0 signature INVALID");
        return PKI_ERR_BADSIG;
    }
    return PKI_OK;
}
