#!/usr/bin/env python3
"""Reassemble a P2 png_chunk stream into a PNG file.

v0.4.14+ firmware: `{"t":"dbg","cmd":"png"}` makes the S3 capture a frame
via AT+SPIDUMP + SPI bulk read, encode it as PNG (BGGR, SUB filter), and
stream it as {t:"png_chunk", seq, total, last, b64} events over the link.
The console page forwards link lines on watcher/telemetry as
events:[{raw, kind, level}], same path as the dbg replies.

This script listens, collects the chunks, verifies the set is complete,
reassembles, decodes with PIL, and saves the PNG. Sends nothing.

Usage:
    python3 reassemble_png.py [--out frame.png] [--timeout 120]

Firmware chunking (eyes.c stream_png_b64): 768 input bytes -> 1024 b64
chars per chunk; each chunk's b64 is a multiple of 4 chars with '='
padding only in the final chunk, so concatenating all b64 payloads in
seq order and base64-decoding the whole string is exact.
"""
import argparse
import base64
import hashlib
import io
import json
import os
import sys
import time

import paho.mqtt.client as mqtt
from PIL import Image

MQTT_HOST = "mqtt.offx1.com"
MQTT_PORT = 8883
MQTT_USER = "muse"
MQTT_PASS_PATH = os.path.expanduser("~/.config/watcher-mqtt/mqtt_pass")
TOPIC = "watcher/telemetry"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", default=None,
                   help="output PNG path (default: png_capture_<epoch>.png)")
    p.add_argument("--timeout", type=int, default=180,
                   help="give up after N seconds without completing (default 180)")
    p.add_argument("--expect", type=int, default=None,
                   help="expected chunk count; warn if the stream's total differs")
    return p.parse_args()


def extract_png_chunk(obj):
    """Return (seq, total, b64) if obj is a png_chunk event, else None."""
    if not isinstance(obj, dict):
        return None
    if obj.get("t") != "png_chunk":
        return None
    try:
        return int(obj["seq"]), int(obj["total"]), str(obj["b64"])
    except (KeyError, TypeError, ValueError):
        return None


def scan_payload(payload_bytes):
    """Yield (seq, total, b64) png_chunk tuples found in one MQTT payload,
    plus True if the terminal {t:"dbg",cmd:"png"} reply was seen."""
    found = []
    terminal = False
    try:
        outer = json.loads(payload_bytes.decode("utf-8", "replace"))
    except Exception:
        return found, terminal
    candidates = []
    if isinstance(outer, dict):
        if "events" in outer:
            for e in outer.get("events") or []:
                raw = (e or {}).get("raw", "")
                if isinstance(raw, dict):
                    candidates.append(raw)
                    continue
                if not isinstance(raw, str) or "png_chunk" not in raw:
                    # still check for the terminal dbg reply
                    if '"cmd"' in raw and '"png"' in raw:
                        try:
                            o = json.loads(raw)
                            if o.get("t") == "dbg" and o.get("cmd") == "png":
                                terminal = True
                        except Exception:
                            pass
                    continue
                try:
                    candidates.append(json.loads(raw))
                except Exception:
                    continue
        else:
            candidates.append(outer)
    for obj in candidates:
        hit = extract_png_chunk(obj)
        if hit:
            found.append(hit)
        elif isinstance(obj, dict) and obj.get("t") == "dbg" and obj.get("cmd") == "png":
            terminal = True
    return found, terminal


def main():
    args = parse_args()
    try:
        mqtt_pass = open(MQTT_PASS_PATH).read().strip()
    except OSError as e:
        print(f"cannot read {MQTT_PASS_PATH}: {e}", file=sys.stderr)
        return 2
    if not mqtt_pass:
        print(f"{MQTT_PASS_PATH} is empty", file=sys.stderr)
        return 2

    chunks = {}          # seq -> b64
    total = None
    terminal_seen = False
    done = {"flag": False}
    started = time.time()

    def on_message(client, userdata, msg):
        nonlocal total, terminal_seen
        found, term = scan_payload(msg.payload)
        if term and not terminal_seen:
            terminal_seen = True
            print("terminal {t:\"dbg\",cmd:\"png\"} reply seen", flush=True)
        for seq, tot, b64 in found:
            if total is None:
                total = tot
                if args.expect is not None and total != args.expect:
                    print(f"WARN: stream total={total} != expected {args.expect}",
                          flush=True)
                print(f"stream: {total} chunks", flush=True)
            elif tot != total:
                print(f"WARN: chunk {seq} reports total={tot}, stream total={total}",
                      flush=True)
                continue
            if seq not in chunks:
                chunks[seq] = b64
                if len(chunks) % 50 == 0 or seq == total - 1:
                    print(f"  {len(chunks)}/{total} chunks", flush=True)
        if total is not None and len(chunks) >= total:
            done["flag"] = True
            client.disconnect()

    c = mqtt.Client()
    c.username_pw_set(MQTT_USER, mqtt_pass)
    c.tls_set()
    c.on_message = on_message
    c.connect(MQTT_HOST, MQTT_PORT)
    c.subscribe(TOPIC)
    print(f"listening on {TOPIC} for png_chunk stream (timeout {args.timeout}s) ...",
          flush=True)
    c.loop_start()
    while not done["flag"] and time.time() - started < args.timeout:
        time.sleep(0.2)
    c.loop_stop()
    try:
        c.disconnect()
    except Exception:
        pass

    if total is None:
        print("no png_chunk stream seen before timeout — "
              "is the capture running? (send {\"t\":\"dbg\",\"cmd\":\"png\"})",
              file=sys.stderr)
        return 1
    missing = [s for s in range(total) if s not in chunks]
    if missing:
        print(f"INCOMPLETE: missing {len(missing)} chunks, e.g. {missing[:10]}",
              file=sys.stderr)
        return 1
    if not terminal_seen:
        print("note: terminal dbg/png reply not seen (chunks complete anyway)",
              flush=True)

    blob_b64 = "".join(chunks[s] for s in range(total))
    try:
        png = base64.b64decode(blob_b64, validate=True)
    except Exception as e:
        print(f"base64 decode failed: {e}", file=sys.stderr)
        return 1
    sha = hashlib.sha256(png).hexdigest()
    try:
        img = Image.open(io.BytesIO(png))
        img.load()
    except Exception as e:
        print(f"PIL could not decode the PNG: {e}", file=sys.stderr)
        return 1

    out = args.out or f"png_capture_{int(time.time())}.png"
    with open(out, "wb") as f:
        f.write(png)
    print(f"saved {out}: {img.width}x{img.height} {img.mode}, "
          f"{len(png)} bytes, sha256 {sha[:16]}…")
    return 0


if __name__ == "__main__":
    sys.exit(main())
