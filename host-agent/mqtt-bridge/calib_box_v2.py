#!/usr/bin/env python3
"""Square-box cursor calibration with chained photos and bright-dot matching.

Chained: photo A -> move -> photo B -> move -> photo C...
The after of one leg is the before of the next (5 photos for 4 legs).

Cursor detection: find bright compact blobs in each photo, match the one
that moved between consecutive photos.

Usage:
    python3 calib_box_v2.py <size> [--out <json_path>]
"""
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime

import numpy as np
from PIL import Image
from scipy import ndimage

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
    m = re.search(r"sent id=([0-9a-f]+)", r.stdout)
    return m.group(1) if m else "unknown"


def find_bright_dots(img_path, thresh=180):
    """Find small bright compact blobs."""
    img = np.asarray(Image.open(img_path))
    gray = img.mean(axis=2)
    bright = gray > thresh
    labeled, n = ndimage.label(bright)
    candidates = []
    for i in range(1, n + 1):
        ys, xs = np.where(labeled == i)
        area = len(xs)
        if 3 <= area <= 50:
            w = xs.max() - xs.min() + 1
            h = ys.max() - ys.min() + 1
            if w <= 12 and h <= 12:
                candidates.append((float(xs.mean()),
                                   float(ys.mean()), area))
    return candidates


def match_cursor(before_path, after_path, expected_dx=0, expected_dy=0):
    """Find the cursor by matching bright dots that moved as expected.

    Returns (dx_cam, dy_cam) displacement, or None.
    """
    before = find_bright_dots(before_path)
    after = find_bright_dots(after_path)

    best = None
    best_score = float('inf')

    # For each before-dot, find the after-dot that moved closest to expected
    for bx, by, ba in before:
        for ax, ay, aa in after:
            dx = ax - bx
            dy = ay - by
            # score: distance from expected displacement
            # (allow some tolerance for measurement noise)
            err = ((dx - expected_dx) ** 2 + (dy - expected_dy) ** 2) ** 0.5
            # prefer similar-sized blobs
            size_err = abs(aa - ba) / max(aa, ba)
            score = err + size_err * 20
            if score < best_score and err < 30:  # within 30px of expected
                best_score = score
                best = {
                    "dx_cam": dx,
                    "dy_cam": dy,
                    "before_xy": (bx, by),
                    "after_xy": (ax, ay),
                    "before_area": ba,
                    "after_area": aa,
                    "match_error": err,
                }

    return best


def main():
    size = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    out_path = None
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]

    # Expected camera displacement per HID unit (from prior J)
    # J = [[0.205862, 0.003448], [-0.007586, 0.194828]]
    J = [[0.205862, 0.003448], [-0.007586, 0.194828]]

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
        "photos": [],
    }

    ts = datetime.now().strftime("%Y%m%d-%H%M%S")

    # Take first photo
    print("taking initial photo...")
    prev_photo = f"/tmp/calib2_{ts}_{size}_p0.jpg"
    take_snap(prev_photo)
    results["photos"].append(prev_photo)

    for i, (name, dx, dy) in enumerate(legs):
        print(f"\n=== leg {i+1}/4: {name} ({dx},{dy}) ===")

        # Expected camera displacement
        exp_dx = J[0][0] * dx + J[0][1] * dy
        exp_dy = J[1][0] * dx + J[1][1] * dy
        print(f"expected camera: ({exp_dx:.1f}, {exp_dy:.1f})")

        # Send glide
        print(f"sending glide...")
        cmd_id = send_glide(dx, dy, dur_ms=1500)
        print(f"glide {cmd_id} sent, waiting 2s...")
        time.sleep(2.0)

        # Take next photo (this is the 'after' for this leg,
        # and the 'before' for the next leg)
        curr_photo = f"/tmp/calib2_{ts}_{size}_p{i+1}.jpg"
        print(f"taking photo...")
        take_snap(curr_photo)
        results["photos"].append(curr_photo)

        # Match cursor between prev and curr
        print(f"matching cursor...")
        match = match_cursor(prev_photo, curr_photo, exp_dx, exp_dy)
        leg = {
            "name": name,
            "dx_hid": dx,
            "dy_hid": dy,
            "cmd_id": cmd_id,
            "before_photo": prev_photo,
            "after_photo": curr_photo,
            "expected_dx_cam": exp_dx,
            "expected_dy_cam": exp_dy,
        }
        if match:
            leg["measured"] = match
            print(f"  MATCH: cursor moved ({match['dx_cam']:.1f}, "
                  f"{match['dy_cam']:.1f}) px")
            print(f"  expected ({exp_dx:.1f}, {exp_dy:.1f}), "
                  f"error {match['match_error']:.1f}px")
        else:
            leg["measured"] = None
            print(f"  WARNING: no cursor match found")

        results["legs"].append(leg)
        prev_photo = curr_photo

    results["completed"] = datetime.now().isoformat()

    print("\n=== calibration complete ===")
    n_ok = sum(1 for l in results["legs"] if l["measured"])
    print(f"  {n_ok}/4 legs matched")

    # Compute refined J from measurements
    # For axis-aligned moves: J_xx = mean(dx_cam/dx_hid) for X legs,
    #                         J_yy = mean(dy_cam/dy_hid) for Y legs
    x_legs = [l for l in results["legs"]
              if l["measured"] and l["dx_hid"] != 0]
    y_legs = [l for l in results["legs"]
              if l["measured"] and l["dy_hid"] != 0]
    if x_legs:
        j_xx = sum(l["measured"]["dx_cam"] / l["dx_hid"]
                   for l in x_legs) / len(x_legs)
        print(f"  J_xx (HID X -> Cam X): {j_xx:.6f}")
        results["J_xx"] = j_xx
    if y_legs:
        j_yy = sum(l["measured"]["dy_cam"] / l["dy_hid"]
                   for l in y_legs) / len(y_legs)
        print(f"  J_yy (HID Y -> Cam Y): {j_yy:.6f}")
        results["J_yy"] = j_yy

    if out_path:
        with open(out_path, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nresults saved to {out_path}")


if __name__ == "__main__":
    main()
