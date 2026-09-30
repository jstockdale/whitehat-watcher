# Firmware Update Path — Audit Notes (2026-09-24)

## The path, end to end

1. **Build**: `idf.py merge-bin` → `firmware/release/watcher_input_poc_vX.Y.Z_merged.bin`
   (factory at 0x20000 per partitions.csv auto-align; never hand-write esptool merge)
2. **Verify**: size, SHA-256, app magic `0xE9` at `0x20000`
3. **Transfer**: `send_firmware.py <bin> --confirm`
   - Publishes `{id, cmd:{t:fwbegin,...}}`, 474× `{t:fwchunk,seq,data:b64}`, `{t:fwfend}`
     to `<prefix>/command` (default prefix `watcher`)
   - Page reassembles, SHA-256 verifies → `fw-ready <transfer>` on `<prefix>/telemetry`
   - Page shows "Yes, flash it" prompt (unless unattended mode)
4. **Flash**: John clicks Yes → page writes via esptool-js, MD5-verifies, resets
5. **Confirm**: `fw-flashed <transfer>` on telemetry; device boots, reports new `fw` version

## The 2026-09-24 outage — root cause

**It was NOT a console regression.** Transfers worked on console v0.4.28 (21:36, 21:44),
then "broke" — but the breakage coincided with a change in MY shell wrapper, not John's page.

My wrapper did:
```bash
export MQTT_TOPIC=$(tr '\0' '\n' < /proc/$PID/environ | grep "^MQTT_TOPIC=" | cut -d= -f2)
```

The subscriber's environment has **no MQTT_TOPIC at all** (it defaults internally).
So `grep` returned nothing, and I exported `MQTT_TOPIC=""` (empty string).

Then in `send_firmware.py`:
```python
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC", "watcher")  # returns "" — NOT "watcher"!
```

`dict.get(key, default)` uses the default **only when the key is absent**.
An existing-but-empty value is returned as-is. So:
- command_topic became `/command` instead of `watcher/command`
- telemetry_topic became `/telemetry` instead of `watcher/telemetry`

The page (on `watcher/*`) never saw anything. The script happily sent 474 chunks
into the void and timed out waiting for a verdict.

### The tell

My probe published to `/command` (leading slash visible in output). The page's
topic is `watcher/command`. One look at that slash and the mismatch was obvious.

### The fix

```python
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC") or "watcher"
```

Applied to both `send_firmware.py` and `send_command.py`.
The `or` treats empty-string (and None) as unset. This is the correct pattern for
ALL env-var-with-default reads — `.get(k, default)` is a trap when the var can
exist-but-be-empty.

### Shell hygiene rule

When extracting env vars from `/proc/<pid>/environ`:
- If `grep` finds nothing, **do not export** the variable (or `unset` it).
- Never `export FOO=$(...)` blindly — an empty result poisons downstream `.get()` defaults.
- Current correct pattern: extract HOST/PORT/USER/PASS, then `unset MQTT_TOPIC`.

## What I verified about the page (v0.4.31) during the audit

All correct, no bugs found:
- `mqpOnPublish`: topic matching (`t.command` fresh from `mqttTopics()` each call) ✓
- CONNACK subscribes to command topic when `state.cmdAllowed` ✓
- `mqpSubPkt` builds a valid SUBSCRIBE packet ✓
- Restructured handler (split `topic!==t.command||!state.cmdAllowed`) is correct ✓
- `fw-declined` explicit rejections in place for both cmdAllowed=false and
  permission-disabled cases ✓
- `mqpFwBegin/mqpFwChunk/mqpFwEnd` + progress bar rendering untouched by recent edits ✓

## Diagnostic approach that worked

When the path is silent, probe it with a harmless read-only command:
```python
{'id': 'PROBE...', 'cmd': {'t':'dbg','cmd':'version'}}
```
- If the page logs `→ mqtt PROBE...`, the MQTT→page path works.
- If the device replies with `fw`/`idf`, the page→device→page path works.
- If the probe goes nowhere, check the TOPIC PREFIX first (print it!).

## Timeline of confusion (for future me)

- 22:20 transfer: sent 474 chunks, no fw-ready. I blamed the page / status pre-check.
- Removed the pre-check (correct simplification, but not the bug).
- Added `fw-declined` explicit rejections (good improvement, but not the bug).
- 22:31 transfer: still nothing. John: "page is here and ready but i don't see the firmware."
- I checked page version (0.4.31 ✓), handler code (✓), subscription (✓) — all fine.
- John: "i see no muse commands at all." → suggested the message never arrives.
- Probe test revealed `/command` vs `watcher/command` → found the empty MQTT_TOPIC.
- Fixed, re-probed ✓, transferred 67dad619 → `fw-ready` ✓.

**Lesson: when the page is fine and the script "works," check what topic the script
is actually publishing to. Print the full topic. The leading slash is the tell.**
