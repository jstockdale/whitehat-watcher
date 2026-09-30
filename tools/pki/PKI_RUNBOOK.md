# Watcher PKI — Runbook

Operational procedures for the Watcher PKI. Architecture and rationale
live in `PKI_ARCHITECTURE.md`; the normative byte layouts in
`PKI_DESIGN_BRIEF.md`. This file is the hands-on-keyboard sequence for
each operation, in the order you will actually perform them.

Standing rules, repeated because they matter:
- **Ask John before any Tier 0 operation.** Tier 0 is his key; you are
  the operator, he is the authority.
- **Private key material is never printed, logged, pasted, or committed.**
  The scripts enforce this; don't work around them.
- Every procedure ends with an audit-log entry. If it isn't logged, it
  didn't happen.

Tooling: `tools/pki/gen_tier0.py`, `issue_tier1_cert.py`,
`verify_tier1_cert.py`, `pki_common.py`. All take `--help`.

---

## Procedure 1 — Tier 0 generation ceremony (one-time, offline)

**When:** once, at PKI stand-up. This is the root of everything.

1. **Prepare an offline machine.** Live-USB boot (e.g. Ubuntu) on a
   machine with Wi-Fi physically disabled is the recommended setup.
   No network, no cloud sync, no shared clipboard.
2. **Copy the tooling over.** `tools/pki/*.py` on a USB stick. Verify
   `python3 -c "import nacl; print('ok')"` (PyNaCl — same lib the seal
   uses). `cryptography` is needed for `issue_tier1_cert.py` later;
   install it on the offline machine now if it will ever issue certs.
3. **Run the ceremony:**
   ```
   ./gen_tier0.py --out-dir ~/.config/watcher-pki --name tier0
   ```
   The script refuses to overwrite an existing key (needs `--force`).
   The private key is written 0600 and **never printed**.
4. **Read the on-screen checklist and do every item:**
   - [ ] Air gap confirmed (this machine).
   - [ ] Back up `<name>_private.key` to **two encrypted offline media**
     (LUKS USB sticks), stored in **separate physical locations**.
   - [ ] Private key never in AWS, never in the repo, never in chat/logs.
   - [ ] Record the **pubkey hex + fingerprint** out-of-band (paper or
     the offline safe). This is what the build pipeline and auditors
     pin to.
5. **Confirm the fingerprint out-of-band with John** before any cert is
   issued or any firmware is baked. The build pipeline prints the Tier 0
   fingerprint on every run — it must match this recorded value.
6. **Shut the machine down** (or keep it as the dedicated offline
   signer). Do not copy the private key to a networked machine "just
   for convenience" — that ends the air gap.

Expected output: `tier0_private.key` (0600), `tier0_public.pub`, and the
printed pubkey hex + `fingerprint: <16 hex chars>`.

---

## Procedure 2 — Tier 1 KMS key (AWS side, infra worker's lane)

Operator-side summary (the infra worker owns the Terraform/CloudFormation):

1. Create the KMS key: key spec `ECC_NIST_P256`, key usage
   `SIGN_VERIFY`. Alias it, e.g. `alias/watcher-tier1-provisioning`.
2. Extract the public half (uncompressed point, hex):
   ```
   aws kms get-public-key --key-id alias/watcher-tier1-provisioning \
       --query PublicKey --output text | base64 -d > /tmp/t1.der
   python3 -c "
   from cryptography.hazmat.primitives import serialization
   from cryptography.hazmat.primitives.asymmetric import ec
   der = open('/tmp/t1.der','rb').read()
   k = serialization.load_der_public_key(der)
   assert isinstance(k, ec.EllipticCurvePublicKey) and isinstance(k.curve, ec.SECP256R1)
   print(k.public_bytes(serialization.Encoding.X962,
                        serialization.PublicFormat.UncompressedPoint).hex())"
   ```
   Expect 130 hex chars starting with `04` (65 bytes).
3. IAM for the signing Lambda: allow `kms:Sign` on this key **only**.
   Explicitly do NOT grant `kms:GetPublicKey` beyond what the build
   pipeline needs, and never `kms:Get*` on private material (KMS won't
   release it anyway — the point is least privilege in the policy).
4. Hand the 65-byte pubkey hex to the Tier 0 operator (John) over a
   trusted channel. The pubkey is public — the channel just needs
   integrity, not secrecy.

---

## Procedure 3 — Issuing a Tier 1 cert (offline, Tier 0 operator)

**When:** initial stand-up, every scheduled rotation (§10 of the
architecture doc: every few v0.x.y in dev), emergency rotation.

1. On the **offline** Tier 0 machine, take the Tier 1 P-256 pubkey hex
   from Procedure 2.
2. Choose the next serial (u32, **must be greater than the previous
   cert's serial**) and the lifetime:
   ```
   ./issue_tier1_cert.py \
       --tier0-key ~/.config/watcher-pki/tier0_private.key \
       --p256-pubkey-hex 04a1b2... \
       --serial 2 \
       --expires-days 90 \
       --out ./tier1-s2.tier1cert.json
   ```
   The script validates the P-256 point is on the curve **before**
   signing (Tier 0 never signs a bogus key), builds the exact cert JSON
   from the brief, signs the canonical bytes, and prints the cert plus
   both fingerprints.
3. **Sanity-check the output:** `format` is `watcher-tier1-cert-v1`,
   `subject` is `watcher-tier1-provisioning`, `issued_at`/`expires_at`
   look right, serial incremented.
4. **Verify with the independent checker** (same machine is fine — the
   point is a second code path through the canonical bytes):
   ```
   ./verify_tier1_cert.py --cert ./tier1-s2.tier1cert.json \
       --tier0-pubkey-file ~/.config/watcher-pki/tier0_public.pub
   ```
   Expect `RESULT: VALID`, exit 0. If it isn't VALID, **stop** — do not
   publish.
5. Move the cert JSON (public by design) to a networked machine for
   publication. The Tier 0 key stays offline. **Airgap transfer via QR
   code** (no retyping hex, no photo OCR):
   ```bash
   # Option A: generate the QR at issue time
   ./issue_tier1_cert.py --tier0-key ~/.config/watcher-pki/tier0_private.key \
       --p256-pubkey-hex <hex> --serial 2 --expires-days 90 \
       --qr ./tier1-s2.svg

   # Option B: QR from an already-issued cert file
   ./cert_to_qr.py --cert ./tier1-s2.tier1cert.json
   ```
   Display the image on the offline machine, scan it with your phone,
   paste the text on the networked side. No pip install needed — the
   `qrcode` package is vendored under `vendor/` (pure Python).
   SVG is the default format (no Pillow required); use `--format png`
   if you prefer PNG and have Pillow.

---

## Procedure 4 — Publishing the cert

1. **Publish to the infra:** `POST /rotation` with `{ "tier1_cert": <cert JSON> }`.
   The infra worker validates the Tier 0 signature server-side, updates
   the Tier 1 key alias, and writes the audit entry. Confirm the
   response carries the new cert `serial` / `key_id`.
2. **Point the firmware build pipeline at the cert.** The pipeline runs:
   ```
   ./verify_tier1_cert.py --cert <new cert> --tier0-pubkey-hex <recorded Tier 0 pubkey>
   ```
   and only on exit 0 bakes Tier 0 pubkey + Tier 1 pubkey + expiry into
   the Tier 2 generated header. The pipeline prints the Tier 0
   fingerprint — **confirm it matches the out-of-band record** before
   flashing any device.
3. **Record in the key log:** date, operator, old serial → new serial,
   expiry, Tier 1 fingerprint, reason (scheduled/emergency).

---

## Procedure 5 — Tier 1 rotation, normal (healthy Tier 1)

This is the routine case: the old Tier 1 key is fine, you're rotating on
schedule or because the cert is nearing expiry.

1. Create the new KMS key (Procedure 2), new alias target.
2. Issue the new cert offline (Procedure 3) with serial = old + 1.
3. Publish (Procedure 4). From this point the Lambda signs with the new
   key; `GET /tier1/pubkey` returns the new cert.
4. **Roll the fleet forward:** send each device a `rotate_tier1`
   provisioning command (signed by the **new** Tier 1 key, or by the old
   one during a grace window — pick one policy and document it):
   `{ "p256_pubkey_hex": "<new>", "expires_at": "<iso>" }`.
   The device verifies the signature against its stored Tier 1 pubkey,
   checks the serial increased, persists via `nvs_task`, and acks.
5. Confirm the fleet: each device's next heartbeat/status should show
   the new Tier 1 fingerprint. Chase stragglers before the old cert
   expires.
6. Retire the old KMS key (disable, then schedule deletion after the
   fleet is confirmed — KMS enforces a 7–30 day waiting period; use it).
7. Audit-log review (§8) to confirm the rotation is the only signing
   activity in the window.

---

## Procedure 6 — Tier 1 rotation, emergency (Tier 1 compromised)

**Assume:** the Tier 1 private key (KMS) is compromised or suspected
compromised. The attacker can sign provisioning commands until the
rotation lands. Speed matters, but the order below matters more.

1. **Do NOT use the old Tier 1 key for anything.** Disable the KMS key
   immediately (`aws kms disable-key`). This stops new forged commands.
2. Create a fresh KMS key (Procedure 2).
3. Issue the new cert offline with Tier 0 (Procedure 3), serial = old + 1.
   (If you cannot reach the offline machine, the fleet stays on the
   disabled key — fail-closed — until you can. Do not mint a cert by any
   other path.)
4. Publish via `POST /rotation` (Procedure 4).
5. **Break-glass device update:** the compromised Tier 1 cannot be
   trusted to deliver its own rotation, so devices take the Tier 0-signed
   path — the `rotate_tier1` payload verified with Ed25519 against the
   Tier 2-baked Tier 0 pubkey (Monocypher), bypassing Tier 1 entirely.
   Push it over whatever channel reaches the devices (MQTT sealed
   command, serial bench run, next firmware flash — the signature, not
   the transport, is the trust).
6. Verify the fleet picked up the new Tier 1 fingerprint (§5 step 5).
7. **Audit forensics:** pull `GET /audit` for the whole compromise
   window. For every `signed` entry, check the command canonical hash
   against operator intent — look for `cmd` values or `device: "*"`
   commands nobody requested. Record findings in the incident log.
8. Post-mortem: how was Tier 1 compromised, what bounded the blast
   radius (cert expiry, nonce replay window, scope rule), what changes.

Why this works when Tier 1 is hostile: the Tier 0 pubkey is baked into
firmware (Tier 2), so the rotation trust path does not depend on the
compromised key.

---

## Procedure 7 — Tier 0 compromise (catastrophic by design)

There is no in-band recovery from a Tier 0 compromise: whoever holds
Tier 0 can mint Tier 1 certs and sign break-glass rotations. The
mitigation was custody (Procedure 1); this procedure is damage control.

1. **Confirm the compromise** (don't rotate a root on suspicion alone —
   but don't dither on evidence either; John decides).
2. **Generate a new Tier 0** (Procedure 1) on a fresh offline machine.
   New pubkey, new fingerprint, new lineage. Serials restart at 1 under
   the new root — record the lineage break in the key log.
3. **Every device needs a firmware update** baking the new Tier 0
   pubkey (Tier 2). There is no remote path that the old Tier 0 can't
   also use — that's what "catastrophic by design" means. Bench/OTA the
   fleet with the new firmware.
4. Issue a fresh Tier 1 cert under the new root (Procedures 2–4).
5. **Treat all audit history under the old root as tainted.** Forensic
   review as in §6 step 7, across the full window of suspected
   compromise.
6. Post-mortem: how custody failed, what changes (this is the procedure
   that should never run — if it runs, the ceremony in Procedure 1
   needs redesign, not repetition).

---

## Procedure 8 — Audit-log review

**Cadence:** weekly in dev, plus after every rotation and every incident.

1. `GET /audit?since=<last review>` — page through the full result set.
2. For each entry check:
   - `result: signed` — was this command authorized? Match the command
     canonical hash and `cmd`/`device` against operator intent for the
     window (deploy log, chat history, key log).
   - `result: rejected` — is the reason understood? A spike in
     rejections is a signal (misbehaving client or probing attacker).
   - `cert serial` / `key_id` — does it match the currently active
     Tier 1? Entries under an unexpected serial are a red flag.
   - `IAM principal` — is it the expected operator/Lambda role?
3. Cross-check against the key log: every serial bump in the audit log
   must have a matching key-log entry (date, operator, reason).
4. Record the review: reviewer, window covered, entries checked,
   anomalies (or "none"). File it with the key log.

The audit log stores the **SHA-256 of the canonical command**, never the
command itself — params may reference secrets (seed fingerprints are
fine; seeds never appear). If you need to match an entry to an intent,
recompute the canonical hash from the original command JSON (§3d of the
architecture doc).

---

## Troubleshooting / FAQ

- **`verify_tier1_cert.py` says INVALID / signature does not verify.**
  Usual causes: wrong Tier 0 pubkey (check the fingerprint against the
  out-of-band record), or the cert was edited after signing (even
  reformatting JSON whitespace is fine — but changing any *value*
  invalidates the signature; re-issue, don't hand-edit). Exit 1 =
  verification failure, exit 2 = usage/I/O error.
- **`issue_tier1_cert.py`: "invalid P-256 public key".** The hex isn't a
  point on the curve. Common cause: copying the DER/PEM from KMS instead
  of the raw uncompressed point — extract per Procedure 2 and retry.
- **Device rejects a provisioning command with a valid signature.**
  Check the clock window (device has no SNTP; `ts_window=0` — the 15-min
  command window absorbs skew, but a device that's been shelved for
  months may be outside it), the nonce cache (replay), and the `device`
  field (`*` is honored only while unkeyed).
- **Lost the Tier 0 private key, backups intact.** Restore from an
  encrypted backup onto the offline machine and re-verify the
  fingerprint matches the out-of-band record before use.
- **Lost ALL copies of the Tier 0 private key.** That key is gone
  forever — there is no recovery. Follow Procedure 7 as if compromised
  (new root, fleet firmware update), because the old root is now
  permanently unusable rather than hostile. Then fix the backup
  procedure.
- **Where do Tier 1 certs live?** They're public: the repo may hold
  *published* certs (they carry pubkeys + a signature, no secrets), but
  the build pipeline should fetch from `GET /tier1/pubkey` or a pinned
  copy — never trust a cert file that hasn't passed
  `verify_tier1_cert.py` against the recorded Tier 0 pubkey.
