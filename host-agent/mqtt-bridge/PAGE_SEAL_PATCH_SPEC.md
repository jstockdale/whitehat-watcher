# Page Patch Spec — WMS2 Seal Verification (FOR JOHN'S REVIEW)

**Do not apply until John approves.** Five insertions into
`watcher-console.html`, plus the vendored crypto bundle.

---

## Insert 1 — Crypto bundle (in `<head>`, before the main script)

Inline `/tmp/nobletest/noble_bundle.js` (38.8 KB minified, MIT:
@noble/curves + @noble/hashes) in its own `<script>` tag. It sets
`window.watcherSealCrypto = { ed25519, blake2b }`. No network fetch —
the page stays fully self-contained.

## Insert 2 — SEAL verifier module (main script, near the top with other modules)

Inline `host-agent/mqtt-bridge/page_seal_draft.js` verbatim (tested:
7/7 pass in Node against real Python-sealed envelopes — valid,
tamper→BAD_SIG, cross-topic→BAD_SIG, replay→silent-dup,
grace-honors-unsigned, strict-drops-unsigned, wrong-key→BAD_SIG).

## Insert 3 — `mqpOnPublish` command-topic branch (the 3-line change)

Current (line ~532):
```js
if(topic!==t.command)return;var msg;try{msg=JSON.parse(payload)}catch(_e){return}
var cmd=msg&&msg.cmd,id=msg&&msg.id;
```

New:
```js
if(topic!==t.command)return;
/* WMS2 seal gate: verified commands only (grace-honors unsigned until strict) */
var _sg=SEAL.guard(payload,topic,function(l){addLine(l)},function(e){sealTelemetryEvent(e)});
if(!_sg.ok||_sg.dup)return;   /* dup = QoS echo of already-processed command */
var cmd=_sg.cmd,id=_sg.id;
```

Everything downstream (cmdAllowed check, allowlist, serial dispatch)
is unchanged — it now operates on verified-or-grace-honored commands.

**Grace-mode parser caveat (Claude's review, 2026-09-25):** in grace
mode the JSON parser runs on *unauthenticated* input — the one
pre-auth parser surface in the system until strict flips. Keep the
grace path's accepted grammar to the trivial subset (bare
`{"t","cmd"}` commands already on the allowlist; no new body shapes),
and treat any parse anomaly as a drop, not a best-effort dispatch.

## Insert 4 — Setup tab UI ("Command authentication" section)

- Public-key paste field (64 hex chars) + fingerprint display
  (first 16 hex of SHA-256(pk), must read `c0ccdd20d4d6e89f` for the
  provisioned commander key `9bc9b461…ae9c43`).
- Grace/Strict toggle (default: grace). Persisted in localStorage.
  **Evidence-gated (Claude's runbook, 2026-09-25): the strict toggle
  is disabled until a key is actually pinned.** The flip is a
  deliberate ceremony, not a checkbox: sealed traffic and unsigned
  traffic share one topic, so a half-flipped pair fails one of two
  ways (device-strict + page-unsigned = self-DoS; page-sealed +
  device-grace = believed-but-absent enforcement). Flip once, at an
  announced moment, after bench validation passes.
- Replay high-water readout (`muse-01: N`) + reset button (confirm
  dialog; needed if the host ever loses its counter file).
- Seal event counters: verified / rejected tallies, **with
  BAD-SIG-on-arrival counted separately** — the page is the only
  party positioned to see broker misrouting in bridged mode, so the
  arrival-side rejection count is the observable (Claude's
  observability rule, 2026-09-25).
- **Rejection log in the UI (not just the console):** every named
  rejection string surfaces in this section, and the no-key case
  names the fix — "no commander key pinned; paste one under Setup
  → Command authentication" (the fleet's NO_PIN line taught its
  own fix; the browser UI promotes it from console to interface).
- Re-pasting the **identical** key is silent (compare-before-adopt;
  idempotent re-provisioning never alarms). Pasting a **different**
  key over an existing pin requires an explicit confirmed replace —
  "this changes who can command this device" — never a silent
  overwrite.

## Insert 5 — Seal status dot

Next to the MUSE pill: green = last command verified, amber = grace
(unsigned honored), red = rejection. Updates in `sealTelemetryEvent`.
**The topbar always shows seal state: mode (GRACE/STRICT) + magic
and algorithm + pinned fingerprint (or NONE)** — e.g.
`WMS2 · Ed25519 · fp c0cc…`. An operator never has to ask whether
the link is sealed — the fleet's `status` verb printed pin count
unconditionally, and the page header does the same. If the magic or
algorithm ever changes again, the UI says so before a human has to
notice.

## `sealTelemetryEvent(e)` helper (new, ~10 lines)

Publishes `{t:"seal_event", ...e}` on the telemetry topic so the host
side sees verifications/rejections in its own event stream.

---

## Provisioning (after patch ships)

1. John pastes `9bc9b4610d920c0cf1415ed47e81c6c450f79bc9b323f4a66810e67b92ae9c43`
   into Setup → fingerprint must show `c0ccdd20d4d6e89f`.
2. Page stays in grace: sealed commands verify silently, unsigned
   commands warn 1-in-64.
3. Bench validation: sealed ping → accepted; tampered → dropped
   loudly; replay → silent dup; then John flips to strict.

## Rollback

Delete the three insertions (or uncheck strict + clear the pinned
key to return to today's behavior). No firmware involved — page-only.
