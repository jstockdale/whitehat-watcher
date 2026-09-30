#!/usr/bin/env python3
"""Send a firmware .bin to the Watcher console over MQTT, in chunks.

RULE: announce in chat and get John's go-ahead before running this.
Transferring bytes is harmless on its own — nothing flashes without the
page's SHA-256 check plus John's Yes in the page prompt (or the unattended
toggle he explicitly enabled). This script still refuses without --confirm.

Protocol (all on watcher/command):
  {"t":"fwbegin","transfer":id,"name":..,"size":N,"sha256":..,"chunks":C}
  {"t":"fwchunk","transfer":id,"seq":i,"data":"<base64>"}   (16KB chunks)
  {"t":"fwend","transfer":id}
The page NACKs missing chunks via telemetry ("fw-missing <id> s,s,..");
this script resends them and re-sends fwend.

Usage:
    MQTT_PASS='...' python3 transfer-flash.py <firmware.bin> --confirm [--no-seal]
Commands are sealed (WMS2) by default; --no-seal sends unsigned (dropped
by the console in strict mode).
"""

import base64
import hashlib
import json
import math
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
TOPIC_PREFIX = os.environ.get("MQTT_TOPIC") or "watcher"
CHUNK = 16384


def main() -> None:
    args = sys.argv[1:]
    use_seal = True
    if "--no-seal" in args:
        use_seal = False
        args = [a for a in args if a != "--no-seal"]
    if len(args) != 2 or args[1] != "--confirm":
        print("usage: transfer-flash.py <firmware.bin> --confirm [--no-seal]")
        sys.exit(2)
    path = args[0]
    if not PASS:
        print("MQTT_PASS is not set")
        sys.exit(2)
    if use_seal:
        try:
            import seal_client  # noqa: F401  (used in pub() below)
        except FileNotFoundError as e:
            print(f"seal key not found ({e}); run seal_keygen.py once, "
                  f"or use --no-seal for grace-mode unsigned send")
            sys.exit(2)
        except Exception as e:
            print(f"sealing unavailable: {e}")
            sys.exit(2)
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        print("refused: empty file")
        sys.exit(2)
    sha = hashlib.sha256(data).hexdigest()
    n_chunks = math.ceil(len(data) / CHUNK)
    transfer = uuid.uuid4().hex[:8]
    name = os.path.basename(path)

    print("=" * 64)
    print("PRIVILEGED OPERATION: sending firmware to the Watcher console.")
    print(f"  file: {name} ({len(data)} bytes, {n_chunks} chunks)")
    print(f"  sha256: {sha}")
    print("Nothing flashes without the page's hash check + John's Yes")
    print("(or his unattended toggle). Did John approve this? Ctrl-C now.")
    print("=" * 64)
    time.sleep(3)

    telemetry_topic = f"{TOPIC_PREFIX}/telemetry"
    command_topic = f"{TOPIC_PREFIX}/command"
    status_topic = f"{TOPIC_PREFIX}/status"
    muse_topic = f"{TOPIC_PREFIX}/muse"
    state = {"missing": [], "terminal": None}
    status_info = {}

    def on_message(_c, _u, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        if msg.topic == status_topic and msg.retain and not status_info:
            if isinstance(payload, dict):
                status_info.update(payload)
            return
        for e in payload.get("events", []):
            raw = str(e.get("raw", ""))
            if transfer not in raw:
                continue
            print(f"[{e.get('level','?')}] {raw[:200]}")
            if raw.startswith("fw-missing"):
                m = re.search(r"fw-missing \S+ ([\d,]+)", raw)
                if m:
                    state["missing"] = [int(s) for s in
                                        m.group(1).split(",") if s]
            elif raw.startswith("fw-ready"):
                state["terminal"] = "ready"
            elif raw.startswith("fw-declined"):
                state["terminal"] = "declined"
            elif raw.startswith("fw-flashed"):
                state["terminal"] = "flashed"
            elif raw.startswith("fw-error"):
                state["terminal"] = "error:" + raw

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"muse-fw-{transfer}")
    client.username_pw_set(USER, PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                   tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_message = on_message
    client.connect(HOST, PORT, keepalive=30)
    client.subscribe([(telemetry_topic, 0)])
    client.loop_start()
    # No pre-check: just send fwbegin. The page is the source of truth for
    # its own permissions — it rejects with fw-declined/fw-error if the
    # command channel or firmware_transfer permission is off.
    time.sleep(1.0)
    # Session take: tell the page we're driving (pill shows the verb).
    publish_checked(client, muse_topic,
                      json.dumps({"t": "muse_driving", "driving": True,
                                  "verb": "MUSE SENDING FIRMWARE"}), qos=0)

    def pub(cmd):
        # Page expects the {id, cmd} envelope (same as send_command.py);
        # raw transfer objects are rejected as malformed. Commands are
        # sealed by default — strict mode drops unsigned ones.
        cmd_id = uuid.uuid4().hex[:8]
        if use_seal:
            sealed = seal_client.seal_command(cmd_id, cmd, command_topic)
            msg = json.dumps({"sealed": sealed["sealed"]})
        else:
            msg = json.dumps({"id": cmd_id, "cmd": cmd})
        while publish_checked(client, command_topic, msg,
                               qos=0).rc != 0:
            time.sleep(0.2)

    pub({"t": "fwbegin", "transfer": transfer,
         "name": name, "size": len(data), "sha256": sha,
         "chunks": n_chunks})
    print(f"transfer {transfer}: sending {n_chunks} chunks…")
    for i in range(n_chunks):
        pub({"t": "fwchunk", "transfer": transfer,
             "seq": i,
             "data": base64.b64encode(
                 data[i * CHUNK:(i + 1) * CHUNK]).decode()})
        if (i + 1) % 25 == 0:
            print(f"  …{i + 1}/{n_chunks}")
        time.sleep(0.01)
    pub({"t": "fwend", "transfer": transfer})
    print("all chunks sent, waiting for page verdict (180s)…")

    deadline = time.time() + 180
    attempts = {}
    MAX_RESEND = 5
    while time.time() < deadline and not state["terminal"]:
        if state["missing"]:
            seqs = state["missing"]
            state["missing"] = []
            retry = [s for s in seqs if attempts.get(s, 0) < MAX_RESEND]
            dead = [s for s in seqs if attempts.get(s, 0) >= MAX_RESEND]
            if dead:
                state["terminal"] = (
                    f"error:chunk(s) {dead} undeliverable "
                    f"after {MAX_RESEND} attempts")
                break
            if retry:
                print(f"resending {len(retry)} missing chunks…")
                for s in retry:
                    pub({"t": "fwchunk",
                         "transfer": transfer, "seq": s,
                         "data": base64.b64encode(
                             data[s * CHUNK:(s + 1) * CHUNK]).decode()})
                    attempts[s] = attempts.get(s, 0) + 1
                    time.sleep(0.01)
                pub({"t": "fwend", "transfer": transfer})
        time.sleep(1)

    client.loop_stop()
    # Session relinquish: we're done — page drops the pill back to idle.
    # (A missed relinquish is harmless: the page's hold timer is the fallback.)
    try:
        publish_checked(client, muse_topic,
                          json.dumps({"t": "muse_driving", "driving": False}),
                          qos=0)
        time.sleep(0.5)
    except Exception:
        pass
    client.disconnect()
    term = state["terminal"]
    if term == "ready":
        print("page: transfer complete, hash OK — "
              "waiting on John's Yes/No in the page (or unattended mode).")
    elif term == "flashed":
        print("page: firmware flashed successfully.")
    elif term == "declined":
        print("page: John declined the flash.")
    elif term and term.startswith("error:"):
        print("page reported an error:", term[6:200])
        sys.exit(1)
    else:
        print("(no terminal state seen — check the console)")


if __name__ == "__main__":
    main()
