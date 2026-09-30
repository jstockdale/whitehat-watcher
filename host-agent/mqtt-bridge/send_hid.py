#!/usr/bin/env python3
"""Send HID mouse moves. Usage: python3 send_hid.py <dx> <dy>"""
import os, re, subprocess, json, sys

with open('/proc/4158/cmdline', 'rb') as f:
    cmdline = f.read().decode()
m = re.search(r"MQTT_PASS='([^']+)'", cmdline)
if not m:
    print("FAIL: credential not found", file=sys.stderr)
    sys.exit(1)
os.environ['MQTT_PASS'] = m.group(1)

dx, dy = int(sys.argv[1]), int(sys.argv[2])
cmd = json.dumps({"t": "mouse_move", "dx": dx, "dy": dy})
r = subprocess.run(['python3', 'send_command.py', cmd],
    capture_output=True, text=True, env={**os.environ}, timeout=20)
# print only the ACK line, not full output
for line in r.stdout.split('\n'):
    if 'ack' in line.lower() or 'mouse' in line.lower() or 'reply' in line.lower():
        print(line[-200:])
