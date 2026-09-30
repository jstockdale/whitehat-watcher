#!/usr/bin/env python3
"""Query the console for fresh fiducial telemetry on demand.

Sends {"t":"report_fiducials"} to watcher/command; the page (v0.4.53+)
replies on watcher/telemetry with:
    fiducials <cmd-id> {"vw":..,"vh":..,"sx":..,"sy":..,"marks":{...}}

Requires: console page v0.4.53+, command channel ON, report_fiducials
allowed (auto-enabled in allowlist v4).

Usage:
    MQTT_PASS='...' python3 report_fiducials.py [--json] [--no-seal]
The query is sealed (WMS2) by default; --no-seal sends unsigned (dropped
by the console in strict mode).
"""
import json
import os
import ssl
import sys
import time
import uuid

import paho.mqtt.client as mqtt

from topic_acl import publish_checked

HOST = os.environ.get("MQTT_HOST", "mqtt.offx1.com")
PORT = int(os.environ.get("MQTT_PORT", "8883"))
USER = os.environ.get("MQTT_USER", "muse")
PASS = os.environ.get("MQTT_PASS", "")
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC") or "watcher"


def main():
    if not PASS:
        print("MQTT_PASS is not set")
        sys.exit(2)
    use_seal = "--no-seal" not in sys.argv
    if use_seal:
        try:
            import seal_client
        except FileNotFoundError as e:
            print(f"seal key not found ({e}); run seal_keygen.py once, "
                  f"or use --no-seal for grace-mode unsigned send")
            sys.exit(2)
        except Exception as e:
            print(f"sealing unavailable: {e}")
            sys.exit(2)
    cmd_id = uuid.uuid4().hex[:8]
    telemetry_topic = f"{TOPIC_PREFIX}/telemetry"
    command_topic = f"{TOPIC_PREFIX}/command"
    result = {}

    def on_message(_c, _u, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        for e in payload.get("events", []):
            raw = str(e.get("raw", ""))
            if raw.startswith(f"fiducials {cmd_id} "):
                try:
                    result["info"] = json.loads(raw[len(f"fiducials {cmd_id} "):])
                    result["id"] = cmd_id
                except Exception as ex:
                    result["error"] = f"bad json: {ex}"

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"muse-fidq-{cmd_id}")
    client.username_pw_set(USER, PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                   tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_message = on_message
    client.connect(HOST, PORT, keepalive=30)
    client.subscribe([(telemetry_topic, 0)])
    client.loop_start()
    time.sleep(1.0)
    cmd = {"t": "report_fiducials"}
    if use_seal:
        sealed = seal_client.seal_command(cmd_id, cmd, command_topic)
        envelope = json.dumps({"sealed": sealed["sealed"]})
        print(f"sealed (ctr={sealed['ctr']})")
    else:
        envelope = json.dumps({"id": cmd_id, "cmd": cmd})
    publish_checked(client, command_topic, envelope, qos=1)
    deadline = time.time() + 15
    while time.time() < deadline and "info" not in result \
            and "error" not in result:
        time.sleep(0.5)
    client.loop_stop()
    client.disconnect()

    if "--json" in sys.argv:
        print(json.dumps(result.get("info", result), indent=1))
        return
    info = result.get("info")
    if not info:
        print("no fiducial report received (page < v0.4.53? "
              "command channel off? report_fiducials not allowed?)")
        sys.exit(1)
    marks = info.get("marks", {})
    print(f"viewport {info.get('vw')}x{info.get('vh')} "
          f"scroll ({info.get('sx')},{info.get('sy')})")
    print(f"marks: {len(marks)}")
    for cid in ("tl", "tr", "bl", "br"):
        m = marks.get(cid)
        if m:
            print(f"  {cid}: screen=({m['x']},{m['y']}) size={m.get('size')}")
        else:
            print(f"  {cid}: --")


if __name__ == "__main__":
    main()
