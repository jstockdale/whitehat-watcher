/* SPDX-License-Identifier: MIT
 * Watcher PKI — device-side provisioning verification (Tier 2 trust anchors).
 *
 * Design brief: tools/pki/PKI_DESIGN_BRIEF.md (shared source of truth).
 *
 * This component is a STUB for logic + API shape: the crypto verification
 * below is REAL and complete, but wiring into the link.c command
 * dispatcher and NVS persistence of verified keys/nonces is a separate
 * firmware task (see INTEGRATION.md).
 *
 * Trust model recap:
 *   Tier 0 (offline root, Ed25519) signs Tier 1 certs. The Tier 0 pubkey
 *     is baked into firmware and verifies Tier 1 ROTATIONS (Monocypher).
 *   Tier 1 (provisioning authority, ECDSA P-256, KMS-backed) signs
 *     provisioning COMMANDS. The Tier 1 pubkey is baked into firmware
 *     (mbedTLS verifies); rotation replaces the stored key later.
 *
 * NVS discipline: this component NEVER calls nvs_* directly. Verified
 * keys and nonces persist via the single nvs_task broker using the
 * seal_dev_nvs_*_sync() APIs (components/seal/include/seal_dev.h).
 * The build-time audit (firmware/tools/audit_psram_flash.py) enforces
 * this — see INTEGRATION.md.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provisioning command schema limits (brief: watcher-provision-v1). */
#define PKI_CMD_NAME_MAX   40   /* e.g. "install_commander_key" */
#define PKI_DEVICE_MAX     64   /* device id, or "*" */
#define PKI_NONCE_HEX_LEN  32   /* 32 hex chars -> 16 raw bytes */
#define PKI_NONCE_RAW_LEN  16
#define PKI_CANON_MAX      1536 /* max canonical-bytes scratch */
#define PKI_JSON_MAX       2048 /* max provisioning command JSON accepted */

/* 15-minute provisioning window mandated by the brief. */
#define PKI_PROV_WINDOW_S  900

/* Nonce replay store size (RAM stub; the full task persists to NVS). */
#define PKI_NONCE_STORE_N  64

typedef enum {
    PKI_OK = 0,
    PKI_ERR_PARSE,    /* JSON unparseable */
    PKI_ERR_SCHEMA,   /* missing/ill-typed field, bad cmd, bad params */
    PKI_ERR_BADSIG,   /* Tier 1 (or Tier 0) signature invalid */
    PKI_ERR_EXPIRED,  /* now > expires_at */
    PKI_ERR_NOTYET,   /* now < issued_at */
    PKI_ERR_WINDOW,   /* issued_at > expires_at, or window > 15 min */
    PKI_ERR_NONCE,    /* replayed nonce, or nonce store full */
    PKI_ERR_SCOPE,    /* device id mismatch */
    PKI_ERR_NOMEM,    /* scratch buffer too small */
    PKI_ERR_ARG,      /* bad argument to an API */
} pki_rc_t;

const char *pki_rc_str(pki_rc_t rc);

typedef enum {
    PKI_CMD_INSTALL_COMMANDER_KEY = 0,
    PKI_CMD_INSTALL_TELEMETRY_SEED,
    PKI_CMD_ROTATE_TIER1,
    PKI_CMD_FACTORY_RESET_KEYS,
    PKI_CMD_UNKNOWN,
} pki_cmd_kind_t;

/* Parsed + verified provisioning command. `params_json` points at the
 * canonical-JSON re-serialization of the params object (stable bytes the
 * signature covered); it aliases the out struct's storage and stays valid
 * as long as `out` is alive. */
typedef struct {
    pki_cmd_kind_t kind;
    char           cmd[PKI_CMD_NAME_MAX];
    char           device[PKI_DEVICE_MAX];
    uint8_t        nonce[PKI_NONCE_RAW_LEN];
    uint64_t       issued_at;   /* unix */
    uint64_t       expires_at;  /* unix */
    char           params_json[PKI_JSON_MAX];
} pki_provision_cmd_t;

/* ---- Device context (set once by the dispatcher at boot) --------------
 * The stub does not reach into other components for identity: the full
 * firmware task calls pki_set_device_context() with the provisioned
 * device name (e.g. hid_ble_device_name()) and whether a commander key
 * is already pinned (seal_dev_has_commander_key()). `keyed == false`
 * means the device honors device:"*" commands (unkeyed devices only). */
void pki_set_device_context(const char *device_id, bool keyed);

/* ---- Pure, unit-testable helpers -------------------------------------- */

/* Strict "YYYY-MM-DDTHH:MM:SSZ" -> unix seconds. */
pki_rc_t pki_parse_iso8601_utc(const char *iso, uint64_t *out_unix);

/* Hex decode: exactly out_len bytes required; 0 on any error, else bytes. */
size_t pki_hex_to_bytes(const char *hex, uint8_t *out, size_t out_len);

/* Canonical JSON per the brief: sort_keys=True, separators=(",",":"),
 * ensure_ascii=True — byte-identical to Python's json.dumps with those
 * options. Numbers: integral doubles only (schema has no floats); a
 * non-integral number is PKI_ERR_SCHEMA, never silently re-printed. */
struct cJSON; /* fwd; real type is cJSON from cJSON.h */
pki_rc_t pki_canon_json(const struct cJSON *node, char *out,
                        size_t out_cap, size_t *out_len);

/* Canonical provisioning bytes:
 *   b"WATCHER-PROVISION-v1" || u16(len(cmd)) || cmd || u16(len(device)) ||
 *   device || 16-byte nonce || u64(issued) || u64(expires) ||
 *   canonical-JSON(params)
 * (all integers big-endian). Byte-identical to
 * bake_tier2_keys.provision_canonical_bytes(). */
pki_rc_t pki_build_provision_canon(const char *cmd, const char *device,
                                   const uint8_t nonce[PKI_NONCE_RAW_LEN],
                                   uint64_t issued_at, uint64_t expires_at,
                                   const char *params_canon_json,
                                   uint8_t *out, size_t out_cap,
                                   size_t *out_len);

/* Canonical Tier 0-signed rotation payload:
 *   b"WATCHER-TIER1-ROTATE-v1" || 65-byte new P-256 pubkey ||
 *   u64(expires_at) || u32(serial)
 * PROPOSED canonical form (the brief only sketches
 * rotate_tier1{new_p256_pubkey, expiry}); the signing tooling
 * (tools/pki) MUST mirror this exactly — flagged in INTEGRATION.md. */
pki_rc_t pki_build_rotate_canon(const uint8_t new_pub65[65],
                                uint64_t expires_at, uint32_t serial,
                                uint8_t *out, size_t out_cap,
                                size_t *out_len);

/* ECDSA P-256 verify: SHA-256(canon) against raw 64-byte r||s.
 * 0 = valid signature; nonzero = mbedTLS error / invalid. */
int pki_ecdsa_p256_verify(const uint8_t pub_uncompressed65[65],
                          const uint8_t *msg, size_t msg_len,
                          const uint8_t sig_raw64[64]);

/* Ed25519 verify via Monocypher (same call pattern as seal/wms2.c).
 * 0 = valid signature. */
int pki_ed25519_verify(const uint8_t pub32[32],
                       const uint8_t *msg, size_t msg_len,
                       const uint8_t sig64[64]);

/* Device clock. Returns 0: the device has no SNTP (ts_window=0), so no
 * wall clock exists yet. The full task must wire a time source before
 * the expiry-window check is meaningful; until then the verifier
 * enforces issued_at <= expires_at and the 15-min window only. */
uint64_t pki_time_now(void);

/* ---- Nonce replay store (RAM STUB) ------------------------------------
 * pki_nonce_seen() / pki_nonce_record() implement the interface the
 * brief mandates ("persist recent nonces in NVS"). The stub keeps the
 * last PKI_NONCE_STORE_N nonces in RAM — it does NOT survive reboot.
 * The full firmware task persists via seal_dev_nvs_write_blob_sync()
 * through the nvs_task broker (see INTEGRATION.md). */
bool     pki_nonce_seen(const uint8_t nonce[PKI_NONCE_RAW_LEN]);
pki_rc_t pki_nonce_record(const uint8_t nonce[PKI_NONCE_RAW_LEN]);

/* ---- Full verification --------------------------------------------------
 * Device acceptance checks, in brief order:
 *   parse -> Tier 1 P-256 signature verify (baked key) -> issued_at <=
 *   now <= expires_at (skipped while pki_time_now()==0; the 15-min
 *   window and issued<=expires are always enforced) -> nonce not seen
 *   -> device scope -> per-cmd params schema validation.
 *
 * On PKI_OK, `out` holds the verified command. `sig_raw64` is the
 * Lambda's DER->raw64 converted r||s (64 bytes).
 * This function performs NO NVS writes and NO dispatching; the caller
 * (link.c handle_command) applies the verified command. */
pki_rc_t pki_verify_provision(const uint8_t *cmd_json, size_t len,
                              const uint8_t *sig_raw64,
                              pki_provision_cmd_t *out);

/* Tier 0-signed rotation payload verification (Monocypher Ed25519
 * against the BAKED Tier 0 pubkey). On PKI_OK the caller may replace
 * the stored Tier 1 key via the nvs_task broker. Also enforces expiry
 * monotonicity: new expiry must exceed the baked cert's expiry, so a
 * rotation can never downgrade the trust anchor's lifetime. */
pki_rc_t pki_verify_rotate_tier1(const uint8_t new_pub65[65],
                                 uint64_t expires_at, uint32_t serial,
                                 const uint8_t sig64[64]);

#ifdef __cplusplus
}
#endif
