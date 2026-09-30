#!/usr/bin/env python3
"""Take a fresh Himax snapshot and save to snapshots/.

Uses credentials from ~/.config/watcher-mqtt.env (the documented
bridge credential file). Sends snaptest, collects chunks, saves JPEG.

Usage:
    python3 take_snap.py [--out <path>]
"""
import base64
import json
import os
import ssl
import sys
import time
from datetime import datetime

import paho.mqtt.client as mqtt

from topic_acl import publish_checked

# load credentials from the documented bridge env file
ENV_FILE = os.path.expanduser("~/.config/watcher-mqtt.env")
env = {}
with open(ENV_FILE) as f:
    for line in f:
        line = line.strip()
        if line and "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip("'\"")

HOST = env.get("MQTT_HOST", "mqtt.offx1.com")
PORT = int(env.get("MQTT_PORT", "8883"))
USER = env.get("MQTT_USER", "muse")
PASS = env.get("MQTT_PASS", "")
PREFIX = env.get("MQTT_TOPIC", "watcher")

OUT_DIR = os.path.expanduser(
    "~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/snapshots")


def main():
    out_path = None
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]
    if not out_path:
        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
        out_path = os.path.join(OUT_DIR, f"snap-{ts}-0.jpg")

    chunks = {}
    total = None
    snap_id = None
    done = False

    def on_message(_c, _u, msg):
        nonlocal total, snap_id, done
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        for e in payload.get("events", []):
            raw = e.get("raw", "")
            try:
                m = json.loads(raw) if isinstance(raw, str) else raw
            except Exception:
                continue
            if not isinstance(m, dict):
                continue
            if m.get("t") == "snaptest_chunk":
                sid = m.get("id", 0)
                if snap_id is None:
                    snap_id = sid
                if sid != snap_id:
                    continue
                seq = m.get("seq", 0)
                total = m.get("total", total)
                chunks[seq] = base64.b64decode(m.get("b64", ""))
                if m.get("last"):
                    done = True

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id="muse-takesnap")
    client.username_pw_set(USER, PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                   tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_message = on_message
    client.connect(HOST, PORT, keepalive=30)
    client.subscribe([(f"{PREFIX}/telemetry", 0)])
    client.loop_start()
    time.sleep(1.0)

    # send snaptest command
    import uuid
    cmd_id = uuid.uuid4().hex[:8]
    envelope = json.dumps({"id": cmd_id,
                           "cmd": {"t": "dbg", "cmd": "snaptest", "send": 1}})
    publish_checked(client, f"{PREFIX}/command", envelope, qos=1)
    print(f"sent snaptest cmd {cmd_id}, waiting for chunks...")

    deadline = time.time() + 60
    while time.time() < deadline and not done:
        time.sleep(0.5)

    client.loop_stop()
    client.disconnect()

    if not chunks:
        print("no chunks received")
        sys.exit(1)

    # reassemble in order
    data = b"".join(chunks[i] for i in sorted(chunks.keys()))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"saved {out_path} ({len(data)} bytes, {len(chunks)} chunks)")


if __name__ == "__main__":
    main()
