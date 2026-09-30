"""
Digital Twin for the Watcher eyes-on-screen cursor.

Tracks the believed cursor position in BOTH:
  - Camera pixel coordinates (for comparing against visual detections)
  - Normalized screen coordinates (0.0–1.0, for sanity-checking the
    HID→screen model independent of camera perspective)

The HID→screen stage models what the OS does (linear when acceleration
is off). The screen→camera stage is a perspective homography estimated
from the visible screen corners.

Usage:
    twin = CursorTwin(calibration_path)
    twin.command(dx, dy, mid="abc123")
    twin.observe(detected_xy, label="snap")
    twin.report()
"""

import json
import time
import numpy as np
from datetime import datetime


# Screen corners in camera pixels (640x480), refined 2026-09-24
# Maps to normalized screen coords (0,0)=top-left, (1,1)=bottom-right
SCREEN_CORNERS_CAM = np.array([
    [90, 60],    # top-left
    [550, 60],   # top-right
    [70, 420],   # bottom-left
    [570, 420],  # bottom-right
], dtype=float)
SCREEN_CORNERS_NORM = np.array([
    [0, 0],
    [1, 0],
    [0, 1],
    [1, 1],
], dtype=float)


def _homography(src, dst):
    """Compute 3x3 homography mapping src -> dst (4+ point pairs)."""
    A = []
    for (x, y), (u, v) in zip(src, dst):
        A.append([-x, -y, -1, 0, 0, 0, u*x, u*y, u])
        A.append([0, 0, 0, -x, -y, -1, v*x, v*y, v])
    A = np.array(A)
    _, _, Vt = np.linalg.svd(A)
    H = Vt[-1].reshape(3, 3)
    return H / H[2, 2]


def _apply_homography(H, pts):
    """Apply 3x3 homography to Nx2 points."""
    pts = np.atleast_2d(pts)
    homo = np.hstack([pts, np.ones((len(pts), 1))])
    out = (H @ homo.T).T
    return out[:, :2] / out[:, 2:3]


class CursorTwin:
    def __init__(self, calibration_path, initial_xy=(320, 240),
                 coord="camera"):
        with open(calibration_path) as f:
            data = json.load(f)
        # Calibration JSON stores J under "jacobian" (or "J" for older files)
        self.J = np.array(data.get("jacobian", data.get("J")))  # HID -> camera px
        self.Jinv = np.linalg.inv(self.J)
        # Camera <-> normalized screen homographies
        self.H_cam2screen = _homography(SCREEN_CORNERS_CAM, SCREEN_CORNERS_NORM)
        self.H_screen2cam = _homography(SCREEN_CORNERS_NORM, SCREEN_CORNERS_CAM)

        if coord == "camera":
            self.believed_cam = np.array(initial_xy, dtype=float)
        elif coord == "screen":
            # initial_xy given as normalized screen coords
            self.believed_cam = _apply_homography(
                self.H_screen2cam, np.array(initial_xy))[0]
        else:
            raise ValueError("coord must be 'camera' or 'screen'")

        self.history = []
        self._log("init", {
            "pos_cam": [round(float(self.believed_cam[0]), 1),
                        round(float(self.believed_cam[1]), 1)],
            "pos_screen": [round(float(v), 4) for v in self.screen_xy],
            "cal": calibration_path,
        })

    @property
    def screen_xy(self):
        """Believed position in normalized screen coordinates."""
        return _apply_homography(self.H_cam2screen, self.believed_cam)[0]

    @property
    def believed(self):
        """Believed position in camera pixels (back-compat)."""
        return self.believed_cam

    def _log(self, kind, detail):
        self.history.append({
            "t": datetime.now().isoformat(timespec="seconds"),
            "kind": kind,
            **detail,
        })

    def command(self, dx, dy, mid=None, note=""):
        """Record a commanded HID move and advance the believed position."""
        before_cam = self.believed_cam.copy()
        before_screen = self.screen_xy.copy()
        delta_cam = self.J @ np.array([dx, dy])
        self.believed_cam = before_cam + delta_cam
        after_screen = self.screen_xy
        self._log("command", {
            "dx": dx, "dy": dy,
            "mid": mid,
            "delta_cam": [round(float(delta_cam[0]), 2),
                          round(float(delta_cam[1]), 2)],
            "before_cam": [round(float(before_cam[0]), 1),
                           round(float(before_cam[1]), 1)],
            "after_cam": [round(float(self.believed_cam[0]), 1),
                          round(float(self.believed_cam[1]), 1)],
            "before_screen": [round(float(before_screen[0]), 4),
                              round(float(before_screen[1]), 4)],
            "after_screen": [round(float(after_screen[0]), 4),
                             round(float(after_screen[1]), 4)],
            "note": note,
        })
        return tuple(self.believed_cam)

    def observe(self, detected_xy, label="snap"):
        """Record a camera detection; return the error vs believed."""
        detected = np.array(detected_xy, dtype=float)
        error = detected - self.believed_cam
        err_px = float(np.linalg.norm(error))
        det_screen = _apply_homography(self.H_cam2screen, detected)[0]
        self._log("observe", {
            "label": label,
            "detected_cam": [round(float(detected[0]), 1),
                             round(float(detected[1]), 1)],
            "believed_cam": [round(float(self.believed_cam[0]), 1),
                             round(float(self.believed_cam[1]), 1)],
            "error_cam": [round(float(error[0]), 1),
                          round(float(error[1]), 1)],
            "error_px": round(err_px, 1),
            "detected_screen": [round(float(det_screen[0]), 4),
                                round(float(det_screen[1]), 4)],
            "believed_screen": [round(float(self.screen_xy[0]), 4),
                                round(float(self.screen_xy[1]), 4)],
        })
        return err_px, tuple(error)

    def reset_believed(self, xy, coord="camera", reason="manual"):
        if coord == "camera":
            self.believed_cam = np.array(xy, dtype=float)
        else:
            self.believed_cam = _apply_homography(
                self.H_screen2cam, np.array(xy))[0]
        self._log("reset", {"pos": list(xy), "coord": coord,
                            "reason": reason})

    def report(self):
        lines = []
        lines.append("=" * 70)
        lines.append("CURSOR TWIN REPORT (camera px + normalized screen coords)")
        lines.append("=" * 70)
        lines.append(f"Believed cam: ({self.believed_cam[0]:.1f}, "
                     f"{self.believed_cam[1]:.1f})")
        lines.append(f"Believed screen: ({self.screen_xy[0]:.4f}, "
                     f"{self.screen_xy[1]:.4f})")
        lines.append(f"J (HID→cam) =\n{self.J}")
        lines.append("")
        lines.append("History:")
        for e in self.history:
            k = e["kind"]
            if k == "command":
                lines.append(
                    f"  [{e['t']}] CMD dx={e['dx']:>5} dy={e['dy']:>5} "
                    f"Δcam=({e['delta_cam'][0]:>7.1f},{e['delta_cam'][1]:>7.1f}) "
                    f"cam {e['before_cam']} → {e['after_cam']} "
                    f"screen {e['before_screen']} → {e['after_screen']} "
                    f"{e['note']}"
                )
            elif k == "observe":
                lines.append(
                    f"  [{e['t']}] OBS {e['label']}: "
                    f"det_cam={e['detected_cam']} bel_cam={e['believed_cam']} "
                    f"err={e['error_cam']} |{e['error_px']:.1f}|px "
                    f"det_scr={e['detected_screen']} bel_scr={e['believed_screen']}"
                )
            elif k == "reset":
                lines.append(f"  [{e['t']}] RESET → {e['pos']} "
                             f"({e['coord']}, {e['reason']})")
            else:
                lines.append(f"  [{e['t']}] {k}: {e}")
        errs = [e["error_px"] for e in self.history if e["kind"] == "observe"]
        if errs:
            lines.append("")
            lines.append(f"Observations: {len(errs)}, mean err "
                         f"{np.mean(errs):.1f}px, max err {np.max(errs):.1f}px")
        return "\n".join(lines)

    def save(self, path):
        with open(path, "w") as f:
            json.dump({
                "believed_cam": list(self.believed_cam),
                "believed_screen": list(self.screen_xy),
                "J": self.J.tolist(),
                "H_cam2screen": self.H_cam2screen.tolist(),
                "history": self.history,
            }, f, indent=2)
