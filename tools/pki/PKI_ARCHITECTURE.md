# Watcher PKI — Architecture

Companion to `PKI_DESIGN_BRIEF.md` (the shared source of truth — if this
document and the brief ever disagree, the brief wins and this document
gets fixed). This file explains the *why* and the *how it fits together*;
the brief carries the normative byte layouts. Normative formats are
repeated here verbatim so firmware and infra workers don't have to
chase references mid-implementation.

Status: dev. The PKI secures **provisioning** (getting commander keys,
telemetry seeds, and trust anchors onto the device). **Runtime**
commands stay under the Ed25519 seal (commander key). The two trust
domains never cross — see §7.

## 1. Key hierarchy

| Tier | Role | Algorithm | Location |
|------|------|-----------|----------|
| **Tier 0** | Offline root. Signs Tier 1 certs and `rotate_tier1` commands **only**. John holds it. | Ed25519 | Local secure file, mode 0600 (default `~/.config/watcher-pki/`). **NEVER in AWS, NEVER in the repo.** |
| **Tier 1** | Provisioning authority. Signs provisioning commands. | ECDSA P-256 (`ECC_NIST_P256`, `ECDSA_SHA_256`) | AWS KMS (HSM-backed). Lambda gets `kms:Sign` **only** — never `kms:Get`; the private key never leaves KMS. |
| **Tier 2** | Trust anchors baked into firmware at build time: Tier 0 Ed25519 pubkey + Tier 1 P-256 pubkey + Tier 1 cert expiry. | — | Firmware image (generated C header). Rotated every few v0.x.y releases during dev (§10). |

### Why these algorithms

- **Tier 1 is P-256 because AWS KMS does not support Ed25519.**
  ECDSA P-256 is verifiable on-device via mbedTLS, which is already in
  ESP-IDF. The KMS key spec is `ECC_NIST_P256` with signing algorithm
  `ECDSA_SHA_256`.
- **Tier 0 is Ed25519 for consistency with project crypto** (PyNaCl /
  Monocypher, already used by the seal). Tier 0 is local-only, so no KMS
  constraint applies. The device already verifies Ed25519 via Monocypher
  (the seal component), so Tier 0-signed rotation commands need no new
  on-device crypto.
- **The runtime seal stays Ed25519.** PKI (provisioning trust) and seal
  (operational trust) are separate domains by design. A compromise in
  one does not automatically compromise the other.

## 2. Trust flows

### 2a. Build pipeline (offline)

1. Operator publishes the Tier 1 cert (JSON, Tier 0-signed).
2. The build pipeline runs `tools/pki/verify_tier1_cert.py` against the
   recorded Tier 0 pubkey. **It never bakes a bare pubkey** — an
   unverified cert fails the build.
3. On success, the pipeline extracts the Tier 1 P-256 pubkey and emits a
   generated C header containing: Tier 0 Ed25519 pubkey, Tier 1 P-256
   pubkey, Tier 1 cert expiry (unix). That header becomes Tier 2.
4. The pipeline prints the Tier 0 fingerprint; the operator confirms it
   matches the out-of-band recorded fingerprint before flashing.

### 2b. Normal provisioning

1. Operator (or management console) submits a provisioning command to
   `POST /sign`.
2. The Lambda **schema-validates** the command (allowed `cmd` values,
   `device` format, expiry window ≤ 15 min, `params` shape per cmd) and
   rejects with 400 + audit entry on any violation. **Never sign
   arbitrary bytes.**
3. Lambda calls KMS `Sign` (`ECDSA_SHA_256`), converts the returned DER
   signature to raw 64-byte `r||s` for transport.
4. The device verifies the P-256 signature against the baked Tier 1
   pubkey (mbedTLS), then runs the acceptance checks in order: parse →
   signature → `issued_at <= now <= expires_at` (15-min window; device
   has `ts_window=0`, no SNTP — accept small skew, document the bound) →
   nonce not seen (persist recent nonces in NVS) → `device` matches self
   or `*` (honor `*` only while unkeyed) → schema-validate `params` per
   `cmd` → apply via the `nvs_task` broker.
5. Every signing is audit-logged (canonical hash of the command, never
   the command itself — params may reference secrets).

### 2c. Tier 1 rotation

1. Operator generates a new Tier 1 KMS key, extracts its P-256 pubkey.
2. Operator runs `tools/pki/issue_tier1_cert.py` **offline** with the
   Tier 0 key: builds the cert JSON, signs the canonical bytes, writes
   `<out>.tier1cert.json`.
3. Operator `POST`s the cert to `/rotation`. The infra worker validates
   the Tier 0 signature, updates the Tier 1 key alias, and writes an
   audit entry.
4. Devices receive `rotate_tier1{new_p256_pubkey, expiry}` — a
   **provisioning command signed by Tier 1** — or, if Tier 1 itself is
   compromised, a **Tier 0-signed** rotation: the device verifies
   Ed25519 against the baked Tier 0 pubkey (Monocypher) and replaces the
   stored Tier 1 pubkey, persisting via the existing `nvs_task` broker.
   (PSRAM-stack tasks must NEVER touch NVS directly — see
   `firmware/components/seal/` and the 0.4.7/0.4.8 NVS broker work.)
5. The baked Tier 0 pubkey is what makes this work: a compromised
   Tier 1 cannot block its own rotation because the rotation path does
   not depend on Tier 1.

## 3. Normative formats

### 3a. Tier 1 certificate (JSON) — `watcher-tier1-cert-v1`

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

Timestamps are UTC `YYYY-MM-DDTHH:MM:SSZ`. `serial` is a u32 and MUST
increase on every rotation (monotonic within a Tier 0 lineage).

### 3b. Tier 1 cert canonical bytes (what Tier 0 signs)

All integers big-endian, no JSON ambiguity:

```
b"WATCHER-TIER1-CERT-v1"
|| u16(len(subject)) || subject.encode()
|| u8(0x01)                       # algo id: ecdsa-p256-sha256
|| 65-byte uncompressed P-256 pubkey
|| u64(issued_at unix) || u64(expires_at unix)
|| u32(serial)
```

Reference implementation: `tools/pki/pki_common.py::cert_canonical_bytes`
(the single source of truth for the Python tooling; independently
cross-checked byte-for-byte in testing). The firmware build pipeline
must reproduce these bytes exactly — any divergence invalidates every
cert, loudly, at build time rather than on a bench device.

Verification order (build pipeline): parse → recompute canonical bytes
→ Ed25519 verify → check `issued_at <= now <= expires_at`.

### 3c. Provisioning command (JSON) — `watcher-provision-v1`

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
    "rotate_tier1":           { "p256_pubkey_hex": "<65-byte uncompressed>", "expires_at": "<iso>" },
    "factory_reset_keys":     { "confirm": true }
  }
}
```

Notes the device enforces:
- `install_telemetry_seed` carries only the seed **fingerprint**. The
  seed itself travels over USB serial during provisioning, never in a
  signed command, never in a log.
- `rotate_tier1` is how a healthy Tier 1 rotates itself forward
  (Tier 0-signed rotation is the break-glass path, §2c step 4).
- `factory_reset_keys` requires `"confirm": true` and is the only
  destructive command.

### 3d. Provisioning command canonical bytes (what Tier 1 signs)

```
b"WATCHER-PROVISION-v1"
|| u16(len(cmd)) || cmd.encode()
|| u16(len(device)) || device.encode()
|| 16-byte nonce (raw)
|| u64(issued_at unix) || u64(expires_at unix)
|| canonical-JSON(params, sort_keys=True, separators=(",",":")).encode()
```

- KMS returns **DER** signatures. The Lambda converts DER → raw 64-byte
  `r||s` for transport. The device may parse DER directly with mbedTLS
  or convert back — the firmware worker's call; document whichever is
  chosen in the firmware integration notes.
- `nonce` is 16 raw bytes on the wire (32 hex chars in JSON).
- `canonical-JSON` means `json.dumps(params, sort_keys=True,
  separators=(",", ":"))` — no whitespace, deterministic key order.

### 3e. Device acceptance checks (in order — do not reorder)

1. parse
2. Tier 1 P-256 signature verify
3. `issued_at <= now <= expires_at` (15-min window; device `ts_window=0`,
   no SNTP — accept small skew, document the bound)
4. nonce not seen (persist recent nonces in NVS)
5. `device` matches self or `*` (honor `*` only while unkeyed)
6. schema-validate `params` per `cmd`
7. apply via `nvs_task` broker

### 3f. Lambda pre-sign validation (MANDATORY)

The signing Lambda MUST schema-validate the command before calling KMS:
allowed `cmd` values, `device` format, expiry window ≤ 15 min,
`params` shape per cmd. Never sign arbitrary bytes. Reject with 400 +
audit entry.

## 4. API surface (API Gateway, IAM auth)

Infra worker's scope; summarized here so all workers share the contract.

- `GET /tier1/pubkey` → `{ p256_pubkey_hex, cert }` (for the firmware build pipeline)
- `POST /sign` → `{ command: <provisioning cmd JSON> }` → `{ signature_hex, cert, key_id }`
- `GET /audit` → queryable signing log (`?since=`, `?cmd=`, `?device=`)
- `POST /rotation` → `{ tier1_cert }` (Tier 0-signed new cert; triggers Tier 1 key alias update + audit)

All responses include `key_id` / cert `serial` so verifiers pin to the
right key.

## 5. Audit log (DynamoDB)

Every `POST /sign` and `POST /rotation` writes: timestamp, IAM
principal, command canonical hash (**SHA-256, NOT the command itself** —
params may reference secrets), `cmd`, `device`, cert serial, KMS key
ARN, result (`signed` / `rejected` + reason). Review cadence and
procedure: see `PKI_RUNBOOK.md` §8.

## 6. Threat model

- **Network attacker:** cannot forge Tier 1 signatures (no KMS access);
  replay blocked by nonce + 15-min expiry; cannot downgrade (device
  checks cert expiry; rotation requires Tier 0).
- **Compromised Tier 1 (KMS):** rotate via Tier 0-signed `rotate_tier1`
  (§2c). Tier 0 pubkey is baked in firmware, so a compromised Tier 1
  cannot block its own rotation. Blast radius is bounded by cert expiry.
- **Compromised build machine:** the build verifies the Tier 0 cert
  before baking and never bakes a bare pubkey. (Full supply-chain
  hardening is out of scope for dev.)
- **Stolen device:** no CRL in dev. Documented limitation; per-device
  revocation is a release-candidate feature.
- **Tier 0 compromise:** catastrophic by design — Tier 0 is offline and
  John's custody is the mitigation. The ceremony is documented in
  `PKI_RUNBOOK.md` §2; the compromise procedure in §7 there.

## 7. Scope rule (read this twice)

**Tier 1 signs provisioning commands only.** Runtime commands stay under
the Ed25519 seal (commander key). Do not let Tier 1 sign arbitrary
runtime commands — that collapses the two trust domains, and the whole
point of the split (a KMS compromise must not yield device control) is
lost.

## 8. Key lifecycle

```
generate (offline) → record pubkey/fingerprint out-of-band
  → issue Tier 1 cert (offline, Tier 0 signs)
  → publish cert (/rotation) + bake into firmware (Tier 2)
  → operate (Tier 1 signs provisioning commands; audit everything)
  → rotate Tier 1 on schedule (§10) or on compromise (runbook §6)
  → Tier 1 cert expires → devices refuse new provisioning commands
       until rotated (fail-closed)
```

- **Tier 0** has no expiry; its lifetime is John's custody. If it is
  ever suspected compromised, follow the runbook §7 — every device needs
  a firmware update with a new baked Tier 0 pubkey.
- **Tier 1** certs expire (default 365 days; shorter in dev). Expiry is
  fail-closed: an expired Tier 1 cert means the device rejects
  provisioning commands until a fresh cert is rotated in.
- **Serials** increase monotonically per Tier 0 lineage. The device
  should reject a `rotate_tier1` whose serial is not greater than the
  currently stored one (downgrade protection within the rotation path).

## 9. Dev vs RC rotation policy (John's ruling)

**During dev: rotate Tier 1 every few v0.x.y firmware releases.**
The point is to exercise the rotation path while the cost of mistakes
is low — the ceremony, the `/rotation` publish, the device-side
`rotate_tier1` handling, and the audit trail should all be boring and
routine long before release candidates. Suggested cadence: rotate on
every 3rd–4th minor release, or whenever the Tier 2 header is
regenerated for other reasons — whichever comes first. Record each
rotation (old serial → new serial, date, operator) in the key log.

Release candidates get a stricter policy (longer-lived Tier 1, formal
change control) — to be defined when RC work starts. Do not invent it
now.

## 10. Firmware integration notes (for the firmware worker)

- Tier 2 = generated C header baked at build time: Tier 0 Ed25519
  pubkey (32 bytes), Tier 1 P-256 pubkey (65 bytes uncompressed),
  Tier 1 cert expiry (u64 unix). Generated only after
  `verify_tier1_cert.py` exits 0.
- On-device verification: Tier 0 Ed25519 via **Monocypher** (already in
  the seal component); Tier 1 ECDSA P-256 via **mbedTLS** (already in
  ESP-IDF). No new crypto libraries.
- Persisted rotation state (new Tier 1 pubkey, seen nonces) goes through
  the **`nvs_task` broker** — PSRAM-stack tasks must NEVER call
  NVS/flash directly (0.4.7/0.4.8 lesson, linker-enforced).
- Device clock: `ts_window=0`, no SNTP. The 15-min provisioning window
  absorbs skew; document the accepted skew bound in the firmware notes.
- DER vs raw `r||s`: the Lambda ships raw 64-byte `r||s`; decide in
  firmware whether to convert back or parse DER with mbedTLS directly,
  and document the choice.

## 11. File map

```
tools/pki/
  PKI_DESIGN_BRIEF.md    shared source of truth (normative)
  PKI_ARCHITECTURE.md    this file (why + how it fits)
  PKI_RUNBOOK.md         operational procedures
  pki_common.py          canonical bytes, timestamps, fingerprints (shared)
  gen_tier0.py           Tier 0 generation ceremony (offline)
  issue_tier1_cert.py    Tier 0 signs a Tier 1 cert (offline)
  verify_tier1_cert.py   build-pipeline verifier (exit 0/1)
  .gitignore             *_private*, tier0*, *.key, *.pem, *.secret
  infra/                 (infra worker: API Gateway + Lambda + KMS + DynamoDB)
```

Key material locations (never in the repo):
- Tier 0 private: `~/.config/watcher-pki/<name>_private.key` (0600, offline machine)
- Tier 0 public: `~/.config/watcher-pki/<name>_public.pub` (also recorded out-of-band)
- Tier 1 private: AWS KMS only (`kms:Sign`, never `kms:Get`)
- Tier 1 certs: published JSON (public by design — they carry pubkeys + a signature)
