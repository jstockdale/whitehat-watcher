# Watcher firmware v0.4.0 — Phase 2 + Phase 3 (MQTT SEAL) build report

**Date:** 2026-09-26 (UTC)
**Binary:** `firmware/release/watcher-v0.4.0-merged.bin`
**Size:** 7,762,246 bytes
**SHA-256:** `f77ae6a62aec7a79c7c5837036aabc940e198fa5bfc751675aeeef4334ece99c` (placeholder-root bench-test build)
**Release SHA-256:** `c94837b353cb5109d88f63db9806edea4914b9cfa9643aa45c40e7c30f3b7c7d` (real bench roots baked in — **this is the release build**)
**Roots:** WIA1_ROOT_PRIMARY fp `8bc06f1a857e04a5`, WIA1_ROOT_BACKUP fp `f8cc458eb0bf9349` — both verified present in the merged binary. ASK resolved 2026-09-25.
**Image checks:** bootloader magic `0xe9` @ 0x0 ✓ · factory app magic `0xe9` @ 0x20000 ✓ (auto-aligned by `idf.py merge-bin`, never hand-written) · partition magic `aa 50` @ 0x8000 ✓
**Flashing:** NOTHING WAS FLASHED. Deliverable is the validated merged binary + this report only. Bench untouched.

## What was built

New `firmware/components/seal/` component (~6.4k lines incl. upstream crypto):

| File | Purpose |
|---|---|
| `monocypher.c/.h` + `optional/monocypher-ed25519.{c,h}` | Upstream Monocypher (CC0/BSD-2), fetched from GitHub master. RFC 8032 Ed25519 via the **optional** module + `crypto_ed25519_check` — the core's `crypto_eddsa_*` is EdDSA-BLAKE2b (wrong algorithm) and is never used. |
| `wms2.c` (`include/wms2.h`) | WMS2 envelope verifier: magic/kind/len → tag/sig (**always fatal, even in grace**) → ctr replay (dup silent, rollback loud) → ts window (`ts_window_ms=0` disables — no SNTP on S3). kind=2 = RFC 8032, kind=1 = BLAKE2b-16 MAC with streaming updates + constant-time compare. Auth input assembled in caller scratch (D6: no large stack buffers, no malloc in verify path). |
| `seal_dev.c` (`include/seal_dev.h`) | Phase 2 device core: NVS-pinned commander key, D4 zero-budget replay high-water, claimed-topic binding, SECLOG named counters, telemetry KDF (labels byte-identical to host `mqtt_seal.py`: `WMS2-tel-mac-v1`, `WMS2-tel-aead-v1`), serial provisioning handlers. Single dedicated `seal_nvs` writer task — all NVS writes flow through it. |
| `mqtt_native.c` (`include/mqtt_native.h`) | Native MQTT: dormant poll task, starts esp-mqtt TLS to the broker only when WiFi is up AND creds provisioned; never blocks boot. Subscribes `watcher/command` QoS 1; accepts raw WMS2 bytes or `{"sealed":"<b64>"}`; verifies against the delivered topic; plain JSON honored only in grace. Telemetry mirrored as JSON + `wms2_mac` (BLAKE2b-16 over `u16le(len)\|\|topic\|\|body`). |
| `wia1.c` (`include/wia1.h`, `include/seal_roots.h`) | Phase 3 WIA1 session machine: first-boot identity mint, challenge/cert validation, ECDH/KDF, scoped controls, 10-fatal-in-60s lockout, RAM-only sessions/counters (wiped on disarm/reboot), grace restrictions, telemetry sealing. Root slots are **placeholder zeros** (see ASK below). |
| `PROVISIONING.md` / `WIA1_WIRE.md` | Bench ceremony docs + wire-byte spec for the parent's mint-cert CLI. |

`components/link/link.c` integration: `sealed_fwd` (base64→verify against *claimed* topic→dispatch inner `{"id","cmd"}`, exactly-once terminal reply), `seal_cert`, `seal_provision`, `seal_unpin`, `seal_strict`, `seal_tel_seed`, `mqtt_cred`, `wifi_cred`, `seal_grace` (provisioning refused over MQTT — serial console only). Heartbeat/ready carry the full serial contract the page (console 0.4.65+) reads: `mqtt_mode`, `seal_dev_verify`, `seal_floor`, `seal_armed`, `seal_session` (string|null), plus `seal_seclog`.

## N=32 rationale (D4 zero-budget variant)

Persist the replay high-water every 32nd accepted command; on boot adopt floor = stored + 32. The persist invariant guarantees executed ≤ stored + 31 at all times, so the boot floor clears everything already executed: **replay budget after crash = 0**. The floor is surfaced in `seal_floor`; the commander resyncs with `next = max(own_counter, floor) + 1`, with an anti-oracle cap (reject floor > own + 2²⁰, or trust the floor only from authenticated status).

## Test results

- **Host WMS2 suite** (`components/seal/test/test_wms2.c`): **74/74 PASS** — RFC 8032 §7 vectors, Monocypher↔PyNaCl both directions (byte-identical deterministic sigs), Python→C and C→Python cross-verification via the real `mqtt_seal.py` (kind-1 and kind-2), every named rejection (bad magic/kind/short, BAD-TAG, BAD-SIG fatal-in-grace, cross-topic fatal, silent OK-DUP, loud REPLAY, STALE, ts-disabled, no-key/no-pin grace+strict, unsealed grace+strict, kind-1 ENC → NO-AEAD-KEY), ordering (auth beats replay beats ts), failed gates never advance high-water.
- **Host WIA1 suite** (`components/seal/test/test_wia1.c`): **196/196 PASS** — RFC 8032 §7.1, byte-identical kind-4/3/2/1 envelopes vs the PyNaCl oracle, `session_id == BLAKE2b-16(cert_body)`, all four KDF labels, libsodium-sealed ENC frame opened by Monocypher, every §6 named rejection, dup-silent/replay-loud, 10-fatal lockout + re-arm, grace mode, expiry, accept-tap flow, 32-op batch limit, all `seal_tel` fail-closed guards.
- **IDF build** (`idf.py build`, ESP-IDF @ `/home/hatch/esp-idf`): clean, **zero warnings from all new code** (seal component + link.c). One pre-existing warning in `main.c` (`wakeword_set_callback` pointer type) is untouched legacy code.
- Rebuilt from a clean-ish state after touching all new sources; `seal_dev.c`, `mqtt_native.c`, `wia1.c`, `wms2.c`, `monocypher*.c` all compile under the real xtensa toolchain.

## Provisioning commands (serial, bench = trusted physical channel)

```json
{"t":"seal_provision","pk":"<64hex commander Ed25519 pubkey>"}
{"t":"seal_unpin"}
{"t":"seal_strict","on":true}
{"t":"seal_tel_seed","seed":"<64hex>"}
{"t":"mqtt_cred","user":"<watcher-role user>","pass":"<pass>"}
{"t":"wifi_cred","ssid":"<ssid>","pass":"<pass>"}
{"t":"seal_grace","on":true}
{"t":"seal_cert","b64":"<base64 WIA1 kind-3 envelope>"}
{"t":"sealed_fwd","topic":"<claimed-topic>","b64":"<base64 WMS2 envelope>"}
```

Full ceremony (order, fingerprint cross-checks, D4 resync rule, strict flip, reset): `components/seal/PROVISIONING.md`.

## Claim and mint-cert flow (Phase 3)

1. Device mints its Ed25519 identity on first boot (`seal/dev_id_sk` in NVS), publishes `dev_id` + shortcode in status.
2. Parent's mint-cert CLI (wire bytes: `components/seal/WIA1_WIRE.md`) builds the 135-byte session cert body, signs with the root key.
3. `{"t":"seal_cert","b64":"…"}` (serial in bridged mode, or MQTT in native mode) → device validates root signature (primary then backup slot), ECDH, KDFs session keys.
4. Keyboard/consumer scopes require an explicit physical accept tap; 10 fatal auth events in 60 s auto-disarm; sessions are RAM-only and wipe on disarm/reboot.

## Limitations / open gaps (pre-release)

1. **Root keys are placeholders** — see ASK. Firmware must not ship to the bench with zero roots except bench-test builds.
2. **Native MQTT TLS has no server-cert verification** (esp-tls without CA): encrypted but unauthenticated. Logged as a warning on every connect. Broker CA pinning is a pre-release TODO.
3. **`wms2_mac` host verification recipe** (`PROVISIONING.md` §2d) is written but not yet exercised end-to-end (no device flashed).
4. **Sealed firmware chunks stay page-verified** in Phase 2 bridged mode — chunks exceed the 2048 B sealed body limit (`SEAL_MAX_CMD_BODY`). Chunked `sealed_fwd` is future work.
5. **Raw-HID report / display hooks** for WIA1 scoped controls are stubbed behind the contract API; bench integration of the actual report injection is follow-up.
6. D4 NVS writer queue is 16 deep; a queue-full drop would in theory violate the crash invariant (NVS writes take ~ms; accepted residual risk, flagged in code).
7. Console 0.4.66 also shipped in this window: fixed the parent's 0.4.64 stale-`msg` repair passing the inner `cmd` (not the `{id,cmd}` envelope) to `mqpHandleFlash` — flash-via-MQTT would have failed its sha256 check. Layout 40/40, seal 36/36, syntax OK.

## ASK — parent (John) action required before any release

**Primary + backup root Ed25519 PUBLIC keys.** Both slots in `firmware/components/seal/include/seal_roots.h` are placeholder zeros, guarded by a STOP comment. The parent holds the root private keys; firmware receives public keys only. Supply 32 raw bytes each (hex) for `WIA1_ROOT_PRIMARY` and `WIA1_ROOT_BACKUP`, and I will bake them in, rebuild, re-merge, and re-verify. Until then the binary above is a **bench-test build only** — Phase 3 sessions cannot arm against a real root.
