#!/usr/bin/env python3
"""Check the Watcher MQTT bridge (subscriber.py) is alive and telemetry is fresh.

Usage: python3 bridge_health.py [--restart]
Exit 0 = healthy. Exit 1 = down (prints reason). With --restart, attempts to
restart a dead subscriber using ~/.config/watcher-mqtt.env.
"""
import os, subprocess, sys, time

BRIDGE = os.path.expanduser("~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge")
LOG = os.path.join(BRIDGE, "telemetry.log")
ENV = os.path.expanduser("~/.config/watcher-mqtt.env")
STALE_S = 180  # no log write in 3 min = suspicious (heartbeats land ~60s)

def sub_pids():
    out = subprocess.run(["pgrep", "-f", "subscriber.py"], capture_output=True, text=True)
    return [p for p in out.stdout.split() if p.strip()]

def main():
    pids = sub_pids()
    if not pids:
        print("BRIDGE DOWN: no subscriber.py process")
        if "--restart" in sys.argv:
            subprocess.Popen(
                ["bash", "-c", f"cd {BRIDGE} && set -a && source {ENV} && set +a && exec python3 subscriber.py >> /tmp/subscriber_restart.log 2>&1"],
                start_new_session=True)
            time.sleep(6)
            pids = sub_pids()
            print("restarted" if pids else "restart FAILED")
            return 0 if pids else 1
        return 1
    try:
        age = time.time() - os.path.getmtime(LOG)
    except OSError:
        print("BRIDGE DEGRADED: subscriber alive but telemetry.log missing")
        return 1
    if age > STALE_S:
        print(f"BRIDGE STALE: subscriber alive (pid {pids[0]}) but telemetry.log untouched for {int(age)}s")
        return 1
    print(f"BRIDGE UP: pid {pids[0]}, telemetry.log {int(age)}s old")
    return 0

sys.exit(main())
