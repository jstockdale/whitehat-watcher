# MQTT Seal Architecture — watcher-input-assistant
**Author:** Muse (authoritative architect for the MQTT system)
**Date:** 2026-09-25
**Status:** Design — Phase 1 build in progress
**Inherits from:** WHM fleet SEAL + `ref/mqtt_seal` v1 ("WMS1") from whm-mqtt-seal-handover-r1 (MIT)

---

## 0. The one structural fact that shapes everything

Our device does not speak MQTT. The trust topology is:

```
[Muse host] --MQTT/TLS--> [broker] <--MQTT/TLS--> [console page] --USB serial--> [Watcher S3]
      |                        ^                         |                            |
   signer                  UNTRUSTED                   verifier                    trusts page
  (Phase 1)               (adversary)                (Phase 1)                  (physical USB)
```

The fleet verifies on-device because fleet devices talk UDP directly.
We cannot do that in Phase 1 — the page is our MQTT endpoint. So:

- **Phase 1 (now):** the page verifies. The broker is the adversary;
  the page is John's trusted agent (it already gates via allowlist +
  the command-channel toggle + John's physical presence).
- **Phase 2 (next firmware rev):** the device verifies end-to-end.
  The page forwards the sealed envelope opaquely over serial; `link.c`
  parses WMS1, verifies against an NVS-pinned commander key, then
  dispatches. This survives even a compromised page (it can drop
  commands, never forge them).

Phase 1 needs no flash. Phase 2 rides the next firmware rev anyway.

---

## 1. Threat model

- **Primary:** attacker with broker access (leaked credential, broker
  compromise, malicious broker admin) publishes to `watcher/command`
  and drives John's cursor / types keys / clicks. **Defeated by
  Phase 1.**
- **Secondary:** attacker replays a captured command (retained
  message, QoS redelivery, malicious re-publish). **Defeated by
  per-commander monotonic counter + timestamp window.**
- **Tertiary:** attacker cross-posts a valid sealed payload to a
  different topic (e.g. command bytes onto a telemetry topic a
  future consumer acts on). **Defeated by topic binding (D1).**
- **Quaternary (Phase 2):** compromised page forges commands.
  **Defeated only by device-side verification.**
- **Out of scope:** attacker with John's laptop (has the private
  key anyway); attacker with physical USB (talks to the device
  directly); TLS failures (belt, not boundary — every guarantee
  below holds with a fully hostile broker, per the brief).

### 1b. Broker ACL (authoritative, 2026-09-25)

Verbatim copy: `mosquitto-acl-20260925.acl`. Two users,
least-privilege:

- **`muse`** (bridge subscriber + Muse agent): read
  `watcher/telemetry`, `watcher/status`, `watcher/audio`;
  readwrite `watcher/bridge`, `watcher/muse`,
  `watcher/muse_pause`, `watcher/muse_queue`; write
  `watcher/command`; read `watcher/keepalive`, `watcher/jiggle`.
- **`watcher`** (console page): write `watcher/telemetry`,
  `watcher/status`; readwrite `watcher/bridge`,
  `watcher/keepalive`, `watcher/jiggle`, `watcher/muse_pause`,
  `watcher/muse_queue`; read `watcher/command`, `watcher/muse`.

The ACL answers *who may speak on which topic*. The seal answers
two questions the ACL cannot: *which key spoke* (authorship
within an allowed topic — anyone holding the `muse` credential
can write `watcher/command`) and *what the broker sees*
(payload encryption for telemetry). They compose; neither
replaces the other.

Audit notes (2026-09-25): (1) the page publishes to
`watcher/heartbeat` and `watcher/console_heartbeat`, which have
**no ACL entry** — mosquitto default-denies unmatched topics, so
both are silently dropped; add write entries for the `watcher`
user or stop publishing. (2) Nobody has write on
`watcher/audio` — the page batches audio chunks into
`watcher/telemetry` today; the dedicated topic is aspirational.
(3) `muse` is read-only on `watcher/jiggle`/`watcher/keepalive`:
remote jiggler/keepalive control must go through sealed
`watcher/command`, never direct topic writes.

---

## 2. Wire format — adopt WMS1 verbatim

Envelope = `HEADER(40) || BODY || AUTH`, little-endian:

```
magic  "WMS1"          4
kind   u8              1   1=telemetry(MAC) 2=control(Ed25519)
flags  u8              1   bit0 reserved ENC (S5)
dev_id char[16]       16   NUL-padded sender identity
ctr    u64             8   strictly monotonic per (dev_id)
ts_ms  u64             8   sender unix-ms
rsv    u16             2
```

AUTH covers exactly `len16(topic) || topic || header || body`:

- kind=1: `tag[16] = BLAKE2b-keyed(key=devkey32, msg=...)`
- kind=2: `sig[64] = Ed25519(devkey) over the same byte string`

**Body stays JSON.** See S7 divergence #1.

**Transport encoding (new decision):** the envelope goes on the MQTT
topic as **base64 inside a JSON wrapper**: `{"sealed":"<b64>"}`.
Rationale: our page's MQTT path is string-oriented (`JSON.parse`
on the command topic); base64 keeps that plumbing untouched, and
grace-mode unsigned `{"id","cmd"}` messages keep working on the same
topic during bring-up. Overhead ~33% on ~200 B commands — trivial.

The signed BODY is the existing command object **including the id**:
`{"id":"<uuid>","cmd":{...}}`. The ack/echo id is now authenticated
too — a forged ack can't be confused with a real one.

---

## 3. The two planes, mapped to our system

| Plane   | Direction        | Primitive (Phase 1)              | Primitive (Phase 2)              |
|---------|------------------|----------------------------------|----------------------------------|
| Control | host → device    | Ed25519 signed, **page verifies** | Ed25519 signed, **device verifies** |
| Control | page → device    | — (serial trust)                 | sealed envelope forwarded opaquely |
| Telemetry | page → host    | BLAKE2b-MAC, page seals, host verifies | same (page remains MQTT endpoint) |
| Telemetry | device → page | — (serial trust)                 | device Ed25519-signs image/ack hashes; page verifies, re-seals for MQTT |

Topics bound into auth: `watcher/command` (kind=2),
`watcher/telemetry` (kind=1). Control topics are **never retained**
(D4). `watcher/status` stays as-is (already has its own semantics;
revisit if it ever drives actions).

**Identities:** `dev_id` = `"watcher-01"` (the device / its page
proxy). Commander id = `"muse-01"`. Both fit the 16-char field.

---

## 4. Verification policy — adopt the fleet matrix verbatim

Order: magic/kind/len → tag/sig (**fatal on mismatch, always**) →
ctr vs per-commander high-water (equal-to-last = silent dup;
less = REPLAY, loud) → `|now - ts_ms| ≤ 120 s` (STALE if exceeded).

| received            | key held | grace (ship state)        | strict (one flag flip)      |
|---------------------|----------|---------------------------|-----------------------------|
| unsigned JSON       | any      | ACCEPT, warn 1-in-64      | DROP + "SEAL: UNSEALED … DROPPED" |
| sealed, tag/sig ok  | yes      | ACCEPT                    | ACCEPT                      |
| sealed, tag/sig bad | yes      | DROP + "SEAL: BAD … DROPPED" | DROP (same)              |
| sealed              | no key   | ACCEPT (provision window) | DROP                        |

- **Grace is loud, never silent.** Rate-limited named log lines go
  to the page event log AND are published on `watcher/telemetry`
  as seal events (so I see them from the host side).
- **Strict flips** via a Setup-tab toggle, persisted in localStorage
  (Phase 1) / NVS (Phase 2). One switch, whole plane.
- **Replay state:** page keeps `high_water` per commander in
  localStorage, adopted only after ALL gates pass (never advance on
  a failed gate — the demo's "failed gates never advance the
  high-water mark" test is our regression test).
- **Counter recovery:** if the host loses its counter file it will
  replay from 1 and be rejected as REPLAY. Recovery = John resets
  the page's high-water in Setup (explicit button, confirm dialog).
  Documented, not automatic.

---

## 5. Key provisioning — deliberate simplification for 1:1

The fleet's X25519 KEYWRAP ceremony exists for N devices joining,
leaving, and rotating. We have **one device and one commander**.
Adopting the full ceremony now would be machinery without a problem.

**Phase 1 ceremony (new decision, see S7 #2):**
1. Generate one Ed25519 keypair for the commander (`muse-01`).
   Private key → Secure Vault (host-side, never in files/chat).
2. John pastes the public key (hex + fingerprint) into the console
   Setup tab once; page pins it in localStorage. TOFU is NOT used —
   explicit paste, no first-contact race.
3. Telemetry MAC key: 32 random bytes, generated alongside; one copy
   to the page (Setup paste), one to the host env/Secure Vault.
   Page seals telemetry; host verifies.
4. Rotation = repeat the paste. Revocation = rotate (1:1, no fleet
   to sweep).

**Phase 2 adds:** device mints its own Ed25519 identity on first boot
(brief Option A, `crypto_id.c` as template — noting its GPL header:
reference-only until John relicenses; the MIT `ref/` is our base).
Commander public key provisioned to device NVS over serial during a
bench session (trusted physical channel). Device public key
registered with the host for telemetry-signature verification.

---

## 6. Page integration (Phase 1)

In `mqpOnPublish`, on the command topic, after `JSON.parse`:

1. If `msg.sealed` present → base64-decode → `mqs_verify` against
   the pinned commander pk, topic `watcher/command`, replay ctx
   from localStorage, policy from the strict flag.
   - `MQS_OK` → extract `{id, cmd}` from verified body → continue
     down the existing allowlist path (`cmd._mid = id` as today).
   - `MQS_OK_DUP` → silently ack as already-delivered.
   - Anything else → drop + named log + seal event on telemetry.
2. Else (unsigned) → grace: loud warn 1-in-64, continue as today;
   strict: drop loudly.

New Setup-tab section: "Command authentication" — public-key paste
+ fingerprint display, grace/strict toggle, replay high-water
display + reset, seal event counter. A seal-status dot next to the
MUSE pill (green = last command verified, amber = grace, red =
rejection).

**The page needs an Ed25519/BLAKE2b implementation in JS.**
Options: (a) noble-curves + noble-hashes (audited, small, MIT) —
recommended; (b) WebCrypto (has Ed25519 in modern browsers, but
Safari support is recent — noble avoids the compat question).
BLAKE2b-keyed is in noble-hashes. This is a page dependency
decision, not a protocol decision.

---

## 7. Divergences from wh-link / fleet — reported back

### D1b. SIGNATURE ALGORITHM: standard Ed25519 (RFC 8032), NOT Monocypher-EdDSA
**Finding (2026-09-25, verified by test):** the brief's §4 claim —
"libsodium is wire-compatible with Monocypher for Ed25519 … verify
our demo's vectors against it" — is **incorrect for signatures**.
Monocypher's `crypto_eddsa_*` uses **Blake2b** for seed→scalar
derivation, nonce derivation, and the challenge hash
(`monocypher.c:2216-2228,2303-2320`); standard Ed25519 (libsodium,
noble-curves, RFC 8032) uses **SHA-512**. A Monocypher signature
does NOT verify under libsodium/noble even with the same keypair
(proven: `EDPK d4f8…` vector, BAD-SIG under PyNaCl). The fleet is
self-consistent (all-Monocypher) so it is unaffected — the break
appears exactly at the ecosystem boundary the MQTT adaptation
crosses.

**Decision:** The WMS1 envelope *format* is adopted verbatim, but it
travels under the **`WMS2` magic**, with `kind=2` carrying **standard
Ed25519 (RFC 8032, SHA-512)**. Rationale (agreed with Claude,
2026-09-25): the magic is the algorithm registry — one deployment,
one algorithm, pinned by four bytes. `WMS1`-as-deployed means
Monocypher EdDSA-BLAKE2b; our signatures are a different algorithm,
so they get a different magic rather than an in-band selector (the
JOSE-`alg`-matrix problem). The 64-byte sig field is opaque to the
envelope, so nothing structural changes.
This gives native support on all three of our verifiers with zero
custom crypto: PyNaCl (host), noble-curves (page), and — Phase 2 —
upstream Monocypher's optional `monocypher-ed25519.{c,h}` module
(`crypto_ed25519_check`), which implements RFC 8032 with its own
SHA-512 under the same CC0/BSD-2 license. **Correction 2026-09-25
(Claude):** the earlier draft claimed "Monocypher can't verify
standard signatures" — true of the vendored core, false of upstream.
`crypto_ed25519_check` verifies detached 64-byte signatures directly
(matching the envelope; TweetNaCl's `crypto_sign_open` wants the
`sig||msg` concatenation and a shim), is substantially faster than
TweetNaCl on the S3 (TweetNaCl verify is unoptimized — expect
hundreds of ms per verify at 240 MHz; M0 will measure both), and
keeps one crypto family on the device. TweetNaCl is demoted to
fallback. The `kind=1` Blake2b-MAC path IS
wire-compatible with the C ref (proven byte-identical both
directions) and is adopted unchanged.

**Interop proven 2026-09-25:**
- Python(PyNaCl) → noble-curves: verify true (sig + tag)
- noble-curves → Python(PyNaCl): verify true
- Python → C ref (Monocypher): byte-identical telemetry envelopes
- C ref → Python: telemetry verifies; control does NOT (expected —
  algorithm divergence, not a bug)

**Recommendation to wh-link:** (1) correct the §4 interop claim —
it holds for X25519 / XChaCha20-Poly1305 / BLAKE2b, not for EdDSA;
(2) the WMS1 spec should name the signature algorithm explicitly
("Ed25519 per RFC 8032") instead of "Ed25519(devkey)" — the name is
ambiguous across the Monocypher/libsodium boundary, and the
envelope's algorithm-agility only helps if the algorithm is pinned
per deployment.
### D1c. No CBOR — JSON bodies inside WMS1 (follow the pattern)
Claude's "sign the WIRE not the json" is already satisfied: WMS1
signs `len16(topic)||topic||header||body` as **exact bytes**. The
verifier checks the signature against the received bytes verbatim
and never re-serializes — there is no canonicalization step, so
JSON's key-order/whitespace issues cannot arise. CBOR would add a
dependency and a new body format for zero security gain.
**Recommendation to wh-link:** none — WMS1 is already correct here;
the CBOR advice applies to designs that sign *parsed fields*.

### D2. KEYWRAP ceremony deferred (deliberate simplification)
Fleet KEYWRAP solves N-device provisioning/rotation. At 1:1 it's
pure overhead; direct provisioning over John's existing trusted
channel (his browser, his machine) is simpler and no weaker.
**Recommendation to wh-link:** none — the ceremony stays correct
for fleets; our deployment descriptor just says "size 1".

### D3. Verifier lives in the page first (architectural adaptation)
Fleet devices verify on-device because they speak the transport.
Our S3 doesn't speak MQTT — the page is the transport endpoint, so
the page is where the broker-adversary is met. Device-side verify
(Phase 2) restores the fleet topology where it matters: the serial
hop is physical-USB trust today, and Phase 2 removes even that.
**Recommendation to wh-link:** consider documenting the
"transport endpoint ≠ execution endpoint" split — any bridged
topology (page, gateway, companion app) needs this two-stage
framing.

### D4. Counter-only freshness on device (Phase 2)
The ref assumes NTP (±120 s window). Our S3 runs no SNTP today.
Device-side verification will ship with `ts_window_ms = 0`
(counter-primary only — the counter alone defeats replay; the
timestamp is a second layer, not the first). Page-side keeps the
full window (browser has wall clock). If/when SNTP lands on the S3,
flip the window on.

**Correction 2026-09-25 (Claude — the replay hole):** the earlier
draft paired a long-lived pinned commander key with `ts_window=0`
and said nothing about counter persistence. The ref's replay state
is RAM-resident and zero-initialized: *zero-init = accept any first
`ctr >= 1`*. After any device reboot, every sealed command the
broker ever recorded verifies and executes again under the
still-valid pinned key. Sender-side counter persistence (we have
it in `seal_client.py`) stops the commander reusing counters; it
does nothing for a verifier that forgot. **Phase 2 therefore
requires device-side NVS high-water persistence, wear-aware —
with the zero-budget variant (Claude's correction 3, 2026-09-25):**
persist every Nth accept; on boot, adopt `floor = stored + N`.
The persist invariant guarantees `executed ≤ stored + N − 1` at
all times, so the floor clears everything already executed —
**replay budget after crash: zero**. (The ≤N-budget variant was
defensible for a mouse-only commander; with `hid_type` in the
command set, the crash branch would replay typed text against
whatever host has focus. Rejected.) The post-boot floor is
surfaced in status, and the commander consumes it:
`next = max(own_persisted_counter, floor) + 1` — gaps are legal;
the law is strictly monotonic, not contiguous. Without the resync
the commander would eat up to N−1 loud REPLAY drops (plus
lockout-counter pollution) walking past the floor on its own;
with it, zero drops. Anti-oracle: `max()` with the commander's own
persisted counter makes a low fake floor a no-op; a sanity cap
(reject `floor > own + 2^20`, or trust the floor only from
authenticated status) stops a hostile broker burning counter
space toward exhaustion. Same flash stride as the budget variant,
no wire change, one status field consumed. Counter NVS writes ride
the single flash-writer task (crypto_id 0.55.1 lesson); when Phase
3 sessions land, gate or delete the NVS counter writes rather than
stranding dead periodic flash writes. Exact N set at
implementation. The alternative — a boot-time nonce handshake
forcing a fresh counter epoch — is session establishment by
another name, and is where the Phase 3 session-cert design
(below) already goes.

**Accepted residual (write it down):** a counter defeats replay of
*old* messages, not delay-and-release of the *newest unseen* one.
With no device clock and window 0, the broker can hold the latest
command and deliver it minutes later. Acceptable for bench/recon
deployments; not mistaken for covered.

**Recommendation to wh-link:** the brief's "widen or disable per
your fleet's reality" already blesses `ts_window=0` — no change
needed. But the ref's zero-init comment deserves a louder warning:
any deployment combining a long-lived key with window 0 MUST
persist the high-water mark, or say so.

### D5. Telemetry origin is the page (Phase 1)
The fleet's telemetry SEAL is device-origin. In Phase 1 the page
originates MQTT telemetry, so the page holds the MAC key. Device-
origin image/ack signatures arrive in Phase 2 with the device's
own Ed25519 identity. Same two-plane split, keyed to who actually
speaks MQTT.
**Recommendation to wh-link:** none — plane split preserved.

### D5b. ENC on frame/audio-bearing telemetry (Claude round-4 correction, accepted; SHIPPED 2026-09-25)
D5's page-origin kind-1 MAC is **authenticity, not privacy**: it
stops the broker tampering with or forging frames (kills
steering-by-fake-pixels) but does nothing to stop the broker
*reading* every pixel — 5,831 chunk events have already transited
the broker in plaintext. The privacy fix is the ENC flag, not the
MAC: frame-bearing and audio-bearing telemetry events set ENC
(kind 1 with XChaCha20-Poly1305 over the chunk batch as published
at the page; noble provides XChaCha). Pure status may stay
MAC-only. Native-mode Phase 2 publishes frames as mandatory-ENC
`TEL_FRAME` (unchanged). A room microphone outranks a screen in
sensitivity — audio policy is byte-identical to frame policy.
Key provisioning without a second ceremony: one pasted 32-byte
telemetry seed; `k_tel_mac` and `k_tel_aead` derived with the
labeled-KDF form from distinct labels, per device. The seed
retires when Phase 3 sessions derive telemetry keys ephemerally.

**As built (console 0.4.56, 2026-09-25; corrected from 0.4.55).** Wire: WMS2 kind-1 ENC,
`body = nonce[24] || XChaCha20-Poly1305(key=k_tel_aead, nonce,
plaintext)` (no separate AD); the outer 16-byte BLAKE2b MAC is
retained over `len16(topic)||topic||header||enc_body`, so topic
binding and ciphertext-tamper detection survive alongside AEAD.
KDF labels pinned: `WMS2-tel-mac-v1`, `WMS2-tel-aead-v1`,
`WMS2-tel-fp-v1`. Page seals in `queueTelemetry()` BEFORE the
4000-char truncation; sealed lines are never truncated. **Fail-closed:**
no seed / no crypto / seal error -> the whole media line is dropped
(a rate-limited `tel_enc_drop` line carrying no media bytes goes in
its place; the Setup card counts "Dropped media lines"). There is no
plaintext path — `maybeSealRaw` returns `{drop:true}`, never
`{plain:true}`.

> **Correction (2026-09-25 evening):** console 0.4.55 shipped with a
> plaintext fallback — its no-seed/no-crypto path enqueued media
> lines unsealed with a `tel_enc_warn` notice. The "fail-closed"
> claim in the 0.4.55 delivery was wrong. 0.4.56 removes the
> fallback: unsealable media lines are dropped whole, proven by
> `test_page_failclosed.js` (no seed -> no media bytes on the wire,
> drop counter increments, warn carries no media; AEAD bundle
> removed -> still dropped). Page emits `{"t":"sealed_chunk","enc":b64}` on
`watcher/telemetry`; the host subscriber unwraps with the same
seed (config file `~/.config/watcher/tel_seed` or
`WATCHER_TEL_SEED`), verifies MAC/topic/freshness/replay/AEAD,
and hands plaintext to the existing handlers — never logging seed
or media. Seed provisioning is a Setup-card paste (64 hex chars,
password field, localStorage per-origin, never redisplayed, only
the 16-hex fingerprint shown). 17 committed kind-1 ENC vectors
under `vectors/WMS2/kind1enc/`; JS/Python cross-language
round-trip proven, including a full page-DOM E2E (fake serial
Watcher + fake broker: sealed PUBLISH on the wire, zero plaintext
bytes, Python host unwraps the captured envelope).

**Implementation scar (recorded so it isn't repeated).** The TEL
module was first inserted inside `mqpOnPublish()` — the patch
anchor matched the seal-gate comment *inside* the message
handler, not the top-level one — making `var TEL`
function-scoped and invisible to the seed UI and
`queueTelemetry`. The UI then silently reported "telemetry seal
module not loaded" while everything else looked green. Fixed by
moving the module to top level (after the SEAL module); caught by
a DOM smoke test that drives the real Setup UI, now a permanent
regression test. Lesson: patch anchors must be unique; verify
module scope, not just syntax, after every page patch.

**Recommendation to wh-link:** §8's "broker never holds plaintext
screenshots" now has the bridged-mode form — ENC at the page, not
just MAC.

### D5c. Audio topic: latent grant, decided planned-with-ENC
The ACL grants `muse` read on `watcher/audio` and no one write —
latent by default-deny. Decision: **planned**, not vestigial. It
ships ENC-mandatory from its first byte, ARMED-gated, policy
identical to `TEL_FRAME` (`TEL_AUDIO`: ENC REQUIRED, session-only,
QoS 0, never retained). Until the console routes `t:audio` there,
audio chunks keep riding inside `watcher/telemetry` batches under
the same D5b ENC rule.
**Recommendation to wh-link:** reserve `TEL_AUDIO` with
`TEL_FRAME`-identical policy (Claude's spec update already does).

### D5d. Truncation invariant (Claude round-4, accepted)
The page's `queueTelemetry` caps event raw text at 4000 chars.
Device snaptest chunks are 1024 b64 chars (≈1.2 KB as page JSON),
so the guard holds today — but nothing pins it, and a truncated
chunk poisons frame reassembly silently. Rule, now enforced in the
page: **frame/audio chunk events are exempt from truncation**
(`noTruncate` flag on the queued event); the guard may only fire
on log/status text, never on a chunk. Once D5/D5b seals a batch,
the guard is applied pre-seal so it can never fire on sealed
payload. (Consolation: post-seal truncation would surface as a
loud MAC failure at the host, not silent corruption.)
Two-codebase invariant, written down: device chunk b64 ≤ 1024
chars ⇒ page chunk JSON ≈ 1.2 KB < 4000 cap, with the exemption
as the backstop.

### D8. Topics manifest, diffed (Claude round-4, accepted)
Round 4 found both directions of drift in one afternoon:
code-without-ACL (heartbeat topics published into default-deny)
and ACL-without-code (latent audio grant). Systemic fix: a
topics manifest — every topic referenced in code appears in the
ACL, every ACL line maps to code; either mismatch fails the
build. The manifest lives next to the ACL record
(`mosquitto-acl-20260925.acl`); the diff check is a build item
alongside the vector regen check (§5 harness). Heartbeats are
the first entry: `watcher` write + `muse` read, matching John's
"bridge listens" decision.

### D6. Fixed 2048-byte sig scratch → static/heap on S3 (Phase 2)
`ref/mqtt_seal.c` assembles the signed bytes in a 2 KB stack
buffer — fine on host, not in `link.c` (we're already stack-careful
there; the 4 KB ring is static for a reason). Phase 2 ports this to
a static buffer sized to our max command envelope.
**Recommendation to wh-link:** consider a `mqs_auth_bytes_needed()`
helper so constrained ports can pre-size without reading internals.

### D7. Base64 envelope on the wire (new, Phase 1)
Keeps the page's string-oriented MQTT path and the unsigned grace
path on one topic. Revisit only if byte-shaving ever matters (it
won't at our rates).
**Recommendation to wh-link:** none — transport encoding is per-
deployment.

---

## 8. Build order
- [x] `mqtt_seal.py` — Python port of `ref/mqtt_seal` (PyNaCl =
      libsodium backend) + interop test vs the C ref vectors
      (telemetry plane byte-identical; control plane uses standard
      Ed25519 per D1b — see below)
- [x] Keypair ceremony — generated 2026-09-25; private
      (`~/.ssh/watcher_seal_muse01`, 0600) never printed; public
      `9bc9b4610d920c0cf1415ed47e81c6c450f79bc9b323f4a66810e67b92ae9c43`
      (fingerprint `c0ccdd20d4d6e89f`) ready for John's page paste
- [x] `send_command.py` — seals every command (kind=2, topic-bound);
      `--no-seal` flag for grace-mode testing
- [x] Page verifier — `page_seal_draft.js` (7/7 tests pass in Node
      against real sealed envelopes); noble bundle 38.8 KB vendored
- [ ] Page patch — spec written (`PAGE_SEAL_PATCH_SPEC.md`);
      **awaiting John's review before touching watcher-console.html**
- [ ] Bench validation — sealed commands end-to-end, tamper/replay/
      cross-topic negative tests against the live page
- [ ] Phase 2 (firmware) — scoped when the next rev is planned:
      upstream `optional/monocypher-ed25519.{c,h}`
      (`crypto_ed25519_check`, RFC 8032 — NOT the vendored
      `crypto_eddsa_*`; M0 benchmarks it vs TweetNaCl on the S3
      first); `link.c` sealed-message type (firmware refuses
      unsealed serial commands outside bench grace mode);
      NVS-pinned commander key; **NVS high-water counter
      persistence, wear-aware** (D4 — closes the reboot replay
      hole); `mqs_auth_bytes_needed()`-style static scratch sizing
      storage via the dedicated persistence context (brief S1
      ESP-target lesson); `ts_window_ms = 0` initially (no SNTP yet)
- [ ] Phase 3 (evaluate) — WIA-SEAL session-cert profile
      (Claude's 2026-09-25 spec): replace the standing pinned
      commander key with short-lived scoped session certificates —
      human arm tap on device, scope bitmap (MOUSE/KEYBOARD/
      CONSUMER), TTL, per-session counters (dissolves the D4
      persistence problem structurally), bench-simplified
      `mint-cert` CLI instead of AWS KMS at the bench. The
      commander types keystrokes, so standing authority is the
      wrong shape long-term. Decision for John after Phase 2
      lands.

---

## 10. Phase 2/3 build status (2026-09-25 evening, John's go-ahead)

**Phase 2 + Phase 3 approved for build.** Firmware coordinator
dispatched; deliverable is a validated merged binary (no flash
without John's explicit approval).

**Bench root key (Phase 3, bench profile — NOT AWS KMS):**
- Private: `~/.ssh/watcher_seal_root` (0600, 32-byte seed, never printed)
- Public (bake into firmware as primary root):
  `8a9f61f08ea4e80e6268e61b9e23029db932378e247f3dfee8b0abac7088e71f`
- Fingerprint (sha256, first 16 hex): `8bc06f1a857e04a5`
- Backup root slot: reserved in firmware, unprovisioned at the bench.

**Host tooling shipped:**
- `wia_seal.py` — WIA1 envelope (64B header, kinds 1–4), cert/challenge
  bodies, ECDH + labeled KDF, `WiaSession` agent client. **8/8 self-tests pass.**
- `mint_cert.py` — bench mint-cert CLI: `register` (claim flow) + `mint`
  (challenge verify vs registry, single-use nonces, scope/TTL policy,
  kind-3 envelope out). E2E proven against a simulated device, including
  nonce-reuse refusal.

**Page 0.4.65 (dual-mode arbitration, shipped):**
- Parses `mqtt_mode` / `seal_dev_verify` / `seal_floor` / `seal_armed` /
  `seal_session` from device heartbeat + `seal_status` lines; shows them
  in the Setup seal card (`sealDevState`).
- Bridged + device-verifies → forwards sealed envelopes opaquely as
  `{"t":"sealed_fwd","topic","b64"}` over serial; page never dispatches
  locally. Unsigned commands still go as plain serial JSON; the device
  applies its own seal policy to the serial path.
- Native mode → page ignores `watcher/command` (device owns the topic)
  and stops forwarding device heartbeats (device publishes directly).
- Layout 40/40, seal 36/36, syntax clean.

**Serial contract (firmware implements; page already does):**
- Device → page: status/heartbeat JSON gains `mqtt_mode`,
  `seal_dev_verify`, `seal_floor`, `seal_armed`, `seal_session`.
- Page → device: `{"t":"sealed_fwd","topic":"<claimed>","b64":"<env>"}`,
  `{"t":"seal_cert","b64":"<kind-3 env>"}`.

**Still needed from John (when firmware lands):**
1. Provision WiFi + broker (`watcher` identity) credentials to the
   device over serial (bench trusted channel); device stays bridged
   until provisioned.
2. Claim flow: register the device identity pubkey via
   `mint_cert.py register`.
3. Approve the flash (transfer + page modal, as usual).
