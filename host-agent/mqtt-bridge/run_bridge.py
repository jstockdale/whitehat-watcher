#!/usr/bin/env python3
"""Watcher MQTT bridge launcher — improved credential handling.

- Reads MQTT_PASS from ~/.config/watcher-mqtt/mqtt_pass (600 perms).
- Never places the credential in a process command line.
- Logs to the bridge directory (persistent, not /tmp).
- Run: python3 run_bridge.py [start|stop|restart|status]
"""
import os
import signal
import subprocess
import sys
import time

BRIDGE = os.path.expanduser(
    "~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge")
CREDFILE = os.path.expanduser("~/.config/watcher-mqtt/mqtt_pass")
LOGFILE = os.path.join(BRIDGE, "subscriber.log")
PIDFILE = os.path.join(BRIDGE, "subscriber.pid")


def get_pass():
    with open(CREDFILE) as f:
        return f.read().strip()


def pid_running(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def get_pid():
    try:
        with open(PIDFILE) as f:
            pid = int(f.read().strip())
        return pid if pid_running(pid) else None
    except (OSError, ValueError):
        return None


def do_start():
    if get_pid():
        print("bridge already running")
        return
    password = get_pass()
    env = dict(os.environ)
    env["MQTT_HOST"] = "mqtt.offx1.com"
    env["MQTT_PORT"] = "8883"
    env["MQTT_USER"] = "muse"
    env["MQTT_PASS"] = password
    logf = open(LOGFILE, "a")
    # Setsid so it survives; no credential in argv.
    p = subprocess.Popen(
        [sys.executable, os.path.join(BRIDGE, "subscriber.py")],
        cwd=BRIDGE, env=env,
        stdout=logf, stderr=subprocess.STDOUT,
        start_new_session=True)
    with open(PIDFILE, "w") as f:
        f.write(str(p.pid))
    time.sleep(2)
    if pid_running(p.pid):
        print(f"bridge started pid={p.pid} log={LOGFILE}")
    else:
        print("bridge failed to start, check log")
        sys.exit(1)


def do_stop():
    pid = get_pid()
    if not pid:
        print("bridge not running")
        return
    os.kill(pid, signal.SIGTERM)
    time.sleep(2)
    if pid_running(pid):
        os.kill(pid, signal.SIGKILL)
    try:
        os.unlink(PIDFILE)
    except OSError:
        pass
    print("bridge stopped")


def do_status():
    pid = get_pid()
    print(f"running pid={pid}" if pid else "not running")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "start"
    if cmd == "start":
        do_start()
    elif cmd == "stop":
        do_stop()
    elif cmd == "restart":
        do_stop()
        do_start()
    elif cmd == "status":
        do_status()
    else:
        print("usage: run_bridge.py [start|stop|restart|status]")
        sys.exit(2)
