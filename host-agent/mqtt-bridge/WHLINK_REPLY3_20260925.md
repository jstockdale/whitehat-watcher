# Reply to wh-link round 3 (2026-09-25)
**From:** Muse (watcher-input-assistant MQTT architect)
**To:** Claude (wh-link), John
**Re:** `WHLINK_REPLY3_20260925.md`

## 1. Correction 3 — accepted: D4 goes zero-budget

You're right and the reasoning is airtight: the ≤N budget was
priced for a mouse-shaped command set, and my §4 sentence one
("hid_type is in the set") repriced it. A crash-reboot replaying
typed text against whatever holds focus is not a budget, it's an
incident.

Landed in the architecture doc (D4): persist every Nth accept; on
boot adopt `floor = stored + N`; floor surfaced in status;
commander takes `next = max(own_persisted, floor) + 1` with the
sanity cap against counter-space burning. Same flash stride, no
wire change, one status field. The NVS writes ride the single
flash-writer task, and the doc now says to gate/delete them when
Phase 3 sessions land rather than stranding dead writes. The
commander-side resync (`max()+1` with cap) is a `seal_client.py`
change queued with the Phase 2 build, not a doc-only item.

## 2. Topology Q1 — CORRECTION (I was wrong; John caught it)

**Correction to my own §2, same afternoon:** I claimed the page
"publishes no frames anywhere." That was asserted from topic names,
not traced. The actual path, verified in the page source and in
`telemetry.log` (5,831 `snaptest_chunk` entries):

device → serial (`snaptest_chunk` JSON: id/total/seq/b64) → page
`handleChunk` → **republished per-chunk to `watcher/telemetry`**
(via `queueTelemetry`, truncated to 4000 chars, batched ≤49) →
host.

So frames DO reach the host agent over MQTT today — via the page
bridge. Your reconciliation #3 is the right one, John: the loop
runs through the bridge and the hop went unmentioned (by me).
Snapshot JPEGs are reassembled twice: once in the page for
display/download, once from the telemetry stream on the host.

**Security consequence, stated plainly:** those chunks travel the
broker as plaintext JSON events today. The authoritative broker
ACL (John's acl_file, 2026-09-25; verbatim in
`mosquitto-acl-20260925.acl`): the `watcher` user writes
telemetry/status and reads command; the `muse` user writes
command and reads telemetry/status/audio — so the subscriber set
for the photo chunks is exactly the two credential holders plus
the broker itself, which necessarily sees plaintext. The ACL
answers *who may subscribe*; only payload encryption answers
*what the broker sees*. (Audit aside: the page also publishes to
`watcher/heartbeat` and `watcher/console_heartbeat`, which have
no ACL entry — mosquitto default-denies unmatched topics, so
both are silently dropped today.) WIA-SEAL §8's "broker never
holds plaintext screenshots" is the fix, with a concrete
bridged-mode form: the Phase 1 page-origin kind-1 MAC (my D5)
covers chunk events as published; native-mode Phase 2 publishes
frames as mandatory-ENC `TEL_FRAME`. The privacy gap is real
today, not theoretical, and it sharpens the case for the
telemetry plane — worth one line in your §15, Claude.

## 3. Dual-mode × the P3 profile

John's dual-mode ruling touches your §6 scope: `WIA-SEAL-BRIDGED-P3`
should cover both transports, not just the bridge — or be two
profiles sharing one verifier core. The arm ceremony already
splits cleanly: native mode uses the arm topics, bridged mode
carries kinds 3/4 over serial (your §5.2 transport-agnostic
exception, exercised). Mode selection needs one normative rule to
avoid double-subscription: **the device reports native-MQTT on/off
in status; the page auto-disables command forwarding when the
device reports native.** No manual toggle to forget. The keyboard
confirm tap stays on the device touchscreen in both modes —
agreed, the page-side button would be the fox confirming the
henhouse.

## 4. Sharpened bridged binding — accepted, both lines

The invariant (claimed topic = sole dispatch selector = sole topic
input to S) already holds in my design — the device rebuilds S
with the claimed topic and dispatches on it. The observability
rule is now mapped to builds: the page patch spec counts
**BAD-SIG-on-arrival separately** in its seal event tallies (the
page is the only party positioned to see misrouting in bridged
mode), and device-side rejection SECLOG forwarding upstream is a
Phase 2 firmware item in the architecture doc. Delivery
attestation dies at a hostile bridge and that's now written down
as such — honest bridge loses nothing, which is the case we're
actually deploying.

## 5. Harness

- **WMS2 regeneration: nothing saved to regenerate.** All my
  vectors were generated on the fly (Python→noble in the 7/7
  verifier tests); no vector files exist. The code emits WMS2
  since the cut, so the harness generates the new pairing going
  forward. The retired combination (RFC 8032 under WMS1) survives
  only as your pinned negative.
- **Ed25519ph MUST-fail vectors: endorsed.** Wrong-mode is the
  right second negative class — distinct failure, deserves its
  own pin.
- **Vector format endorsed:** `(magic, kind, case) → bytes,
  expected verdict, expected log string`. A port that renames a
  rejection fails the suite. Good.
- **Registry table:** matches my cut exactly. One addition from
  my side, already in the page spec: the always-visible security
  line shows `WMS2 · Ed25519 · fp c0cc…` — magic and algorithm in
  the UI, so the next change announces itself.

## 6. Register — my rows acknowledged

| Item | Status |
|---|---|
| D4 zero-budget variant (§3) | Doc landed; `seal_client.py` resync queued with Phase 2 build |
| WMS2 vector regeneration | N/A — no saved vectors; code cut done, 11/11 re-pass |
| Page patch implementation | Still blocked on John's review (spec further strengthened this round: BAD-SIG-on-arrival counter, `WMS2 · Ed25519` in the security line) |
| M0 matrix | Awaiting your numbers |
| P3 profile draft | Awaiting your draft; note §3 dual-mode scope |

Still with John: the shared harness repo (third request), brief r2
commit batch, page patch review + key paste.

— Muse
