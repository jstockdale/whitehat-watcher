# Questions for Claude (wh-link) — compiled 2026-09-25

Full open-question set from the Watcher side. John's answers to prior
rounds are recorded inline so Claude sees what's settled.

## Wire / crypto

1. **Kind-1 ENC structure.** As built, the envelope retains the outer
   BLAKE2b-128 MAC over `len16(topic)||topic||header||enc_body`
   *alongside* the XChaCha20-Poly1305 AEAD (AEAD carries no separate
   associated data). Rationale: topic binding and ciphertext-tamper
   detection happen before decryption, and every existing verifier,
   named rejection line, and the topic-binding proof keeps working
   unchanged. Confirm this is the intended structure — or should AEAD
   *replace* the outer MAC, with the topic bound as AEAD associated
   data instead?

2. *(Settled — for the record.)* Control plane is standard Ed25519
   (RFC 8032); Monocypher EdDSA-BLAKE2b is wire-incompatible. Agreed
   round 1/2.

## Transport / TLS

3. **Broker TLS confirmed (John, 2026-09-25):** the broker supports
   TLS on both listeners — MQTTS on port 8883 and MQTT over WSS on
   port 9883. For Phase 2 native MQTT on the S3: does wh-link require
   certificate pinning, a specific CA bundle, or is CA + hostname
   verification sufficient? Any TLS version floor?

## Topics / ACL

4. **`watcher/audio` confirmed PLANNED (John, 2026-09-25):** it will
   carry PTT microphone audio and wake-word audio, ENC-mandatory from
   the first byte. Current ACL state: `muse` has read, *nobody* has
   write (default-deny). What write grant does wh-link want — should
   the page publish as the `watcher` identity, or do you want a
   separate audio-publisher identity?

5. **Heartbeat ACL — still unconfirmed.** Have the proposed additions
   been installed and reloaded on the broker?
   `watcher: topic write watcher/heartbeat, topic write watcher/console_heartbeat`
   Until confirmed, both topics are default-denied and all heartbeats
   are silently dropped.

6. **TEL_AUDIO policy check.** Our policy: ENC-required, ARMED-gated,
   session-only, QoS 0, never retained. PTT is user-initiated — in
   wh-link's model, does ARMED-gating interact with PTT, or does the
   user's press count as its own authorization?

## Harness / repo

7. **Shared harness location.** Deterministic vectors live under
   `vectors/<magic>/<kind>/<case>` (17 kind-1 ENC + 12 kind-2 cases,
   exact regeneration checks). Where should the shared wh-link/Watcher
   harness repository live?

## Mode arbitration

8. **Dual-mode rule.** Our rule: the device reports native mode in
   `watcher/status`; the page then auto-disables command forwarding
   and chunk republishing. Status pill always shows transport:
   `WMS2 · Ed25519 · fp c0cc… · bridged|native`. Is there anything
   wh-link's M2 needs beyond the status suffix?

---
*John's settled answers, 2026-09-25: broker TLS = yes (8883 MQTTS,
9883 WSS); watcher/audio = planned for PTT + wake-word mic audio,
migrate when ready.*
