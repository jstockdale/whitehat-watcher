# WMS2 seal component — host test harness

Host-side cross-verification for the Phase 2 WMS2 envelope verifier
(`wms2.c`, upstream Monocypher RFC 8032 Ed25519).

## Build (warning-clean)

From this directory:

```
gcc -std=c99 -Wall -Wextra -Werror -Wno-comment -O2 \
    -I.. -I../include \
    test_wms2.c ../wms2.c ../monocypher.c ../optional/monocypher-ed25519.c \
    -o test_wms2
```

`-Wno-comment` works around a pre-existing `*body/*body_len` sequence in
the (untouchable) contract header `include/wms2.h:90` tripping
`-Werror=comment`. Everything else builds under `-Wall -Wextra -Werror`.

## Run

```
./test_wms2            # exit 0 on ALL PASS, 1 on any failure
```

The harness shells out to the system `python3` for the PyNaCl interop
and the `mqtt_seal.py` cross-verification. It locates the bridge at
`$WMS2_MQTT_BRIDGE`, defaulting to
`$HOME/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge`.

## Coverage (74 checks)

- RFC 8032 §7 vectors 1–3: `crypto_ed25519_check` accepts, rejects a
  tampered sig, `crypto_ed25519_key_pair` reproduces the RFC public key,
  deterministic `crypto_ed25519_sign` reproduces the RFC signature.
- Monocypher ↔ PyNaCl interop, both directions; byte-identical
  deterministic signatures.
- Cross-verification vs `mqtt_seal.py`, both directions, kind=2
  (control) and kind=1 (telemetry).
- Every named rejection: bad magic (strict UNSEALED / grace OK-GRACE),
  bad kind, short, tampered body (BAD-SIG, fatal even in grace),
  cross-topic republish (fatal), dup ctr (silent OK-DUP), ctr rollback
  (loud REPLAY), stale ts, `ts_window_ms=0` disabling the ts gate,
  no-key grace vs strict (both kinds), unsealed grace vs strict,
  kind-1 ENC without AEAD key (NO-AEAD-KEY), gate ordering
  (auth → replay → ts), failed gates never advancing high-water,
  `wms2_seal_control` edge cases, tiny-scratch SPACE, `rp == NULL`.

Note: `WMS2_E_MAGIC` is reserved — like the MIT reference it descends
from, the verifier never returns it (bad magic is treated as
unauthenticated input: UNSEALED under strict, OK-GRACE otherwise).

## WIA1 session machine (`test_wia1.c`, `oracle_wia1.py`)

Host-side cross-verification for the Phase 3 WIA1 session machine
(`wia1.c`, upstream Monocypher + `optional/monocypher-ed25519`,
RFC 8032 Ed25519). The oracle builds every envelope kind
independently with PyNaCl and emits `vectors.h`; the C test asserts
byte-identical reproduction plus the full §6 rejection battery.

Build (from `components/seal/`):

```
python3 test/oracle_wia1.py   # deterministic throwaway keys -> test/vectors.h
# one-time host stub (NOT a repo file; wia1.h needs esp_err.h):
mkdir -p /tmp/wia1_host_stubs
printf '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL 1\n' \
    > /tmp/wia1_host_stubs/esp_err.h
gcc -std=c99 -Wall -Wextra -Werror -O2 -DWIA1_HOST_TEST \
    -Iinclude -Itest -I/tmp/wia1_host_stubs -I. \
    test/test_wia1.c wia1.c monocypher.c optional/monocypher-ed25519.c \
    -o /tmp/test_wia1 && /tmp/test_wia1
```

Coverage (196 checks):

- RFC 8032 §7.1 TEST 1: `crypto_ed25519_check` accepts,
  `crypto_ed25519_key_pair` reproduces the RFC pubkey (note:
  Monocypher's `key_pair` **wipes its seed argument** — the test feeds
  it a mutable copy), deterministic `crypto_ed25519_sign` reproduces
  the RFC signature, tampered sig rejected.
- Golden challenge envelope byte-identical to the PyNaCl oracle;
  short code `BLAKE2b-16(0x01||nonce||x_pk)[:3]` matches.
- Golden cert ingest: `session_id == BLAKE2b-16(cert_body)`, all four
  labeled KDF outputs match, backup-root path accepted.
- Standalone KDF vectors (hardcoded ss/session_id).
- Golden kind-1 MAC telemetry byte-identical; kind-1 ENC (libsodium)
  opened by Monocypher's `crypto_aead_unlock`.
- Golden kind-2 CTL accepted; MOVE_REL/BUTTON/WHEEL/DWELL executed
  through the `hid_ble` doubles.
- Every named rejection: retained, malformed (short/magic/flags/rsv/
  kind), wrong device, disarmed, stale session, bad sig (fatal),
  dup-of-last silent, replay (fatal), stale ts, scope violation
  (fatal), bad body (unknown op/truncated/empty/>32 ops), cross-topic
  republish, bad cert sig (fatal), cert out of window, cert field
  rejects (ver/device/nonce/x_pk/ttl/scope), 10-fatal lockout +
  auto-disarm + re-arm, lockout 60 s windowing.
- Keyboard/consumer accept-tap flow (pending → tap → scope granted);
  consumer op on keyboard session → SCOPE; accept tap with nothing
  pending → -1.
- Ping/status-reply poll-and-clear, remote disarm, banner strings,
  seclog codes.
- Grace: unsigned mouse batch honored (1-in-16 warn), unsigned
  KEY_REPORT/DISARM refused.
- Expiry: TTL check per §6 step order (after the ts gate);
  `poll_expiry` disarms; expired sessions refuse `seal_tel`; the
  accept-tap wait does not consume TTL.
- `seal_tel` guards: NULL out, NULL body with len, FRAME-without-ENC
  refused, small cap refused.
- Exactly 32 batch ops accepted and executed (terminator-overflow
  guard); 33 → BAD BODY.
- Accept tap resets t0 and both counters.
- Fatal accounting counts a fatal at monotonic second zero (sentinel
  `now_s + 1`).

