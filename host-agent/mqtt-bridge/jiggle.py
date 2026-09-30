#!/usr/bin/env python3
"""Toggle the Watcher Console's page-side OS jiggler over MQTT.

Usage:
    MQTT_PASS='...' python3 jiggle.py on      # 1px, 60s (John's preference)
    MQTT_PASS='...' python3 jiggle.py off
    MQTT_PASS='...' python3 jiggle.py on --px 2 --interval 30

The page listens on <prefix>/jiggle_bridge for {"t":"jiggle_state",...}
and starts/stops its net-zero mouse_move pair timer. <prefix>/jiggle is
the page-owned pill state (single writer: the page); this script
publishes to the bridge-owned direction. "by" must not be "page"
(the page ignores its own echoes).
"""
import argparse
import json
import os
import sys
import time

import paho.mqtt.client as mqtt

from topic_acl import publish_checked

HOST = os.environ.get("MQTT_HOST", "mqtt.offx1.com")
PORT = int(os.environ.get("MQTT_PORT", "8883"))
USER = os.environ.get("MQTT_USER", "muse")
PASS = os.environ.get("MQTT_PASS", "")
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC") or "watcher"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("state", choices=["on", "off"])
    ap.add_argument("--px", type=int, default=1)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--subpixel", action="store_true")
    args = ap.parse_args()

    if not PASS:
        print("MQTT_PASS env required", file=sys.stderr)
        sys.exit(2)

    topic = f"{TOPIC_PREFIX}/jiggle_bridge"
    msg = {
        "t": "jiggle_state",
        "on": args.state == "on",
        "px": max(1, min(127, args.px)),
        "interval_s": max(5, min(3600, args.interval)),
        "subpixel": args.subpixel,
        "by": "bridge",
    }

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"muse-jiggle-{os.getpid()}")
    client.username_pw_set(USER, PASS)
    client.tls_set()
    client.connect(HOST, PORT, 30)
    client.loop_start()
    time.sleep(1.0)
    publish_checked(client, topic, json.dumps(msg), qos=0)
    print(f"published to {topic}: {json.dumps(msg)}")
    time.sleep(1.0)
    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
