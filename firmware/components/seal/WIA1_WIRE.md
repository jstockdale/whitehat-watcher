# WIA1 wire & integration guide (Phase 3 session machine)

Companion to `WIA-SEAL-v1_20260925.md` (the normative spec) and
`components/seal/include/wia1.h` (the contract). This document records
the exact byte layouts, formulas, and integration contracts as
implemented in `components/seal/wia1.c`, plus the gaps the
coordinator/integration layer must still close. Everything below was
verified against the host test oracle (`test/oracle_wia1.py`, PyNaCl)
and the C unit test (`test/test_wia1.c`, 196 checks, all passing).

Conventions: all integers little-endian. `H(x, n)` = BLAKE2b with
`digest_size=n`, raw output. `||` is byte concatenation.

## 1. Envelope (all kinds)

Fixed 64-byte header, then a kind-specific body, then a 64-byte
Ed25519 signature (kinds 2–4) or a 16-byte BLAKE2b tag (kind 1).

### 1.1 Header (64 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"WIA1"` |
| 4 | 1 | kind: 1=telemetry, 2=control, 3=cert, 4=challenge |
| 5 | 1 | flags (kind 1: bit 0 = ENC; kinds 2–4: must be 0) |
| 6 | 16 | `dev_id` (ASCII, e.g. `wia-336903ad0966`) |
| 22 | 16 | `session_id` (kinds 1, 2; zero for kinds 3, 4) |
| 38 | 8 | `ctr` u64 (kind 2 message counter; kind 4: 0) |
| 46 | 8 | `ts_rel_ms` u64 (kind 2: ms since session t0; else 0) |
| 54 | 2 | `msg_type` u16 (kind 2: CTL_BATCH/PING/DISARM; else 0) |
| 56 | 8 | reserved, zero |

Structural gates (§6 steps 1–2): `len >= 64 + 64` (or `64 + 16`
minimum for kind 1), magic, known kind, flags/rsv zero. Anything else
is dropped as `MALFORMED`.

### 1.2 What gets signed / MACed (the `S` construction)

- **Kinds 3 and 4:** `S = header64 || body`. No topic. The challenge
  and cert are minted and verified off-topic (serial, KMS, console).
- **Kinds 1 and 2:** `S = u16le(len(topic)) || topic || header64 || body`.
  The delivered MQTT topic is part of the authenticated input, so a
  cross-topic republish dies at the tag/signature (logged as
  `BAD SIG on <delivered-topic> - DROPPED`).
- **Kind-1 ENC AAD:** `AAD = u16le(len(topic)) || topic || header64`
  (XChaCha20-Poly1305; the 24-byte nonce is sent in the clear after
  the header, then `ct || tag16`).

### 1.3 Kind 4 — challenge body (52 bytes)

Emitted by `wia1_arm_tap()` into the caller's buffer (180 bytes total:
`header64 || body52 || sig64`).

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `chal_ver = 1` |
| 1 | 1 | `proto_max = 1` |
| 2 | 2 | offered scope LE (`0x0007` = mouse\|keyboard\|consumer) |
| 4 | 16 | challenge nonce (fresh `wia1_random_bytes` per tap) |
| 20 | 32 | device arm X25519 public key (fresh ephemeral per tap) |

Signed by the device identity key (NVS `seal/dev_id_sk`, minted on
first boot). The arm window is 120 s; one cert per nonce.

**Arm short code** (shown on the device screen / serial for the human
to compare against the console): the first 3 bytes of
`BLAKE2b-16(0x01 || nonce16 || device_x_pk32)` rendered as 6 lowercase
hex chars. `wia1_arm_shortcode()` returns it while a challenge is
pending.

### 1.4 Kind 3 — certificate body (135 bytes)

Total envelope: `header64 || cert_body135 || sig64` = 263 bytes.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | `cert_ver = 1` |
| 1 | 2 | scope LE (bitmap; must be ⊆ `0x0007`) |
| 3 | 4 | TTL seconds LE (1 … 14400) |
| 7 | 16 | device ID (must equal self) |
| 23 | 16 | challenge nonce (must equal the pending tap's nonce) |
| 39 | 32 | agent Ed25519 public key (control-plane signer) |
| 71 | 32 | agent X25519 public key (ECDH peer) |
| 103 | 32 | device arm X25519 public key (must equal the pending tap's) |

Verification (§4 step 5, in order): root signature (primary, then
backup) over `header || body` → cert_ver → device → nonce → device
X25519 key → TTL → scope. A bad root signature is fatal:
`BAD CERT SIG - DROPPED` (feeds the §4 lockout counter).

### 1.5 Kind 2 — control

`header64 || body || sig64`. `msg_type`: `0x0001` = BATCH,
`0x0002` = PING, `0x0003` = DISARM. The session Ed25519 key from the
cert signs `S`. Verification order on the device (§6 steps 1–12):

1. retained → `RETAINED CTL - DROPPED`
2. malformed → `MALFORMED`
3. `dev_id` → `WRONG DEVICE`
4. not armed → `CTL while DISARMED`
5. `session_id` → `STALE SESSION`
6. signature → `BAD SIG on <topic> - DROPPED` (fatal, always)
7. `ctr` dup → silent `WIA1_OK_DUP`; rollback → `REPLAY ctr <n> <= <hw> - DROPPED` (fatal)
8. `|mono_since_t0 − ts_rel_ms| > 10 000 ms` → `STALE (|dt|=<n>ms)`
9. session TTL expired → wipe, `SESSION EXPIRED - DISARMED`
10. opcode scope → `SCOPE VIOLATION` (fatal)
11. body grammar → `BAD BODY`
12. adopt `ctr`, execute batch in order

Batch grammar (`msg_type = 0x0001`): 1…32 ops, each `op || args`.
Ops: `0x01` MOVE_REL (dx i16, dy i16), `0x02` BUTTON (mask, action),
`0x03` WHEEL (delta i8), `0x04` DWELL (ms u16 ≤ 2000),
`0x10` KEY_REPORT (mod, 6 keycodes), `0x20` CONSUMER (usage u16).
Unknown op, truncated args, empty batch, or > 32 ops → the whole
batch dies as `BAD BODY`.

### 1.6 Kind 1 — telemetry

`header64 || [nonce24] || body || tag16`. MAC mode:
`tag16 = BLAKE2b-keyed(k_tel_mac, out=16, S)`. ENC mode (mandatory
for `TEL_FRAME`): XChaCha20-Poly1305 with `k_tel_aead`, layout
`header64 || nonce24 || ct || tag16`. **Fail-closed:** `TEL_FRAME`
without ENC is refused; `wia1_seal_tel()` returns -1 on any guard
failure (NULL out, session not armed/expired).

**Fixed topic contract:** `wia1_seal_tel()` has no topic argument;
telemetry is always sealed for `wia/<dev_id>/tel`. The MQTT layer
must publish the envelope to exactly that topic, because the topic is
part of `S`/AAD.

## 2. Session formulas

- `session_id = BLAKE2b-16(cert_body)` (16 bytes, over the 135-byte body).
- `ss = X25519(device_arm_x_sk, agent_x_pk)` — device side; the agent
  computes `X25519(agent_x_sk, device_arm_x_pk)` (same value).
- `k_<label> = BLAKE2b-keyed(key=ss, out=32, msg="wia1:" || label || ":" || session_id)`
  with labels `tel-mac`, `tel-aead`, `ctl-mac`, `ctl-aead` (ASCII, no NUL).
- `t0` (session clock) starts at the accept tap for keyboard/consumer
  sessions, at cert ingest for mouse-only sessions. First valid
  control `ctr` is 1; counters start at 0.
- Session ID as hex: first 8 bytes of `session_id`, 16 lowercase hex
  chars (`wia1_session_id_hex()`).

## 3. Arm ceremony (physical)

1. Human taps ARM on the device → `wia1_arm_tap()` emits the kind-4
   challenge (180 bytes) over serial; device shows the 6-char short
   code. 120 s window.
2. Agent/host forwards the challenge to the minting service, which
   verifies the identity signature against the registered identity
   pubkey and returns a kind-3 cert (263 bytes).
3. `wia1_ingest_cert()`: mouse-only scope → ARMED immediately;
   keyboard/consumer scope → waits for the physical accept tap
   (`wia1_needs_accept_tap()` / `wia1_accept_tap()`), which starts
   `t0`. The accept-tap wait does **not** consume the session TTL.
4. Disarm: physical tap (`wia1_disarm_tap()`), remote
   `CTL_DISARM`, session expiry, or the §4 lockout (10 fatal
   rejections in 60 s → auto-disarm, physical re-arm required).

## 4. Worked example (non-secret, from the deterministic test oracle)

From `test/oracle_wia1.py` (deterministic throwaway keys; regenerating
the oracle always emits these exact values):

- `dev_id` = `wia-336903ad0966`
- identity fingerprint = `336903ad0966ad78`
- arm short code = `5c8df2`
- `session_id` = `d8d9a3abb5e280a8c344823a0d685043`
- challenge envelope (180 B) and cert envelope (263 B) are in
  `test/vectors.h` as `VEC_CHAL` / `VEC_CERT`; the C test asserts
  byte-identical reproduction.

## 5. Grace mode (bench only)

`wia1_grace_set(true)`: an **absent** (all-zero) kind-2 signature is
honored for mouse-scope opcodes only, with a 1-in-16 warn log
(`GRACE unsigned CTL honored (1-in-16 warn)`). A present-but-wrong
signature is fatal in every mode. Unsigned KEY_REPORT / CONSUMER /
DISARM / PING are refused (`SCOPE VIOLATION`). The banner shows
`UNSIGNED MODE`.

## 6. Integration hooks (what the coordinator must wire)

`wia1.h` declares the contract; the following are declared at the
call site (documented here, not in the header):

| Function | Purpose |
|---|---|
| `void wia1_claim_info(char *out, size_t cap)` | `dev_id` + identity fingerprint, for the claim UI |
| `void wia1_arm_shortcode(char out[7])` | 6-char short code while a challenge is pending |
| `void wia1_banner_state(char *out, size_t cap)` | `DISARMED` / `ARMED <scopes> [UNSIGNED]` for the LED panel |
| `int wia1_wants_status_reply(void)` | poll-and-clear flag set by `CTL_PING` |
| `int wia1_poll_expiry(void)` | 1 Hz tick: arm-window and session-TTL expiry |
| `int wia1_raw_key_report(uint8_t mod, const uint8_t keys[6])` | **weak** stub — coordinator overrides to inject into `hid_ble` |
| `int wia1_raw_consumer(uint16_t usage)` | **weak** stub — coordinator overrides to inject into `hid_ble` |

Mouse ops call `hid_ble_*` directly (REQUIRES `hid_ble` in
CMakeLists). Keyboard/consumer go through the weak raw-report hooks,
which return 0 as stubs that only log (`KEY_REPORT not wired`,
`CONSUMER not wired`) — **until the coordinator provides strong
overrides, keyboard and consumer sessions arm fine but type nothing.**
That is the known raw-HID gap.

## 7. Retained control

`wia1_ingest_ctl()` has no retained flag: **the caller must drop
retained MQTT control messages before calling it.** The internal
retained-aware helper (`wia1_ingest_ctl_r`, exercised by the host
test) rejects retained envelopes with `RETAINED CTL - DROPPED`.
Nothing in this system is retained, either direction (§8).

## 8. Session state & locking

All session/pending state lives in one static struct guarded by a
FreeRTOS mutex (host test: no-op). No large stack buffers: the 2 KB
scratch used for `S`/AAD construction is static. NVS writes happen in
exactly one context (the seal writer task); the identity key
(`seal/dev_id_sk`) is minted once and never rotated in normal
operation.

Root keys: `seal_roots.h` ships two 32-byte **zero placeholders**
(`WIA1_ROOT_PRIMARY`, `WIA1_ROOT_BACKUP`). Provision both before any
release build — with both zero, every cert is rejected
(`CERT no root provisioned - DROPPED`) and `wia1_init` logs
`roots_provisioned=0`.
