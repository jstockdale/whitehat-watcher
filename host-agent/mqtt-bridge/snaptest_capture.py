#!/usr/bin/env python3
"""Collect snaptest_chunk events from watcher/telemetry and reassemble the JPEG.

Subscribes to the telemetry topic, gathers chunks for one snapshot run
(grouped by id), and writes the reassembled JPEG to disk. Also captures the
{"t":"snaptest"} stats result if seen. Exits when a run completes or on timeout.

Credentials: recovered transiently from the running subscriber.py process
environ (same procedure as the flash flow); never printed or stored.
"""
import base64
import json
import os
import ssl
import subprocess
import sys
import time

import paho.mqtt.client as mqtt

TIMEOUT_S = 90
OUT_DIR = os.path.expanduser("~/workspace/projects/watcher-input-assistant/himax-captures")


def recover_env():
    pids = subprocess.run(["pgrep", "-f", r"subscriber\.py"],
                          capture_output=True, text=True).stdout.split()
    for pid in pids:
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = f.read()
        except (PermissionError, FileNotFoundError):
            continue
        if b"MQTT_PASS=" in env:
            out = {}
            for entry in env.split(b"\0"):
                if b"=" not in entry:
                    continue
                k, v = entry.split(b"=", 1)
                key = k.decode("utf-8", "replace")
                if key.startswith("MQTT_"):
                    out[key] = v.decode("utf-8", "replace")
            # skip processes whose environ doesn't parse (e.g. stale
            # launcher shells that merely contain the substring)
            if "MQTT_PASS" in out:
                return out
    raise SystemExit("no subscriber with MQTT creds found")


def main():
    env = recover_env()
    host = env.get("MQTT_HOST", "mqtt.offx1.com")
    port = int(env.get("MQTT_PORT", "8883"))
    user = env.get("MQTT_USER", "muse")
    passwd = env["MQTT_PASS"]
    os.makedirs(OUT_DIR, exist_ok=True)

    runs = {}   # id -> {"total": n, "parts": {seq: b64}, "last": bool}
    stats = {}  # id -> stats dict
    done_id = None
    deadline = time.time() + TIMEOUT_S

    def on_message(client, userdata, msg):
        nonlocal done_id
        if msg.topic != "watcher/telemetry":
            return
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        for e in payload.get("events", []):
            raw = e.get("raw", "")
            try:
                j = json.loads(raw)
            except Exception:
                continue
            t = j.get("t")
            if t == "snaptest_chunk":
                rid = j.get("id", 0)
                run = runs.setdefault(rid, {"total": j.get("total"), "parts": {}, "last": False})
                run["total"] = j.get("total", run["total"])
                seq = j.get("seq")
                if seq not in run["parts"]:
                    run["parts"][seq] = j.get("b64", "")
                if j.get("last"):
                    run["last"] = True
                total = run["total"] or 0
                if run["last"] and total and len(run["parts"]) >= total:
                    done_id = rid
                    client.disconnect()
                    return
            elif t == "snaptest":
                stats[j.get("id", 0)] = j

    # unique client id per run: a stale listener with the same id would be
    # kicked by the broker (or kick us), silently losing the chunk stream
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"muse-snapcap-{os.getpid()}-{int(time.time())}")
    client.username_pw_set(user, passwd)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_message = on_message
    client.connect(host, port, keepalive=30)
    client.subscribe("watcher/telemetry")
    print("listening for snaptest chunks...", flush=True)
    client.loop_start()
    try:
        while time.time() < deadline and done_id is None:
            time.sleep(0.5)
    finally:
        client.loop_stop()
        try:
            client.disconnect()
        except Exception:
            pass

    if done_id is None:
        print("TIMEOUT: no complete run captured")
        for rid, run in runs.items():
            print(f"  partial run id={rid}: {len(run['parts'])}/{run['total']} chunks, last={run['last']}")
        raise SystemExit(1)

    run = runs[done_id]
    total = run["total"]
    b64 = "".join(run["parts"][i] for i in range(total))
    try:
        jpeg = base64.b64decode(b64)
    except Exception as ex:
        print(f"base64 decode failed: {ex}")
        raise SystemExit(1)

    out = os.path.join(OUT_DIR, f"himax-snapshot-{done_id}.jpg")
    with open(out, "wb") as f:
        f.write(jpeg)
    print(f"run id={done_id}: {total} chunks -> {len(jpeg)} bytes -> {out}")
    print(f"SOI={jpeg[:2]==b'\\xff\\xd8'} EOI={jpeg[-2:]==b'\\xff\\xd9'}")
    if done_id in stats:
        s = stats[done_id]
        print("device stats: bytes=%s ms=%s soi=%s eoi=%s fnv=%s mean=%.1f" % (
            s.get("bytes"), s.get("ms"), s.get("soi"), s.get("eoi"),
            s.get("fnv"), s.get("mean", 0)))


if __name__ == "__main__":
    main()
