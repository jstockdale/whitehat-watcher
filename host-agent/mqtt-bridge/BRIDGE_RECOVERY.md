# MQTT Bridge Recovery

## Robust Setup (Recommended)

The subscriber now supports auto-restart via systemd. Set it up once:

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
./setup-subscriber.sh
# Enter the MQTT password when prompted (user=muse)
```

This creates:
- `~/.config/watcher-mqtt.env` (600 perms) - credentials file
- `~/.config/systemd/user/watcher-subscriber.service` - systemd service

The service auto-restarts on failure and on boot. Check status:
```bash
systemctl --user status watcher-subscriber.service
tail -f ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/subscriber.log
```

## Manual Recovery (if systemd not set up)

If the `subscriber.py` bridge process dies (e.g. service restart), all
MQTT command capability is lost. The `MQTT_PASS` credential lives ONLY
in the subscriber's process environment — it is never stored on disk
(unless you used setup-subscriber.sh above).

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
export MQTT_HOST=mqtt.offx1.com MQTT_PORT=8883 MQTT_USER=muse
export MQTT_PASS='...'
nohup python3 subscriber.py > subscriber.log 2>&1 &
```

Verify it's up:
```bash
pgrep -f subscriber.py
# Then test:
python3 -c "from eyes_fast import EyesLink; l=EyesLink(); l._send_cmd({'t':'hid_status'}); import time; time.sleep(5); l.close()"
# Check telemetry.log for: {"t":"hid_status","connected":true,...}
```

## Prevention
- Run subscriber under a process supervisor (systemd user service, or
  a simple `while true` loop) so it auto-restarts.
- The credential must come from a secure source John controls
  (EnvironmentFile with 600 perms, or a secrets manager).
- Muse MUST NOT store MQTT_PASS in memory, files, or logs.

## Auto-recovery (cron watchdog, added 2026-10-04)

No systemd user bus exists in this environment, so the systemd path above
can't work here. Instead:

- `bridge-watchdog.sh` (same dir): revives `subscriber.py` if the process
  is gone, sourcing `~/.config/watcher-mqtt.env` — the same file used for
  manual restarts. Exits quietly when the bridge is alive; appends
  restarts to `watchdog.log`; never prints credentials. Uses an anchored
  pgrep pattern (`^python3 subscriber\.py`) and a flock guard so it can't
  double-start.
- Cron `bridge-watchdog` (interval 1m): runs the script, stays quiet on
  no-op, verifies a restart via pgrep + subscriber.log tail.
- Per John's standing rule (2026-09-30): the bridge should just come up
  automatically when we're on the bench together. subscriber.py died twice
  silently on 2026-10-04 with no traceback; the revive path is log-verified
  on paper only — check `watchdog.log` if it ever fires for real.

## Keep-alive Contract (John's rule 2026-09-23)
Once the bridge is up, run:
```bash
python3 keepalive.py --practice
```
This moves the mouse at least every 90s (net-zero keep-alive when
idle, strict locate+hop when practicing). The screen will not sleep.

## What Muse fixed 2026-09-23
- `keepalive.py`: new daemon, 90s max gap, --practice mode
- `eyes_fast.py`: fixed `locate_cursor_strict` docstring (returns 4 values)
- `mouse_move(journal=False)`: back_out no longer clobbers journal
- Servo scale: ignores <4px displacement, clamps to [0.05, 2.0]
