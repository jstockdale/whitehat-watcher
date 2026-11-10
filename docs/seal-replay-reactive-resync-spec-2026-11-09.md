# Seal REPLAY-reactive resync — spec (2026-11-09)

Status: SPEC ONLY. No host tooling changed. Prepared by the
background research pass after the third live instance of the
failure (below); implementation waits on John's go, like the
console-manifest spec.

## The failure, three times

`seal_client.auto_resync()` syncs the CLI counter from the freshest
telemetry-advertised floor (`latest_telemetry_floor()`, a regex over
the log tail). That floor is the device's *persisted/boot* floor.
Two mechanisms make the device's **live** high-water higher than
anything telemetry advertises:

1. **Crash-reboot persist step.** The verifier floor jumps by the
   NVS persist stride (+32) on reboot, while telemetry floor lines
   stay at the pre-crash value. 2026-11-09 morning: device floor
   77571, telemetry still reading 77070 — two sealed sends silently
   dropped as REPLAY before a manual resync.
2. **The page's separate counter store.** The console page seals
   under the same `muse-01` key from its own counter; its connect
   traffic advances the device high-water without moving the CLI
   counter or the telemetry floor. 2026-11-09 evening: ping dropped
   with the device saying so — `SEAL: REPLAY ctr 78456 <= 78485` —
   while the telemetry floor read 78199. Manual `--resync 78600`
   fixed it instantly. (Same shape as the first recorded instance,
   2026-11-02: device floor 63726 vs CLI counter 63697.)

In every case the information needed to heal was already in the
telemetry stream: the REPLAY line itself names the live high-water.
Nothing parses it.

## Proposal

In `seal_client.py`:

1. Add a `_REPLAY_RE` over the same telemetry tail scan, matching
   `SEAL: REPLAY ctr <sent> <= <live>` and extracting `<live>` as a
   floor candidate.
2. `auto_resync()` takes `floor = max(advertised_floor,
   latest_replay_highwater)` when both exist. Forward-only semantics
   are unchanged (`resync_counter` still refuses backward moves, so
   a stale REPLAY line in the tail can never drag the counter down).
3. Headroom: when the binding candidate came from a REPLAY line,
   resync to `live + margin` rather than `live + 1` — the page may
   seal again between our scan and our send. Manual practice used
   +115 (78485 → 78600). Proposed margin: **+128**, one line for
   John to rule on.
4. `seal_command()` already auto-resyncs before every send, so this
   heals reactively on the *next* send after a drop — the dropped
   command itself is still lost (the device gives no retry signal),
   but the session stops bleeding: one drop per desync event
   instead of silent drops until a human reads the log.
5. Log the source of the binding floor (`advertised` vs `replay`)
   in the existing `resync` field of the seal result, so the heal
   is visible in telemetry forensics.

## Explicitly not proposed

- No change to the device firmware, the seal scheme, or the page.
  The page advancing a shared key's high-water from a separate
  store is the deeper design smell; unifying the counter stores is
  a bigger conversation (and touches John's PKI rulings) — this
  spec only makes the CLI side recover automatically from the
  symptom, as it already does for the advertised floor.
- No polling daemon: the scan rides the existing per-send
  auto-resync path.

## Test sketch (for the implementation pass)

- Unit: synthetic telemetry tails — advertised floor only, REPLAY
  line only, both (REPLAY higher / advertised higher), stale REPLAY
  below current counter (must no-op), malformed lines ignored.
- Live: after the next natural desync (page connect + crash
  reboot), first CLI send drops, second send succeeds with a
  `replay`-sourced resync in its result — no manual step.

*Decision left to John: build as specced (margin +128), adjust the
margin, or leave manual resync as the accepted runbook.*
