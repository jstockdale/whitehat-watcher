#!/usr/bin/env python3
"""
Closed-loop cursor servo for Watcher eyes-on-screen control.

The core robustness upgrade: instead of a human squinting at photos and
hand-computing corrections, this loop converges on its own:

    while error > tolerance and iterations < max:
        snap -> detect cursor (wiggle method)
        compute error vector (target - cursor)
        if error < tolerance: break (converged)
        move by error * gain (via J^-1, routed around avoid rects)
        snap -> verify

Key design decisions (from 2026-09-25 lessons):
- Detection uses cursor_detect.detect_wiggle (primary) — the method that
  beat human eyeballing by ~170px in one case.
- Every correction move routes around avoid_rects via cursor_ops.plan_around
  (never through text fields — John's rule).
- Gain < 1.0 (default 0.7) for stability: the Jacobian is approximate, so
  we under-correct and iterate rather than overshoot.
- The twin (twin_ops) tracks belief across iterations for outlier rejection.
- If detection returns None (cursor lost/invisible), we run John's recovery:
  nudge in increments until the cursor reappears, then resume.

This module sends REAL hardware commands. All entry points require the
caller to have bench approval (John's "bench is mine" convention).
"""

import math
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import cursor_ops
import cursor_detect
from twin_ops import TwinOps


class Servo:
    """
    Closed-loop cursor servo.

    target: (x, y) in camera px.
    avoid_rects: list of (x0, y0, x1, y1) camera-px rectangles to route around.
    tolerance_px: convergence threshold (default 8px ~ button radius).
    gain: correction gain 0-1 (default 0.7, under-correct for stability).
    max_iters: give up after this many corrections (default 8).
    """

    def __init__(self, target, avoid_rects=None, tolerance_px=8.0,
                 gain=0.7, max_iters=8, twin=None):
        self.target = tuple(target)
        self.avoid_rects = avoid_rects or []
        self.tolerance_px = tolerance_px
        self.gain = gain
        self.max_iters = max_iters
        self.twin = twin or TwinOps()
        self.history = []

    def _snap(self, path):
        return cursor_ops.snap(str(path))

    def _detect_wiggle(self, wiggle_hid=20):
        """
        Wiggle-detect the cursor. Returns (x, y) camera px or None.
        Takes two snapshots with a small HID nudge between them.
        """
        a = self._snap("/tmp/servo_a.jpg")
        # Small nudge: +wiggle_hid in x (twin tracks it)
        self.twin.glide(wiggle_hid, 0, dur_ms=500, note="servo wiggle")
        time.sleep(1.0)
        b = self._snap("/tmp/servo_b.jpg")
        pos = cursor_detect.detect_wiggle(a, b, wiggle_hid, 0.0)
        if pos is not None:
            # Nudge back to undo the wiggle
            self.twin.glide(-wiggle_hid, 0, dur_ms=500, note="servo unwriggle")
            time.sleep(1.0)
        return pos

    def _recover(self):
        """John's recovery: nudge until cursor reappears."""
        print("  servo: cursor lost, running recovery nudges", flush=True)
        for i in range(6):
            pos = self._detect_wiggle(wiggle_hid=30)
            if pos is not None:
                print(f"  servo: recovered at {pos}", flush=True)
                return pos
            # Nudge down 40 (John's increment) and try again
            self.twin.glide(0, 40, dur_ms=800, note="servo recovery")
            time.sleep(1.5)
        return None

    def run(self):
        """
        Run the servo loop. Returns (converged: bool, final_pos, iterations).
        """
        print(f"servo: target={self.target} tol={self.tolerance_px}px "
              f"gain={self.gain}", flush=True)

        for it in range(1, self.max_iters + 1):
            pos = self._detect_wiggle()
            if pos is None:
                pos = self._recover()
                if pos is None:
                    print("  servo: FAILED - cursor unrecoverable", flush=True)
                    return False, None, it

            # Feed detection to twin
            self.twin.observe(pos, confidence=0.85, source="servo")

            # Use twin belief if available (filtered), else raw detection
            belief = self.twin.believed()
            cx, cy = belief if belief else pos

            err_x = self.target[0] - cx
            err_y = self.target[1] - cy
            err = math.hypot(err_x, err_y)
            self.history.append({"iter": it, "pos": (cx, cy), "err": err})
            print(f"  iter {it}: pos=({cx:.0f},{cy:.0f}) err={err:.1f}px",
                  flush=True)

            if err <= self.tolerance_px:
                print(f"  servo: CONVERGED in {it} iters", flush=True)
                return True, (cx, cy), it

            # Correction: move by err * gain, routed around avoid rects
            legs = cursor_ops.plan_around(
                (cx, cy),
                (cx + err_x * self.gain, cy + err_y * self.gain),
                self.avoid_rects,
            )
            waypoints = []
            for dx_cam, dy_cam in legs:
                dx_hid, dy_hid = cursor_ops.hid_from_camera(dx_cam, dy_cam)
                waypoints.append([dx_hid, dy_hid, 1200])
            if len(waypoints) == 1:
                dx, dy, dm = waypoints[0]
                self.twin.glide(dx, dy, dur_ms=dm, note=f"servo corr {it}")
            else:
                self.twin.glide_waypoints(waypoints, note=f"servo corr {it}")
            time.sleep(2.5)

        print(f"  servo: MAX ITERS ({self.max_iters}) without converging",
              flush=True)
        return False, (cx, cy), self.max_iters


def servo_to(target, avoid_rects=None, tolerance_px=8.0, **kwargs):
    """One-shot convenience: servo to a camera-px target."""
    s = Servo(target, avoid_rects=avoid_rects,
              tolerance_px=tolerance_px, **kwargs)
    return s.run()


if __name__ == "__main__":
    # Self-test: verify imports and planning math without hardware.
    # (Full servo run needs bench approval + live cursor.)
    print("servo.py self-test (no hardware):")
    # Target INSIDE the avoid rect (submit button in text field area)
    legs = cursor_ops.plan_around((345, 380), (379, 455),
                                  [(100, 440, 380, 475)])
    print(f"  plan_around routes via {len(legs)} legs: OK")
    assert len(legs) == 2, "should route around text field"
    # Gain math check
    err = (34, 41)
    gain = 0.7
    corr = (err[0] * gain, err[1] * gain)
    print(f"  gain 0.7 on err {err} -> correction {corr}: OK")
    print("servo.py self-test OK (live run requires bench approval)")
