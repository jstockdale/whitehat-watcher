/* SPDX-License-Identifier: MIT
 * Phase 3 WIA1 session machine — device side (ESP32-S3).
 *
 * Implements components/seal/include/wia1.h against the normative spec
 * ~/workspace/user/files/WIA-SEAL-v1_20260925.md (§§3-10).
 *
 * Crypto: upstream Monocypher + optional/monocypher-ed25519 (RFC 8032
 * Ed25519, X25519, XChaCha20-Poly1305, keyed BLAKE2b). Adaptations vs
 * the task brief's assumed signatures (verified against upstream):
 *   - keyed BLAKE2b is crypto_blake2b_keyed() (unkeyed: crypto_blake2b())
 *   - ephemeral X25519 pubkey via crypto_x25519_public_key(), not
 *     crypto_x25519() with an explicit base point
 *   - crypto_ed25519_check() returns int (0 = ok), as assumed
 *
 * Session state is RAM-only and guarded by a mutex; adoption is
 * temp-struct-then-swap, never field-by-field (§10). No large stack
 * buffers (D6): the 706 B auth-input scratch lives in the static state.
 *
 * NVS ("seal" namespace): device identity keypair ("dev_id_sk", 64 B,
 * written once at first boot) and the grace flag ("grace", 1 B). All
 * NVS access goes through the nvs task (§10) — reads and writes.
 * v0.4.7: the old "reads use nvs_get_blob directly (safe anywhere)"
 * was wrong for PSRAM-stack tasks; every nvs_* call now goes through
 * seal_dev_nvs_*_sync().
 *
 * TRNG note (§14): nonces/ephemerals come from esp_random(), properly
 * seeded when the radio is up. This protocol only runs over Wi-Fi/MQTT,
 * so the radio is up at every arm/seal call site. The first-boot
 * identity mint in wia1_init() may run before Wi-Fi associates; the
 * hardware RNG still functions, and the mint happens exactly once.
 *
 * Integration hooks NOT declared in wia1.h (declare at the call site;
 * prototypes documented in WIA1_WIRE.md §"firmware integration"):
 *   void wia1_claim_info(char *out, size_t cap);
 *   void wia1_arm_shortcode(char out[7]);
 *   void wia1_banner_state(char *out, size_t cap);
 *   int  wia1_wants_status_reply(void);   - poll-and-clear
 *   int  wia1_poll_expiry(void);          - 1 Hz tick
 * Raw HID report paths (weak; coordinator overrides at integration):
 *   int wia1_raw_key_report(uint8_t mod, const uint8_t keys[6]);
 *   int wia1_raw_consumer(uint16_t usage);
 */
#include <stddef.h>
#include "wia1.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

#include "monocypher.h"
#include "optional/monocypher-ed25519.h"

#ifdef WIA1_HOST_TEST
/* ═══ HOST-TEST SHIMS — clearly marked, never compiled on device ═══
 * esp_err.h comes from test/host_stubs (typedef int esp_err_t). */
#include "esp_err.h"
uint32_t wia1_test_random(void);
uint64_t wia1_test_now_ms(void);
int      wia1_test_nvs_read(const char *key, void *out, size_t *inout_len);
/* test double (defined in test_wia1.c) */
esp_err_t seal_dev_nvs_write_blob_sync(const char *ns, const char *key,
                                       const void *data, size_t len,
                                       uint32_t timeout_ms);
esp_err_t seal_dev_nvs_read_blob_sync(const char *ns, const char *key,
                                      void *out, size_t *inout_len,
                                      uint32_t timeout_ms);
/* hid_ble test doubles (defined in test_wia1.c) */
esp_err_t hid_ble_mouse_move(int dx, int dy);
esp_err_t hid_ble_mouse_wheel(int delta);
esp_err_t hid_ble_mouse_click(const char *button);
esp_err_t hid_ble_mouse_button(const char *button, bool pressed);
#define WIA1_RANDOM32()   wia1_test_random()
#define WIA1_NOW_MS()     wia1_test_now_ms()
#define WIA1_DELAY_MS(ms) ((void)(ms))
#define WIA1_LOCK()       ((void)0)
#define WIA1_UNLOCK()     ((void)0)
#else
#include "esp_log.h"
#include "esp_random.h"    /* esp_random (declared here since IDF 5.x) */
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "seal_dev.h"     /* seal_dev_nvs_write_blob (writer task) */
#include "hid_ble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define WIA1_RANDOM32()   esp_random()
#define WIA1_NOW_MS()     ((uint64_t)(esp_timer_get_time() / 1000LL))
#define WIA1_DELAY_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms))
static SemaphoreHandle_t s_lock = NULL;
#define WIA1_LOCK()   do { if (s_lock != NULL) { xSemaphoreTake(s_lock, portMAX_DELAY); } } while (0)
#define WIA1_UNLOCK() do { if (s_lock != NULL) { xSemaphoreGive(s_lock); } } while (0)
#endif

#define WIA1_TAG "wia1"

/* Forward declarations for integration hooks defined later in this file
 * (not declared in wia1.h; the coordinator declares them at call site). */
void wia1_arm_shortcode(char out[7]);
int wia1_raw_key_report(uint8_t mod, const uint8_t keys[6]);
int wia1_raw_consumer(uint16_t usage);

/* Device-offered scope bitmap carried in the challenge (§5.4). */
#define WIA1_OFFERED_SCOPE \
    (WIA1_SCOPE_MOUSE | WIA1_SCOPE_KEYBOARD | WIA1_SCOPE_CONSUMER)

/* D6: static scratch sized for the largest topic-bound auth input:
 * u16 + topic(128) + header(64) + max control body(512). Kind-1 MAC
 * over large bodies streams through the incremental BLAKE2b API. */
#define WIA1_S_MAX (2u + WIA1_MAX_TOPIC + WIA1_HDR_LEN + WIA1_MAX_CTL_BODY)

/* TEL_SECLOG event codes (§8): code u16, count u16. Codes only, never
 * attacker-controlled bytes. */
#define WIA1_SEC_BADSIG      0x01
#define WIA1_SEC_REPLAY      0x02
#define WIA1_SEC_SCOPE        0x03
#define WIA1_SEC_BAD_CERT     0x04
#define WIA1_SEC_LOCKOUT      0x05
#define WIA1_SEC_STALE_TS     0x06
#define WIA1_SEC_MALFORMED   0x07
#define WIA1_SEC_WRONG_DEV   0x08
#define WIA1_SEC_STALE_SESS  0x09
#define WIA1_SEC_DISARMED    0x0A
#define WIA1_SEC_RETAINED    0x0B
#define WIA1_SEC_BAD_BODY    0x0C
#define WIA1_SEC_CERT_REJECT 0x0D
#define WIA1_SEC_EXPIRED     0x0E

/* Session material: built fully in s_tmp, then memcpy-swapped (§10). */
typedef struct {
    uint8_t  sess_id[16];
    uint8_t  sess_ed_pk[32];
    uint8_t  k_tel_mac[32];
    uint8_t  k_tel_aead[32];
    uint8_t  k_ctl_mac[32];
    uint8_t  k_ctl_aead[32];
    uint16_t scope;
    uint64_t t0_ms;
    uint32_t ttl_s;
} wia1_sess_t;

typedef struct {
    bool     pending;
    uint8_t  pending_nonce[16];
    uint8_t  pending_x_sk[32];
    uint8_t  pending_x_pk[32];
    char     pending_sc[7];   /* 6-hex short code, shown on screen (§4) */
    uint64_t pending_deadline_ms;
    bool     awaiting_accept;
    bool     armed;
    wia1_sess_t sess;
    uint64_t ctr_tx;          /* kind-1 outbound, next = ctr_tx + 1 */
    uint64_t ctr_rx;          /* kind-2 high-water (last adopted) */
    bool     wants_status;   /* CTL_PING -> caller sends TEL_STATUS */
    uint64_t fatal_ts[WIA1_LOCKOUT_FATALS];  /* monotonic s, ring */
    uint8_t  fatal_idx;
    uint32_t auth_fails;
    uint16_t seclog_code;
    uint16_t seclog_count;
    bool     grace;
    uint32_t grace_warn_ctr;
    uint8_t  scratch[WIA1_S_MAX];
    struct {
        uint8_t op;
        uint8_t need;         /* scope bits required */
        uint8_t args[7];      /* largest op payload (KEY_RPT) */
    } ops[33];                /* 32 ops + terminator (never index [32]
                               * without the +1) */
} wia1_state_t;

static wia1_state_t s_st;
static wia1_sess_t  s_tmp;   /* adoption temp */

static uint8_t s_id_sk[64];  /* device identity secret (seed||pub) */
static uint8_t s_id_pk[32];
static char    s_dev_id[17]; /* "wia-" + 12 hex, NUL-terminated */
static bool    s_id_ready = false;

/* ---- little-endian field codecs (never memcpy structs, §5) ---- */
static uint16_t wia1_r16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t wia1_r32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t wia1_r64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}
static void wia1_w16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static void wia1_w64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}
static bool wia1_is_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

/* ---- logging: every rejection carries exactly one named string (§6).
 * The last line is kept for the serial contract / host test. ---- */
static char s_last_log[160];

static void wia1_emit_log(bool warn, const char *buf)
{
    size_t n = strlen(buf);
    if (n >= sizeof(s_last_log)) {
        n = sizeof(s_last_log) - 1;
    }
    memcpy(s_last_log, buf, n);
    s_last_log[n] = '\0';
#ifdef WIA1_HOST_TEST
    (void)warn;
    fprintf(stderr, "[wia1] %s\n", buf);
#else
    if (warn) {
        ESP_LOGW(WIA1_TAG, "%s", buf);
    } else {
        ESP_LOGI(WIA1_TAG, "%s", buf);
    }
#endif
}

static void wia1_logf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wia1_emit_log(true, buf);
}

static void wia1_infof(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wia1_emit_log(false, buf);
}

/* ---- randomness (TRNG note in the file header) ---- */
static void wia1_random_bytes(uint8_t *out, size_t n)
{
    while (n > 0) {
        uint32_t w = WIA1_RANDOM32();
        size_t c = n < 4 ? n : 4;
        for (size_t i = 0; i < c; i++) {
            out[i] = (uint8_t)((w >> (8 * i)) & 0xFF);
        }
        out += c;
        n -= c;
    }
}

/* ---- NVS reads (v0.4.7: via the nvs task, §10) ---- */
static bool wia1_nvs_read(const char *key, void *out, size_t *inout_len)
{
#ifdef WIA1_HOST_TEST
    return wia1_test_nvs_read(key, out, inout_len) == 0;
#else
    return seal_dev_nvs_read_blob_sync("", key, out, inout_len,
                                       5000) == ESP_OK;
#endif
}

/* ---- header parse (§5.1): fixed offsets + caps only, pre-auth ---- */
typedef struct {
    uint8_t  kind;
    uint8_t  flags;
    uint8_t  dev_id[16];
    uint8_t  sess_id[16];
    uint64_t ctr;
    uint64_t ts;
    uint16_t msg_type;
} wia1_hdr_t;

static wia1_rc wia1_parse_hdr(const uint8_t *env, size_t env_len, wia1_hdr_t *h)
{
    if (env_len < WIA1_HDR_LEN) {
        return WIA1_E_SHORT;
    }
    if (memcmp(env, WIA1_MAGIC, 4) != 0) {
        return WIA1_E_MAGIC;
    }
    h->kind = env[4];
    h->flags = env[5];
    if (h->kind < WIA1_KIND_TEL || h->kind > WIA1_KIND_CHAL) {
        return WIA1_E_KIND;
    }
    if (h->kind == WIA1_KIND_TEL) {
        /* ENC defined for kind 1; every other flag bit reserved-zero */
        if ((h->flags & (uint8_t)~WIA1_FLAG_ENC) != 0) {
            return WIA1_E_KIND;
        }
    } else if (h->flags != 0) {
        /* ENC reserved for kinds 2/5 (§5.2); kinds 3/4 never set flags */
        return WIA1_E_KIND;
    }
    memcpy(h->dev_id, env + 6, 16);
    memcpy(h->sess_id, env + 22, 16);
    h->ctr = wia1_r64le(env + 38);
    h->ts = wia1_r64le(env + 46);
    h->msg_type = wia1_r16le(env + 54);
    for (int i = 0; i < 8; i++) {
        if (env[56 + i] != 0) {
            return WIA1_E_KIND;  /* rsv MUST be zero */
        }
    }
    if ((h->kind == WIA1_KIND_CERT || h->kind == WIA1_KIND_CHAL) &&
        (!wia1_is_zero(h->sess_id, 16) || h->ctr != 0 || h->ts != 0)) {
        return WIA1_E_KIND;
    }
    return WIA1_OK;
}

/* ---- seclog + fatal accounting (§§4, 6, 8) ---- */
static void wia1_seclog_note(uint16_t code)
{
    if (s_st.seclog_code == code) {
        if (s_st.seclog_count < 0xFFFFu) {
            s_st.seclog_count++;
        }
    } else {
        s_st.seclog_code = code;
        s_st.seclog_count = 1;
    }
}

static void wia1_wipe_session_locked(void)
{
    crypto_wipe(&s_st.sess, sizeof(s_st.sess));
    s_st.armed = false;
    s_st.awaiting_accept = false;
    s_st.wants_status = false;
    s_st.ctr_tx = 0;
    s_st.ctr_rx = 0;
}

static void wia1_wipe_pending_locked(void)
{
    crypto_wipe(s_st.pending_nonce, sizeof(s_st.pending_nonce));
    crypto_wipe(s_st.pending_x_sk, sizeof(s_st.pending_x_sk));
    crypto_wipe(s_st.pending_x_pk, sizeof(s_st.pending_x_pk));
    crypto_wipe(s_st.pending_sc, sizeof(s_st.pending_sc));
    s_st.pending = false;
    s_st.pending_deadline_ms = 0;
}

/* Returns true when this fatal trips the §4 lockout (10+ in 60 s):
 * session + pending state are wiped; the CALLER logs the lockout as the
 * final named line and returns WIA1_E_LOCKOUT. */
static bool wia1_fatal_locked(uint16_t code)
{
    /* +1 keeps slot value 0 as the "unused" sentinel even when a fatal
     * lands during monotonic second zero. */
    uint64_t now_s = WIA1_NOW_MS() / 1000u + 1u;
    s_st.fatal_ts[s_st.fatal_idx % WIA1_LOCKOUT_FATALS] = now_s;
    s_st.fatal_idx++;
    s_st.auth_fails++;
    wia1_seclog_note(code);
    unsigned n = 0;
    for (unsigned i = 0; i < WIA1_LOCKOUT_FATALS; i++) {
        uint64_t t = s_st.fatal_ts[i];
        if (t != 0 && now_s - t < WIA1_LOCKOUT_WIN_S) {
            n++;
        }
    }
    if (n >= WIA1_LOCKOUT_FATALS) {
        /* §4: auto-disarm + wipe. Physical re-arm required (pending
         * window closed too). */
        for (unsigned i = 0; i < WIA1_LOCKOUT_FATALS; i++) {
            s_st.fatal_ts[i] = 0;
        }
        wia1_wipe_session_locked();
        wia1_wipe_pending_locked();
        wia1_seclog_note(WIA1_SEC_LOCKOUT);
        return true;
    }
    return false;
}

/* ---- §3 key schedule ---- */
#ifndef WIA1_HOST_TEST
static
#endif
void wia1_kdf_label(const uint8_t ss[32], const uint8_t sess_id[16],
                    const char *label, uint8_t out[32])
{
    /* msg = "wia1:" || label || ":" || session_id  (byte-exact, §3) */
    uint8_t msg[5 + 8 + 1 + 16];
    size_t n = 0;
    memcpy(msg + n, "wia1:", 5);
    n += 5;
    size_t ll = strlen(label);
    memcpy(msg + n, label, ll);
    n += ll;
    msg[n++] = ':';
    memcpy(msg + n, sess_id, 16);
    n += 16;
    crypto_blake2b_keyed(out, 32, ss, 32, msg, n);
    crypto_wipe(msg, sizeof(msg));
}

/* ================= public API ================= */

const char *wia1_rc_str(wia1_rc rc)
{
    switch (rc) {
    case WIA1_OK:            return "OK";
    case WIA1_OK_DUP:        return "DUP (silent)";
    case WIA1_E_SHORT:
    case WIA1_E_MAGIC:
    case WIA1_E_KIND:        return "MALFORMED";
    case WIA1_E_BADSIG:      return "BAD SIG";
    case WIA1_E_WRONG_DEV:   return "WRONG DEVICE";
    case WIA1_E_DISARMED:    return "CTL while DISARMED";
    case WIA1_E_STALE_SESSION: return "STALE SESSION";
    case WIA1_E_REPLAY:      return "REPLAY";
    case WIA1_E_STALE_TS:    return "STALE";
    case WIA1_E_SCOPE:       return "SCOPE VIOLATION";
    case WIA1_E_BAD_BODY:    return "BAD BODY";
    case WIA1_E_BAD_CERT:    return "BAD CERT";
    case WIA1_E_NO_WINDOW:   return "CERT out of window - IGNORED";
    case WIA1_E_RETAINED:    return "RETAINED CTL - DROPPED";
    case WIA1_E_LOCKOUT:     return "LOCKOUT - DISARMED";
    default:                 return "?";
    }
}

esp_err_t wia1_init(void)
{
#ifndef WIA1_HOST_TEST
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_FAIL;
        }
    }
#endif
    WIA1_LOCK();
    uint8_t sk[64];
    size_t len = sizeof(sk);
    if (wia1_nvs_read("dev_id_sk", sk, &len) && len == sizeof(sk)) {
        memcpy(s_id_sk, sk, sizeof(sk));
        /* monocypher secret_key = seed(32) || pubkey(32) */
        memcpy(s_id_pk, sk + 32, 32);
    } else {
        uint8_t seed[32];
        wia1_random_bytes(seed, sizeof(seed));
        crypto_ed25519_key_pair(sk, s_id_pk, seed);
        crypto_wipe(seed, sizeof(seed));
        memcpy(s_id_sk, sk, sizeof(sk));
        /* single NVS context: the nvs task (§10). v0.4.7: synchronous. */
        seal_dev_nvs_write_blob_sync("", "dev_id_sk", sk, sizeof(sk),
                                     5000);
    }
    crypto_wipe(sk, sizeof(sk));
    /* dev_id = "wia-" + 12 hex chars of BLAKE2b-8(pubkey)[0:6] (§3) */
    uint8_t fp[8];
    crypto_blake2b(fp, sizeof(fp), s_id_pk, sizeof(s_id_pk));
    static const char hexd[] = "0123456789abcdef";
    s_dev_id[0] = 'w';
    s_dev_id[1] = 'i';
    s_dev_id[2] = 'a';
    s_dev_id[3] = '-';
    for (int i = 0; i < 6; i++) {
        s_dev_id[4 + 2 * i]     = hexd[(fp[i] >> 4) & 0xF];
        s_dev_id[4 + 2 * i + 1] = hexd[fp[i] & 0xF];
    }
    s_dev_id[16] = '\0';
    s_id_ready = true;
    uint8_t g = 0;
    len = sizeof(g);
    if (wia1_nvs_read("grace", &g, &len) && len == sizeof(g)) {
        s_st.grace = (g != 0);
    }
    WIA1_UNLOCK();
    wia1_infof("wia1 init dev_id=%s grace=%d roots_provisioned=%d",
               s_dev_id, (int)s_st.grace, (int)wia1_roots_provisioned());
    return ESP_OK;
}

const char *wia1_dev_id(void)
{
    return s_id_ready ? s_dev_id : "";
}

bool wia1_is_armed(void)
{
    bool a;
    WIA1_LOCK();
    a = s_st.armed;
    WIA1_UNLOCK();
    return a;
}

void wia1_session_id_hex(char out[17])
{
    /* 16 hex chars of the 16-byte session id + NUL (§4 step 7). */
    static const char hexd[] = "0123456789abcdef";
    WIA1_LOCK();
    if (!s_st.armed) {
        out[0] = '\0';
    } else {
        for (int i = 0; i < 8; i++) {
            out[2 * i]     = hexd[(s_st.sess.sess_id[i] >> 4) & 0xF];
            out[2 * i + 1] = hexd[s_st.sess.sess_id[i] & 0xF];
        }
        out[16] = '\0';
    }
    WIA1_UNLOCK();
}

/* ================= arm ceremony (§4) ================= */

int wia1_arm_tap(uint8_t *chal_out, size_t cap)
{
    if (!s_id_ready || chal_out == NULL) {
        return -1;
    }
    if (cap < (size_t)(WIA1_HDR_LEN + WIA1_CHAL_BODY_LEN + 64)) {
        return -1;
    }
    WIA1_LOCK();
    if (s_st.armed || s_st.awaiting_accept) {
        WIA1_UNLOCK();
        wia1_logf("ARM TAP refused - session already active (disarm tap first)");
        return -1;
    }
    if (!wia1_roots_provisioned()) {
        wia1_logf("ARM TAP warn - root pubkeys unprovisioned (bench only)");
    }
    uint8_t nonce[16], x_sk[32], x_pk[32];
    wia1_random_bytes(nonce, sizeof(nonce));
    wia1_random_bytes(x_sk, sizeof(x_sk));
    crypto_x25519_public_key(x_pk, x_sk);
    /* challenge_body, §5.4 */
    uint8_t *b = chal_out + WIA1_HDR_LEN;
    b[0] = 1;  /* chal_ver */
    b[1] = 1;  /* proto_max */
    wia1_w16le(b + 2, WIA1_OFFERED_SCOPE);
    memcpy(b + 4, nonce, 16);
    memcpy(b + 20, x_pk, 32);
    /* header */
    uint8_t *h = chal_out;
    memcpy(h, WIA1_MAGIC, 4);
    h[4] = WIA1_KIND_CHAL;
    h[5] = 0;
    memcpy(h + 6, s_dev_id, 16);
    memset(h + 22, 0, 16 + 8 + 8);  /* session_id, ctr, ts_rel */
    wia1_w16le(h + 54, 0);
    memset(h + 56, 0, 8);
    /* S = header || body (no topic, §5.2), signed by identity key */
    memcpy(s_st.scratch, h, WIA1_HDR_LEN);
    memcpy(s_st.scratch + WIA1_HDR_LEN, b, WIA1_CHAL_BODY_LEN);
    uint8_t sig[64];
    crypto_ed25519_sign(sig, s_id_sk, s_st.scratch,
                        WIA1_HDR_LEN + WIA1_CHAL_BODY_LEN);
    memcpy(chal_out + WIA1_HDR_LEN + WIA1_CHAL_BODY_LEN, sig, 64);
    crypto_wipe(sig, sizeof(sig));
    /* pending arm state */
    memcpy(s_st.pending_nonce, nonce, 16);
    memcpy(s_st.pending_x_sk, x_sk, 32);
    memcpy(s_st.pending_x_pk, x_pk, 32);
    /* short code: first 3 bytes of BLAKE2b-16(chal_ver || nonce || x_pk)
     * as 6 hex chars (byte-exact with the host oracle). */
    {
        static const char hexd[] = "0123456789abcdef";
        uint8_t hb[16];
        crypto_blake2b_ctx hctx;
        crypto_blake2b_init(&hctx, sizeof(hb));
        crypto_blake2b_update(&hctx, b, 1);
        crypto_blake2b_update(&hctx, nonce, 16);
        crypto_blake2b_update(&hctx, x_pk, 32);
        crypto_blake2b_final(&hctx, hb);
        for (int i = 0; i < 3; i++) {
            s_st.pending_sc[2 * i]     = hexd[(hb[i] >> 4) & 0xF];
            s_st.pending_sc[2 * i + 1] = hexd[hb[i] & 0xF];
        }
        s_st.pending_sc[6] = '\0';
        crypto_wipe(hb, sizeof(hb));
    }
    s_st.pending = true;
    s_st.awaiting_accept = false;
    s_st.pending_deadline_ms = WIA1_NOW_MS() + (uint64_t)WIA1_ARM_WINDOW_S * 1000u;
    crypto_wipe(nonce, sizeof(nonce));
    crypto_wipe(x_sk, sizeof(x_sk));
    crypto_wipe(x_pk, sizeof(x_pk));
    WIA1_UNLOCK();
    char sc[7];
    wia1_arm_shortcode(sc);
    wia1_infof("ARM TAP - challenge issued, window %us, shortcode %s",
               WIA1_ARM_WINDOW_S, sc);
    return WIA1_HDR_LEN + WIA1_CHAL_BODY_LEN + 64;
}

void wia1_arm_shortcode(char out[7])
{
    WIA1_LOCK();
    if (!s_st.pending) {
        out[0] = '\0';
    } else {
        memcpy(out, s_st.pending_sc, 7);
    }
    WIA1_UNLOCK();
}

int wia1_disarm_tap(void)
{
    WIA1_LOCK();
    wia1_wipe_session_locked();
    wia1_wipe_pending_locked();
    WIA1_UNLOCK();
    wia1_infof("DISARM TAP - session wiped");
    return 0;
}

int wia1_disarm_remote(void)
{
    WIA1_LOCK();
    wia1_wipe_session_locked();
    WIA1_UNLOCK();
    wia1_infof("REMOTE DISARM - session wiped");
    return 0;
}

bool wia1_needs_accept_tap(void)
{
    bool v;
    WIA1_LOCK();
    v = s_st.awaiting_accept;
    WIA1_UNLOCK();
    return v;
}

int wia1_accept_tap(void)
{
    WIA1_LOCK();
    if (!s_st.awaiting_accept) {
        WIA1_UNLOCK();
        return -1;
    }
    s_st.awaiting_accept = false;
    /* §4 step 7: the session clock starts at the accept tap, not at
     * cert ingest. Counters start at 0 (first valid message ctr = 1). */
    s_st.sess.t0_ms = WIA1_NOW_MS();
    s_st.ctr_tx = 0;
    s_st.ctr_rx = 0;
    s_st.armed = true;
    uint16_t scope = s_st.sess.scope;
    uint32_t ttl = s_st.sess.ttl_s;
    WIA1_UNLOCK();
    wia1_infof("ACCEPT TAP - ARMED scope=0x%04x ttl=%us", scope, ttl);
    return 0;
}

/* ================= kind-3 ingest: §4 step 5 ================= */

int wia1_ingest_cert(const uint8_t *env, size_t env_len)
{
    if (env == NULL) {
        return WIA1_E_SHORT;
    }
    WIA1_LOCK();
    uint64_t now = WIA1_NOW_MS();
    if (s_st.armed || s_st.awaiting_accept ||
        !s_st.pending || now >= s_st.pending_deadline_ms) {
        if (s_st.pending && now >= s_st.pending_deadline_ms) {
            wia1_wipe_pending_locked();
            wia1_logf("ARM WINDOW EXPIRED");
        }
        WIA1_UNLOCK();
        wia1_logf("CERT out of window - IGNORED");
        return WIA1_E_NO_WINDOW;
    }
    if (env_len != (size_t)(WIA1_HDR_LEN + WIA1_CERT_BODY_LEN + 64)) {
        wia1_seclog_note(WIA1_SEC_MALFORMED);
        WIA1_UNLOCK();
        wia1_logf("MALFORMED");
        return WIA1_E_SHORT;
    }
    wia1_hdr_t h;
    if (wia1_parse_hdr(env, env_len, &h) != WIA1_OK ||
        h.kind != WIA1_KIND_CERT || h.msg_type != 0) {
        wia1_seclog_note(WIA1_SEC_MALFORMED);
        WIA1_UNLOCK();
        wia1_logf("MALFORMED");
        return WIA1_E_KIND;
    }
    if (memcmp(h.dev_id, s_dev_id, 16) != 0) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT wrong device - DROPPED");
        return WIA1_E_WRONG_DEV;
    }
    /* root signature over S = header || body (no topic, §5.2).
     * Fail closed when no root is provisioned: no trust anchor means
     * no certificate can verify. */
    if (!wia1_roots_provisioned()) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT no root provisioned - DROPPED");
        return WIA1_E_BAD_CERT;
    }
    memcpy(s_st.scratch, env, WIA1_HDR_LEN + WIA1_CERT_BODY_LEN);
    const uint8_t *sig = env + WIA1_HDR_LEN + WIA1_CERT_BODY_LEN;
    bool ok = crypto_ed25519_check(sig, WIA1_ROOT_PRIMARY, s_st.scratch,
                                   WIA1_HDR_LEN + WIA1_CERT_BODY_LEN) == 0;
    if (!ok && !wia1_is_zero(WIA1_ROOT_BACKUP, 32)) {
        ok = crypto_ed25519_check(sig, WIA1_ROOT_BACKUP, s_st.scratch,
                                  WIA1_HDR_LEN + WIA1_CERT_BODY_LEN) == 0;
    }
    if (!ok) {
        bool locked = wia1_fatal_locked(WIA1_SEC_BAD_CERT);
        WIA1_UNLOCK();
        if (locked) {
            wia1_logf("LOCKOUT - 10 fatals in 60s, auto-disarm, physical re-arm required");
            return WIA1_E_LOCKOUT;
        }
        wia1_logf("BAD CERT SIG - DROPPED");
        return WIA1_E_BADSIG;
    }
    /* §4 step 5 order: cert_ver -> device_id -> nonce -> device_x_pk
     * -> ttl_s -> scope. All fields were authenticated by the root
     * signature above; the order is spec fidelity, not security. */
    const uint8_t *b = env + WIA1_HDR_LEN;
    if (b[0] != 1) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT bad version - DROPPED");
        return WIA1_E_BAD_CERT;
    }
    if (memcmp(b + 7, s_dev_id, 16) != 0) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT wrong device - DROPPED");
        return WIA1_E_WRONG_DEV;
    }
    if (memcmp(b + 23, s_st.pending_nonce, 16) != 0) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT nonce mismatch - DROPPED");
        return WIA1_E_BAD_CERT;
    }
    if (memcmp(b + 103, s_st.pending_x_pk, 32) != 0) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT x_pk mismatch - DROPPED");
        return WIA1_E_BAD_CERT;
    }
    uint32_t ttl = wia1_r32le(b + 3);
    if (ttl == 0 || ttl > WIA1_MAX_TTL_S) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT bad ttl - DROPPED");
        return WIA1_E_BAD_CERT;
    }
    uint16_t scope = wia1_r16le(b + 1);
    if ((scope & (uint16_t)(~WIA1_OFFERED_SCOPE)) != 0) {
        wia1_seclog_note(WIA1_SEC_CERT_REJECT);
        WIA1_UNLOCK();
        wia1_logf("CERT scope exceeds offer - DROPPED");
        return WIA1_E_BAD_CERT;
    }
    /* build the full session in the temp struct, then swap (§10) */
    crypto_blake2b(s_tmp.sess_id, 16, b, WIA1_CERT_BODY_LEN);
    memcpy(s_tmp.sess_ed_pk, b + 39, 32);
    uint8_t ss[32];
    crypto_x25519(ss, s_st.pending_x_sk, b + 71);  /* X25519(x_sk, agent_x_pk) */
    wia1_kdf_label(ss, s_tmp.sess_id, "tel-mac", s_tmp.k_tel_mac);
    wia1_kdf_label(ss, s_tmp.sess_id, "tel-aead", s_tmp.k_tel_aead);
    wia1_kdf_label(ss, s_tmp.sess_id, "ctl-mac", s_tmp.k_ctl_mac);
    wia1_kdf_label(ss, s_tmp.sess_id, "ctl-aead", s_tmp.k_ctl_aead);
    crypto_wipe(ss, sizeof(ss));
    s_tmp.scope = scope;
    s_tmp.t0_ms = now;
    s_tmp.ttl_s = ttl;
    memcpy(&s_st.sess, &s_tmp, sizeof(s_tmp));
    crypto_wipe(&s_tmp, sizeof(s_tmp));
    s_st.ctr_tx = 0;
    s_st.ctr_rx = 0;
    s_st.wants_status = false;
    wia1_wipe_pending_locked();  /* one cert per nonce: window closes */
    int rc;
    if ((scope & (uint16_t)(WIA1_SCOPE_KEYBOARD | WIA1_SCOPE_CONSUMER)) != 0) {
        /* keyboard/consumer: explicit accept tap required (§4 step 6) */
        s_st.awaiting_accept = true;
        s_st.armed = false;
        rc = 0;
        WIA1_UNLOCK();
        wia1_logf("CERT accepted scope=0x%04x - waiting for accept tap", scope);
    } else {
        s_st.armed = true;
        rc = 1;
        WIA1_UNLOCK();
        wia1_infof("ARMED scope=0x%04x ttl=%us", scope, ttl);
    }
    return rc;
}

/* ================= kind-2 ingest: §6 order ================= */

static wia1_rc wia1_parse_batch_locked(const uint8_t *body, size_t body_len,
                                       bool grace_unsigned)
{
    size_t pos = 0;
    unsigned n = 0;
    while (pos < body_len) {
        if (n >= 32) {
            return WIA1_E_BAD_BODY;  /* > 32 ops */
        }
        uint8_t op = body[pos++];
        uint8_t need = 0;
        size_t nargs = 0;
        switch (op) {
        case WIA1_OP_MOVE_REL: need = WIA1_SCOPE_MOUSE;    nargs = 4; break;
        case WIA1_OP_BUTTON:   need = WIA1_SCOPE_MOUSE;    nargs = 2; break;
        case WIA1_OP_WHEEL:    need = WIA1_SCOPE_MOUSE;    nargs = 1; break;
        case WIA1_OP_DWELL:    need = 0;                   nargs = 2; break;
        case WIA1_OP_KEY_RPT:  need = WIA1_SCOPE_KEYBOARD; nargs = 7; break;
        case WIA1_OP_CONSUMER: need = WIA1_SCOPE_CONSUMER; nargs = 2; break;
        default: return WIA1_E_BAD_BODY;  /* unknown op: whole batch dies */
        }
        if (pos + nargs > body_len) {
            return WIA1_E_BAD_BODY;
        }
        if (op == WIA1_OP_BUTTON) {
            uint8_t mask = body[pos];
            uint8_t action = body[pos + 1];
            if (((mask & (uint8_t)~0x07u) != 0) || mask == 0 || action > 2) {
                return WIA1_E_BAD_BODY;
            }
        } else if (op == WIA1_OP_DWELL) {
            if (wia1_r16le(body + pos) > 2000) {
                return WIA1_E_BAD_BODY;
            }
        }
        if ((s_st.sess.scope & need) != need) {
            return WIA1_E_SCOPE;
        }
        if (grace_unsigned && need != 0 && need != WIA1_SCOPE_MOUSE) {
            /* §9: grace admits MOUSE-scope opcodes only (DWELL is "any") */
            return WIA1_E_SCOPE;
        }
        s_st.ops[n].op = op;
        s_st.ops[n].need = need;
        memcpy(s_st.ops[n].args, body + pos, nargs);
        n++;
        pos += nargs;
    }
    if (n == 0) {
        return WIA1_E_BAD_BODY;  /* empty batch */
    }
    s_st.ops[n].op = 0xFF;  /* terminator */
    return WIA1_OK;
}

static void wia1_exec_batch_locked(void)
{
    static const char *btn_names[3] = { "left", "right", "middle" };
    for (unsigned n = 0; n < 33 && s_st.ops[n].op != 0xFF; n++) {
        uint8_t op = s_st.ops[n].op;
        const uint8_t *a = s_st.ops[n].args;
        switch (op) {
        case WIA1_OP_MOVE_REL: {
            int16_t dx = (int16_t)wia1_r16le(a);
            int16_t dy = (int16_t)wia1_r16le(a + 2);
            hid_ble_mouse_move((int)dx, (int)dy);
            break;
        }
        case WIA1_OP_BUTTON: {
            uint8_t mask = a[0];
            uint8_t action = a[1];
            for (int b = 0; b < 3; b++) {
                if ((mask & (uint8_t)(1u << b)) != 0) {
                    if (action == 2) {
                        hid_ble_mouse_click(btn_names[b]);
                    } else {
                        hid_ble_mouse_button(btn_names[b], action == 1);
                    }
                }
            }
            break;
        }
        case WIA1_OP_WHEEL:
            hid_ble_mouse_wheel((int)(int8_t)a[0]);
            break;
        case WIA1_OP_DWELL:
            WIA1_DELAY_MS(wia1_r16le(a));
            break;
        case WIA1_OP_KEY_RPT: {
            /* no raw-report API in hid_ble.h: weak stub, see file header */
            int r = wia1_raw_key_report(a[0], a + 1);
            if (r != 0) {
                wia1_logf("KEY_REPORT not wired (stub rc=%d)", r);
            }
            break;
        }
        case WIA1_OP_CONSUMER: {
            int r = wia1_raw_consumer(wia1_r16le(a));
            if (r != 0) {
                wia1_logf("CONSUMER not wired (stub rc=%d)", r);
            }
            break;
        }
        default:
            break;  /* unreachable: parse validated every op */
        }
    }
}

/* Retained-drop lives here so the host test can drive it; the public
 * wia1_ingest_ctl() always passes retained=false — the MQTT layer MUST
 * drop retained deliveries before calling (caller contract, §6 step 1). */
#ifndef WIA1_HOST_TEST
static
#endif
wia1_rc wia1_ingest_ctl_r(const uint8_t *env, size_t env_len,
                          const char *topic, bool retained)
{
    if (env == NULL) {
        return WIA1_E_SHORT;
    }
    if (topic == NULL) {
        topic = "";
    }
    size_t tlen = strlen(topic);
    WIA1_LOCK();
    if (retained) {
        wia1_seclog_note(WIA1_SEC_RETAINED);
        WIA1_UNLOCK();
        wia1_logf("RETAINED CTL - DROPPED");
        return WIA1_E_RETAINED;
    }
    if (tlen > WIA1_MAX_TOPIC) {
        wia1_seclog_note(WIA1_SEC_MALFORMED);
        WIA1_UNLOCK();
        wia1_logf("MALFORMED");
        return WIA1_E_SHORT;
    }
    if (env_len < (size_t)(WIA1_HDR_LEN + 64)) {
        wia1_seclog_note(WIA1_SEC_MALFORMED);
        WIA1_UNLOCK();
        wia1_logf("MALFORMED");
        return WIA1_E_SHORT;
    }
    size_t body_len = env_len - (size_t)(WIA1_HDR_LEN + 64);
    if (body_len > WIA1_MAX_CTL_BODY) {
        wia1_seclog_note(WIA1_SEC_MALFORMED);
        WIA1_UNLOCK();
        wia1_logf("MALFORMED");
        return WIA1_E_SHORT;
    }
    wia1_hdr_t h;
    if (wia1_parse_hdr(env, env_len, &h) != WIA1_OK || h.kind != WIA1_KIND_CTL) {
        wia1_seclog_note(WIA1_SEC_MALFORMED);
        WIA1_UNLOCK();
        wia1_logf("MALFORMED");
        return WIA1_E_KIND;
    }
    if (memcmp(h.dev_id, s_dev_id, 16) != 0) {
        wia1_seclog_note(WIA1_SEC_WRONG_DEV);
        WIA1_UNLOCK();
        wia1_logf("WRONG DEVICE");
        return WIA1_E_WRONG_DEV;
    }
    if (!s_st.armed) {
        wia1_seclog_note(WIA1_SEC_DISARMED);
        WIA1_UNLOCK();
        wia1_logf("CTL while DISARMED");
        return WIA1_E_DISARMED;
    }
    if (memcmp(h.sess_id, s_st.sess.sess_id, 16) != 0) {
        wia1_seclog_note(WIA1_SEC_STALE_SESS);
        WIA1_UNLOCK();
        wia1_logf("STALE SESSION");
        return WIA1_E_STALE_SESSION;
    }
    /* ---- crypto: S = u16le(len) || topic || header || body (D1) ---- */
    const uint8_t *body = env + WIA1_HDR_LEN;
    const uint8_t *sig = body + body_len;
    bool zero_sig = wia1_is_zero(sig, 64);
    uint8_t *S = s_st.scratch;
    wia1_w16le(S, (uint16_t)tlen);
    memcpy(S + 2, topic, tlen);
    memcpy(S + 2 + tlen, env, (size_t)WIA1_HDR_LEN + body_len);
    size_t s_len = (size_t)2 + tlen + (size_t)WIA1_HDR_LEN + body_len;
    bool grace_unsigned = false;
    if (zero_sig && s_st.grace) {
        /* §9: grace admits ABSENT auth, never failed auth */
        grace_unsigned = true;
        s_st.grace_warn_ctr++;
        if ((s_st.grace_warn_ctr % 16) == 1) {
            wia1_logf("GRACE unsigned CTL honored (1-in-16 warn)");
        }
    } else if (crypto_ed25519_check(sig, s_st.sess.sess_ed_pk, S, s_len) != 0) {
        /* present-but-wrong is fatal in every mode, grace included */
        bool locked = wia1_fatal_locked(WIA1_SEC_BADSIG);
        WIA1_UNLOCK();
        if (locked) {
            wia1_logf("LOCKOUT - 10 fatals in 60s, auto-disarm, physical re-arm required");
            return WIA1_E_LOCKOUT;
        }
        wia1_logf("BAD SIG on %s - DROPPED", topic);
        return WIA1_E_BADSIG;
    }
    /* ---- freshness: counter primary, timestamp secondary ---- */
    if (h.ctr == s_st.ctr_rx && s_st.ctr_rx != 0) {
        WIA1_UNLOCK();
        return WIA1_OK_DUP;  /* QoS 1 echo: silent */
    }
    if (h.ctr <= s_st.ctr_rx) {
        bool locked = wia1_fatal_locked(WIA1_SEC_REPLAY);
        WIA1_UNLOCK();
        if (locked) {
            wia1_logf("LOCKOUT - 10 fatals in 60s, auto-disarm, physical re-arm required");
            return WIA1_E_LOCKOUT;
        }
        wia1_logf("REPLAY ctr %llu <= %llu - DROPPED",
                  (unsigned long long)h.ctr, (unsigned long long)s_st.ctr_rx);
        return WIA1_E_REPLAY;
    }
    uint64_t now = WIA1_NOW_MS();
    int64_t dt = (int64_t)now - (int64_t)s_st.sess.t0_ms - (int64_t)h.ts;
    if (dt < 0) {
        dt = -dt;
    }
    if ((uint64_t)dt > (uint64_t)WIA1_TS_WINDOW_MS) {
        wia1_seclog_note(WIA1_SEC_STALE_TS);
        WIA1_UNLOCK();
        wia1_logf("STALE (|dt|=%lldms)", (long long)dt);
        return WIA1_E_STALE_TS;
    }
    if (now >= s_st.sess.t0_ms + (uint64_t)s_st.sess.ttl_s * 1000u) {
        wia1_wipe_session_locked();
        wia1_seclog_note(WIA1_SEC_EXPIRED);
        WIA1_UNLOCK();
        wia1_logf("SESSION EXPIRED - DISARMED");
        return WIA1_E_STALE_SESSION;
    }
    /* ---- semantics: parse fully, then adopt, then execute ---- */
    wia1_rc rc = WIA1_OK;
    switch (h.msg_type) {
    case WIA1_CTL_BATCH:
        rc = wia1_parse_batch_locked(body, body_len, grace_unsigned);
        break;
    case WIA1_CTL_DISARM:
    case WIA1_CTL_PING:
        if (body_len != 0) {
            rc = WIA1_E_BAD_BODY;
        } else if (grace_unsigned) {
            /* §9 strict reading: grace admits MOUSE-scope opcodes only.
             * Unsigned DISARM/PING are rejected; the physical tap always
             * works on the bench. */
            rc = WIA1_E_SCOPE;
        }
        break;
    default:
        rc = WIA1_E_BAD_BODY;
        break;
    }
    if (rc == WIA1_E_SCOPE) {
        bool locked = wia1_fatal_locked(WIA1_SEC_SCOPE);
        WIA1_UNLOCK();
        if (locked) {
            wia1_logf("LOCKOUT - 10 fatals in 60s, auto-disarm, physical re-arm required");
            return WIA1_E_LOCKOUT;
        }
        wia1_logf("SCOPE VIOLATION");
        return rc;
    }
    if (rc != WIA1_OK) {
        wia1_seclog_note(WIA1_SEC_BAD_BODY);
        WIA1_UNLOCK();
        wia1_logf("BAD BODY");
        return rc;
    }
    s_st.ctr_rx = h.ctr;  /* counter adoption is last, after all gates */
    if (h.msg_type == WIA1_CTL_DISARM) {
        wia1_wipe_session_locked();
        wia1_infof("REMOTE DISARM - session wiped");
    } else if (h.msg_type == WIA1_CTL_PING) {
        s_st.wants_status = true;
    } else {
        wia1_exec_batch_locked();
    }
    WIA1_UNLOCK();
    return WIA1_OK;
}

wia1_rc wia1_ingest_ctl(const uint8_t *env, size_t env_len, const char *topic)
{
    /* Caller contract: the MQTT layer drops retained deliveries before
     * calling, so retained=false here (see wia1_ingest_ctl_r). */
    return wia1_ingest_ctl_r(env, env_len, topic, false);
}

/* ================= kind-1 seal (§8) ================= */

int wia1_seal_tel(uint16_t msg_type, const uint8_t *body, size_t body_len,
                  uint8_t *out, size_t cap, bool enc)
{
    if (out == NULL) {
        return -1;
    }
    if (body == NULL && body_len > 0) {
        return -1;
    }
    if (msg_type == WIA1_TEL_FRAME && !enc) {
        return -1;  /* frames are ENC-mandatory (§8) */
    }
    if (body_len > WIA1_MAX_TEL_BODY) {
        return -1;
    }
    WIA1_LOCK();
    if (!s_st.armed) {
        WIA1_UNLOCK();
        return -1;  /* telemetry exists only inside a session */
    }
    uint64_t now = WIA1_NOW_MS();
    if (now >= s_st.sess.t0_ms + (uint64_t)s_st.sess.ttl_s * 1000u) {
        /* an expired-but-unpolled session must not emit telemetry */
        wia1_wipe_session_locked();
        wia1_seclog_note(WIA1_SEC_EXPIRED);
        WIA1_UNLOCK();
        wia1_logf("SESSION EXPIRED - DISARMED");
        return -1;
    }
    size_t need = (size_t)WIA1_HDR_LEN + (enc ? 24u : 0u) + body_len + 16u;
    if (cap < need) {
        WIA1_UNLOCK();
        return -1;
    }
    /* topic binding: device publishes telemetry on wia/<dev_id>/tel */
    char topic[4 + 16 + 4 + 1];
    int tn = snprintf(topic, sizeof(topic), "wia/%s/tel", s_dev_id);
    size_t tlen = (tn > 0) ? (size_t)tn : 0;
    uint8_t *h = out;
    memcpy(h, WIA1_MAGIC, 4);
    h[4] = WIA1_KIND_TEL;
    h[5] = enc ? WIA1_FLAG_ENC : 0;
    memcpy(h + 6, s_dev_id, 16);
    memcpy(h + 22, s_st.sess.sess_id, 16);
    s_st.ctr_tx++;
    wia1_w64le(h + 38, s_st.ctr_tx);
    wia1_w64le(h + 46, WIA1_NOW_MS() - s_st.sess.t0_ms);
    wia1_w16le(h + 54, msg_type);
    memset(h + 56, 0, 8);
    if (enc) {
        uint8_t *nonce = out + WIA1_HDR_LEN;
        wia1_random_bytes(nonce, 24);
        /* AAD = u16le(len(topic)) || topic || header (§5.2) */
        uint8_t *aad = s_st.scratch;
        wia1_w16le(aad, (uint16_t)tlen);
        memcpy(aad + 2, topic, tlen);
        memcpy(aad + 2 + tlen, h, WIA1_HDR_LEN);
        static const uint8_t empty[1] = { 0 };
        crypto_aead_lock(out + WIA1_HDR_LEN + 24, out + need - 16,
                         s_st.sess.k_tel_aead, nonce,
                         aad, (size_t)2 + tlen + WIA1_HDR_LEN,
                         body != NULL ? body : empty, body_len);
    } else {
        if (body_len > 0) {
            memcpy(out + WIA1_HDR_LEN, body, body_len);
        }
        /* tag16 = BLAKE2b-keyed(k_tel_mac, S), S streamed (D6) */
        crypto_blake2b_ctx ctx;
        crypto_blake2b_keyed_init(&ctx, 16, s_st.sess.k_tel_mac, 32);
        uint8_t le[2];
        wia1_w16le(le, (uint16_t)tlen);
        crypto_blake2b_update(&ctx, le, 2);
        crypto_blake2b_update(&ctx, (const uint8_t *)topic, tlen);
        crypto_blake2b_update(&ctx, h, WIA1_HDR_LEN);
        if (body_len > 0 && body != NULL) {
            crypto_blake2b_update(&ctx, body, body_len);
        }
        crypto_blake2b_final(&ctx, out + WIA1_HDR_LEN + body_len);
    }
    WIA1_UNLOCK();
    return (int)need;
}

/* ================= grace (§9) ================= */

int wia1_grace_set(bool on)
{
    WIA1_LOCK();
    s_st.grace = on;
    uint8_t g = on ? 1 : 0;
    /* persisted via the nvs task; caller contract: this is only
     * ever invoked from the USB serial console on the bench, never over
     * MQTT/BLE. Not the shipping default (absent flag = strict).
     * v0.4.7: synchronous. */
    seal_dev_nvs_write_blob_sync("", "grace", &g, 1, 5000);
    WIA1_UNLOCK();
    if (on) {
        wia1_logf("GRACE ON - UNSIGNED MODE (bench only, mouse-scope only)");
    } else {
        wia1_infof("GRACE OFF - strict mode");
    }
    return 0;
}

bool wia1_grace_on(void)
{
    bool g;
    WIA1_LOCK();
    g = s_st.grace;
    WIA1_UNLOCK();
    return g;
}

/* ================= expiry / status / seclog ================= */

int wia1_poll_expiry(void)
{
    WIA1_LOCK();
    uint64_t now = WIA1_NOW_MS();
    int rc = 0;
    if (s_st.pending && now >= s_st.pending_deadline_ms) {
        wia1_wipe_pending_locked();
        wia1_logf("ARM WINDOW EXPIRED");
        rc = 1;
    }
    /* §4: the session TTL runs only while ARMED. An awaiting-accept
     * certificate must not expire off the cert-ingest timestamp: the
     * session clock starts at the accept tap (see wia1_accept_tap). */
    if (s_st.armed &&
        now >= s_st.sess.t0_ms + (uint64_t)s_st.sess.ttl_s * 1000u) {
        wia1_wipe_session_locked();
        wia1_seclog_note(WIA1_SEC_EXPIRED);
        wia1_logf("SESSION EXPIRED - DISARMED");
        rc = 1;
    }
    WIA1_UNLOCK();
    return rc;
}

int wia1_wants_status_reply(void)
{
    WIA1_LOCK();
    bool v = s_st.wants_status;
    s_st.wants_status = false;
    WIA1_UNLOCK();
    return v ? 1 : 0;
}

uint32_t wia1_auth_fail_count(void)
{
    uint32_t v;
    WIA1_LOCK();
    v = s_st.auth_fails;
    WIA1_UNLOCK();
    return v;
}

void wia1_seclog_poll(uint16_t *code_out, uint16_t *count_out)
{
    WIA1_LOCK();
    if (code_out != NULL) {
        *code_out = s_st.seclog_code;
    }
    if (count_out != NULL) {
        *count_out = s_st.seclog_count;
    }
    s_st.seclog_count = 0;
    WIA1_UNLOCK();
}

/* ================= display / claim hooks (not in wia1.h) ================= */

void wia1_claim_info(char *out, size_t cap)
{
    /* "dev_id=<id> fp=<16 hex of identity pubkey>" for the bench claim flow */
    static const char hexd[] = "0123456789abcdef";
    uint8_t fp[8];
    char fphex[17];
    WIA1_LOCK();
    bool ready = s_id_ready;
    if (ready) {
        crypto_blake2b(fp, sizeof(fp), s_id_pk, sizeof(s_id_pk));
        for (int i = 0; i < 8; i++) {
            fphex[2 * i]     = hexd[(fp[i] >> 4) & 0xF];
            fphex[2 * i + 1] = hexd[fp[i] & 0xF];
        }
        fphex[16] = '\0';
    }
    WIA1_UNLOCK();
    if (!ready || out == NULL || cap == 0) {
        if (out != NULL && cap > 0) {
            out[0] = '\0';
        }
        return;
    }
    snprintf(out, cap, "dev_id=%s fp=%s", s_dev_id, fphex);
}

void wia1_banner_state(char *out, size_t cap)
{
    /* "ARMED <scope> <mm:ss>" / "UNSIGNED MODE" / "DISARMED" (§§4, 9) */
    if (out == NULL || cap == 0) {
        return;
    }
    WIA1_LOCK();
    bool armed = s_st.armed;
    bool grace = s_st.grace;
    uint16_t scope = s_st.sess.scope;
    uint64_t rem_ms = 0;
    if (armed) {
        uint64_t exp = s_st.sess.t0_ms + (uint64_t)s_st.sess.ttl_s * 1000u;
        uint64_t now = WIA1_NOW_MS();
        rem_ms = (exp > now) ? (exp - now) : 0;
    }
    WIA1_UNLOCK();
    if (armed) {
        char sc[8];
        size_t n = 0;
        if ((scope & WIA1_SCOPE_MOUSE) != 0) {
            sc[n++] = 'M';
        }
        if ((scope & WIA1_SCOPE_KEYBOARD) != 0) {
            if (n > 0) {
                sc[n++] = '+';
            }
            sc[n++] = 'K';
        }
        if ((scope & WIA1_SCOPE_CONSUMER) != 0) {
            if (n > 0) {
                sc[n++] = '+';
            }
            sc[n++] = 'C';
        }
        sc[n] = '\0';
        uint64_t rem_s = rem_ms / 1000u;
        snprintf(out, cap, "ARMED %s %02llu:%02llu%s", sc,
                 (unsigned long long)(rem_s / 60u),
                 (unsigned long long)(rem_s % 60u),
                 grace ? " UNSIGNED" : "");
    } else if (grace) {
        snprintf(out, cap, "UNSIGNED MODE");
    } else {
        snprintf(out, cap, "DISARMED");
    }
}

/* ================= raw HID report stubs (weak) =================
 * hid_ble.h has no raw-report API. These weak stubs let wia1.c link and
 * run; the coordinator provides strong definitions at integration that
 * push the raw report / consumer usage to the BLE HID path.
 * Return 0 on success, nonzero otherwise.
 */
__attribute__((weak)) int wia1_raw_key_report(uint8_t mod, const uint8_t keys[6])
{
    (void)mod;
    (void)keys;
    return -1;
}

__attribute__((weak)) int wia1_raw_consumer(uint16_t usage)
{
    (void)usage;
    return -1;
}

#ifdef WIA1_HOST_TEST
/* ═══ HOST-TEST HOOKS — clearly marked, never compiled on device ═══ */

void wia1_test_last_log(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    size_t n = strlen(s_last_log);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s_last_log, n);
    out[n] = '\0';
}

/* RAM state reset between cases (NVS double + identity survive, like
 * real flash across a reboot). */
void wia1_test_reset(void)
{
    bool g = s_st.grace;
    crypto_wipe(&s_st, sizeof(s_st));
    crypto_wipe(&s_tmp, sizeof(s_tmp));
    s_st.grace = g;
}

/* Copy live session material out for oracle cross-checks. */
void wia1_test_session_keys(uint8_t mac[32], uint8_t aead[32],
                            uint8_t cmac[32], uint8_t caead[32],
                            uint8_t sid[16])
{
    WIA1_LOCK();
    if (mac != NULL) {
        memcpy(mac, s_st.sess.k_tel_mac, 32);
    }
    if (aead != NULL) {
        memcpy(aead, s_st.sess.k_tel_aead, 32);
    }
    if (cmac != NULL) {
        memcpy(cmac, s_st.sess.k_ctl_mac, 32);
    }
    if (caead != NULL) {
        memcpy(caead, s_st.sess.k_ctl_aead, 32);
    }
    if (sid != NULL) {
        memcpy(sid, s_st.sess.sess_id, 16);
    }
    WIA1_UNLOCK();
}
#endif
