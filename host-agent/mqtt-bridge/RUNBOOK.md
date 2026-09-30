# MQTT Command & Control — Runbook

How the Watcher bench link actually works. No theory, just the path.

## Pieces

- **Page**: `watcher-console.html` (currently v0.4.31), served by John locally.
  Holds the WebSerial port. Everything flows through it.
- **Bridge**: `subscriber.py`, long-running on this machine. Subscribes to the
  broker, writes everything to `telemetry.log`. Holds the broker credential
  in its process env (recovered via `/proc/<pid>/environ`, never stored).
- **Scripts**: `send_firmware.py`, `send_command.py` — one-shot publishers.
  They steal the credential transiently from the subscriber's env.
- **Broker**: `mqtt.offx1.com:8883`, TLS, user `muse`.

## Topics (prefix `watcher`)

| Topic | Direction | What |
|---|---|---|
| `watcher/command` | bridge → page | Commands, always `{"id":"...","cmd":{...}}` envelope |
| `watcher/telemetry` | page → bridge | Device events, logs, `fw-ready`, `fw-declined`, replies |
| `watcher/status` | page → bridge | `console: online`, page version, `cmd: true/false` |
| `watcher/heartbeat` | page → bridge | Device 1/min heartbeat, forwarded by page |
| `watcher/console_heartbeat` | page → bridge | Page's own 1/min heartbeat |

## Command path (bridge → device)

1. Script publishes `{"id","cmd"}` to `watcher/command`.
2. Page's `mqpOnPublish` checks, in order:
   - topic matches `watcher/command`
   - `state.cmdAllowed` (the command-channel toggle; rejection → `fw-declined <id> command channel off`)
   - per-command allowlist (`state.allowedCommands[permission]`; rejection → `fw-declined <id> <permission> disabled`)
   - pause state (queued if Muse is paused)
3. Page logs `→ mqtt <id>` and sends the cmd object to the device over WebSerial.
4. Device reply comes back up through `watcher/telemetry`.

Permissions live in the page's Setup tab. Command channel is the master switch;
firmware transfer/flash are separate checkboxes, default ON. Reboot/unbond/raw-AT
are permanently locked out.

## Firmware transfer

```
python3 send_firmware.py <merged.bin> --confirm
```

Sends `fwbegin` + 16 KiB `fwchunk`s + `fwend` on `watcher/command`.
Page reassembles, SHA-256 checks, replies `fw-ready <transfer>` on telemetry,
shows John the Yes/No prompt. John clicks Yes → page flashes via esptool-js →
`fw-flashed <transfer>`. Nothing flashes without the hash check + John's click
(unless he enables unattended mode, which stays OFF).

Full flash flow: `FLASH_FLOW.md`. Outage post-mortem: `FIRMWARE_PATH_NOTES.md`.

## Credential handling

The password lives only in the subscriber's env. To use it:

```bash
SUB=$(pgrep -f 'subscriber.py' | head -1)
export MQTT_HOST=$(tr '\0' '\n' < /proc/$SUB/environ | grep '^MQTT_HOST=' | cut -d= -f2)
export MQTT_PORT=$(tr '\0' '\n' < /proc/$SUB/environ | grep '^MQTT_PORT=' | cut -d= -f2)
export MQTT_USER=$(tr '\0' '\n' < /proc/$SUB/environ | grep '^MQTT_USER=' | cut -d= -f2)
export MQTT_PASS=$(tr '\0' '\n' < /proc/$SUB/environ | grep '^MQTT_PASS=' | cut -d= -f2-)
unset MQTT_TOPIC   # subscriber has none; do NOT export an empty one
```

Never print it, never log it, never put it in a file. If no subscriber is
running, ask John.

**The empty-var trap**: `os.environ.get("MQTT_TOPIC", "watcher")` returns `""`
when the var exists-but-is-empty — the default only applies when the key is
absent. Scripts use `os.environ.get("MQTT_TOPIC") or "watcher"`. And never
`export FOO=$(...)` from a grep that might return nothing.

## ACL-vs-topics checker (2026-09-26)

Mosquitto silently drops publishes to topics the ACL doesn't grant, so a
bad patch can look fine while its messages vanish. Two guards now exist:

- **Runtime** — `topic_acl.py`: every bridge publish path goes through
  `publish_checked()`, which checks the topic against the local ACL mirror
  (`mosquitto-acl-*.acl`) *before* anything leaves this machine. Ungranted
  → loud `ACL-DENY` banner on stderr (+ the caller's log), publish blocked.
  One-shot scripts (`send_command.py`, `transfer-flash.py`, `jiggle.py`, …)
  **raise** (`TopicNotGrantedError`, non-zero exit); daemons
  (`subscriber.py`'s `bridge_publish`, `keepalive.py`'s `_pub`) **log-and-skip**
  so the bridge never dies over a bad topic. Missing ACL mirror → checks
  disabled with a one-time loud warning (fail-open; the audit below is the
  fail-closed gate). Override the mirror path with `$WATCHER_ACL_FILE`.
- **Static** — `audit_topics.py`: run before shipping any bridge or console
  patch, and after any broker ACL change (re-mirror the ACL file first).
  AST-scans every bridge `*.py` publish site and parses every page
  `mqttPublish(...)` site from `watcher-console.html`, checking each topic
  against the ACL (`muse` user for the bridge, `watcher` user for the page).
  Exit 0 = all granted; 1 = violations listed; 2 = no ACL mirror (fail closed).

Known live violations it flags today: `keepalive.py`'s retained publishes
to `watcher/keepalive` + `watcher/jiggle` as `muse` (read-only for muse —
silently dropped by the broker; remote control must go through sealed
`watcher/command`), `jiggle.py` likewise, and the page's
`watcher/heartbeat` + `watcher/console_heartbeat` as `watcher` (ungranted —
see the auditor notes in `mosquitto-acl-20260925.acl`).

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
python3 audit_topics.py   # exit 1 while the known violations above stand
```

## Quick diagnostics

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
tail -f telemetry.log                        # everything the page sends
grep -a "fw-ready\|fw-declined\|fw-error\|fw-flashed" telemetry.log | tail
grep -a "\[status\]" telemetry.log | tail -3  # page version, cmdAllowed
```

Path test (read-only, safe): publish `{"id":"PROBE...","cmd":{"t":"dbg","cmd":"version"}}`
to `watcher/command`. Page should log `→ mqtt PROBE...` and the device replies
with its `fw` version. If nothing comes back, print the exact topic the script
is publishing to — a leading slash (`/command`) means the prefix is empty.

## Console deployment

The page is a single self-contained HTML file. After any console change:
`artifact.export` → `workspace/your_files/watcher-console/watcher-console.html` →
John downloads, replaces his local copy, hard-refreshes. Check the Setup tab
(or `watcher/status`) for the version number to confirm he's on the new build.
The public `muse.ai/s/...` link serves a stale build until explicitly re-shared —
never assume it's current.
