#!/usr/bin/env python3
"""Flash the Watcher over MQTT (Muse side). PRIVILEGED.

RULE: John must approve in chat first — every single time. No exceptions.

The console only flashes the firmware .bin John himself selected in its
Flash tab, and only when its SHA-256 matches the argument below. There is
no way to push an arbitrary image: John controls which binary is cached.

Usage:
    MQTT_PASS='...' python3 send_flash.py <sha256> --confirm

Watches watcher/telemetry for up to 120s for the flash markers.
Credentials via env: MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS.
"""

import json
import os
import re
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
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC", "watcher")


def main() -> None:
    if len(sys.argv) != 3 or sys.argv[2] != "--confirm":
        print("usage: send_flash.py <sha256-of-Johns-cached-bin> --confirm")
        sys.exit(2)
    sha = sys.argv[1].lower()
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        print("refused: not a 64-char hex sha256")
        sys.exit(2)
    if not PASS:
        print("MQTT_PASS is not set")
        sys.exit(2)

    print("=" * 64)
    print("PRIVILEGED OPERATION: flashing the Watcher over MQTT.")
    print("Did John approve this exact flash in chat? If not, Ctrl-C now.")
    print("=" * 64)
    time.sleep(3)

    cmd_id = uuid.uuid4().hex[:8]
    telemetry_topic = f"{TOPIC_PREFIX}/telemetry"
    command_topic = f"{TOPIC_PREFIX}/command"
    status_topic = f"{TOPIC_PREFIX}/status"
    done = []
    status_info = {}

    def on_message(_c, _u, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        if msg.topic == status_topic and msg.retain and not status_info:
            status_info.update(payload if isinstance(payload, dict) else {})
            return
        for e in payload.get("events", []):
            raw = str(e.get("raw", ""))
            if cmd_id in raw:
                print(f"[{e.get('level','?')}] {raw[:220]}")
                if "flash complete" in raw or "flash failed" in raw \
                        or "flash rejected" in raw:
                    done.append(True)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"muse-flash-{cmd_id}")
    client.username_pw_set(USER, PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                   tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_message = on_message
    client.connect(HOST, PORT, keepalive=30)
    client.subscribe([(telemetry_topic, 0), (status_topic, 0)])
    client.loop_start()
    time.sleep(2.5)
    cmd_on = status_info.get("cmd", False)
    unattended = status_info.get("unattended", False)
    print(f"console status: cmd_channel={cmd_on} "
          f"unattended_flash={unattended}")
    if not cmd_on:
        print("refused: the Muse command channel is OFF on the console")
        sys.exit(4)
    if not unattended:
        print("UNATTENDED FLASHES DISABLED — you need John's explicit "
              "yes in chat for this flash. Ask with a yes/no prompt.")
    else:
        print("unattended flashes ENABLED — proceeding on your judgment.")
    envelope = json.dumps({"id": cmd_id,
                           "cmd": {"t": "flash", "sha256": sha}})
    rc = publish_checked(client, command_topic, envelope, qos=0).rc
    if rc != 0:
        print(f"publish failed rc={rc}")
        sys.exit(1)
    print(f"flash requested id={cmd_id} sha256={sha[:16]}...")
    print("--- watching for flash markers (120s) ---")
    deadline = time.time() + 120
    while time.time() < deadline and not done:
        time.sleep(1)
    client.loop_stop()
    client.disconnect()
    if not done:
        print("(no flash completion marker seen — check the console)")


if __name__ == "__main__":
    main()
