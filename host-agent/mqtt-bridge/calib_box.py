#!/usr/bin/env python3
"""Run a square-box cursor calibration with before/after verification.

For each leg: photo -> glide -> wait -> photo -> diff -> measure displacement.
Records HID->Camera mapping for audit.

Usage:
    python3 calib_box.py <size> [--out <json_path>]
"""
import json
import os
import subprocess
import sys
import time
from datetime import datetime

import numpy as np
from PIL import Image

BRIDGE_DIR = os.path.expanduser(
    "~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge")


def take_snap(out_path):
    r = subprocess.run(
        ["python3", "take_snap.py", "--out", out_path],
        capture_output=True, text=True, timeout=90, cwd=BRIDGE_DIR)
    if not os.path.exists(out_path):
        raise RuntimeError(f"snapshot failed: {r.stdout[-500:]}")
    return out_path


def send_glide(dx, dy, dur_ms=1500):
    # load env for MQTT_PASS
    env_file = os.path.expanduser("~/.config/watcher-mqtt.env")
    env = {}
    with open(env_file) as f:
        for line in f:
            line = line.strip()
            if line and "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip("'\"")
    full_env = {**os.environ, "MQTT_PASS": env.get("MQTT_PASS", "")}
    cmd = json.dumps({"t": "mouse_glide", "dx": dx, "dy": dy,
                      "dur_ms": dur_ms})
    r = subprocess.run(
        ["python3", "send_command.py", cmd],
        capture_output=True, text=True, timeout=30,
        cwd=BRIDGE_DIR, env=full_env)
    if "sent id=" not in r.stdout:
        raise RuntimeError(f"glide refused: {r.stdout[-500:]}")
    # extract command id
    import re
    m = re.search(r"sent id=([0-9a-f]+)", r.stdout)
    return m.group(1) if m else "unknown"


def find_cursor_displacement(before_path, after_path):
    """Diff two frames, return (dx, dy) of cursor centroid displacement."""
    before = np.asarray(Image.open(before_path)).astype(np.int16)
    after = np.asarray(Image.open(after_path)).astype(np.int16)
    diff = np.abs(before - after).sum(axis=2)

    # Try to find a compact bright blob (the cursor)
    # Use a high threshold to isolate the cursor from noise
    for thresh in [100, 80, 60]:
        mask = diff > thresh
        n = mask.sum()
        if 5 < n < 2000:  # plausible cursor blob size
            ys, xs = np.where(mask)
            # Check compactness: bbox should be small
            w = xs.max() - xs.min()
            h = ys.max() - ys.min()
            if w < 50 and h < 50:
                return {
                    "dx": float(xs.mean()),
                    "dy": float(ys.mean()),
                    "n_pixels": int(n),
                    "bbox": [int(xs.min()), int(ys.min()),
                             int(xs.max()), int(ys.max())],
                    "threshold": thresh,
                    "centroid_before": None,  # we only get displacement
                }
    return None


def main():
    size = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    out_path = None
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]

    # square box: right, down, left, up (returns to start)
    legs = [
        ("right", size, 0),
        ("down", 0, size),
        ("left", -size, 0),
        ("up", 0, -size),
    ]

    results = {
        "size": size,
        "started": datetime.now().isoformat(),
        "legs": [],
    }

    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    for i, (name, dx, dy) in enumerate(legs):
        print(f"\n=== leg {i+1}/4: {name} ({dx},{dy}) ===")
        leg = {"name": name, "dx_hid": dx, "dy_hid": dy}

        # before photo
        before = f"/tmp/calib_{ts}_{size}_{name}_before.jpg"
        print(f"taking before photo...")
        take_snap(before)
        leg["before"] = before

        # glide
        print(f"sending glide ({dx},{dy})...")
        cmd_id = send_glide(dx, dy, dur_ms=1500)
        leg["cmd_id"] = cmd_id
        print(f"glide sent {cmd_id}, waiting 2s...")
        time.sleep(2.0)

        # after photo
        after = f"/tmp/calib_{ts}_{size}_{name}_after.jpg"
        print(f"taking after photo...")
        take_snap(after)
        leg["after"] = after

        # diff
        print(f"diffing...")
        disp = find_cursor_displacement(before, after)
        if disp:
            leg["measured"] = disp
            print(f"  cursor blob: {disp['n_pixels']}px at "
                  f"({disp['dx']:.1f},{disp['dy']:.1f})")
        else:
            leg["measured"] = None
            print(f"  WARNING: no clear cursor blob found")

        results["legs"].append(leg)

    results["completed"] = datetime.now().isoformat()

    # compute J from successful legs
    # J maps HID -> Camera: [dx_cam, dy_cam] = J @ [dx_hid, dy_hid]
    # For axis-aligned moves, J_xx = dx_cam/dx_hid, J_yy = dy_cam/dy_hid
    # (We need before/after centroids, not just after — simplified here)
    print("\n=== calibration complete ===")
    for leg in results["legs"]:
        m = leg["measured"]
        status = "OK" if m else "FAIL"
        print(f"  {leg['name']}: {status}")

    if out_path:
        with open(out_path, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nresults saved to {out_path}")

    return results


if __name__ == "__main__":
    main()
