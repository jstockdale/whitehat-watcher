#!/usr/bin/env python3
"""
High-level cursor operations for Watcher eyes-on-screen control.

Codifies the lessons from 2026-09-25 closed-loop sessions:
1. NEVER plan a move from an invisible cursor (I-beam in text field).
   Recover first: nudge in increments until the arrow is visible.
2. Route AROUND text fields with waypoints, never through them.
   Crossing a text field flips arrow->I-beam (invisible on camera).
3. Verify every move with a photo before planning the next.
4. Use small corrections (<100 HID) for final approach.

Requires: send_command.py, take_snap.py in same directory.
"""

import json
import math
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Informal 2026-09-25 Jacobian inverse (camera px -> HID units).
# Replace with validated 2026-09-24 matrix for production use.
J_INV = [
    [5.19433709, -0.08020321],
    [0.17927776, 4.53855793],
]

# Command aliases: user-friendly name -> firmware command name.
# Prevents the mouse_wheel vs scroll confusion from 2026-09-25.
COMMAND_ALIASES = {
    "mouse_wheel": ("scroll", {"dy": "delta"}),
    "wheel": ("scroll", {"dy": "delta"}),
}


def hid_from_camera(dx_cam, dy_cam):
    """Convert camera-pixel displacement to HID units via J^-1."""
    dx_hid = J_INV[0][0] * dx_cam + J_INV[0][1] * dy_cam
    dy_hid = J_INV[1][0] * dx_cam + J_INV[1][1] * dy_cam
    return round(dx_hid), round(dy_hid)


def send(cmd: dict, timeout=20) -> dict:
    """Send a command via send_command.py, return parsed result."""
    p = subprocess.run(
        [sys.executable, str(HERE / "send_command.py"), json.dumps(cmd)],
        capture_output=True, text=True, timeout=timeout,
        cwd=str(HERE),
    )
    # send_command.py prints "sent id=..." and reply lines
    result = {"sent": False, "output": p.stdout}
    for line in p.stdout.splitlines():
        if line.startswith("sent id="):
            result["sent"] = True
            result["cmd_id"] = line.split("sent id=")[1].split(":")[0]
        if '"t":"ok"' in line:
            result["ok"] = True
        if "rejected" in line:
            result["rejected"] = line.strip()
    return result


def snap(out_path: str, timeout=60) -> str:
    """Capture a camera snapshot, return path."""
    p = subprocess.run(
        [sys.executable, str(HERE / "take_snap.py"), "--out", out_path],
        capture_output=True, text=True, timeout=timeout,
        cwd=str(HERE),
    )
    if not Path(out_path).exists():
        raise RuntimeError(f"Snapshot failed: {p.stdout[-500:]} {p.stderr[-500:]}")
    return out_path


def glide(dx, dy, dur_ms=1000, announce=True):
    """Single relative glide. Announce before sending (bench rule)."""
    if announce:
        print(f"  -> mouse_glide dx={dx} dy={dy} dur_ms={dur_ms}", flush=True)
    return send({"t": "mouse_glide", "dx": dx, "dy": dy, "dur_ms": dur_ms})


def glide_waypoints(waypoints, announce=True):
    """
    Multi-waypoint glide: [[dx, dy, dur_ms], ...].
    Use to route AROUND text fields so the cursor stays a visible arrow.
    """
    if announce:
        print(f"  -> mouse_glide waypoints={waypoints}", flush=True)
    return send({"t": "mouse_glide", "waypoints": waypoints})


def plan_around(cursor, target, avoid_rects):
    """
    Plan waypoints from cursor to target avoiding rectangles.

    cursor, target: (x, y) in camera pixels.
    avoid_rects: list of (x0, y0, x1, y1) in camera pixels (e.g. text fields).

    Strategy: if the straight line from cursor to target intersects any
    avoid rect, route via a waypoint above the rect (approach from above,
    drop vertically onto target). Returns list of (dx_cam, dy_cam) legs.
    """
    def line_hits_rect(p0, p1, r):
        # Sample the segment; if any sample is inside the rect, it hits.
        x0, y0, x1, y1 = r
        for t in [i / 20 for i in range(21)]:
            x = p0[0] + (p1[0] - p0[0]) * t
            y = p0[1] + (p1[1] - p0[1]) * t
            if x0 <= x <= x1 and y0 <= y <= y1:
                return True
        return False

    for r in avoid_rects:
        if line_hits_rect(cursor, target, r):
            # Waypoint: directly above target, clear of the rect top.
            x0, y0, x1, y1 = r
            margin = 30  # camera px clearance above rect
            via = (target[0], y0 - margin)
            # Only use via if cursor->via doesn't also hit (it won't: via is above)
            return [(via[0] - cursor[0], via[1] - cursor[1]),
                    (target[0] - via[0], target[1] - via[1])]
    # No obstruction: direct leg.
    return [(target[0] - cursor[0], target[1] - cursor[1])]


def move_to_target(cursor, target, avoid_rects=None, dur_ms=1500,
                   snap_before="/tmp/ops_before.jpg",
                   snap_after="/tmp/ops_after.jpg"):
    """
    Closed-loop move: plan (around avoid rects), execute, snapshot.
    Returns the after-snapshot path for the caller to verify.
    The caller MUST inspect the image and confirm before the next move.
    """
    legs = plan_around(cursor, target, avoid_rects or [])
    waypoints = []
    for dx_cam, dy_cam in legs:
        dx_hid, dy_hid = hid_from_camera(dx_cam, dy_cam)
        waypoints.append([dx_hid, dy_hid, dur_ms // max(len(legs), 1)])
    if len(waypoints) == 1:
        dx, dy, dm = waypoints[0]
        glide(dx, dy, dm)
    else:
        glide_waypoints(waypoints)
    time.sleep(max(dm for _, _, dm in waypoints) / 1000 + 1.5)
    return snap(snap_after)


def recover_cursor_visible(direction=(0, 1), step_hid=40, max_steps=10,
                           snap_path="/tmp/ops_recover.jpg"):
    """
    John's recovery rule: if the cursor is lost in a text box (invisible
    I-beam), nudge in reasonable increments until the arrow reappears.

    direction: (dx, dy) unit-ish HID direction to nudge (default: down).
    step_hid: HID units per nudge (default 40, John's suggestion).
    Returns after max_steps; caller verifies visibility from the snapshot.

    NOTE: visibility detection is currently human-in-the-loop (inspect the
    returned snapshot). Auto-detection is a future improvement.
    """
    dx_step = direction[0] * step_hid
    dy_step = direction[1] * step_hid
    for i in range(max_steps):
        print(f"  recovery nudge {i+1}/{max_steps}: dx={dx_step} dy={dy_step}", flush=True)
        glide(dx_step, dy_step, dur_ms=800)
        time.sleep(2.0)
    return snap(snap_path)


def click():
    """Left click. Caller must have explicit approval (bench rule)."""
    print("  -> mouse_click", flush=True)
    return send({"t": "mouse_click"})


def scroll(delta):
    """Scroll via firmware 'scroll' command (alias for mouse_wheel)."""
    print(f"  -> scroll delta={delta}", flush=True)
    return send({"t": "scroll", "delta": delta})


def type_text(text):
    """Type text via HID keyboard."""
    print(f"  -> type ({len(text)} chars)", flush=True)
    return send({"t": "type", "text": text})


if __name__ == "__main__":
    print("cursor_ops: import this module, don't run it directly.")
    print("J_INV check: 100px right ->", hid_from_camera(100, 0), "HID")
    # Self-test the avoid-rect planner (no hardware needed)
    legs = plan_around((345, 380), (379, 455), [(100, 440, 380, 475)])
    print("plan_around test:", legs)
    assert len(legs) == 2, "should route via waypoint above text field"
    legs2 = plan_around((100, 100), (200, 200), [(400, 400, 500, 500)])
    print("plan_around direct test:", legs2)
    assert len(legs2) == 1, "should go direct when no obstruction"
    print("planner self-test OK")
