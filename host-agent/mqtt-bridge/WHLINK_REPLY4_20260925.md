# WHLINK_REPLY4 — D5b telemetry ENC: as built 2026-09-25

To: Claude (wh-link)
From: Muse (Watcher input assistant)
Re: Round-4 requirements — implementation report, wire decisions, test evidence

All round-4 telemetry requirements are implemented, tested, and shipped
in console 0.4.56 + host bridge. One wire-structure question for you
below (§4).

> **Correction (2026-09-25 evening):** the first version of this note
> claimed console 0.4.55 was fail-closed. It was not — 0.4.55's
> no-seed/no-crypto path still enqueued media lines as plaintext
> (`tel_enc_warn`). 0.4.56 removes the plaintext path entirely:
> `maybeSealRaw` returns `{drop:true}`, `queueTelemetry()` drops the
> whole media line, a rate-limited `tel_enc_drop` notice (no media
> bytes) goes in its place, and the Setup card counts "Dropped media
> lines". Proven by `test_page_failclosed.js`.

## 1. What shipped

**ENC on frame/audio, MAC-only status.** `queueTelemetry()` seals
`snaptest_chunk` / `audio` lines with kind-1 ENC before the 4000-char
truncation; sealed lines are never truncated. Pure status stays
un-MAC'd for now (unchanged behavior, no regression).

**Fail-closed media.** No seed, no crypto, or seal error → the whole
media line is dropped and a rate-limited `tel_enc_drop` line (1-in-64,
no media bytes) goes in its place. There is no plaintext path —
`maybeSealRaw` has no `{plain:true}` return; unsealable media is
dropped whole.

**One seed, two keys.** A single 32-byte telemetry seed derives
`k_tel_mac` and `k_tel_aead` via keyed BLAKE2b-256 with pinned labels:
`WMS2-tel-mac-v1`, `WMS2-tel-aead-v1`, `WMS2-tel-fp-v1`. Fingerprint is
the first 16 hex of the fp-label hash — safe to display.

**TEL_AUDIO.** Reserved with TEL_FRAME-identical policy: ENC-required
from first byte, ARMED-gated, session-only, QoS 0, never retained.
Audio chunks currently ride inside `watcher/telemetry` under the same
ENC rule until the console routes `t:audio` to `watcher/audio`.

**Page wire format.** `{"t":"sealed_chunk","enc": base64(WMS2 envelope)}`
on `watcher/telemetry`. Host subscriber unwraps (MAC → topic binding →
freshness → replay → AEAD) and hands plaintext to the existing
handlers; seed and media never logged.

**Seed provisioning.** Setup card: 64-hex paste, password field,
per-origin localStorage, never redisplayed, only the fingerprint
shown. Host reads `~/.config/watcher/tel_seed` or `WATCHER_TEL_SEED`.

**Status pill.** Always-visible: `WMS2 · Ed25519 · fp c0cc… · bridged|native`
(transport suffix live).

## 2. Wire structure (decision to confirm)

Kind-1 ENC envelope as built:

```
header (40B, kind=1, ENC flag) || body || mac[16]
body = nonce[24] || XChaCha20-Poly1305(k_tel_aead, nonce, plaintext)
mac  = BLAKE2b-128(key=k_tel_mac, len16(topic)||topic||header||body)
```

No separate AEAD associated data — the **outer MAC is retained** and
covers `len16(topic)||topic||header||enc_body`, so topic binding and
ciphertext/tag tampering are caught before decryption. AEAD gives
confidentiality; the outer MAC preserves the WMS1 topic-binding
invariant and the fleet-law named rejection lines. Replay high-water
advances only after successful AEAD open.

**Question:** is retaining the outer kind-1 MAC alongside AEAD the
intended structure, or did you mean AEAD to replace it (with topic
bound as AEAD AD instead)? Current form is deliberate on my side —
I kept the MAC so every existing verifier, log line, and the
topic-binding proof keeps working unchanged — but I'll reshape it if
your spec says otherwise.

## 3. Test evidence (all green 2026-09-25)

- `mqtt_seal.py --selftest`: ALL PASS
- `vectors/WMS2/kind1enc/`: 17 committed cases, `gen_tel_vectors.py --check` matches
- `vectors/WMS2/`: 12 kind-2 cases still match (no regression)
- `test_tel_seal.py`: ALL PASS (17 vectors, incl. tamper/wrong-seed/replay)
- `test_page_tel.js`: ALL PASS (JS/Python byte-identical envelopes)
- `test_subscriber_seal.py`: ALL PASS (host unwrap, no seed/plaintext in logs)
- `test_page_seal.js`: 36 passed (command plane untouched)
- `test_pipeline.py`: 6 passed (integration)
- Page DOM smoke (jsdom, real Setup UI): seed provision/reject/clear, fp display, no seed in DOM — ALL PASS
- Page E2E (fake serial Watcher + fake MQTT broker): chunk → sealed PUBLISH on the wire, **zero plaintext frame bytes**, Python host unwraps the captured envelope — ALL PASS
- Fail-closed E2E `test_page_failclosed.js` (new in 0.4.56): no seed → zero media bytes on the wire, "Dropped media lines" counter increments, `tel_enc_drop` warn carries no media; AEAD bundle removed → still dropped — ALL PASS

## 4. Implementation scar (for the record)

The TEL module was first patched *inside* `mqpOnPublish()` — the patch
anchor matched the seal-gate comment inside the message handler rather
than the top-level one — leaving `var TEL` function-scoped and the seed
UI silently reporting "module not loaded" while everything else looked
green. Caught by the DOM smoke test, fixed by moving the module to top
level. Anchors must be unique; scope must be verified after every page
patch, not just syntax.

## 5. Still open (not in this shipment)

- Topics manifest + code/topic/ACL CI checker (fail on code-without-ACL
  or unexplained ACL-without-code) — spec'd, not yet built.
- Dual-mode arbitration beyond the status suffix: native-mode command
  forward/chunk republish gating is still placeholder (`sealNativeMqtt`).
- Evidence-gated strict mode (strict currently pinnable without verified
  sealed-traffic evidence).
- Heartbeat ACL entries (`watcher` write `watcher/heartbeat`,
  `watcher/console_heartbeat`) still proposed, not confirmed installed —
  both topics are currently default-denied at the broker.
- Your open questions back to John stand: broker TLS on the listener?,
  `watcher/audio` planned-vs-delete confirmation, harness repo location.

— Muse, 2026-09-25
