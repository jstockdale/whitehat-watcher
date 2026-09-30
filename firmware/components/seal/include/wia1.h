/* SPDX-License-Identifier: MIT
 * Phase 3 WIA1 session machine (bench profile).
 *
 * Wire: HEADER(64) || [nonce24 if ENC] || BODY || AUTH, LE.
 *   magic "WIA1" 4 | kind u8 | flags u8 (bit0 ENC, rest zero) |
 *   dev_id[16] | session_id[16] (BLAKE2b-16 of cert_body; zero for 3/4) |
 *   ctr u64 (1-based per session+direction; zero for 3/4) |
 *   ts_rel_ms u64 (zero for 3/4) | msg_type u16 | rsv[8] (zero)
 * Kinds: 1 TEL (tag16 or AEAD), 2 CTL (sig64, agent session key),
 *        3 CERT (sig64, root key; body = cert_body[135]), 4 CHAL (sig64,
 *        device identity key; body = challenge_body[52]).
 *   cert_body (§5.3): cert_ver u8=1 | scope u16 | ttl_s u32 |
 *     device_id[16] | nonce[16] | agent_ed_pk[32] | agent_x_pk[32] |
 *     device_x_pk[32]  = 135 bytes
 *   challenge_body (§5.4): chal_ver u8=1 | proto_max u8=1 |
 *     scope_offered u16 | nonce[16] | device_x_pk[32] = 52 bytes
 *
 * All signatures are RFC 8032 Ed25519 (crypto_ed25519_*). Key schedule:
 *   ss = X25519(own_x_sk, peer_x_pk); wipe ss after derivation.
 *   k_label = BLAKE2b-keyed(key=ss, out=32, msg="wia1:"||label||":"||session_id)
 *   labels: "tel-mac" | "tel-aead" | "ctl-mac" | "ctl-aead"
 *
 * Session state is RAM-only; reboot disarms by construction. Every
 * rejection has exactly one named log string (§6); rejections are
 * never answered on the wire (drop and count, no oracle).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIA1_MAGIC          "WIA1"
#define WIA1_HDR_LEN        64
#define WIA1_KIND_TEL       1
#define WIA1_KIND_CTL       2
#define WIA1_KIND_CERT      3
#define WIA1_KIND_CHAL      4
#define WIA1_CERT_BODY_LEN  135
#define WIA1_CHAL_BODY_LEN  52
#define WIA1_FLAG_ENC       0x01
#define WIA1_MAX_TOPIC      128
#define WIA1_MAX_CTL_BODY   512
#define WIA1_MAX_TEL_BODY   (64u * 1024u)
#define WIA1_MAX_TTL_S      (4u * 3600u)
#define WIA1_TS_WINDOW_MS   10000u
#define WIA1_LOCKOUT_FATALS 10u
#define WIA1_LOCKOUT_WIN_S  60u
#define WIA1_ARM_WINDOW_S   120u

#define WIA1_SCOPE_MOUSE    (1u << 0)
#define WIA1_SCOPE_KEYBOARD (1u << 1)
#define WIA1_SCOPE_CONSUMER (1u << 2)

/* Control msg_types (§7) */
#define WIA1_CTL_BATCH   0x0001
#define WIA1_CTL_DISARM  0x0002
#define WIA1_CTL_PING    0x0003
/* Batch ops */
#define WIA1_OP_MOVE_REL 0x01  /* dx i16, dy i16 — MOUSE */
#define WIA1_OP_BUTTON   0x02  /* mask u8, action u8 — MOUSE */
#define WIA1_OP_WHEEL    0x03  /* dz i8 — MOUSE */
#define WIA1_OP_DWELL    0x04  /* ms u16 ≤ 2000 — any */
#define WIA1_OP_KEY_RPT  0x10  /* modifiers u8, keys u8[6] — KEYBOARD */
#define WIA1_OP_CONSUMER 0x20  /* usage u16 — CONSUMER */
/* Telemetry msg_types (§8) */
#define WIA1_TEL_ARMED_ACK 0x0001
#define WIA1_TEL_STATUS    0x0002
#define WIA1_TEL_FRAME     0x0003  /* ENC REQUIRED */
#define WIA1_TEL_SECLOG    0x0004

/* Root pubkeys live in seal_roots.h (primary + backup, baked from day
 * one; the parent fills the placeholder bytes before release). */
#include "seal_roots.h"

typedef enum {
    WIA1_OK = 0,
    WIA1_OK_DUP,
    WIA1_E_SHORT = -1,
    WIA1_E_MAGIC = -2,
    WIA1_E_KIND  = -3,
    WIA1_E_BADSIG = -4,   /* always fatal */
    WIA1_E_WRONG_DEV = -5,
    WIA1_E_DISARMED = -6,
    WIA1_E_STALE_SESSION = -7,
    WIA1_E_REPLAY = -8,
    WIA1_E_STALE_TS = -9,
    WIA1_E_SCOPE = -10,
    WIA1_E_BAD_BODY = -11,
    WIA1_E_BAD_CERT = -12,
    WIA1_E_NO_WINDOW = -13,
    WIA1_E_RETAINED = -14,
    WIA1_E_LOCKOUT = -15,
} wia1_rc;

const char *wia1_rc_str(wia1_rc rc);   /* named log strings, §6 */

esp_err_t wia1_init(void);   /* mints device identity on first boot */
/* Device id: "wia-" + 12 hex of BLAKE2b-8(identity pubkey). */
const char *wia1_dev_id(void);

bool wia1_is_armed(void);
/* hex16 session fingerprint, "" when disarmed — for the serial contract. */
void wia1_session_id_hex(char out[17]);

/* Arm ceremony. Physical tap -> kind-4 challenge bytes out (caller
 * publishes on wia/<dev_id>/arm/challenge or shows b64 on screen). */
int wia1_arm_tap(uint8_t *chal_out, size_t cap);
/* Physical disarm tap: always works, needs no signature. */
int wia1_disarm_tap(void);
/* Agent-signed CTL_DISARM path. */
int wia1_disarm_remote(void);

/* Kind-3 ingest. Returns 1 = ARMED immediately (mouse-only scope),
 * 0 = cert accepted, waiting for the keyboard/consumer accept tap,
 * <0 = wia1_rc reject (arm state unchanged). */
int wia1_ingest_cert(const uint8_t *env, size_t env_len);
bool wia1_needs_accept_tap(void);
int  wia1_accept_tap(void);   /* explicit on-screen accept -> ARMED */

/* Kind-2 ingest: full §6 verify order, then CTL_BATCH exec.
 * topic = delivered MQTT topic (or "serial" for USB-console grace). */
wia1_rc wia1_ingest_ctl(const uint8_t *env, size_t env_len, const char *topic);

/* Kind-1 seal for native telemetry. enc=true -> XChaCha20-Poly1305
 * (REQUIRED for TEL_FRAME). Counters are per-session, first = 1. */
int wia1_seal_tel(uint16_t msg_type, const uint8_t *body, size_t body_len,
                  uint8_t *out, size_t cap, bool enc);

/* Grace: USB serial console only, mouse-scope only, all-zero sig64
 * marker. Never persisted as the shipping default. */
int wia1_grace_set(bool on);
bool wia1_grace_on(void);

/* Lockout / seclog counters for TEL_SECLOG + status. */
uint32_t wia1_auth_fail_count(void);
void wia1_seclog_poll(uint16_t *code_out, uint16_t *count_out);

#ifdef __cplusplus
}
#endif
