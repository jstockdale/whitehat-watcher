#!/usr/bin/env python3
"""
Live digital-twin wiring for Watcher eyes-on-screen control.

Connects screen_twin.ScreenTwin to the cursor_ops flow:
- Every commanded move feeds twin.command() (Kalman predict).
- Every cursor detection feeds twin.observe_cursor() (Kalman correct).
- Planners use twin.believed_cam() instead of raw last-seen position.
- Named UI elements (submit button, text field) persist in the twin's
  element map across sessions via twin.save()/load().

This is the "wiring" step: the twin was built and unit-tested (49 tests)
but never connected to live operations. This module is that connection.

Usage:
    from twin_ops import TwinOps
    ops = TwinOps()  # loads or creates ~/workspace/.../twin_state.json
    ops.glide(100, 0)              # moves + feeds twin.command
    ops.observe((342, 218), conf=0.8)  # detection -> twin.observe_cursor
    print(ops.believed())          # filtered camera-px position
    ops.remember("submit_btn", (379, 421))
    dx, dy = ops.move_to("submit_btn")  # HID delta from belief
"""

from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from screen_twin import ScreenTwin
import cursor_ops

TWIN_STATE = HERE / "twin_state.json"


class TwinOps:
    """
    Twin-aware wrapper around cursor_ops primitives.

    The twin tracks belief in the HID-odometry frame (robust to camera
    jumps); conversions to camera px use the twin's S<->C homography
    (or the raw Jacobian fallback when no homography is solved yet).
    """

    def __init__(self, state_path=TWIN_STATE, j_inv=None):
        self.state_path = Path(state_path)
        if self.state_path.exists():
            self.twin = ScreenTwin.load(str(self.state_path))
            print(f"twin loaded from {self.state_path}")
        else:
            # Validated 2026-09-24 Jacobian (HID -> camera px).
            J = [[0.205862, 0.003448], [-0.007586, 0.194828]]
            self.twin = ScreenTwin(J=J)
            print("new twin created (validated 2026-09-24 J)")
        self._j_inv = j_inv or cursor_ops.J_INV

    # -- low-level ops (mirror cursor_ops, feed twin) -------------------

    def glide(self, dx, dy, dur_ms=1000, note=""):
        """Glide + twin.command() predict step."""
        result = cursor_ops.glide(dx, dy, dur_ms)
        self.twin.command(dx, dy, note=note)
        self.save()
        return result

    def glide_waypoints(self, waypoints, note=""):
        """Waypoint glide + per-leg twin.command() predicts."""
        result = cursor_ops.glide_waypoints(waypoints)
        for dx, dy, _ in waypoints:
            self.twin.command(dx, dy, note=note)
        self.save()
        return result

    def click(self):
        """Click (caller must have approval). No twin state change."""
        return cursor_ops.click()

    def scroll(self, delta):
        """Scroll. No cursor motion -> no twin predict."""
        return cursor_ops.scroll(delta)

    def type_text(self, text):
        """Type. No cursor motion -> no twin predict."""
        return cursor_ops.type_text(text)

    # -- perception ----------------------------------------------------

    def observe(self, cam_xy, confidence=0.8, source="detect"):
        """
        Feed a cursor detection (camera px) into the twin.
        Returns the twin's verdict dict (accepted/rejected as outlier).
        """
        verdict = self.twin.observe_cursor(cam_xy, confidence=confidence)
        self.save()
        return verdict

    def reanchor(self, cam_xy, confidence=0.9):
        """Hard re-anchor after LOST state or big jump."""
        verdict = self.twin.reanchor(cam_xy, confidence=confidence)
        self.save()
        return verdict

    def believed(self):
        """
        Filtered cursor position in camera px, or None if unknown/lost.
        Planners should use this instead of last raw detection.
        """
        return self.twin.believed_cam

    def believed_hid(self):
        """Filtered cursor position in HID-odometry frame."""
        return self.twin.believed_hid

    def is_lost(self):
        """True if the twin has rejected too many observations in a row."""
        # ScreenTwin tracks consecutive rejections internally; LOST after 5.
        return getattr(self.twin, "_consec_reject", 0) >= 5

    # -- elements ------------------------------------------------------

    def remember(self, name, cam_xy):
        """Store a named UI element at a camera-px position."""
        self.twin.add_element(name, cam_xy=cam_xy)
        self.save()

    def move_to(self, name):
        """
        HID (dx, dy) to reach a named element from current belief.
        Raises if belief is LOST (caller must reanchor first).
        """
        return self.twin.move_to_element(name)

    def sanity(self, detected_cam):
        """Check a fresh detection against belief; flags Jacobian problems."""
        return self.twin.sanity_check(detected_cam)

    # -- persistence ---------------------------------------------------

    def save(self):
        try:
            self.twin.save(str(self.state_path))
        except Exception as e:
            print(f"twin save failed: {e}")

    def report(self):
        return self.twin.report()


if __name__ == "__main__":
    # Self-test: twin predict/observe cycle WITHOUT hardware.
    # We exercise ScreenTwin directly (no MQTT), verifying the wiring math.
    import tempfile, os
    with tempfile.TemporaryDirectory() as td:
        ops = TwinOps(state_path=os.path.join(td, "t.json"))
        # Predict only (no glide -> no MQTT)
        ops.twin.command(100, 0, note="test predict")
        b = ops.believed_hid()
        print(f"belief after +100 HID x: {b}")
        # Observe a detection near the predicted position
        v = ops.observe((20, 1), confidence=0.9)
        print(f"observe verdict: {v.get('accepted', v)}")
        print(f"belief cam: {ops.believed()}")
        # Element round-trip
        ops.remember("submit_btn", (379, 421))
        print("remembered submit_btn")
        print("twin wiring self-test OK (no hardware touched)")
