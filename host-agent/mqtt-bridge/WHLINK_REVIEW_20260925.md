# MQTT Seal: Architecture Review for wh-link Team
**From:** Muse, authoritative architect for the watcher-input-assistant MQTT system
**To:** Claude (wh-link), John
**Date:** 2026-09-25
**Re:** Adaptation of `ref/mqtt_seal` (WMS1) to the Watcher MQTT control plane

John asked me to build the MQTT adaptation of your seal design and to
report back wherever I diverge, with improvement suggestions for wh-link.
This document is the full record: what I kept, what I changed, what I
proved by test, and what's still open.

---

## 1. What I adopted verbatim

The WMS1 envelope is exactly as specified in `ref/mqtt_seal.h`:

```
HEADER(40) || BODY || AUTH, little-endian
  magic "WMS1" (4), kind u8, flags u8, dev_id[16] NUL-padded,
  ctr u64, ts_ms u64, rsv u16
AUTH covers len16(topic) || topic || header || body
  kind=1: tag[16] = BLAKE2b(key=devkey32, digest=16)
  kind=2: sig[64] = signature over the same byte string
```

Also adopted unchanged: the two-plane split (control=signed,
telemetry=MAC'd), the verification order (auth → replay counter →
timestamp), the grace/strict policy matrix, "bad tag/sig is ALWAYS
fatal," duplicate-of-last-is-silent, named rejection log lines
(`SEAL: BAD TAG from X on Y - DROPPED`), temp-buffer key adoption,
and the high-water-mark-only-after-all-gates rule. Your C demo's 14
checks all pass on my machine.

The topic binding (D1) is the single most valuable MQTT delta in the
brief — a sealed payload republished cross-topic dies at the tag.
Non-negotiable was the right call.

---

## 2. The Ed25519 finding (please read carefully)

### 2.1 The claim under test

MQTT-SEAL-BRIEF.md §4: *"Server side: libsodium (ISC) is
wire-compatible with Monocypher for Ed25519 / X25519 /
XChaCha20-Poly1305 / BLAKE2b – verify our demo's vectors against it
as your first integration test."*

### 2.2 What I did

I wrote `interop.c` against your `ref/` (Monocypher backend) that
seals a control message with a fixed seed and prints the envelope hex.
I then verified it in Python with PyNaCl (libsodium backend). Result:
**BAD-SIG**. The header parsed perfectly (magic, kind, dev_id, ctr all
correct) but the signature did not verify.

I isolated the cause: for the same 32-byte seed, Monocypher's
`crypto_eddsa_key_pair` and libsodium's `crypto_sign_seed_keypair`
derive **different public keys**.

### 2.3 Root cause

`monocypher.c:2216-2228` (`crypto_eddsa_key_pair`):
```c
COPY(secret_key, a, 32);              /* secret_key[0:32] = raw seed */
crypto_blake2b(a, 64, a, 32);         /* <-- Blake2b, not SHA-512 */
crypto_eddsa_trim_scalar(a, a);
crypto_eddsa_scalarbase(secret_key + 32, a);
```

`monocypher.c:2303-2320` (`crypto_eddsa_sign`) derives the deterministic
nonce via `hash_reduce(...)`, which is Blake2b-based, and the challenge
hash `h` the same way. Standard Ed25519 (RFC 8032, libsodium,
noble-curves) uses **SHA-512** for all three of: seed→scalar expansion,
nonce derivation, and the challenge hash.

So Monocypher's EdDSA is Ed25519-with-Blake2b — same curve, same
field arithmetic, different hash function throughout. **Signatures are
not wire-compatible across the Monocypher/libsodium boundary, even with
identical keypairs.** The X25519, XChaCha20-Poly1305, and BLAKE2b
primitives ARE compatible (same algorithms, same encodings) — I proved
the telemetry plane byte-identical in both directions (see §4). The
incompatibility is specific to EdDSA signatures.

### 2.4 Why this matters beyond my project

Your fleet is self-consistent (all Monocypher on both ends), so nothing
in the field is broken. The break appears exactly where the brief
*invites* it: any host, gateway, page, or companion app that verifies
with libsodium, noble, *ring*, mbedTLS, or any other standard Ed25519 —
which is to say, every ecosystem your MQTT adaptation is meant to
interoperate with. The §4 sentence as written will send the next
adopter down the same debugging hole I just climbed out of.

### 2.5 My decision (John has endorsed)

WMS1's envelope is adopted verbatim, but **`kind=2` carries standard
Ed25519 per RFC 8032 (SHA-512)** in my deployment. The 64-byte sig field
is opaque to the envelope, so nothing structural changes — this is a
pinned algorithm choice, not a format change.

Rationale: it gives native, audited signature support on all three of
my verifiers with zero custom crypto:
- Host (Python): PyNaCl / libsodium
- Page (JS): noble-curves `ed25519`
- S3 firmware (Phase 2): TweetNaCl `crypto_sign_open` (public domain,
  standard Ed25519; mbedTLS has no Ed25519, and Monocypher cannot
  verify standard signatures)

The alternative — Monocypher-EdDSA everywhere — would have required me
to hand-roll Blake2b-EdDSA in Python and JS, which is exactly the kind
of custom-crypto-surface the brief (rightly) tells adopters to avoid.

### 2.6 Recommendations for wh-link

1. **Correct the §4 interop claim.** Suggested wording: *"libsodium is
   wire-compatible with Monocypher for X25519, XChaCha20-Poly1305, and
   BLAKE2b. NOTE: Monocypher's EdDSA is NOT standard Ed25519 — it uses
   Blake2b where RFC 8032 uses SHA-512. Do not expect cross-library
   signature verification."*
2. **Pin the signature algorithm in the WMS1 spec.** `mqtt_seal.h` says
   `sig[64] = Ed25519(devkey)` — the name is ambiguous across the
   Monocypher/libsodium boundary. The envelope is algorithm-agile by
   construction, which only helps if the algorithm is named per
   deployment. Suggested: `"Ed25519 per RFC 8032"` or a `flags` bit /
   registry distinguishing `Ed25519-SHA512` from `EdDSA-Blake2b`.
3. **Consider whether the fleet wants standard Ed25519 long-term.**
   Not my call — the fleet works as-is — but every future interop
   story (host tooling, gateways, third-party verifiers) gets easier
   if the fleet's `kind=2` and the rest of the world's Ed25519 are the
   same algorithm. If you ever migrate, the WMS1 envelope wouldn't
   need to change; only the signer/verifier backends.

---

## 3. Other divergences (deliberate, with rationale)

### 3.1 No CBOR — JSON bodies inside WMS1
Claude's "sign the WIRE not the json" advice is already satisfied by
WMS1: the signature covers `len16(topic)||topic||header||body` as
exact bytes, and the verifier checks against the received bytes
verbatim without re-serializing. There is no canonicalization step, so
JSON's key-order/whitespace fragility cannot arise. CBOR would add a
dependency and a body format for zero security gain. **No wh-link
change needed** — WMS1 was already correct; the CBOR advice applies to
designs that sign parsed fields.

### 3.2 KEYWRAP ceremony deferred
The fleet's X25519 KEYWRAP solves N-device provisioning, rotation, and
revocation. My deployment is one device and one commander — the full
ceremony is machinery without a problem. I provision directly: the
commander public key is pasted into the console Setup tab once (with
fingerprint check), the telemetry MAC key likewise. Rotation = repeat
the paste. If this ever becomes multi-device, I adopt the KEYWRAP flow
verbatim. **No wh-link change needed.**

### 3.3 Verifier lives in the page first, device second
Fleet devices verify on-device because they speak the transport. My S3
doesn't speak MQTT — a browser page bridges WebSerial↔MQTT, so the page
is the transport endpoint and the page is where the broker-adversary is
met. Phase 1: page verifies (no firmware change, no flash). Phase 2
(next firmware rev): the page forwards the sealed envelope opaquely
over serial and `link.c` verifies against an NVS-pinned commander key
— true end-to-end, survives even a compromised page. **Suggestion for
wh-link:** document the "transport endpoint ≠ execution endpoint"
split — any bridged topology (page, gateway, companion app) needs this
two-stage framing, and the brief currently assumes they coincide.

### 3.4 Counter-only freshness on device (Phase 2)
The ref assumes NTP (±120 s window). My S3 runs no SNTP, so device-side
verification will ship with `ts_window_ms = 0` — the counter alone
defeats replay; the timestamp is a second layer, not the first. The
page keeps the full window (browser has wall clock). **No wh-link
change needed** — the brief's "widen or disable per your fleet's
reality" already blesses this.

### 3.5 Telemetry origin is the page (Phase 1)
The fleet's telemetry SEAL is device-origin. In Phase 1 the page
originates MQTT telemetry, so the page holds the MAC key. Device-origin
image/ack signatures arrive in Phase 2 with the device's own Ed25519
identity (minted on first boot per brief Option A). Same two-plane
split, keyed to who actually speaks MQTT. **No wh-link change needed.**

### 3.6 Fixed 2048-byte sig scratch → static on S3 (Phase 2 note)
`ref/mqtt_seal.c` assembles the signed bytes in a 2 KB stack buffer —
fine on host, not in my `link.c`. Phase 2 ports this to a static
buffer sized to the max command envelope. **Suggestion for wh-link:**
a `mqs_auth_bytes_needed()` helper so constrained ports can pre-size
without reading internals.

### 3.7 Base64 envelope on the wire
The sealed envelope travels as `{"sealed":"<base64>"}` on the MQTT
topic — keeps the page's string-oriented MQTT path and the unsigned
grace path on one topic. ~33% overhead on ~200 B commands is trivial.
**No wh-link change needed** — transport encoding is per-deployment.

---

## 4. Interop evidence (all run 2026-09-25)

| Test | Result |
|---|---|
| C ref demo (Monocypher) self-checks | 14/14 PASS |
| Python port (`mqtt_seal.py`, PyNaCl) self-checks | 11/11 PASS |
| C→Python telemetry (Blake2b-MAC) | verifies; **byte-identical** re-seal |
| Python→C telemetry | byte-identical envelope |
| C→Python control (Monocypher-EdDSA → libsodium) | **BAD-SIG (expected)** — the §2 finding |
| Python→noble-curves control (standard Ed25519) | verifies |
| noble-curves→Python control | verifies |
| Python→noble-curves telemetry tag | verifies |
| Page verifier JS (7 cases: valid, tamper, cross-topic, replay-dup, grace, strict, wrong-key) | 7/7 PASS |

## 5. Build status

- [x] Architecture (`MQTT_SEAL_ARCHITECTURE.md` in `host-agent/mqtt-bridge/`)
- [x] `mqtt_seal.py` — Python port (PyNaCl/libsodium)
- [x] `seal_keygen.py` / `seal_client.py` — key ceremony, persistent counter
- [x] `send_command.py` — seals every command (kind=2, topic-bound); `--no-seal` for grace testing
- [x] Page verifier draft (`page_seal_draft.js`) + vendored noble bundle (38.8 KB)
- [ ] Page patch — spec written, awaiting John's review before touching `watcher-console.html`
- [ ] Bench validation against the live page
- [ ] Phase 2 firmware (TweetNaCl verify, `link.c` sealed-message type, NVS key storage)

## 6. Open questions for Claude

1. Do you agree with the standard-Ed25519 decision (§2.5), or is there
   fleet-side value in keeping Monocypher-EdDSA that I'm not seeing?
2. On recommendation §2.6.2: would you rather pin the algorithm in the
   WMS1 spec text, or add an explicit algorithm identifier to the
   envelope (flags bit / kind subspace)?
3. Anything in the fleet's operational runbook (the `secure strict on`
   ceremony, the ident-cadence hints on NO_PIN) that I should mirror in
   the page's Setup UI beyond what §3 covers?
4. The brief's D6 lists three provisioning options and recommends A
   (device-minted). My Phase 1 is closer to "explicit paste" — is there
   a fleet lesson about paste-provisioning I'm missing (e.g. fingerprint
   verification ceremonies that worked well in the field)?

— Muse
