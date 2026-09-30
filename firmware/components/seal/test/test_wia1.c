/* SPDX-License-Identifier: MIT
 * WIA1 Phase 3 session-machine host unit test.
 *
 * Cross-verifies the device-side implementation (wia1.c, upstream
 * Monocypher + optional/monocypher-ed25519) against the independent
 * Python oracle (oracle_wia1.py, PyNaCl): golden envelopes built by the
 * oracle must verify in C, C-built artifacts must match the oracle's
 * bytes, and every named rejection in §6 must fire with its exact log
 * string. Also exercises the full arm/cert/session/CTL lifecycle,
 * grace mode, lockout, expiry, and the weak raw-HID hooks.
 *
 * Build (from components/seal/):
 *   python3 test/oracle_wia1.py
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -DWIA1_HOST_TEST \
 *       -Iinclude -Itest -Itest/host_stubs -I. \
 *       test/test_wia1.c wia1.c monocypher.c optional/monocypher-ed25519.c \
 *       -o /tmp/test_wia1 && /tmp/test_wia1
 *
 * Exit 0 on ALL PASS, 1 on any failure. Test doubles live here, never
 * in firmware: the crypto is the real upstream Monocypher.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "wia1.h"     /* contract header, unmodified; WIA1_HOST_TEST set */
#include "vectors.h"  /* oracle-generated golden vectors + root hooks */
#include "monocypher.h"
#include "optional/monocypher-ed25519.h"

/* ---- wia1.c HOST_TEST-only hooks (declared here, defined there) ---- */
wia1_rc wia1_ingest_ctl_r(const uint8_t *env, size_t env_len,
                          const char *topic, bool retained);
void wia1_kdf_label(const uint8_t ss[32], const uint8_t sess_id[16],
                    const char *label, uint8_t out[32]);
void wia1_claim_info(char *out, size_t cap);
void wia1_arm_shortcode(char out[7]);
void wia1_banner_state(char *out, size_t cap);
int  wia1_wants_status_reply(void);
int  wia1_poll_expiry(void);
void wia1_test_last_log(char *out, size_t cap);
void wia1_test_reset(void);
void wia1_test_session_keys(uint8_t mac[32], uint8_t aead[32],
                            uint8_t cmac[32], uint8_t caead[32],
                            uint8_t sid[16]);
void wia1_test_nvs_clear(void);
uint32_t wia1_test_random(void);
uint64_t wia1_test_now_ms(void);
void wia1_test_set_now(uint64_t ms);
void wia1_test_rng_seed(uint64_t seed);
int  wia1_test_nvs_read(const char *key, void *out, size_t *inout_len);
/* v0.4.7: sync NVS API stubs (ns + timeout). */
esp_err_t seal_dev_nvs_write_blob_sync(const char *ns, const char *key,
                                       const void *data, size_t len,
                                       uint32_t timeout_ms);
esp_err_t seal_dev_nvs_read_blob_sync(const char *ns, const char *key,
                                      void *out, size_t *inout_len,
                                      uint32_t timeout_ms);
/* esp_err_t values used by the stubs (host has no esp_err.h). */
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL 1
#endif
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x105
#endif
#ifndef ESP_ERR_INVALID_SIZE
#define ESP_ERR_INVALID_SIZE 0x106
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif

/* ================= deterministic doubles ================= */

static uint64_t s_rng;   /* splitmix64 */
static uint64_t s_now_ms;

void wia1_test_set_now(uint64_t ms) { s_now_ms = ms; }
uint64_t wia1_test_now_ms(void) { return s_now_ms; }
void wia1_test_rng_seed(uint64_t seed) { s_rng = seed; }
uint32_t wia1_test_random(void)
{
    s_rng += 0x9E3779B97F4A7C15ULL;
    uint64_t z = s_rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (uint32_t)(z & 0xFFFFFFFFu);
}

/* NVS double: simple key/blob table. */
#define NVS_MAX 8
static struct { char key[32]; uint8_t blob[128]; size_t len; } s_nvs[NVS_MAX];
static int s_nvs_n;

void wia1_test_nvs_clear(void) { s_nvs_n = 0; }

/* v0.4.7: matches seal_dev_nvs_write_blob_sync (ns + timeout). */
esp_err_t seal_dev_nvs_write_blob_sync(const char *ns, const char *key,
                                       const void *data, size_t len,
                                       uint32_t timeout_ms)
{
    (void)ns;
    (void)timeout_ms;
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) {
            if (len > sizeof(s_nvs[i].blob)) {
                len = sizeof(s_nvs[i].blob);
            }
            memcpy(s_nvs[i].blob, data, len);
            s_nvs[i].len = len;
            return ESP_OK;
        }
    }
    if (s_nvs_n < NVS_MAX && len <= 128) {
        snprintf(s_nvs[s_nvs_n].key, sizeof(s_nvs[0].key), "%s", key);
        memcpy(s_nvs[s_nvs_n].blob, data, len);
        s_nvs[s_nvs_n].len = len;
        s_nvs_n++;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

/* v0.4.7: matches seal_dev_nvs_read_blob_sync (ns + timeout). */
esp_err_t seal_dev_nvs_read_blob_sync(const char *ns, const char *key,
                                      void *out, size_t *inout_len,
                                      uint32_t timeout_ms)
{
    (void)ns;
    (void)timeout_ms;
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) {
            if (s_nvs[i].len > *inout_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(out, s_nvs[i].blob, s_nvs[i].len);
            *inout_len = s_nvs[i].len;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

int wia1_test_nvs_read(const char *key, void *out, size_t *inout_len)
{
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) {
            if (*inout_len < s_nvs[i].len) {
                return -1;
            }
            memcpy(out, s_nvs[i].blob, s_nvs[i].len);
            *inout_len = s_nvs[i].len;
            return 0;
        }
    }
    return -1;
}

/* hid_ble doubles: record calls for assertions. */
static int s_moves[256][2];
static int s_n_moves;
static struct { char btn[8]; int pressed; int click; } s_btns[64];
static int s_n_btns;
static int s_wheels[64];
static int s_n_wheels;
static uint8_t s_keyr_mod;
static uint8_t s_keyr_keys[6];
static int s_n_keyr;
static uint16_t s_cons[32];
static int s_n_cons;

static void hid_clear(void)
{
    s_n_moves = 0;
    s_n_btns = 0;
    s_n_wheels = 0;
    s_n_keyr = 0;
    s_n_cons = 0;
}

esp_err_t hid_ble_mouse_move(int dx, int dy)
{
    if (s_n_moves < 256) {
        s_moves[s_n_moves][0] = dx;
        s_moves[s_n_moves][1] = dy;
        s_n_moves++;
    }
    return ESP_OK;
}

esp_err_t hid_ble_mouse_wheel(int delta)
{
    if (s_n_wheels < 64) {
        s_wheels[s_n_wheels++] = delta;
    }
    return ESP_OK;
}

esp_err_t hid_ble_mouse_click(const char *button)
{
    if (s_n_btns < 64) {
        snprintf(s_btns[s_n_btns].btn, sizeof(s_btns[0].btn), "%s", button);
        s_btns[s_n_btns].pressed = 0;
        s_btns[s_n_btns].click = 1;
        s_n_btns++;
    }
    return ESP_OK;
}

esp_err_t hid_ble_mouse_button(const char *button, bool pressed)
{
    if (s_n_btns < 64) {
        snprintf(s_btns[s_n_btns].btn, sizeof(s_btns[0].btn), "%s", button);
        s_btns[s_n_btns].pressed = pressed ? 1 : 0;
        s_btns[s_n_btns].click = 0;
        s_n_btns++;
    }
    return ESP_OK;
}

/* Strong overrides of wia1.c's weak raw-HID stubs. */
int wia1_raw_key_report(uint8_t mod, const uint8_t keys[6])
{
    s_keyr_mod = mod;
    memcpy(s_keyr_keys, keys, 6);
    s_n_keyr++;
    return 0;
}

int wia1_raw_consumer(uint16_t usage)
{
    if (s_n_cons < 32) {
        s_cons[s_n_cons++] = usage;
    }
    return 0;
}

/* ================= CHECK framework ================= */

static int s_pass;
static int s_fail;

#define CHECK(cond, name) do {                                          \
        if (cond) {                                                     \
            s_pass++;                                                   \
            printf("  PASS %s\n", name);                                \
        } else {                                                        \
            s_fail++;                                                   \
            printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);     \
        }                                                               \
    } while (0)

static bool last_log_is(const char *s)
{
    char buf[160];
    wia1_test_last_log(buf, sizeof(buf));
    return strcmp(buf, s) == 0;
}

static bool last_log_starts(const char *prefix)
{
    char buf[160];
    wia1_test_last_log(buf, sizeof(buf));
    return strncmp(buf, prefix, strlen(prefix)) == 0;
}

static bool last_log_has(const char *sub)
{
    char buf[160];
    wia1_test_last_log(buf, sizeof(buf));
    return strstr(buf, sub) != NULL;
}

/* ================= envelope builders (test-side agent) ================= */

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void w64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

static char s_topic_ctl[64];
static char s_topic_tel[64];

static void make_topics(void)
{
    snprintf(s_topic_ctl, sizeof(s_topic_ctl), "wia/%s/ctl", VEC_DEV_ID);
    snprintf(s_topic_tel, sizeof(s_topic_tel), "wia/%s/tel", VEC_DEV_ID);
}

/* sig_mode: 0 = sign with agent session key, 1 = all-zero, 2 = sign+corrupt */
static size_t build_ctl(uint8_t *out, uint16_t msg_type,
                        const uint8_t *cbody, size_t cbody_len,
                        uint64_t ctr, uint64_t ts,
                        const uint8_t dev_id[16], const uint8_t sess_id[16],
                        const char *topic, int sig_mode)
{
    uint8_t *h = out;
    memcpy(h, "WIA1", 4);
    h[4] = WIA1_KIND_CTL;
    h[5] = 0;
    memcpy(h + 6, dev_id, 16);
    memcpy(h + 22, sess_id, 16);
    w64(h + 38, ctr);
    w64(h + 46, ts);
    w16(h + 54, msg_type);
    memset(h + 56, 0, 8);
    memcpy(out + 64, cbody, cbody_len);
    size_t tlen = strlen(topic);
    uint8_t S[2 + 128 + 64 + 512];
    S[0] = (uint8_t)(tlen & 0xFF);
    S[1] = (uint8_t)((tlen >> 8) & 0xFF);
    memcpy(S + 2, topic, tlen);
    memcpy(S + 2 + tlen, out, 64 + cbody_len);
    uint8_t *sig = out + 64 + cbody_len;
    if (sig_mode == 1) {
        memset(sig, 0, 64);
    } else {
        crypto_ed25519_sign(sig, VEC_AGENT_ED_SK64, S, 2 + tlen + 64 + cbody_len);
        if (sig_mode == 2) {
            sig[17] ^= 0x40;
        }
    }
    return 64 + cbody_len + 64;
}

/* root: 0 = primary, 1 = backup, 2 = corrupt primary sig */
static size_t build_cert(uint8_t *out, uint8_t ver, uint16_t scope,
                         uint32_t ttl, const uint8_t dev_id16[16],
                         const uint8_t nonce[16], const uint8_t dev_x_pk[32],
                         int root)
{
    uint8_t *h = out;
    memcpy(h, "WIA1", 4);
    h[4] = WIA1_KIND_CERT;
    h[5] = 0;
    memcpy(h + 6, VEC_DEV_ID, 16);
    memset(h + 22, 0, 16 + 8 + 8);
    w16(h + 54, 0);
    memset(h + 56, 0, 8);
    uint8_t *b = out + 64;
    b[0] = ver;
    w16(b + 1, scope);
    b[3] = (uint8_t)(ttl & 0xFF);
    b[4] = (uint8_t)((ttl >> 8) & 0xFF);
    b[5] = (uint8_t)((ttl >> 16) & 0xFF);
    b[6] = (uint8_t)((ttl >> 24) & 0xFF);
    memcpy(b + 7, dev_id16, 16);
    memcpy(b + 23, nonce, 16);
    memcpy(b + 39, VEC_AGENT_ED_PK, 32);
    memcpy(b + 71, VEC_AGENT_X_PK, 32);
    memcpy(b + 103, dev_x_pk, 32);
    uint8_t S[64 + 135];
    memcpy(S, h, 64);
    memcpy(S + 64, b, 135);
    uint8_t *sig = out + 64 + 135;
    const uint8_t *rk = (root == 1) ? VEC_ROOT_BACKUP_SK64 : VEC_ROOT_PRIMARY_SK64;
    crypto_ed25519_sign(sig, rk, S, sizeof(S));
    if (root == 2) {
        sig[3] ^= 0x80;
    }
    return 64 + 135 + 64;
}

/* Challenge fields out of a kind-4 envelope (for building certs). */
static void chal_fields(const uint8_t *chal, uint8_t nonce[16],
                        uint8_t x_pk[32])
{
    memcpy(nonce, chal + 64 + 4, 16);
    memcpy(x_pk, chal + 64 + 20, 32);
}

/* ================= fixtures ================= */

#define T0 1000000000ULL  /* fixed test clock (ms) */

static bool arm_mouse(void)
{
    uint8_t chal[180];
    wia1_test_reset();
    hid_clear();
    wia1_test_set_now(T0);
    wia1_test_rng_seed(VEC_RNG_S1_ARM);
    int n = wia1_arm_tap(chal, sizeof(chal));
    if (n != (int)sizeof(VEC_CHAL)) {
        return false;
    }
    int rc = wia1_ingest_cert(VEC_CERT, sizeof(VEC_CERT));
    return rc == 1 && wia1_is_armed();
}

/* Keyboard-scope cert -> awaiting accept tap (not armed). chal_out gets
 * the fresh challenge so the cert binds to it. */
static bool arm_keyboard_pending(uint8_t chal[180], uint8_t cert[263])
{
    uint8_t nonce[16], x_pk[32];
    wia1_test_reset();
    hid_clear();
    wia1_test_set_now(T0);
    wia1_test_rng_seed(VEC_RNG_S1_ARM);
    int n = wia1_arm_tap(chal, 180);
    if (n != 180) {
        return false;
    }
    chal_fields(chal, nonce, x_pk);
    build_cert(cert, 1, 0x0003, 3600, (const uint8_t *)VEC_DEV_ID,
               nonce, x_pk, 0);
    int rc = wia1_ingest_cert(cert, 263);
    return rc == 0 && wia1_needs_accept_tap() && !wia1_is_armed();
}

/* ================= main ================= */

int main(void)
{
    printf("== WIA1 host test ==\n");
    make_topics();

    /* ---- one-time device init (identity minted from RNG_S0) ---- */
    wia1_test_nvs_clear();
    wia1_test_rng_seed(VEC_RNG_S0_IDENT);
    wia1_test_set_now(T0);
    CHECK(wia1_init() == ESP_OK, "init ok");

    printf("-- primitives: RFC 8032 + identity --\n");
    {
        uint8_t sk[64], pk[32], seed32[32];
        /* NB: Monocypher's crypto_ed25519_key_pair WIPES its seed argument,
         * so feed it a mutable copy, never a const vector. */
        memcpy(seed32, VEC_RFC8032_T1_SEED, 32);
        crypto_ed25519_key_pair(sk, pk, seed32);
        CHECK(memcmp(pk, VEC_RFC8032_T1_PK, 32) == 0,
              "rfc8032 t1 key_pair reproduces pubkey");
        uint8_t sig[64];
        crypto_ed25519_sign(sig, sk, (const uint8_t *)"", 0);
        CHECK(memcmp(sig, VEC_RFC8032_T1_SIG, 64) == 0,
              "rfc8032 t1 sign deterministic, matches RFC");
        uint8_t bad[64];
        memcpy(bad, VEC_RFC8032_T1_SIG, 64);
        bad[0] ^= 0x01;
        CHECK(crypto_ed25519_check(bad, VEC_RFC8032_T1_PK,
                                   (const uint8_t *)"", 0) != 0,
              "rfc8032 t1 tampered sig rejected");
    }
    CHECK(strcmp(wia1_dev_id(), VEC_DEV_ID) == 0, "dev_id matches oracle");
    {
        char ci[80];
        wia1_claim_info(ci, sizeof(ci));
        CHECK(strstr(ci, VEC_DEV_ID) != NULL && strstr(ci, VEC_FP16) != NULL,
              "claim_info carries dev_id + fp16");
    }

    printf("-- golden arm challenge (oracle byte-identical) --\n");
    {
        uint8_t chal[180];
        char sc[7];
        wia1_test_reset();
        wia1_test_set_now(T0);
        wia1_test_rng_seed(VEC_RNG_S1_ARM);
        int n = wia1_arm_tap(chal, sizeof(chal));
        CHECK(n == (int)sizeof(VEC_CHAL), "arm_tap returns 180");
        CHECK(memcmp(chal, VEC_CHAL, sizeof(VEC_CHAL)) == 0,
              "challenge envelope byte-identical to PyNaCl oracle");
        wia1_arm_shortcode(sc);
        CHECK(strcmp(sc, VEC_SHORTCODE) == 0, "shortcode matches oracle");
        wia1_disarm_tap();
    }

    printf("-- golden cert ingest + session --\n");
    {
        uint8_t chal[180];
        uint8_t mac[32], aead[32], cmac[32], caead[32], sid[16];
        char sidhex[17];
        wia1_test_reset();
        hid_clear();
        wia1_test_set_now(T0);
        wia1_test_rng_seed(VEC_RNG_S1_ARM);
        CHECK(wia1_arm_tap(chal, sizeof(chal)) == 180, "re-arm for cert");
        int rc = wia1_ingest_cert(VEC_CERT, sizeof(VEC_CERT));
        CHECK(rc == 1, "mouse cert -> ARMED immediately (rc=1)");
        CHECK(wia1_is_armed(), "is_armed true");
        wia1_test_session_keys(mac, aead, cmac, caead, sid);
        CHECK(memcmp(sid, VEC_SESSION_ID, 16) == 0,
              "session_id == BLAKE2b-16(cert_body)");
        CHECK(memcmp(mac, VEC_K_TEL_MAC, 32) == 0, "k_tel_mac matches oracle");
        CHECK(memcmp(aead, VEC_K_TEL_AEAD, 32) == 0, "k_tel_aead matches oracle");
        CHECK(memcmp(cmac, VEC_K_CTL_MAC, 32) == 0, "k_ctl_mac matches oracle");
        CHECK(memcmp(caead, VEC_K_CTL_AEAD, 32) == 0,
              "k_ctl_aead matches oracle");
        wia1_session_id_hex(sidhex);
        {
            char expect[17];
            static const char hx[] = "0123456789abcdef";
            for (int i = 0; i < 8; i++) {
                expect[2 * i] = hx[(VEC_SESSION_ID[i] >> 4) & 0xF];
                expect[2 * i + 1] = hx[VEC_SESSION_ID[i] & 0xF];
            }
            expect[16] = '\0';
            CHECK(strcmp(sidhex, expect) == 0, "session_id_hex == hex16");
        }
        wia1_disarm_tap();
        wia1_session_id_hex(sidhex);
        CHECK(sidhex[0] == '\0', "session_id_hex empty when disarmed");
    }

    printf("-- standalone KDF vectors --\n");
    {
        uint8_t out[32];
        wia1_kdf_label(VEC_KDF_SS, VEC_KDF_SID, "tel-mac", out);
        CHECK(memcmp(out, VEC_KDF_TEL_MAC, 32) == 0, "kdf tel-mac");
        wia1_kdf_label(VEC_KDF_SS, VEC_KDF_SID, "tel-aead", out);
        CHECK(memcmp(out, VEC_KDF_TEL_AEAD, 32) == 0, "kdf tel-aead");
        wia1_kdf_label(VEC_KDF_SS, VEC_KDF_SID, "ctl-mac", out);
        CHECK(memcmp(out, VEC_KDF_CTL_MAC, 32) == 0, "kdf ctl-mac");
        wia1_kdf_label(VEC_KDF_SS, VEC_KDF_SID, "ctl-aead", out);
        CHECK(memcmp(out, VEC_KDF_CTL_AEAD, 32) == 0, "kdf ctl-aead");
    }

    printf("-- golden telemetry seal (kind 1 MAC, byte-identical) --\n");
    CHECK(arm_mouse(), "arm for telemetry test");
    {
        uint8_t out[128];
        const uint8_t ack_body[3] = { 0x01, 0x01, 0x00 };
        int n = wia1_seal_tel(WIA1_TEL_ARMED_ACK, ack_body, sizeof(ack_body),
                              out, sizeof(out), false);
        CHECK(n == (int)sizeof(VEC_TEL_MAC), "seal_tel mac length");
        CHECK(n > 0 && memcmp(out, VEC_TEL_MAC, sizeof(VEC_TEL_MAC)) == 0,
              "sealed TEL_ARMED_ACK byte-identical to oracle");
    }

    printf("-- golden telemetry ENC decrypt (libsodium -> monocypher) --\n");
    {
        /* VEC_TEL_ENC = hdr || nonce24 || ct||tag16; open with k_tel_aead */
        const uint8_t *e = VEC_TEL_ENC;
        size_t body_len = 64; /* VEC_FRAME_BODY */
        CHECK(e[4] == WIA1_KIND_TEL && (e[5] & WIA1_FLAG_ENC) != 0,
              "enc envelope kind/flags");
        CHECK(memcmp(e + 6, VEC_DEV_ID, 16) == 0, "enc dev_id");
        CHECK(memcmp(e + 22, VEC_SESSION_ID, 16) == 0, "enc session_id");
        const uint8_t *nonce = e + 64;
        const uint8_t *ct = e + 64 + 24;
        size_t ct_len = sizeof(VEC_TEL_ENC) - 64 - 24;
        uint8_t aad[2 + 128 + 64];
        size_t tlen = strlen(s_topic_tel);
        aad[0] = (uint8_t)(tlen & 0xFF);
        aad[1] = (uint8_t)((tlen >> 8) & 0xFF);
        memcpy(aad + 2, s_topic_tel, tlen);
        memcpy(aad + 2 + tlen, e, 64);
        uint8_t pt[64];
        int r = crypto_aead_unlock(pt, ct + ct_len - 16, VEC_K_TEL_AEAD,
                                   nonce, aad, 2 + tlen + 64,
                                   ct, ct_len - 16);
        CHECK(r == 0, "monocypher opens libsodium-sealed frame");
        CHECK(r == 0 && memcmp(pt, VEC_FRAME_BODY, body_len) == 0,
              "frame plaintext matches oracle");
    }

    printf("-- golden CTL ingest + HID execution --\n");
    {
        hid_clear();
        wia1_rc rc = wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl);
        CHECK(rc == WIA1_OK, "golden CTL accepted");
        CHECK(s_n_moves == 1 && s_moves[0][0] == 10 && s_moves[0][1] == -20,
              "MOVE_REL(10,-20) executed");
        CHECK(s_n_btns == 1 && strcmp(s_btns[0].btn, "left") == 0 &&
              s_btns[0].click == 1, "BUTTON left click executed");
        CHECK(s_n_wheels == 1 && s_wheels[0] == 5, "WHEEL(5) executed");
    }

    printf("-- §6 rejection battery --\n");
    /* retained */
    CHECK(arm_mouse(), "arm: retained case");
    {
        wia1_rc rc = wia1_ingest_ctl_r(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl,
                                       true);
        CHECK(rc == WIA1_E_RETAINED, "retained -> E_RETAINED");
        CHECK(last_log_is("RETAINED CTL - DROPPED"), "retained log string");
    }
    /* malformed */
    CHECK(arm_mouse(), "arm: malformed cases");
    {
        uint8_t short_env[100];
        memcpy(short_env, VEC_CTL, sizeof(short_env));
        CHECK(wia1_ingest_ctl(short_env, sizeof(short_env), s_topic_ctl) ==
              WIA1_E_SHORT, "short envelope rejected");
        CHECK(last_log_is("MALFORMED"), "short log = MALFORMED");
        uint8_t badmagic[sizeof(VEC_CTL)];
        memcpy(badmagic, VEC_CTL, sizeof(badmagic));
        badmagic[0] = 'X';
        CHECK(wia1_ingest_ctl(badmagic, sizeof(badmagic), s_topic_ctl) ==
              WIA1_E_KIND, "bad magic rejected");
        CHECK(last_log_is("MALFORMED"), "bad magic log = MALFORMED");
        uint8_t badflags[sizeof(VEC_CTL)];
        memcpy(badflags, VEC_CTL, sizeof(badflags));
        badflags[5] = 0x02; /* reserved flag bit */
        CHECK(wia1_ingest_ctl(badflags, sizeof(badflags), s_topic_ctl) ==
              WIA1_E_KIND, "nonzero reserved flags rejected");
        CHECK(last_log_is("MALFORMED"), "bad flags log = MALFORMED");
        uint8_t badrsv[sizeof(VEC_CTL)];
        memcpy(badrsv, VEC_CTL, sizeof(badrsv));
        badrsv[60] = 0x01;
        CHECK(wia1_ingest_ctl(badrsv, sizeof(badrsv), s_topic_ctl) ==
              WIA1_E_KIND, "nonzero rsv rejected");
        CHECK(last_log_is("MALFORMED"), "bad rsv log = MALFORMED");
        uint8_t kind5[sizeof(VEC_CTL)];
        memcpy(kind5, VEC_CTL, sizeof(kind5));
        kind5[4] = 5; /* kind 5 reserved */
        CHECK(wia1_ingest_ctl(kind5, sizeof(kind5), s_topic_ctl) == WIA1_E_KIND,
              "kind 5 (reserved) rejected");
        CHECK(last_log_is("MALFORMED"), "kind5 log = MALFORMED");
    }
    /* wrong device (re-signed over the wrong id: structural check first) */
    CHECK(arm_mouse(), "arm: wrong-device case");
    {
        uint8_t env[640], dev[16], sid[16];
        uint8_t body[13];
        memcpy(body, VEC_CTL + 64, 13);
        memcpy(dev, VEC_DEV_ID, 16);
        dev[4] ^= 0x01;
        memcpy(sid, VEC_SESSION_ID, 16);
        size_t n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 7, 0,
                             dev, sid, s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_WRONG_DEV,
              "wrong dev_id -> E_WRONG_DEV");
        CHECK(last_log_is("WRONG DEVICE"), "wrong device log string");
    }
    /* CTL while disarmed */
    {
        wia1_test_reset(); /* disarmed, identity survives */
        wia1_test_set_now(T0);
        CHECK(wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl) ==
              WIA1_E_DISARMED, "disarmed -> E_DISARMED");
        CHECK(last_log_is("CTL while DISARMED"), "disarmed log string");
    }
    /* stale session */
    CHECK(arm_mouse(), "arm: stale-session case");
    {
        uint8_t env[640], body[13], sid[16];
        memcpy(body, VEC_CTL + 64, 13);
        memset(sid, 0xAA, sizeof(sid));
        size_t n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 7, 0,
                             (const uint8_t *)VEC_DEV_ID, sid, s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_STALE_SESSION,
              "wrong session_id -> E_STALE_SESSION");
        CHECK(last_log_is("STALE SESSION"), "stale session log string");
    }
    /* bad sig */
    CHECK(arm_mouse(), "arm: bad-sig case");
    {
        uint8_t env[sizeof(VEC_CTL)];
        memcpy(env, VEC_CTL, sizeof(env));
        env[sizeof(env) - 1] ^= 0x01;
        CHECK(wia1_ingest_ctl(env, sizeof(env), s_topic_ctl) == WIA1_E_BADSIG,
              "corrupt sig -> E_BADSIG");
        CHECK(last_log_starts("BAD SIG on wia/"), "bad sig log prefix");
        CHECK(last_log_has("- DROPPED"), "bad sig log suffix");
    }
    /* dup-of-last silent, then replay loud */
    CHECK(arm_mouse(), "arm: dup/replay cases");
    {
        char before[160];
        CHECK(wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl) ==
              WIA1_OK, "ctr=1 accepted");
        wia1_test_last_log(before, sizeof(before));
        CHECK(wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl) ==
              WIA1_OK_DUP, "dup ctr=1 -> OK_DUP silent");
        {
            char after[160];
            wia1_test_last_log(after, sizeof(after));
            CHECK(strcmp(before, after) == 0, "dup leaves no log line");
        }
        uint8_t env[640], body[13];
        memcpy(body, VEC_CTL + 64, 13);
        size_t n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 2, 0,
                             (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                             s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK, "ctr=2 accepted");
        n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 1, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_REPLAY,
              "ctr rollback -> E_REPLAY");
        CHECK(last_log_is("REPLAY ctr 1 <= 2 - DROPPED"), "replay log string");
    }
    /* stale timestamp */
    CHECK(arm_mouse(), "arm: stale-ts case");
    {
        uint8_t env[640], body[13];
        memcpy(body, VEC_CTL + 64, 13);
        size_t n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 7,
                             50000, (const uint8_t *)VEC_DEV_ID,
                             VEC_SESSION_ID, s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_STALE_TS,
              "|dt| > 10 s -> E_STALE_TS");
        CHECK(last_log_is("STALE (|dt|=50000ms)"), "stale log string");
    }
    /* scope violation: KEY_REPORT on a MOUSE-only session */
    CHECK(arm_mouse(), "arm: scope case");
    {
        uint8_t env[640];
        const uint8_t key_body[8] = { 0x10, 0x02, 0x04, 0x05,
                                      0x06, 0x07, 0x08, 0x09 };
        size_t n = build_ctl(env, WIA1_CTL_BATCH, key_body, sizeof(key_body),
                             7, 0, (const uint8_t *)VEC_DEV_ID,
                             VEC_SESSION_ID, s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_SCOPE,
              "KEY_REPORT on mouse session -> E_SCOPE");
        CHECK(last_log_is("SCOPE VIOLATION"), "scope log string");
        CHECK(s_n_keyr == 0, "no keystroke executed");
    }
    /* bad body: unknown op, truncated args, empty batch, >32 ops */
    CHECK(arm_mouse(), "arm: bad-body cases");
    {
        uint8_t env[640];
        const uint8_t unk[1] = { 0xFF };
        size_t n = build_ctl(env, WIA1_CTL_BATCH, unk, sizeof(unk), 7, 0,
                             (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                             s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_BAD_BODY,
              "unknown op -> E_BAD_BODY");
        CHECK(last_log_is("BAD BODY"), "bad body log string");
        const uint8_t trunc[2] = { 0x01, 0x0A }; /* MOVE needs 4 arg bytes */
        n = build_ctl(env, WIA1_CTL_BATCH, trunc, sizeof(trunc), 8, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_BAD_BODY,
              "truncated args -> E_BAD_BODY");
        n = build_ctl(env, WIA1_CTL_BATCH, NULL, 0, 9, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_BAD_BODY,
              "empty batch -> E_BAD_BODY");
        uint8_t many[33 * 5];
        for (int i = 0; i < 33; i++) {
            many[5 * i] = 0x01;
            many[5 * i + 1] = 0x00;
            many[5 * i + 2] = 0x00;
            many[5 * i + 3] = 0x00;
            many[5 * i + 4] = 0x00;
        }
        n = build_ctl(env, WIA1_CTL_BATCH, many, sizeof(many), 10, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_BAD_BODY,
              "33 ops -> E_BAD_BODY");
        /* exactly 32 ops must still work (terminator overflow guard) */
        uint8_t ok32[32 * 5];
        for (int i = 0; i < 32; i++) {
            ok32[5 * i] = 0x01;
            ok32[5 * i + 1] = (uint8_t)(i & 0xFF);
            ok32[5 * i + 2] = 0x00;
            ok32[5 * i + 3] = 0x00;
            ok32[5 * i + 4] = 0x00;
        }
        hid_clear();
        n = build_ctl(env, WIA1_CTL_BATCH, ok32, sizeof(ok32), 11, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK,
              "32 ops accepted");
        CHECK(s_n_moves == 32, "32 moves executed");
    }
    /* cross-topic replay dies at the tag */
    CHECK(arm_mouse(), "arm: cross-topic case");
    {
        CHECK(wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), "wia/other/ctl") ==
              WIA1_E_BADSIG, "wrong topic -> E_BADSIG");
        CHECK(last_log_starts("BAD SIG on wia/other/ctl - DROPPED"),
              "cross-topic log names the delivered topic");
    }

    printf("-- cert rejection battery --\n");
    {
        uint8_t chal[180], cert[263], nonce[16], x_pk[32], bad[263];
        /* bad root signature */
        wia1_test_reset();
        wia1_test_set_now(T0);
        wia1_test_rng_seed(VEC_RNG_S1_ARM);
        CHECK(wia1_arm_tap(chal, sizeof(chal)) == 180, "arm: bad-cert-sig");
        memcpy(bad, VEC_CERT, sizeof(bad));
        bad[sizeof(bad) - 1] ^= 0x01;
        CHECK(wia1_ingest_cert(bad, sizeof(bad)) == WIA1_E_BADSIG,
              "bad root sig -> E_BADSIG");
        CHECK(last_log_is("BAD CERT SIG - DROPPED"), "bad cert sig log");
        CHECK(!wia1_is_armed(), "still disarmed after bad cert");
        /* cert outside arm window (deadline passed) */
        wia1_test_set_now(T0 + 121000);
        CHECK(wia1_ingest_cert(VEC_CERT, sizeof(VEC_CERT)) == WIA1_E_NO_WINDOW,
              "late cert -> E_NO_WINDOW");
        CHECK(last_log_is("CERT out of window - IGNORED"),
              "out-of-window log string");
        /* cert while ARMED */
        CHECK(arm_mouse(), "re-arm for armed-window case");
        CHECK(wia1_ingest_cert(VEC_CERT, sizeof(VEC_CERT)) == WIA1_E_NO_WINDOW,
              "cert while armed -> E_NO_WINDOW");
        CHECK(last_log_is("CERT out of window - IGNORED"),
              "armed-window log string");
        /* field-level rejects (fresh arm each) */
        wia1_test_reset();
        wia1_test_set_now(T0);
        wia1_test_rng_seed(VEC_RNG_S1_ARM);
        CHECK(wia1_arm_tap(chal, sizeof(chal)) == 180, "arm: field rejects");
        chal_fields(chal, nonce, x_pk);
        build_cert(cert, 2, 0x0001, 3600, (const uint8_t *)VEC_DEV_ID,
                   nonce, x_pk, 0);
        CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_BAD_CERT,
              "bad cert_ver rejected");
        CHECK(last_log_is("CERT bad version - DROPPED"), "cert ver log");
        {
            uint8_t dev[16];
            memcpy(dev, VEC_DEV_ID, 16);
            dev[0] = 'x';
            build_cert(cert, 1, 0x0001, 3600, dev, nonce, x_pk, 0);
            CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_WRONG_DEV,
                  "cert wrong device rejected");
            CHECK(last_log_is("CERT wrong device - DROPPED"),
                  "cert device log");
        }
        {
            uint8_t nn[16];
            memcpy(nn, nonce, 16);
            nn[0] ^= 0x01;
            build_cert(cert, 1, 0x0001, 3600, (const uint8_t *)VEC_DEV_ID,
                       nn, x_pk, 0);
            CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_BAD_CERT,
                  "cert wrong nonce rejected");
            CHECK(last_log_is("CERT nonce mismatch - DROPPED"),
                  "cert nonce log");
        }
        {
            uint8_t xp[32];
            memcpy(xp, x_pk, 32);
            xp[0] ^= 0x01;
            build_cert(cert, 1, 0x0001, 3600, (const uint8_t *)VEC_DEV_ID,
                       nonce, xp, 0);
            CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_BAD_CERT,
                  "cert wrong device_x_pk rejected");
            CHECK(last_log_is("CERT x_pk mismatch - DROPPED"),
                  "cert x_pk log");
        }
        build_cert(cert, 1, 0x0001, 5 * 3600, (const uint8_t *)VEC_DEV_ID,
                   nonce, x_pk, 0);
        CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_BAD_CERT,
              "cert ttl > 4 h rejected");
        CHECK(last_log_is("CERT bad ttl - DROPPED"), "cert ttl log");
        build_cert(cert, 1, 0x0001, 0, (const uint8_t *)VEC_DEV_ID,
                   nonce, x_pk, 0);
        CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_BAD_CERT,
              "cert ttl = 0 rejected");
        build_cert(cert, 1, 0x0008, 3600, (const uint8_t *)VEC_DEV_ID,
                   nonce, x_pk, 0);
        CHECK(wia1_ingest_cert(cert, sizeof(cert)) == WIA1_E_BAD_CERT,
              "cert scope exceeds offer rejected");
        CHECK(last_log_is("CERT scope exceeds offer - DROPPED"),
              "cert scope log");
        /* backup root path */
        build_cert(cert, 1, 0x0001, 3600, (const uint8_t *)VEC_DEV_ID,
                   nonce, x_pk, 1);
        CHECK(wia1_ingest_cert(cert, sizeof(cert)) == 1,
              "backup-root cert accepted");
        CHECK(wia1_is_armed(), "armed via backup root");
        wia1_disarm_tap();
    }

    printf("-- keyboard/consumer accept-tap flow --\n");
    {
        uint8_t chal[180], cert[263];
        uint8_t env[640];
        CHECK(arm_keyboard_pending(chal, cert), "keyboard cert -> pending");
        CHECK(wia1_accept_tap() == 0, "accept tap arms");
        CHECK(wia1_is_armed() && !wia1_needs_accept_tap(), "armed, tap done");
        /* KEY_REPORT now works (scope granted) */
        hid_clear();
        const uint8_t key_body[8] = { 0x10, 0x02, 0x04, 0x05,
                                      0x06, 0x07, 0x08, 0x09 };
        size_t n = build_ctl(env, WIA1_CTL_BATCH, key_body, sizeof(key_body),
                             1, 0, (const uint8_t *)VEC_DEV_ID,
                             VEC_SESSION_ID, s_topic_ctl, 0);
        /* note: session_id differs (new cert) -> rebuild below */
        (void)n;
    }
    {
        /* redo cleanly: capture the live session id first */
        uint8_t chal[180], cert[263];
        uint8_t env[640], sid[16];
        CHECK(arm_keyboard_pending(chal, cert), "keyboard cert -> pending (2)");
        CHECK(wia1_accept_tap() == 0, "accept tap arms (2)");
        wia1_test_session_keys(NULL, NULL, NULL, NULL, sid);
        hid_clear();
        const uint8_t key_body[8] = { 0x10, 0x02, 0x04, 0x05,
                                      0x06, 0x07, 0x08, 0x09 };
        size_t n = build_ctl(env, WIA1_CTL_BATCH, key_body, sizeof(key_body),
                             1, 0, (const uint8_t *)VEC_DEV_ID, sid,
                             s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK,
              "KEY_REPORT accepted after tap");
        CHECK(s_n_keyr == 1 && s_keyr_mod == 0x02 && s_keyr_keys[0] == 0x04,
              "raw key report reached the HID hook");
        /* consumer op on a keyboard-only session -> scope violation */
        const uint8_t cons_body[3] = { 0x20, 0xE9, 0x00 };
        n = build_ctl(env, WIA1_CTL_BATCH, cons_body, sizeof(cons_body),
                      2, 0, (const uint8_t *)VEC_DEV_ID, sid, s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_SCOPE,
              "CONSUMER on keyboard session -> E_SCOPE");
        CHECK(s_n_cons == 0, "no consumer usage executed");
        /* consumer cert flow */
        wia1_disarm_tap();
        wia1_test_reset();
        wia1_test_set_now(T0);
        wia1_test_rng_seed(VEC_RNG_S1_ARM);
        CHECK(wia1_arm_tap(chal, sizeof(chal)) == 180, "arm: consumer cert");
        {
            uint8_t nonce[16], x_pk[32];
            chal_fields(chal, nonce, x_pk);
            build_cert(cert, 1, 0x0005, 3600, (const uint8_t *)VEC_DEV_ID,
                       nonce, x_pk, 0);
            CHECK(wia1_ingest_cert(cert, sizeof(cert)) == 0,
                  "consumer cert -> pending accept");
            CHECK(wia1_accept_tap() == 0, "consumer accept tap");
            wia1_test_session_keys(NULL, NULL, NULL, NULL, sid);
            hid_clear();
            n = build_ctl(env, WIA1_CTL_BATCH, cons_body, sizeof(cons_body),
                          1, 0, (const uint8_t *)VEC_DEV_ID, sid,
                          s_topic_ctl, 0);
            CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK,
                  "CONSUMER accepted after tap");
            CHECK(s_n_cons == 1 && s_cons[0] == 0x00E9,
                  "consumer usage reached the HID hook");
        }
        CHECK(wia1_accept_tap() == -1, "accept tap with nothing pending");
    }

    printf("-- ping / disarm / banner / seclog --\n");
    CHECK(arm_mouse(), "arm: ping/disarm");
    {
        uint8_t env[640];
        size_t n = build_ctl(env, WIA1_CTL_PING, NULL, 0, 7, 0,
                             (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                             s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK, "PING accepted");
        CHECK(wia1_wants_status_reply() == 1, "status reply flagged");
        CHECK(wia1_wants_status_reply() == 0, "status flag poll-and-clear");
        {
            char banner[64];
            wia1_banner_state(banner, sizeof(banner));
            CHECK(strncmp(banner, "ARMED M ", 8) == 0, "banner ARMED M");
        }
        n = build_ctl(env, WIA1_CTL_DISARM, NULL, 0, 8, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK,
              "CTL_DISARM accepted");
        CHECK(!wia1_is_armed(), "disarmed by remote");
        CHECK(wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl) ==
              WIA1_E_DISARMED, "CTL after disarm rejected");
        {
            char banner[64];
            wia1_banner_state(banner, sizeof(banner));
            CHECK(strcmp(banner, "DISARMED") == 0, "banner DISARMED");
        }
        /* seclog mirrors the named rejections: last code was DISARMED */
        {
            uint16_t code, count;
            wia1_seclog_poll(&code, &count);
            CHECK(count >= 1, "seclog count nonzero");
            (void)code;
        }
    }

    printf("-- grace mode --\n");
    CHECK(arm_mouse(), "arm: grace");
    {
        uint8_t env[640], body[13];
        memcpy(body, VEC_CTL + 64, 13);
        CHECK(wia1_grace_set(true) == 0 && wia1_grace_on(), "grace on");
        {
            char banner[64];
            wia1_banner_state(banner, sizeof(banner));
            CHECK(strstr(banner, "UNSIGNED") != NULL,
                  "banner shows UNSIGNED MODE in grace");
        }
        hid_clear();
        size_t n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 7, 0,
                             (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                             s_topic_ctl, 1 /* zero sig */);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK,
              "unsigned mouse batch honored in grace");
        CHECK(last_log_has("GRACE unsigned CTL honored"),
              "grace warn logged (1-in-16)");
        CHECK(s_n_moves == 1, "unsigned move executed");
        /* unsigned KEY_REPORT: never honored */
        const uint8_t key_body[8] = { 0x10, 0x02, 0x04, 0x05,
                                      0x06, 0x07, 0x08, 0x09 };
        n = build_ctl(env, WIA1_CTL_BATCH, key_body, sizeof(key_body), 8, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 1);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_SCOPE,
              "unsigned KEY_REPORT -> E_SCOPE");
        CHECK(last_log_is("SCOPE VIOLATION"), "unsigned key scope log");
        /* unsigned DISARM: not a MOUSE-scope opcode -> refused */
        n = build_ctl(env, WIA1_CTL_DISARM, NULL, 0, 9, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 1);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_SCOPE,
              "unsigned DISARM refused");
        CHECK(wia1_is_armed(), "still armed after refused disarm");
        /* signed still works in grace */
        n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 10, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_OK,
              "signed CTL still honored in grace");
        /* present-but-wrong sig is fatal even in grace */
        n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 11, 0,
                      (const uint8_t *)VEC_DEV_ID, VEC_SESSION_ID,
                      s_topic_ctl, 2 /* corrupt */);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_BADSIG,
              "bad sig fatal in grace");
        CHECK(wia1_grace_set(false) == 0 && !wia1_grace_on(), "grace off");
    }

    printf("-- session expiry --\n");
    CHECK(arm_mouse(), "arm: expiry");
    {
        uint8_t env[640], body[13];
        memcpy(body, VEC_CTL + 64, 13);
        /* ttl was 3600 s; jump past it. ts_rel = now - t0 so the
         * §6 step-8 timestamp gate passes and step 9 (TTL) fires. */
        wia1_test_set_now(T0 + 3601u * 1000u);
        size_t n = build_ctl(env, WIA1_CTL_BATCH, body, sizeof(body), 7,
                             3601u * 1000u, (const uint8_t *)VEC_DEV_ID,
                             VEC_SESSION_ID, s_topic_ctl, 0);
        CHECK(wia1_ingest_ctl(env, n, s_topic_ctl) == WIA1_E_STALE_SESSION,
              "CTL past TTL rejected");
        CHECK(last_log_is("SESSION EXPIRED - DISARMED"), "expiry log");
        CHECK(!wia1_is_armed(), "expired session disarmed");
        CHECK(wia1_poll_expiry() == 0, "poll after wipe: nothing");
    }
    CHECK(arm_mouse(), "re-arm: poll expiry");
    {
        wia1_test_set_now(T0 + 3601u * 1000u);
        CHECK(wia1_poll_expiry() == 1, "poll_expiry reports expiry");
        CHECK(!wia1_is_armed(), "poll disarms");
        CHECK(last_log_is("SESSION EXPIRED - DISARMED"), "poll expiry log");
        /* sealing telemetry on an expired session fails closed */
        uint8_t out[128];
        const uint8_t b[1] = { 0x00 };
        CHECK(wia1_seal_tel(WIA1_TEL_STATUS, b, sizeof(b), out, sizeof(out),
                            false) == -1,
              "seal_tel refused when disarmed");
    }

    printf("-- seal_tel guards --\n");
    CHECK(arm_mouse(), "arm: seal guards");
    {
        uint8_t out[128];
        const uint8_t b[1] = { 0x00 };
        CHECK(wia1_seal_tel(WIA1_TEL_STATUS, b, sizeof(b), NULL, sizeof(out),
                            false) == -1, "out=NULL refused");
        CHECK(wia1_seal_tel(WIA1_TEL_STATUS, NULL, 1, out, sizeof(out),
                            false) == -1, "body=NULL with len refused");
        CHECK(wia1_seal_tel(WIA1_TEL_STATUS, NULL, 0, out, sizeof(out),
                            false) > 0, "empty body ok");
        CHECK(wia1_seal_tel(WIA1_TEL_FRAME, b, sizeof(b), out, sizeof(out),
                            false) == -1, "FRAME without ENC refused");
        CHECK(wia1_seal_tel(WIA1_TEL_STATUS, b, sizeof(b), out, 10,
                            false) == -1, "small cap refused");
        CHECK(wia1_seal_tel(WIA1_TEL_FRAME, b, sizeof(b), out, sizeof(out),
                            true) > 0, "FRAME with ENC ok");
    }

    printf("-- lockout: 10 fatals in 60 s --\n");
    CHECK(arm_mouse(), "arm: lockout");
    {
        uint8_t env[sizeof(VEC_CTL)];
        wia1_rc rc = WIA1_OK;
        for (int i = 0; i < 9; i++) {
            memcpy(env, VEC_CTL, sizeof(env));
            env[sizeof(env) - 1] ^= (uint8_t)(0x01u << (i % 7));
            rc = wia1_ingest_ctl(env, sizeof(env), s_topic_ctl);
            CHECK(rc == WIA1_E_BADSIG, "fatal 1..9 -> E_BADSIG");
        }
        CHECK(wia1_is_armed(), "still armed after 9 fatals");
        CHECK(last_log_starts("BAD SIG on "), "9th fatal log is BAD SIG");
        memcpy(env, VEC_CTL, sizeof(env));
        env[sizeof(env) - 1] ^= 0x02;
        rc = wia1_ingest_ctl(env, sizeof(env), s_topic_ctl);
        CHECK(rc == WIA1_E_LOCKOUT, "10th fatal -> E_LOCKOUT");
        CHECK(last_log_is("LOCKOUT - 10 fatals in 60s, auto-disarm, "
                          "physical re-arm required"),
              "lockout is the final log line");
        CHECK(!wia1_is_armed(), "lockout disarmed");
        {
            uint16_t code, count;
            wia1_seclog_poll(&code, &count);
            CHECK(code == 0x05 && count >= 1, "seclog lockout code");
        }
        /* physical re-arm works after lockout */
        CHECK(arm_mouse(), "re-arm after lockout");
        CHECK(wia1_ingest_ctl(VEC_CTL, sizeof(VEC_CTL), s_topic_ctl) ==
              WIA1_OK, "CTL works after re-arm");
    }
    /* windowing: fatals 61 s apart do not accumulate */
    CHECK(arm_mouse(), "arm: lockout window");
    {
        uint8_t env[sizeof(VEC_CTL)];
        for (int i = 0; i < 9; i++) {
            memcpy(env, VEC_CTL, sizeof(env));
            env[sizeof(env) - 1] ^= 0x04;
            CHECK(wia1_ingest_ctl(env, sizeof(env), s_topic_ctl) ==
                  WIA1_E_BADSIG, "windowed fatal -> E_BADSIG");
        }
        wia1_test_set_now(T0 + 61000); /* outside the 60 s window */
        memcpy(env, VEC_CTL, sizeof(env));
        env[sizeof(env) - 1] ^= 0x04;
        CHECK(wia1_ingest_ctl(env, sizeof(env), s_topic_ctl) == WIA1_E_BADSIG,
              "10th fatal outside window -> E_BADSIG, no lockout");
        CHECK(wia1_is_armed(), "still armed: window expired");
        wia1_test_set_now(T0);
    }

    printf("\n%d passed, %d failed\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
