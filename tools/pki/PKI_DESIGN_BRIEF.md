# Watcher PKI — Design Brief (shared source of truth)

John-approved architecture for secure MQTT key provisioning. All workers
(key tooling, AWS infra, management console, firmware integration) MUST
follow this brief. If the brief is wrong, fix the brief first.

## Key hierarchy

| Tier | Role | Algorithm | Location |
|------|------|-----------|----------|
| Tier 0 | Offline root. Signs Tier 1 certs ONLY. John holds it. | Ed25519 | Local secure file (0600). NEVER in AWS, NEVER in repo. |
| Tier 1 | Provisioning authority. Signs provisioning commands. | ECDSA P-256 (KMS key spec `ECC_NIST_P256`, `ECDSA_SHA_256`) | AWS KMS (HSM-backed). Lambda gets `kms:Sign` ONLY — never `kms:Get`; private key never leaves KMS. |
| Tier 2 | Trust anchors baked into firmware at build time: Tier 0 Ed25519 pubkey + Tier 1 P-256 pubkey + Tier 1 cert expiry. | — | Firmware image (generated header). Rotate every few v0.x.y releases during dev. |

**Why P-256 for Tier 1:** AWS KMS does not support Ed25519. ECDSA P-256 is
verifiable on-device via mbedTLS (already in ESP-IDF). The runtime seal
stays Ed25519 — PKI (provisioning trust) and seal (operational trust)
are separate domains by design.

**Why Ed25519 for Tier 0:** consistent with project crypto (PyNaCl /
Monocypher, already used by the seal). Tier 0 is local-only, so no KMS
constraint applies. The device already verifies Ed25519 via Monocypher
(seal component), so Tier 0-signed rotation commands need no new crypto
on device.

## Trust flow

1. Build pipeline (offline): verifies Tier 0 signature over the Tier 1
   cert in Python, extracts the Tier 1 P-256 pubkey, bakes Tier 0 pubkey +
   Tier 1 pubkey + expiry into firmware as a generated C header.
2. Normal provisioning: device verifies the P-256 signature on the
   provisioning command against the baked Tier 1 pubkey (mbedTLS),
   validates schema/nonce/expiry/scope, then applies.
3. Tier 1 rotation: Tier 0 signs the rotation canonical bytes
   (`WATCHER-TIER1-ROTATE-v1 || new_p256_pubkey || u64 expiry || u32
   serial` — exact form defined below) with Ed25519; device verifies
   against the baked Tier 0 pubkey (Monocypher) and replaces the stored
   Tier 1 pubkey (persist via the existing `nvs_task` broker —
   PSRAM-stack tasks must NEVER touch NVS directly; see firmware
   `components/seal/` and the 0.4.7/0.4.8 NVS broker work). The Tier 0
   countersignature is REQUIRED: a compromised Tier 1 must not be able
   to rotate to its own key.

## Tier 1 certificate format (JSON)

```json
{
  "format": "watcher-tier1-cert-v1",
  "tier": 1,
  "subject": "watcher-tier1-provisioning",
  "algo": "ecdsa-p256-sha256",
  "p256_pubkey_hex": "<65-byte uncompressed 04||x||y, hex>",
  "issued_at": "2026-09-27T00:00:00Z",
  "expires_at": "2027-09-27T00:00:00Z",
  "serial": 1,
  "tier0_sig_hex": "<64-byte Ed25519 signature, hex>"
}
```

Canonical bytes signed by Tier 0 (all big-endian, no JSON ambiguity):

```
b"WATCHER-TIER1-CERT-v1"
|| u16(len(subject)) || subject.encode()
|| u8(0x01)                       # algo id: ecdsa-p256-sha256
|| 65-byte uncompressed P-256 pubkey
|| u64(issued_at unix) || u64(expires_at unix)
|| u32(serial)
```

Verification order: parse → recompute canonical bytes → Ed25519 verify
→ check `issued_at <= now <= expires_at`.

## Provisioning command schema (JSON)

```json
{
  "format": "watcher-provision-v1",
  "cmd": "install_commander_key | install_telemetry_seed | rotate_tier1 | factory_reset_keys",
  "device": "<device-id or '*' for any unkeyed device>",
  "nonce": "<32 hex chars, random per command>",
  "issued_at": "2026-09-27T00:00:00Z",
  "expires_at": "2026-09-27T00:15:00Z",
  "params": {
    "install_commander_key": { "pubkey_hex": "<32-byte Ed25519 pubkey>" },
    "install_telemetry_seed": { "seed_fp": "<fingerprint; seed itself NEVER in this command>" },
    "rotate_tier1":           { "p256_pubkey_hex": "<65-byte uncompressed>", "expires_at": "<iso>",
                             "tier0_sig_hex": "<128 hex chars, REQUIRED — Ed25519 over the rotation canonical bytes below>",
                             "serial": "<u32, REQUIRED — strictly greater than the baked cert serial>" },
    "factory_reset_keys":     { "confirm": true }
  }
}
```

Canonical bytes signed by Tier 1 (KMS `ECDSA_SHA_256`):

```
b"WATCHER-PROVISION-v1"
|| u16(len(cmd)) || cmd.encode()
|| u16(len(device)) || device.encode()
|| 16-byte nonce (raw)
|| u64(issued_at unix) || u64(expires_at unix)
|| canonical-JSON(params, sort_keys=True, separators=(",",":")).encode()
```

KMS returns DER signatures. The Lambda converts DER → raw 64-byte
r||s for transport; the device feeds r/s to mbedTLS as MPIs, so no DER
parsing happens on device.

### Tier 1 rotation canonical bytes (v1)

Rotation is the one command the Tier 1 key holder cannot authorize
alone: a compromised Tier 1 (KMS) must not be able to rotate to its own
key. The rotation payload is therefore countersigned by Tier 0
(Ed25519), and the device verifies that countersignature against the
baked Tier 0 pubkey (Monocypher) **in addition to** the outer Tier 1
(P-256) signature on the command.

Rotation canonical bytes (signed by Tier 0):

```
b"WATCHER-TIER1-ROTATE-v1"
|| new_p256_pubkey (65 bytes, uncompressed, 0x04 prefix)
|| u64(expires_at unix, big-endian)
|| u32(serial, big-endian)
```

`new_p256_pubkey` / `expires_at` are the `rotate_tier1` params;
`serial` is the new Tier 1 cert serial and MUST be strictly greater
than the baked `TIER1_CERT_SERIAL`. The device additionally requires
the new `expires_at` to be strictly later than the baked
`TIER1_CERT_EXPIRES_AT` — rotation must never shorten the trust
anchor's lifetime.

Transport: `tier0_sig_hex` (128 hex chars) and `serial` ride inside the
`rotate_tier1` params of the normal `pki_provision` command. The outer
command keeps the Tier 1 (P-256) signature (the current key holder
co-signs the transport); the Tier 0 countersignature is the actual
rotation authorization. **A `rotate_tier1` command without a valid Tier
0 countersignature is rejected** — this field is REQUIRED, not
optional.

**Device acceptance checks (in order):** parse → Tier 1 P-256 signature
verify → `issued_at <= now <= expires_at` (15-min window; device
`ts_window=0`, no SNTP — accept small skew, document) → nonce not seen
(persist recent nonces in NVS) → `device` matches self or `*` (only
honor `*` while unkeyed) → schema-validate `params` per `cmd` →
apply via `nvs_task` broker.

**Lambda pre-sign validation (MANDATORY):** the signing Lambda MUST
schema-validate the command (allowed `cmd` values, `device` format,
expiry window ≤ 15 min, `params` shape per cmd) before calling KMS.
Never sign arbitrary bytes. Reject with 400 + audit entry.

## Scope rule

Tier 1 signs **provisioning commands only**. Runtime commands stay
under the Ed25519 seal (commander key). Do not let Tier 1 sign
arbitrary runtime commands — that collapses the two trust domains.

## API surface (API Gateway, IAM auth)

- `GET /tier1/pubkey` → `{ p256_pubkey_hex, cert }` (for firmware build pipeline)
- `POST /sign` → `{ command: <provisioning cmd JSON> }` → `{ signature_hex, cert, key_id }`
- `GET /audit` → queryable signing log (`?since=`, `?cmd=`, `?device=`)
- `POST /rotation` → `{ tier1_cert }` (Tier 0-signed new cert; triggers Tier 1 key alias update + audit)

All responses include `key_id` / cert `serial` so verifiers pin to the
right key.

## Audit log (DynamoDB)

Every `POST /sign` and `POST /rotation` writes: timestamp, IAM
principal, command canonical hash (SHA-256, NOT the command itself —
params may reference secrets), `cmd`, `device`, cert serial, KMS key
ARN, result (signed/rejected + reason).

## Threat model (summary)

- Network attacker: cannot forge Tier 1 signatures (no KMS access);
  replay blocked by nonce + 15-min expiry; cannot downgrade (device
  checks cert expiry; rotation requires Tier 0).
- Compromised Tier 1 (KMS): rotate via Tier 0-signed `rotate_tier1`;
  Tier 0 pubkey baked in firmware so a compromised Tier 1 cannot block
  its own rotation. Blast radius bounded by cert expiry.
- Compromised build machine: build verifies Tier 0 cert before baking;
  never bakes a bare pubkey. (Full supply-chain hardening is out of
  scope for dev.)
- Stolen device: no CRL in dev. Documented limitation; per-device
  revocation is a release-candidate feature.
- Tier 0 compromise: catastrophic by design — Tier 0 is offline and
  John's custody is the mitigation. Document the ceremony.

## Naming / paths

- Project root: `~/workspace/projects/watcher-input-assistant/`
- PKI tooling: `~/workspace/projects/watcher-input-assistant/tools/pki/`
- Infra: `~/workspace/projects/watcher-input-assistant/tools/pki/infra/`
- Console: `~/workspace/your_files/watcher-console/` (John's pattern:
  downloadable HTML served from his local webserver)
- Firmware hook: `~/workspace/projects/watcher-input-assistant/firmware/tools/`
- Private keys: NEVER in repo. `.gitignore`: `*_private*`, `tier0*`,
  `*.key`, `*.pem` under `tools/pki/`.

## Existing crypto reference

- `host-agent/mqtt-bridge/mqtt_seal.py` — PyNaCl Ed25519 seal (WMS2 envelopes)
- `firmware/components/seal/` — device-side seal (Monocypher Ed25519)
- NVS broker pattern: `nvs_task` FIFO queue, `xTaskCreateStatic` +
  `static DRAM_ATTR` stack (0.4.7/0.4.8 work). PSRAM-stack tasks must
  NEVER call NVS/flash directly.
