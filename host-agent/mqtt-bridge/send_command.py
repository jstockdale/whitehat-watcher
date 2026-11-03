#!/usr/bin/env python3
"""Send an authorized command to the Watcher via MQTT (Muse side).

CANONICAL SENDER — always use this script for device commands.
See TESTING_PROTOCOL.md for the full testing protocol.

Usage:
    MQTT_PASS='...' python3 send_command.py '{"t":"dbg","cmd":"mem"}'
    MQTT_PASS='...' python3 send_command.py --seal '{"t":"ping"}'
    MQTT_PASS='...' python3 send_command.py --unkeyed '{"t":"ping"}'

Sealing (DEFAULT: ON):
    Commands are published as WMS2-sealed {"sealed": <base64>} envelopes
    to watcher/command (Ed25519, commander muse-01, topic-bound).
    Seal is the preferred path and best practice for ALL keyed devices.
    In strict mode the device/bridge DROPS unsealed messages, so an
    unsealed send to a keyed device in strict mode silently does nothing —
    this is correct behavior.

    --seal      Explicitly request sealing (same as the default; documents
                intent in scripts and logs).
    --unkeyed   Send the legacy unsigned {"id","cmd"} JSON. For devices
                WITHOUT seal keys — fresh/unprovisioned hardware where you
                need to reach the device BEFORE the seal is configured
                (bootstrap, provisioning). Also the flag to deliberately
                test that strict mode REJECTS unsealed traffic (expect
                "SEAL: UNSEALED msg on 'watcher/command' - DROPPED" in
                telemetry.log). Self-documenting: use --unkeyed when the
                device is unkeyed, --no-seal only when you mean it.
    --no-seal   Legacy alias for --unkeyed (same behavior). Prefer
                --unkeyed in new code — it states the reason in the
                command line.

    Policy (John 2026-09-27): seal is best practice for keyed devices;
    unsealed is supported for unkeyed devices. See TESTING_PROTOCOL.md.

The console only accepts commands when John's "Allow Muse to run
commands" toggle is on, and only commands on the authorized list
below. Coordinate with John in chat before driving the device.

Credentials via env: MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS.
Signing key: ~/.ssh/watcher_seal_muse01 (0600).
"""

import argparse
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

DBG_ALLOWED = {"version", "mem", "heap", "tasks", "pwr", "i2c",
               "touch", "wheel", "pointer", "spitest",
               "eyes", "at", "snaptest", "png", "crop", "strip"}

# HID input class: approved by John 2026-09-23 for the eyes-drive-cursor
# loop. Standing rule: NEVER send one without his explicit per-use
# permission in chat — the allowlist makes it possible, his word makes
# it allowed.
HID_ALLOWED = {"hid_status", "mouse_move", "mouse_glide", "mouse_wheel",
               "scroll",
               "mouse_click", "key", "type", "consumer"}


def allowed(cmd) -> bool:
    """Mirror of the console's COMMAND_REGISTRY check (watcher-console.html).
    CANONICAL SOURCE: watcher-console.html::COMMAND_REGISTRY.
    Keep this in sync when adding new command types there.
    Read-only, plus the HID input class (John-approved 2026-09-23, per-use permission only)."""
    if not isinstance(cmd, dict):
        return False
    t = cmd.get("t")
    if t == "ping":
        return set(cmd.keys()) == {"t"}
    if t == "logs":
        n = cmd.get("n")
        return (set(cmd.keys()) == {"t", "n"}
                and isinstance(n, int) and not isinstance(n, bool)
                and 1 <= n <= 100)
    if t == "dbg":
        return (cmd.get("cmd") in DBG_ALLOWED
                and not any(k in cmd for k in ("on", "v", "color")))
    if t in HID_ALLOWED:
        return True
    if t == "himax_at":
        at = cmd.get("at")
        return (set(cmd.keys()) == {"t", "at"}
                and isinstance(at, str)
                and at.startswith("AT"))
    if t == "himax_at_batch":
        ats = cmd.get("ats")
        return (set(cmd.keys()) <= {"t", "ats", "_mid"}
                and isinstance(ats, list)
                and len(ats) > 0
                and all(isinstance(a, str) and a.startswith("AT") for a in ats))
    return False


def parse_args(argv):
    p = argparse.ArgumentParser(
        description="Send an authorized, WMS2-sealed command to the Watcher "
                    "via MQTT. Sealing is ON by default (best practice for "
                    "keyed devices); --unkeyed covers unprovisioned "
                    "devices.",
        epilog="Policy: seal for keyed devices (default). --unkeyed / "
               "--no-seal for unkeyed devices (bootstrap/provisioning) or "
               "deliberately testing strict-mode rejection. In strict mode "
               "an unsealed command to a keyed device is dropped "
               "(SEAL: UNSEALED msg - DROPPED) and never reaches the "
               "device.")
    seal_group = p.add_mutually_exclusive_group()
    seal_group.add_argument(
        "--seal", dest="seal", action="store_true", default=True,
        help="seal the command with WMS2/Ed25519 (default; explicit)")
    seal_group.add_argument(
        "--unkeyed", dest="seal", action="store_false",
        help="send UNSIGNED for an unkeyed/unprovisioned device "
             "(bootstrap/provisioning), or to test strict-mode rejection. "
             "Self-documenting: prefer this over --no-seal.")
    seal_group.add_argument(
        "--no-seal", dest="seal", action="store_false",
        help="legacy alias for --unkeyed (same behavior).")
    p.add_argument("command", help='JSON command, e.g. \'{"t":"ping"}\'')
    p.add_argument("--timeout", type=int, default=12,
        help="seconds to wait for replies (default 12; use 3-5 for fast himax_at)")
    p.add_argument("--batch", action="store_true",
        help="command JSON contains 'ats' array for himax_at_batch (sends as t:himax_at_batch)")
    return p.parse_args(argv)


def main() -> None:
    opts = parse_args(sys.argv[1:])
    use_seal = opts.seal
    try:
        cmd = json.loads(opts.command)
        if opts.batch and "ats" in cmd:
            cmd["t"] = "himax_at_batch"
    except json.JSONDecodeError as e:
        print(f"not valid JSON: {e}")
        sys.exit(2)
    if not allowed(cmd):
        print("refused: command is not on the authorized list")
        print("authorized: ping; dbg version|mem|heap|tasks|pwr|i2c|"
              "touch|wheel|pointer|spitest (no on/v/color); logs n=1..100; "
              "hid: hid_status|mouse_move|mouse_wheel|mouse_click|key|type|"
              "consumer (John's per-use permission required)")
        sys.exit(3)
    if not PASS:
        print("MQTT_PASS is not set")
        sys.exit(2)
    if not use_seal:
        print("WARNING: unsealed send (--unkeyed/--no-seal). Legitimate for "
              "UNKEYED devices (bootstrap/provisioning before the seal is "
              "configured) or for deliberately testing strict-mode "
              "rejection. Against a KEYED device in strict mode this will "
              "be dropped (SEAL: UNSEALED msg - DROPPED) and never reach "
              "the device — that is the seal working, not a bug.",
              file=sys.stderr)

    cmd_id = uuid.uuid4().hex[:8]
    telemetry_topic = f"{TOPIC_PREFIX}/telemetry"
    command_topic = f"{TOPIC_PREFIX}/command"
    muse_topic = f"{TOPIC_PREFIX}/muse"
    seen_marker = []

    def on_message(_c, _u, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        for e in payload.get("events", []):
            raw = str(e.get("raw", ""))
            if f"mqtt {cmd_id}" in raw and not seen_marker:
                seen_marker.append(True)
                print(f"[echo] {raw[:160]}")
            elif seen_marker:
                print(f"[{e.get('level','?')}/{e.get('kind','?')}] "
                      f"{raw[:200]}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"muse-cmd-{cmd_id}")
    client.username_pw_set(USER, PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                   tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_message = on_message
    client.connect(HOST, PORT, keepalive=30)
    client.subscribe(telemetry_topic, qos=0)
    client.loop_start()
    time.sleep(1.5)
    # Session take: pill shows MUSE DRIVING until we relinquish below.
    publish_checked(client, muse_topic,
                      json.dumps({"t": "muse_driving", "driving": True,
                                  "verb": "MUSE DRIVING"}), qos=0)
    envelope = json.dumps({"id": cmd_id, "cmd": cmd})
    sealed_info = ""
    if use_seal:
        try:
            import seal_client
            sealed = seal_client.seal_command(cmd_id, cmd, command_topic)
            envelope = json.dumps({"sealed": sealed["sealed"]})
            sealed_info = (f" [sealed ctr={sealed['ctr']}]")
        except FileNotFoundError as e:
            print(f"seal key not found ({e}); run seal_keygen.py once, "
                  f"or use --no-seal for grace-mode unsigned send")
            sys.exit(2)
        except Exception as e:
            print(f"sealing failed: {e}")
            sys.exit(2)
    rc = publish_checked(client, command_topic, envelope, qos=0).rc
    if rc != 0:
        print(f"publish failed rc={rc}")
        sys.exit(1)
    print(f"sent id={cmd_id}{sealed_info} "
          f"[{'SEALED' if use_seal else 'UNSEALED'}]: {envelope[:120]}"
          f"{'...' if len(envelope) > 120 else ''}")
    print(f"--- waiting for replies ({opts.timeout}s) ---")
    deadline = time.time() + opts.timeout
    while time.time() < deadline:
        time.sleep(0.5)
    client.loop_stop()
    # Session relinquish.
    try:
        publish_checked(client, muse_topic,
                          json.dumps({"t": "muse_driving", "driving": False}),
                          qos=0)
        time.sleep(0.5)
    except Exception:
        pass
    client.disconnect()
    if not seen_marker:
        print("(no echo seen — console toggle may be off)")


if __name__ == "__main__":
    main()
