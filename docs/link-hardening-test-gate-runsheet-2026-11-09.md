# Link-hardening test gate — bench runsheet (S3 0.4.50 × Himax 0.4.20.62)

**Prepared:** 2026-11-09 evening (PT), from the two build reports
(`link-hardening-s3-build-2026-11-09.md`, `link-hardening-himax-build-2026-11-09.md`)
and the approved spec (`link-hardening-spec-2026-11-09.md`).
**Status:** Both artifacts BUILT AND VERIFIED from the artifacts themselves.
NOTHING has been flashed. Every flash below is its own gate.

## The one trap to know before you start

The Himax makefile's own "Verify fingerprint" line prints `0001020304050607` —
that is a lookup-table constant, NOT the build fingerprint. The real .62
fingerprint is **`fc0bac1be88797e1`** (boot DIAG line / strings on the ELF or
output.img). Do not reject the .62 flash for showing the wrong number in the
makefile line.

## Preflight (John's standing order — connectivity first)

1. Bridge health: `python3 ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/bridge_health.py` — exit 0 (subscriber alive + telemetry fresh) before ANY bench work.
2. Acquire the bench lock (`bench_lock.py`); hold it for the session. If busy, stop and report the holder.
3. Resync the seal replay floor on the bridge host before the first device command.
4. Bounce the device — the S3→Himax link has been wedged since the 22:39 UTC crop set; nothing re-runs S3 link init in the current boot.

## Flash order: does not matter

Neither side changes the wire protocol; any combination of
{0.4.49.1, 0.4.50} × {.61, .62} interoperates, and the link is safe to run
mixed between flashes. Suggested order is S3 first purely for practicality:
the 0.4.50 landing verifies in seconds via heartbeat (see below).

- **0.4.50 on .61:** wedges still occur at the old rate, but the ladder now recovers them in-session.
- **.62 on 0.4.49.1:** wedges become rarer (stuck-unarmed closed), but an unrecovered wedge still needs a bounce.

## Gate 1 — S3 fw 0.4.50 (needs John's flash go)

```bash
curl -L -o watcher-v0.4.50-merged.bin \
  "https://muse.ai/files/676619942205455/1889847318516036/5dc77bmplcdh5yzzhjq5jm7g/watcher-v0.4.50-merged.bin"
sha256sum watcher-v0.4.50-merged.bin
# Firmware build fingerprint (matches the DIAG line at boot):
# fc7e2e1ac672725a
# File hash (verifies the download — match against the sha256sum output above):
# 6e926729b0df30bc5de42839d31cea2fee8c4f858dad0e3a37ea68e1dd8c0776
```

⚠️ Download link expires **Nov 12, 00:30 UTC** — re-stage after that.

**Landing bar (all three, before calling it landed):**
- [ ] `fw-flashed` in telemetry for this transfer
- [ ] Heartbeat reports fw **0.4.50**
- [ ] Heartbeat carries the nested **`himax_link`** object (state / streak / gen / rec_drain / rec_reset / rec_rst / rec_rail / last_rung)

**Rollback:** kept 0.4.49.1 artifact. Trigger for rollback discussion: false-positive ladder recoveries on a healthy link.

## Gate 2 — Himax 0.4.20.62 (John's own run)

- Image: `sscma-fork/sscma-example-we2-main/we2_image_gen_local/output_case1_sec_wlcsp/output.img`
  — 622,592 B — sha256 `10f4ae0d706dc3cb9e35d409db2d8ec7964fdd4e039cc09df621c12ddcf3a130`
- Expected identity after flashing: FW ID `watcher-himax-0.4.20.62-spi`,
  fingerprint **`fc0bac1be88797e1`** (NOT the makefile's `0001020304050607` line — see trap above).
- Test hooks are OFF in the shipped binary (`WATCHER_LINK_TEST_HOOKS 0`, proven by hash identity against the hooks-OFF cleanbuild).
- **Rollback:** reflash the .61 image (sha256 begins `f1e5a0c3…`).

### .62 deviations John should know he is accepting (from the build report)

1. `spi_poll()` head check relaxed so Guard-3 is reachable while the transport is closed — without it, H-A(2) is dead code.
2. **H-B predicate gained an armed-state term (`!_spi_rx_armed`)** the spec's literal flag list lacks — a literal implementation would tear the link down every ~2 s in normal idle. Same intent, same 2 s bound.
3. H-A(1) also applied to `_spi_arm_rx_parked()` (identical silent early-return).
4. H-C arm readback implemented, but scoped to Guard 1's re-arm branch — the one arm in `SPI::init` is not readback-verified (residual boundary, noted not fixed).
5. Guard 2 teardown now also attempts an open when already closed (recovery-positive).

S3-side note already flagged in chat: remote `eyes_reset`'s "highest seal scope
tier" is implemented as the `_sealed` verified-envelope check — the shipped seal
has a single pinned commander key and no per-key tiers yet; the check sits where
a tier test plugs in later.

## Gate 3 — the soak (acceptance, per the approved spec)

- **Crop CLEAR before every set** — forces the full path on all 20 cycles.
- **20 full-path crop+capture cycles.**
- A wedge the ladder recovers mid-soak = **PASS-with-evidence** (the rung
  telemetry in `himax_link` — counters + last_rung — is the proof).
- An **unrecovered** wedge = fail.
- Watch during the soak: `himax_link` counters, supervisor task in
  `stack_hwm` (its stack sizing is explicitly deferred to this soak's data),
  and any `E DIAG STUCK_UNARMED` / `POLL_GAP` lines from .62 (POLL_GAP ≈ stall
  duration points at H2; no POLL_GAP with a wedge points at H1).

## After the soak (queued, in the synthesis order)

1. Strip-capture legibility retest + the precise mouse-find bonus ask — both were waiting on this bounce.
2. Audio bench proof (B6) on the hardened pair — offline side is fully closed (bridge 32/32, console 25/25, byte-identity 12/12; B3 fix landed as `bf15bd8`).
3. Round close-out commit naming the fw 0.4.49 + console 0.4.189 pairing, alongside the .61 pin (already landed: `9e34913` + `7643d1d`).

## Open design call (not blocking the test gate)

**D-V2 tombstone:** a duplicate of the final audio chunk arriving *after*
completion, followed by `audio_end`, resurrects a spurious 1-chunk fragment
WAV. Latent only — nothing double-feeds a take today. Fix = a completed-take
tombstone in the reassembler. John's call, whenever; the harness xfail stays
in place until then.
