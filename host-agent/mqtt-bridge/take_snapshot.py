#!/usr/bin/env python3
"""Take a snapshot: recover MQTT cred transiently, send snaptest, capture."""
import os, re, subprocess, json, time, sys

# recover credential from launcher cmdline (transient, never printed/logged)
with open('/proc/4158/cmdline', 'rb') as f:
    cmdline = f.read().decode()
m = re.search(r"MQTT_PASS='([^']+)'", cmdline)
if not m:
    print("FAIL: credential not found", file=sys.stderr)
    sys.exit(1)
os.environ['MQTT_PASS'] = m.group(1)

# start capture listener
listener = subprocess.Popen(['python3', 'snaptest_capture.py'],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(1)

# send snapshot command
cmd = json.dumps({'t': 'dbg', 'cmd': 'snaptest', 'send': 1})
r = subprocess.run(['python3', 'send_command.py', cmd],
    capture_output=True, text=True, env={**os.environ}, timeout=30)
print("--- send output ---")
print(r.stdout[-600:])

try:
    out, _ = listener.communicate(timeout=40)
    print("--- capture output ---")
    print(out[-600:])
except subprocess.TimeoutExpired:
    listener.kill()
    print("capture listener timed out", file=sys.stderr)
