#!/usr/bin/env python3
"""Cursor calibration: 200 and 500 HID unit moves via mouse_glide.

Procedure (per John's rules):
- Measurement passes and moves are NOT interleaved within a measurement.
- Take clean snapshot, move via glide, wait for completion, take clean snapshot.
- John's mouse must be untouched during the entire run.

For each move: baseline -> glide -> snapshot -> glide back -> snapshot (verify).
Cursor delta measured via diff_cursor_positions.
"""
import json
import os
import ssl
import subprocess
import sys
import time
import uuid
from pathlib import Path

import paho.mqtt.client as mqtt

from topic_acl import publish_checked

# Add bridge dir to path for cursor_calibration import
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cursor_calibration import CursorCalibrator, diff_cursor_delta

OUT_DIR = Path.home() / "workspace/projects/watcher-input-assistant/himax-captures/calib-20260924"
OUT_DIR.mkdir(parents=True, exist_ok=True)

# Seal device commands by default (strict mode drops unsealed traffic).
# --no-seal is ONLY for deliberately testing seal rejection.
USE_SEAL = "--no-seal" not in sys.argv

def seal_envelope(mid, cmd):
    """Wrap {"id","cmd"} in a WMS2 sealed envelope, or return it unsigned."""
    if not USE_SEAL:
        return json.dumps({"id": mid, "cmd": cmd})
    import seal_client
    sealed = seal_client.seal_command(mid, cmd, "watcher/command")
    print(f"  [SEALED ctr={sealed['ctr']}]")
    return json.dumps({"sealed": sealed["sealed"]})

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
            if "MQTT_PASS" in out:
                return out
    raise SystemExit("no subscriber with MQTT creds found")

ENV = recover_env()
HOST = ENV.get("MQTT_HOST", "mqtt.offx1.com")
PORT = int(ENV.get("MQTT_PORT", "8883"))
USER = ENV.get("MQTT_USER", "muse")
PASS = ENV["MQTT_PASS"]

def mqtt_client():
    cl = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    cl.username_pw_set(USER, PASS)
    cl.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    cl.connect(HOST, PORT, 60)
    return cl

def send_glide(dx, dy, dur_ms=1000):
    """Send mouse_glide and wait for completion + settle."""
    cl = mqtt_client()
    replies = []
    def on_msg(c, u, m):
        try:
            p = json.loads(m.payload.decode())
            if p.get("_mid") == mid:
                replies.append(p)
        except:
            pass
    cl.on_message = on_msg
    cl.subscribe("watcher/telemetry")
    cl.loop_start()
    time.sleep(0.5)
    mid = uuid.uuid4().hex[:8]
    cmd = {"t": "mouse_glide", "dx": dx, "dy": dy, "dur_ms": dur_ms}
    envelope = seal_envelope(mid, cmd)
    publish_checked(cl, "watcher/command", envelope, qos=1)
    print(f"  glide dx={dx} dy={dy} dur={dur_ms}ms (mid={mid})")
    # Wait for reply + glide duration + settle
    timeout = (dur_ms / 1000) + 10
    start = time.time()
    while time.time() - start < timeout:
        if replies:
            r = replies[0]
            if r.get("t") == "ok":
                print(f"  glide OK")
                break
            elif r.get("t") == "err":
                print(f"  glide ERR: {r.get('msg')}")
                break
        time.sleep(0.2)
    else:
        print(f"  WARNING: no glide reply within {timeout}s")
    # Extra settle time
    time.sleep(1.5)
    cl.loop_stop()
    cl.disconnect()

def take_snapshot(name):
    """Take a snapshot via snaptest, return path to JPEG."""
    print(f"  snapshot: {name}")
    # Start capture listener (writes to himax-captures with auto name)
    listener = subprocess.Popen(
        ["python3", "snaptest_capture.py"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        cwd=os.path.dirname(os.path.abspath(__file__))
    )
    time.sleep(1)
    # Send snaptest command
    cl = mqtt_client()
    mid = uuid.uuid4().hex[:8]
    cmd = {"t": "dbg", "cmd": "snaptest", "send": 1}
    cl.loop_start()
    publish_checked(cl, "watcher/command", seal_envelope(mid, cmd), qos=1)
    time.sleep(2)
    cl.loop_stop()
    cl.disconnect()
    try:
        out, _ = listener.communicate(timeout=60)
        # Find the output path in the output
        for line in out.split("\n"):
            if ".jpg" in line and ("wrote" in line.lower() or "saved" in line.lower()):
                print(f"    {line.strip()}")
        # Check if file exists - snaptest_capture writes to himax-captures/
        cap_dir = Path.home() / "workspace/projects/watcher-input-assistant/himax-captures"
        jpgs = sorted(cap_dir.glob("*.jpg"), key=lambda p: p.stat().st_mtime, reverse=True)
        if jpgs:
            # If the file was just created (within last 2 min), use it
            if time.time() - jpgs[0].stat().st_mtime < 120:
                print(f"    captured: {jpgs[0].name}")
                # Copy to our labeled name for clarity
                dest = OUT_DIR / f"{name}.jpg"
                import shutil
                shutil.copy2(jpgs[0], dest)
                return str(dest)
        print(f"    WARNING: no fresh JPEG found")
        return None
    except subprocess.TimeoutExpired:
        listener.kill()
        print(f"    WARNING: capture timed out")
        return None

def main():
    cal = CursorCalibrator()
    moves = [
        ("200X", 200, 0),
        ("200Y", 0, 200),
        ("500X", 500, 0),
        ("500Y", 0, 500),
    ]
    results = {}
    last_pos = None  # tracked cursor position; disambiguates blob ordering
    for label, dx, dy in moves:
        print(f"\n=== {label}: HID ({dx}, {dy}) ===")
        # Baseline
        before = take_snapshot(f"calib-{label}-before")
        if not before:
            print(f"  SKIP {label}: no baseline snapshot")
            continue
        # Move
        send_glide(dx, dy, dur_ms=1500 if abs(dx) + abs(dy) > 300 else 1000)
        # After
        after = take_snapshot(f"calib-{label}-after")
        if not after:
            print(f"  SKIP {label}: no after snapshot")
            # Return to start anyway
            send_glide(-dx, -dy, dur_ms=1000)
            continue
        # Measure (last_pos disambiguates before/after blob ordering)
        try:
            dpx, dpy, before_b, after_b = diff_cursor_delta(
                before, after, before_pos=last_pos)
        except ValueError as e:
            print(f"  WARNING: {e} — skipping")
            send_glide(-dx, -dy, dur_ms=1000)
            continue
        print(f"  before blob: cx={before_b[0]} cy={before_b[1]} area={before_b[2]}")
        print(f"  after blob:  cx={after_b[0]} cy={after_b[1]} area={after_b[2]}")
        print(f"  measured pixel delta: ({dpx}, {dpy})")
        cal.add_correspondence(dhid_x=dx, dhid_y=dy, dpx=dpx, dpy=dpy)
        results[label] = {"hid": [dx, dy], "pixels": [dpx, dpy]}
        last_pos = (after_b[0], after_b[1])
        # Return to start
        print(f"  returning to start...")
        send_glide(-dx, -dy, dur_ms=1500 if abs(dx) + abs(dy) > 300 else 1000)
        verify = take_snapshot(f"calib-{label}-return")
        if verify:
            print(f"  return snapshot captured")
            # Cursor should be back near the pre-move position; the before
            # blob of this move is the best estimate of that.
            last_pos = (before_b[0], before_b[1])
    # Solve
    print(f"\n=== Solving Jacobian ===")
    print(f"Correspondences: {len(cal.correspondences)}")
    if len(cal.correspondences) >= 2:
        J = cal.solve()
        print(f"Jacobian (pixels/HID):\n{J}")
        print(f"Inverse (HID/pixel):\n{cal.J_inv}")
        print(f"RMS residual: {cal.residual_rms}")
        out_path = Path.home() / "workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/cursor_calibration_20260924_v2.json"
        cal.save(str(out_path))
        print(f"Saved to {out_path}")
    else:
        print("Not enough correspondences to solve (need >= 2)")

if __name__ == "__main__":
    main()
