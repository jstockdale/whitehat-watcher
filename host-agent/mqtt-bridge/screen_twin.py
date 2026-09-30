#!/usr/bin/env python3
"""
Digital twin for the Watcher eyes-on-screen input assistant.

Three coordinate frames:
  Screen (S): pixels of the display under test, origin top-left.
              This is where intent lives ("click the submit button").
  HID (H):    cumulative HID units from an arbitrary origin, defined as
              the cursor's position when the twin is created/reset.
              This is where commands live. Stable: never depends on the
              camera.
  Camera (C): camera pixels (e.g. 416x416). This is where noisy
              observations live.

Mappings maintained:
  H <-> C : affine, from CursorCalibrator (J, J_inv) + one anchor point
            (hid_origin_cam = camera position of HID (0,0)).
  S <-> C : 3x3 homography, from screen-edge detection and/or the
            fiducial. Refit continuously; a jump means the camera moved.

What the twin buys over raw per-frame detection:
  1. Predict + correct cursor tracking (Kalman filter in H frame).
     Commands predict for free; camera detections correct drift.
     A systematic move bias (miscalibration) is corrected over a few
     observations instead of compounding.
  2. Persistent element map: screenshot/fiducial/camera observations
     give elements addresses in all three frames with timestamps.
     A viewport epoch (resize/scroll) invalidates stale positions.
  3. Outlier rejection: detections far from prediction are UI changes,
     not the cursor. Repeated disagreement declares the cursor LOST;
     the twin then refuses to plan moves until re-anchored.
  4. Sanity checking: predicted-vs-detected residual flags bad moves
     instead of compounding them.
  5. Camera-disturbance recovery: a homography jump re-anchors S<->C
     while H-odom (and everything derived from it) stays valid.

Bench I/O is injected (no MQTT here). Testable offline.

Usage:
    from screen_twin import ScreenTwin

    twin = ScreenTwin(calibration_path="cursor_calibration_20260924_v2.json")
    twin.command(200, 0)                       # dead-reckoning predict
    twin.observe_cursor(cam_xy=(130, 214), confidence=0.8)  # correct
    twin.update_geometry(corners_cam, screen_wh=(1920, 1080))
    twin.add_element("submit", screen_xy=(242*scale...), source="screenshot")
    dx, dy = twin.move_to_element("submit")    # HID delta from believed pos
    twin.save("twin_state.json")
"""
import json
import math
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np

from cursor_calibration import CursorCalibrator
from screen_edges import (apply_homography, homography_reprojection_error,
                          rectangle_homography, solve_homography)

Point = Tuple[float, float]

# Outlier gate: reject detections further than this many sigma from prediction.
OUTLIER_SIGMA = 3.0
# Consecutive rejected detections after which the cursor is declared LOST.
# The twin then refuses to plan moves until re-anchored -- acting on a
# belief we know is wrong is how errors compound.
LOST_AFTER_OUTLIERS = 5
# A homography "jump" bigger than this mean corner shift (px) = camera moved.
GEOMETRY_JUMP_PX = 12.0
# Elements older than this are re-verified before use (screenshots go stale).
ELEMENT_STALE_S = 600.0


def estimate_translation_correction(src: np.ndarray,
                                    dst: np.ndarray) -> np.ndarray:
    """
    3x3 translation matrix mapping src -> dst (least-squares mean shift).
    src, dst: (N,2). Used for single-anchor refinement of an existing H.
    """
    shift = np.mean(dst - src, axis=0)
    T = np.eye(3)
    T[0, 2] = shift[0]
    T[1, 2] = shift[1]
    return T


def estimate_similarity(src: np.ndarray, dst: np.ndarray) -> np.ndarray:
    """
    3x3 similarity transform (translation + rotation + isotropic scale)
    mapping src -> dst via Umeyama. Needs >= 2 points.
    """
    n = src.shape[0]
    mu_s = src.mean(axis=0)
    mu_d = dst.mean(axis=0)
    sc = src - mu_s
    dc = dst - mu_d
    var_s = float(np.sum(sc ** 2) / n)
    if var_s < 1e-12:
        return estimate_translation_correction(src, dst)
    cov = (dc.T @ sc) / n
    U, D, Vt = np.linalg.svd(cov)
    S = np.eye(2)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        S[1, 1] = -1
    R = U @ S @ Vt
    scale = float(np.sum(D * np.diag(S)) / var_s)
    t = mu_d - scale * (R @ mu_s)
    M = np.eye(3)
    M[:2, :2] = scale * R
    M[:2, 2] = t
    return M


def estimate_affine(src: np.ndarray, dst: np.ndarray) -> np.ndarray:
    """
    3x3 affine transform mapping src -> dst (least squares). Needs >= 3
    non-collinear points. Falls back to similarity if degenerate.
    """
    n = src.shape[0]
    A = np.zeros((2 * n, 6))
    b = np.zeros(2 * n)
    for i, (s, d) in enumerate(zip(src, dst)):
        A[2 * i] = [s[0], s[1], 1, 0, 0, 0]
        A[2 * i + 1] = [0, 0, 0, s[0], s[1], 1]
        b[2 * i] = d[0]
        b[2 * i + 1] = d[1]
    try:
        p, residuals, rank, _ = np.linalg.lstsq(A, b, rcond=None)
    except np.linalg.LinAlgError:
        return estimate_similarity(src, dst)
    if rank < 6:
        return estimate_similarity(src, dst)
    M = np.eye(3)
    M[0] = [p[0], p[1], p[2]]
    M[1] = [p[3], p[4], p[5]]
    return M


class ScreenTwin:
    """
    The digital twin. Owns cursor belief (Kalman), element map, and the
    three frame mappings. All positions are floats; rounding happens only
    at the command boundary.
    """

    def __init__(self,
                 calibration_path: Optional[str] = None,
                 J: Optional[np.ndarray] = None,
                 initial_cam: Optional[Point] = None):
        """
        calibration_path: JSON from CursorCalibrator.save().
        J: 2x2 HID->camera Jacobian (alternative to calibration_path).
        initial_cam: camera position of the cursor now; defines the HID
            origin anchor. If None, the anchor is set on first observation.
        """
        if calibration_path:
            cal = CursorCalibrator.load(calibration_path)
            self.J = np.array(cal.J, dtype=float)
            self.J_inv = np.array(cal.J_inv, dtype=float)
            self.calibration_path = calibration_path
        elif J is not None:
            self.J = np.array(J, dtype=float)
            self.J_inv = np.linalg.inv(self.J)
            self.calibration_path = None
        else:
            raise ValueError("need calibration_path or J")

        # --- Kalman state in HID frame: [hx, hy, vx, vy] ---
        # vx/vy are HID units per SECOND (see command()/predict_dt()).
        self.x = np.zeros(4)
        # Start uncertain: the anchor observation snaps this down.
        # 50 HID ~= 10 camera px at our Jacobian -- honest for a first
        # detection. Velocity starts tighter; it is learned from commands.
        self.P = np.eye(4) * 2500.0
        self.P[2:, 2:] = np.eye(2) * 100.0
        self._last_cmd_t: Optional[float] = None

        # --- H<->C anchor: camera position of HID (0,0) ---
        self.hid_origin_cam: Optional[np.ndarray] = (
            np.array(initial_cam, dtype=float) if initial_cam else None)

        # --- S<->C homography (screen->camera), None until observed ---
        self.H_sc: Optional[np.ndarray] = None
        self.screen_wh: Optional[Tuple[float, float]] = None
        self.geometry_confidence: float = 0.0
        self.geometry_updated_at: Optional[float] = None

        # --- viewport epoch: bumped on resize/scroll; elements observed
        #     under an older epoch are stale (page layout moved under us)
        self.viewport_epoch: int = 0
        self._viewport: Optional[Tuple[float, float, float, float]] = None

        # --- element map: name -> record ---
        self.elements: Dict[str, dict] = {}

        self.outliers_rejected: int = 0
        self.consecutive_outliers: int = 0
        self.cursor_lost: bool = False
        self.geometry_jumps: int = 0
        self.history: List[dict] = []
        self._log("init", {"J": self.J.tolist()})

    # ------------------------------------------------------------------
    # frame conversions
    # ------------------------------------------------------------------
    def hid_to_cam(self, hid: Point) -> Point:
        """H -> C. Needs the origin anchor."""
        if self.hid_origin_cam is None:
            raise RuntimeError("HID origin anchor unknown: observe the cursor first")
        v = self.hid_origin_cam + self.J @ np.asarray(hid, dtype=float)
        return (float(v[0]), float(v[1]))

    def cam_to_hid(self, cam: Point) -> Point:
        """C -> H. Needs the origin anchor."""
        if self.hid_origin_cam is None:
            raise RuntimeError("HID origin anchor unknown: observe the cursor first")
        v = self.J_inv @ (np.asarray(cam, dtype=float) - self.hid_origin_cam)
        return (float(v[0]), float(v[1]))

    def screen_to_cam(self, screen: Point) -> Point:
        """S -> C. Needs the homography."""
        if self.H_sc is None:
            raise RuntimeError("screen<->camera homography unknown")
        v = apply_homography(self.H_sc, np.array(screen))[0]
        return (float(v[0]), float(v[1]))

    def cam_to_screen(self, cam: Point) -> Point:
        """C -> S. Needs the homography."""
        if self.H_sc is None:
            raise RuntimeError("screen<->camera homography unknown")
        H_cs = np.linalg.inv(self.H_sc)
        v = apply_homography(H_cs, np.array(cam))[0]
        return (float(v[0]), float(v[1]))

    def screen_to_hid(self, screen: Point) -> Point:
        """S -> H via C."""
        return self.cam_to_hid(self.screen_to_cam(screen))

    @property
    def believed_hid(self) -> Point:
        return (float(self.x[0]), float(self.x[1]))

    @property
    def believed_cam(self) -> Optional[Point]:
        if self.hid_origin_cam is None:
            return None
        return self.hid_to_cam(self.believed_hid)

    # ------------------------------------------------------------------
    # cursor tracking: command (predict) + observe (correct)
    # ------------------------------------------------------------------
    def command(self, dx: float, dy: float, note: str = "") -> Point:
        """
        Record a commanded HID move. Kalman predict step.
        Returns the new believed HID position.

        Process noise is honest about OS ballistics: a commanded move can
        deviate by ~5% of its size, so large moves grow uncertainty fast.
        """
        now = time.time()
        self.x[0] += dx
        self.x[1] += dy
        # velocity in HID/second, learned from command history
        if self._last_cmd_t is not None:
            dt = max(now - self._last_cmd_t, 1e-3)
            vx_inst, vy_inst = dx / dt, dy / dt
            alpha = 0.5
            self.x[2] = alpha * self.x[2] + (1 - alpha) * vx_inst
            self.x[3] = alpha * self.x[3] + (1 - alpha) * vy_inst
        self._last_cmd_t = now
        # process noise: ~5% of move size (OS ballistics are imperfect)
        q = 1.0 + (0.05 * (abs(dx) + abs(dy))) ** 2
        self.P[:2, :2] += np.eye(2) * q
        self.P[2:, 2:] += np.eye(2) * q * 0.1
        self._log("command", {"dx": dx, "dy": dy,
                              "believed_hid": [round(self.x[0], 1),
                                               round(self.x[1], 1)],
                              "note": note})
        return self.believed_hid

    def predict_dt(self, dt_s: float):
        """
        Coast the belief forward dt_s seconds with no new command.
        Uses the learned velocity; grows uncertainty with time. This is
        what carries the estimate through weak/occluded detections --
        e.g. OS jiggler drift or slow UI animation between observations.
        """
        if dt_s <= 0:
            return
        self.x[0] += self.x[2] * dt_s
        self.x[1] += self.x[3] * dt_s
        # uncertainty grows ~2 HID/s of unmodeled drift
        self.P[:2, :2] += np.eye(2) * (2.0 * dt_s) ** 2
        self._log("predict_dt", {"dt_s": round(dt_s, 2),
                                 "believed_hid": [round(self.x[0], 1),
                                                  round(self.x[1], 1)]})

    def observe_cursor(self, cam_xy: Point,
                       confidence: float = 0.7) -> dict:
        """
        Correct the belief with a camera detection of the cursor.
        Sets the HID origin anchor on first call.

        Returns {"accepted": bool, "residual_px": float, "reason": str}.
        Detections failing the outlier gate are rejected (counted, not fused).
        """
        cam = np.asarray(cam_xy, dtype=float)
        if self.hid_origin_cam is None:
            # First observation defines the anchor: current believed HID
            # (starts at (0,0)) sits at this camera position.
            self.hid_origin_cam = cam.copy()
            z = np.array([self.x[0], self.x[1]])
            self._kalman_update(z, r_scale=confidence)
            self._log("anchor", {"hid_origin_cam": [round(float(cam[0]), 1),
                                                    round(float(cam[1]), 1)]})
            return {"accepted": True, "residual_px": 0.0,
                    "reason": "anchor-initialized"}

        # predicted camera position of believed HID
        pred_cam = np.asarray(self.hid_to_cam(self.believed_hid))
        residual = cam - pred_cam
        residual_px = float(np.linalg.norm(residual))

        # measurement in HID frame
        z = np.asarray(self.cam_to_hid(cam))
        # innovation covariance for gating (position block, mapped to cam px)
        S_cam = self.J @ self.P[:2, :2] @ self.J.T + np.eye(2) * 100.0
        try:
            mahal2 = float(residual @ np.linalg.inv(S_cam) @ residual)
        except np.linalg.LinAlgError:
            mahal2 = 0.0

        if mahal2 > OUTLIER_SIGMA ** 2:
            self.outliers_rejected += 1
            self.consecutive_outliers += 1
            if (self.consecutive_outliers >= LOST_AFTER_OUTLIERS
                    and not self.cursor_lost):
                self.cursor_lost = True
                self._log("lost", {
                    "consecutive_outliers": self.consecutive_outliers,
                    "message": "CURSOR LOST: detections disagree with "
                               "prediction repeatedly. Refusing to plan "
                               "moves until re-anchored via reanchor()."})
            self._log("outlier", {
                "detected_cam": [round(float(cam[0]), 1), round(float(cam[1]), 1)],
                "predicted_cam": [round(float(pred_cam[0]), 1),
                                  round(float(pred_cam[1]), 1)],
                "residual_px": round(residual_px, 1),
                "mahal2": round(mahal2, 1)})
            return {"accepted": False, "residual_px": round(residual_px, 1),
                    "reason": f"outlier-gate mahal2={mahal2:.1f}"}

        # R scales inversely with confidence: confident detections pull harder
        r = max(0.25, 1.5 - confidence)
        self._kalman_update(z, r_scale=r)
        self.consecutive_outliers = 0
        if self.cursor_lost:
            self.cursor_lost = False
            self._log("relocalized",
                      {"via": "accepted observation after LOST"})
        self._log("observe", {
            "detected_cam": [round(float(cam[0]), 1), round(float(cam[1]), 1)],
            "confidence": round(confidence, 2),
            "residual_px": round(residual_px, 1),
            "believed_hid": [round(self.x[0], 1), round(self.x[1], 1)]})
        return {"accepted": True, "residual_px": round(residual_px, 1),
                "reason": "fused"}

    def reanchor(self, cam_xy: Point, confidence: float = 0.9) -> dict:
        """
        Relocalize after the cursor is LOST (or whenever the belief is
        known-bad): bypass the outlier gate and snap the belief to a
        trusted detection. The HID origin anchor is unchanged -- only the
        believed position/velocity and covariance are reset.
        """
        if self.hid_origin_cam is None:
            # no anchor at all: this IS the anchor
            return self.observe_cursor(cam_xy, confidence=confidence)
        hid = np.asarray(self.cam_to_hid(cam_xy), dtype=float)
        self.x[0], self.x[1] = hid[0], hid[1]
        self.x[2], self.x[3] = 0.0, 0.0
        self.P[:2, :2] = np.eye(2) * 2500.0
        self.P[2:, 2:] = np.eye(2) * 100.0
        self.consecutive_outliers = 0
        self.cursor_lost = False
        self._log("reanchor",
                  {"cam": [round(float(cam_xy[0]), 1),
                           round(float(cam_xy[1]), 1)],
                   "hid": [round(float(hid[0]), 1), round(float(hid[1]), 1)]})
        return {"reanchored": True,
                "hid": (float(hid[0]), float(hid[1]))}

    def _kalman_update(self, z: np.ndarray, r_scale: float = 1.0):
        """Standard KF update. Measurement = HID position (Hx = [I 0])."""
        Hm = np.zeros((2, 4))
        Hm[0, 0] = 1.0
        Hm[1, 1] = 1.0
        # base measurement noise: ~10px detection noise mapped through J_inv
        R_cam = (10.0 * r_scale) ** 2
        R = self.J_inv @ (np.eye(2) * R_cam) @ self.J_inv.T
        S = Hm @ self.P @ Hm.T + R
        K = self.P @ Hm.T @ np.linalg.inv(S)
        y = z - Hm @ self.x
        self.x = self.x + K @ y
        self.P = (np.eye(4) - K @ Hm) @ self.P

    def sanity_check(self, detected_cam: Point,
                     warn_px: float = 15.0) -> dict:
        """
        Compare a fresh detection against prediction WITHOUT fusing it.
        Returns {"residual_px", "ok", "message"}. This is the check that
        would have caught the step-5 failure: prediction vs reality.
        """
        if self.hid_origin_cam is None or self.believed_cam is None:
            return {"residual_px": None, "ok": True,
                    "message": "no belief yet; nothing to check against"}
        pred = np.asarray(self.believed_cam)
        det = np.asarray(detected_cam, dtype=float)
        residual = float(np.linalg.norm(det - pred))
        ok = residual <= warn_px
        return {"residual_px": round(residual, 1), "ok": ok,
                "message": ("prediction matches detection"
                            if ok else
                            f"PREDICTION MISMATCH: {residual:.1f}px -- "
                            f"move may not have executed or model drifted")}

    # ------------------------------------------------------------------
    # geometry: screen <-> camera homography from edges / fiducial
    # ------------------------------------------------------------------
    def _adopt_homography(self, H_new: np.ndarray,
                          screen_wh: Tuple[float, float],
                          via: str, confidence: float) -> dict:
        """
        Adopt a new S<->C homography. Detects camera jumps: if the new
        homography moves the screen corners by > GEOMETRY_JUMP_PX vs the
        old one, the camera was disturbed -- S<->C is replaced, H-odom
        stays valid, element camera positions are marked stale.
        """
        from screen_edges import screen_corners_wh
        jumped = False
        if (self.H_sc is not None and self.screen_wh is not None
                and tuple(self.screen_wh) == (float(screen_wh[0]),
                                              float(screen_wh[1]))):
            sc = screen_corners_wh(*screen_wh)
            old_c = apply_homography(self.H_sc, sc)
            new_c = apply_homography(H_new, sc)
            shift = float(np.mean(np.linalg.norm(new_c - old_c, axis=1)))
            if shift > GEOMETRY_JUMP_PX:
                jumped = True
                self.geometry_jumps += 1
                for rec in self.elements.values():
                    rec["cam_stale"] = True
        self.H_sc = np.asarray(H_new, dtype=float)
        self.screen_wh = (float(screen_wh[0]), float(screen_wh[1]))
        self.geometry_confidence = float(confidence)
        self.geometry_updated_at = time.time()
        self._log("geometry", {"updated": True, "jumped": jumped,
                               "via": via,
                               "confidence": round(confidence, 3),
                               "screen_wh": list(self.screen_wh)})
        return {"updated": True, "jumped": jumped, "via": via,
                "reason": "camera-disturbance: homography re-anchored"
                          if jumped else f"homography refined via {via}"}

    def update_geometry(self, corners_cam: Dict[str, Optional[Point]],
                        screen_wh: Tuple[float, float],
                        confidence: float) -> dict:
        """
        Refit S<->C from detected display corners.

        Updates with confidence < 0.4 are refused outright: a
        wrong-but-confident homography is worse than no update.
        """
        if confidence < 0.4:
            self._log("geometry", {"updated": False,
                                   "reason": f"confidence {confidence:.2f} "
                                             f"below 0.4 threshold"})
            return {"updated": False,
                    "reason": f"confidence {confidence:.2f} below threshold; "
                              f"keeping previous homography"}
        # a viewport size change invalidates stored element positions
        self.note_viewport(screen_wh[0], screen_wh[1])
        H_new = rectangle_homography(corners_cam, screen_wh[0], screen_wh[1])
        if H_new is None:
            return {"updated": False, "reason": "need all 4 corners"}
        return self._adopt_homography(H_new, screen_wh, "edges", confidence)

    def update_geometry_fiducials(self, marks: list,
                                  screen_wh: Tuple[float, float]) -> dict:
        """
        Refine S<->C from detected fiducial marks.

        marks: list of {"corner_id": "tl"|"tr"|"bl"|"br"|None,
                        "screen_xy": (x, y),   # from page telemetry
                        "cam_xy": (x, y),      # from find_fiducials()
                        "confidence": float}

        Design: ONE homography H, continuously refined by whatever the
        camera sees. Each observation counts as a CORRECTION to the
        current H at the degrees of freedom the anchor count supports --
        never a blind replacement (except the 4-anchor DLT, which is a
        full re-solve with jump detection):
          4 marks -> full DLT homography (the money case).
          3 marks -> affine correction  (translation/rotation/scale/shear).
          2 marks -> similarity correction (translation/rotation/scale).
          1 mark  -> translation correction (camera bump / drift fix).
          0 marks -> dead reckoning (no update; H-odom carries us).
        A single anchor thus improves absolute accuracy: it pins the
        translation so every camera pixel maps to an absolute viewport
        coordinate, with uncertainty growing by distance from the anchor.
        Marks without a corner_id are ignored for geometry (identity
        unknown) but still logged.
        """
        self.note_viewport(screen_wh[0], screen_wh[1])
        identified = [m for m in marks if m.get("corner_id") in
                      ("tl", "tr", "bl", "br")]
        by_id = {}
        for m in identified:
            c = by_id.get(m["corner_id"])
            if c is None or m["confidence"] > c["confidence"]:
                by_id[m["corner_id"]] = m
        need = ("tl", "tr", "br", "bl")
        n = len(by_id)
        if n == 0:
            return {"updated": False, "jumped": False, "via": "fiducials",
                    "reason": "no identified fiducial marks; dead reckoning"}
        if n == 4 and all(k in by_id for k in need):
            src = np.array([by_id[k]["screen_xy"] for k in need])
            dst = np.array([by_id[k]["cam_xy"] for k in need])
            H_new = solve_homography(src, dst)
            err = homography_reprojection_error(H_new, src, dst)
            conf = float(np.mean([by_id[k]["confidence"] for k in need]))
            res = self._adopt_homography(H_new, screen_wh, "4 fiducials",
                                         conf)
            res["reprojection_px"] = round(err, 2)
            return res
        if self.H_sc is None:
            # no prior H: bootstrap directly at the supported DOF.
            # 4 marks -> DLT (handled above). 3 -> affine, 2 -> similarity,
            # 1 -> translation seed (via update_geometry_fiducial).
            if n == 1:
                m = next(iter(by_id.values()))
                return self.update_geometry_fiducial(
                    m["cam_xy"], m["screen_xy"], screen_wh)
            ids = sorted(by_id.keys())
            src = np.array([by_id[k]["screen_xy"] for k in ids])
            dst = np.array([by_id[k]["cam_xy"] for k in ids])
            if n == 3:
                H_new = estimate_affine(src, dst)
                via = "3 fiducials (affine bootstrap)"
                conf = float(np.mean([by_id[k]["confidence"]
                                      for k in ids])) * 0.7
            elif n == 2:
                H_new = estimate_similarity(src, dst)
                via = "2 fiducials (similarity bootstrap)"
                conf = float(np.mean([by_id[k]["confidence"]
                                      for k in ids])) * 0.5
            else:
                return {"updated": False, "jumped": False,
                        "via": f"{n} fiducials",
                        "reason": "no prior homography; need 1-4 marks"}
            res = self._adopt_homography(H_new, screen_wh, via, conf)
            res["n_marks"] = n
            return res
        # correction path: estimate C mapping (H @ screen) -> cam_obs,
        # then H <- C @ H. DOF grows with anchor count.
        ids = sorted(by_id.keys())
        src_s = np.array([by_id[k]["screen_xy"] for k in ids])
        dst_c = np.array([by_id[k]["cam_xy"] for k in ids])
        pred_c = apply_homography(self.H_sc, src_s)
        if n == 1:
            C = estimate_translation_correction(pred_c, dst_c)
            via = "1 fiducial (translation correction)"
            conf = float(by_id[ids[0]]["confidence"]) * 0.5
        elif n == 2:
            C = estimate_similarity(pred_c, dst_c)
            via = "2 fiducials (similarity correction)"
            conf = float(np.mean([by_id[k]["confidence"] for k in ids])) \
                * 0.7
        else:  # n == 3
            C = estimate_affine(pred_c, dst_c)
            via = "3 fiducials (affine correction)"
            conf = float(np.mean([by_id[k]["confidence"] for k in ids])) \
                * 0.85
        H_new = C @ self.H_sc
        resid = float(np.mean(np.linalg.norm(
            apply_homography(H_new, src_s) - dst_c, axis=1)))
        res = self._adopt_homography(H_new, screen_wh, via, conf)
        res["correction_residual_px"] = round(resid, 2)
        res["n_marks"] = n
        return res

    def update_geometry_fiducial(self, fiducial_cam: Point,
                                 fiducial_screen: Point,
                                 screen_wh: Tuple[float, float]) -> dict:
        """
        Refine S<->C with one absolute correspondence from the fiducial.
        With no prior homography, seeds a translation-only map (low
        confidence; edges or more anchors should refine it).
        With a prior H, APPLIES the translation correction: measures where
        H thought the anchor was, shifts H by the error. This is what lets
        a single anchor improve absolute accuracy -- it pins the
        translation so camera pixels map to absolute viewport coords.
        """
        if self.H_sc is None:
            # seed: translation mapping fiducial_screen -> fiducial_cam.
            # Mark low confidence: rotation/scale/perspective unknown.
            H = np.eye(3)
            H[0, 2] = fiducial_cam[0] - fiducial_screen[0]
            H[1, 2] = fiducial_cam[1] - fiducial_screen[1]
            self.H_sc = H
            self.screen_wh = (float(screen_wh[0]), float(screen_wh[1]))
            self.geometry_confidence = 0.3
            self.geometry_updated_at = time.time()
            self._log("geometry", {"updated": True, "jumped": False,
                                   "via": "fiducial-seed", "confidence": 0.3})
            return {"updated": True, "jumped": False,
                    "reason": "seeded translation-only map from fiducial; "
                              "awaiting more anchors or edge homography"}
        # correction: shift H so the anchor lands where the camera sees it
        pred = self.screen_to_cam(fiducial_screen)
        err = (fiducial_cam[0] - pred[0], fiducial_cam[1] - pred[1])
        err_px = math.dist(pred, fiducial_cam)
        T = np.eye(3)
        T[0, 2], T[1, 2] = err
        H_new = T @ self.H_sc
        res = self._adopt_homography(H_new, screen_wh,
                                     "1 fiducial (translation correction)",
                                     0.5)
        res["correction_px"] = round(err_px, 2)
        res["n_marks"] = 1
        self._log("fiducial-check",
                  {"screen": list(fiducial_screen), "cam": list(fiducial_cam),
                   "correction_px": round(err_px, 2)})
        return res

    # ------------------------------------------------------------------
    # viewport epoch: resize / scroll / rerender moves page elements
    # ------------------------------------------------------------------
    def note_viewport(self, vw: float, vh: float,
                      scroll_x: Optional[float] = None,
                      scroll_y: Optional[float] = None) -> dict:
        """
        Call when fiducial/console telemetry arrives. Any change in
        viewport size or scroll position bumps the epoch: element screen
        positions observed under an older epoch are stale and must be
        re-observed before use. (The fiducial itself is position:fixed,
        so IT doesn't move -- everything else might have.)
        scroll_x/scroll_y = None means "unknown, leave the stored value".
        """
        if self._viewport is None:
            self._viewport = (float(vw), float(vh),
                              float(scroll_x or 0.0), float(scroll_y or 0.0))
            return {"epoch": self.viewport_epoch, "changed": False}
        _, _, cur_sx, cur_sy = self._viewport
        vp = (float(vw), float(vh),
              cur_sx if scroll_x is None else float(scroll_x),
              cur_sy if scroll_y is None else float(scroll_y))
        if vp != self._viewport:
            self._viewport = vp
            self.viewport_epoch += 1
            self._log("viewport", {"epoch": self.viewport_epoch,
                                   "viewport": list(vp),
                                   "message": "viewport changed; element "
                                              "positions from older epochs "
                                              "are stale"})
            return {"epoch": self.viewport_epoch, "changed": True}
        return {"epoch": self.viewport_epoch, "changed": False}

    # ------------------------------------------------------------------
    # element map
    # ------------------------------------------------------------------
    def add_element(self, name: str,
                    screen_xy: Optional[Point] = None,
                    cam_xy: Optional[Point] = None,
                    hid_xy: Optional[Point] = None,
                    source: str = "camera",
                    confidence: float = 0.7) -> dict:
        """
        Register a UI element's position. Give whichever frame(s) you have;
        the twin fills in the rest via current mappings. Re-observations
        refine the stored position (confidence-weighted average in HID).
        """
        now = time.time()
        rec = self.elements.get(name)
        # resolve to HID as the canonical frame
        hid = None
        if hid_xy is not None:
            hid = np.asarray(hid_xy, dtype=float)
        elif cam_xy is not None and self.hid_origin_cam is not None:
            hid = np.asarray(self.cam_to_hid(cam_xy))
        elif screen_xy is not None and self.H_sc is not None \
                and self.hid_origin_cam is not None:
            hid = np.asarray(self.screen_to_hid(screen_xy))

        if rec is None:
            rec = {"hid": None, "screen": None, "cam": None,
                   "source": source, "confidence": 0.0,
                   "first_seen": now, "last_seen": now,
                   "observations": 0, "cam_stale": False,
                   "viewport_epoch": self.viewport_epoch}
            self.elements[name] = rec

        if hid is not None:
            # A new viewport epoch means the old stored position is not
            # just noisy -- it is WRONG (layout moved). Discard it and
            # start fresh instead of averaging truth with garbage.
            if rec.get("viewport_epoch", 0) != self.viewport_epoch:
                rec["hid"] = None
                rec["observations"] = 0
                rec["confidence"] = 0.0
            w_new = confidence
            w_old = rec["confidence"] * rec["observations"]
            if rec["hid"] is None or w_old == 0:
                rec["hid"] = hid
            else:
                rec["hid"] = (rec["hid"] * w_old + hid * w_new) / (w_old + w_new)
            rec["observations"] += 1
            rec["confidence"] = min(1.0, (w_old + w_new) /
                                    max(rec["observations"], 1))
            rec["last_seen"] = now
            rec["source"] = source
            rec["cam_stale"] = False
            rec["viewport_epoch"] = self.viewport_epoch
            # refresh derived frames
            try:
                rec["cam"] = self.hid_to_cam(tuple(rec["hid"]))
            except RuntimeError:
                rec["cam"] = None
            try:
                if rec["cam"] is not None and self.H_sc is not None:
                    rec["screen"] = self.cam_to_screen(rec["cam"])
            except RuntimeError:
                pass
        elif screen_xy is not None:
            # geometry not ready yet; keep the screen position for later
            rec["screen"] = tuple(float(v) for v in screen_xy)
            rec["last_seen"] = now
            rec["source"] = source

        self._log("element", {"name": name,
                              "hid": [round(float(v), 1) for v in rec["hid"]]
                                     if rec["hid"] is not None else None,
                              "source": source,
                              "observations": rec["observations"]})
        return {"name": name,
                "hid": tuple(float(v) for v in rec["hid"])
                       if rec["hid"] is not None else None,
                "observations": rec["observations"]}

    def get_element(self, name: str,
                    max_age_s: float = ELEMENT_STALE_S) -> Optional[dict]:
        """
        Fetch an element. Returns None if unknown, too old, or observed
        under an older viewport epoch (page scrolled/resized/rerendered
        since -- the stored position is no longer trustworthy).
        """
        rec = self.elements.get(name)
        if rec is None or rec["hid"] is None:
            return None
        if rec.get("viewport_epoch", 0) != self.viewport_epoch:
            return None
        age = time.time() - rec["last_seen"]
        if age > max_age_s:
            return None
        return {"name": name,
                "hid": tuple(float(v) for v in rec["hid"]),
                "screen": rec["screen"], "cam": rec["cam"],
                "age_s": round(age, 1), "observations": rec["observations"],
                "cam_stale": rec["cam_stale"], "source": rec["source"]}

    def move_to_element(self, name: str) -> Tuple[int, int]:
        """
        HID delta from believed position to the element's stored position.
        Raises KeyError if unknown/stale, RuntimeError if the cursor
        belief is LOST (re-anchor first -- never plan from a known-bad
        belief).
        """
        if self.cursor_lost:
            raise RuntimeError(
                "cursor belief is LOST (detections disagree with prediction); "
                "call reanchor() with a trusted detection before planning moves")
        rec = self.get_element(name)
        if rec is None:
            raise KeyError(f"element {name!r} unknown or stale")
        dx = rec["hid"][0] - self.x[0]
        dy = rec["hid"][1] - self.x[1]
        return (int(round(dx)), int(round(dy)))

    def refresh_element_frames(self):
        """Re-derive cam/screen positions after a geometry update."""
        for rec in self.elements.values():
            if rec["hid"] is None:
                continue
            try:
                rec["cam"] = self.hid_to_cam(tuple(rec["hid"]))
                rec["cam_stale"] = False
            except RuntimeError:
                pass
            try:
                if rec["cam"] is not None and self.H_sc is not None:
                    rec["screen"] = self.cam_to_screen(rec["cam"])
            except RuntimeError:
                pass

    # ------------------------------------------------------------------
    # persistence / reporting
    # ------------------------------------------------------------------
    def _log(self, kind: str, detail: dict):
        self.history.append({"t": time.time(), "kind": kind, **detail})
        if len(self.history) > 2000:
            self.history = self.history[-2000:]

    def save(self, path: str) -> str:
        data = {
            "J": self.J.tolist(),
            "kalman_x": self.x.tolist(),
            "kalman_P": self.P.tolist(),
            "hid_origin_cam": (self.hid_origin_cam.tolist()
                               if self.hid_origin_cam is not None else None),
            "H_sc": self.H_sc.tolist() if self.H_sc is not None else None,
            "screen_wh": self.screen_wh,
            "geometry_confidence": self.geometry_confidence,
            "elements": {k: {**v,
                             "hid": v["hid"].tolist()
                                    if isinstance(v["hid"], np.ndarray)
                                    else v["hid"]}
                         for k, v in self.elements.items()},
            "outliers_rejected": self.outliers_rejected,
            "consecutive_outliers": self.consecutive_outliers,
            "cursor_lost": self.cursor_lost,
            "geometry_jumps": self.geometry_jumps,
            "viewport_epoch": self.viewport_epoch,
            "viewport": self._viewport,
            "calibration_path": self.calibration_path,
        }
        Path(path).write_text(json.dumps(data, indent=2))
        return path

    @classmethod
    def load(cls, path: str) -> "ScreenTwin":
        data = json.loads(Path(path).read_text())
        twin = cls(J=np.array(data["J"]))
        twin.x = np.array(data["kalman_x"])
        twin.P = np.array(data["kalman_P"])
        if data["hid_origin_cam"] is not None:
            twin.hid_origin_cam = np.array(data["hid_origin_cam"])
        if data["H_sc"] is not None:
            twin.H_sc = np.array(data["H_sc"])
        twin.screen_wh = tuple(data["screen_wh"]) if data["screen_wh"] else None
        twin.geometry_confidence = data["geometry_confidence"]
        for k, v in data["elements"].items():
            if v["hid"] is not None:
                v["hid"] = np.array(v["hid"])
            twin.elements[k] = v
        twin.outliers_rejected = data["outliers_rejected"]
        twin.consecutive_outliers = data.get("consecutive_outliers", 0)
        twin.cursor_lost = data.get("cursor_lost", False)
        twin.geometry_jumps = data["geometry_jumps"]
        twin.viewport_epoch = data.get("viewport_epoch", 0)
        twin._viewport = (tuple(data["viewport"])
                          if data.get("viewport") else None)
        twin.calibration_path = data.get("calibration_path")
        return twin

    def report(self) -> str:
        lines = ["=" * 70,
                 "SCREEN TWIN REPORT",
                 "=" * 70,
                 f"believed HID: ({self.x[0]:.1f}, {self.x[1]:.1f})",
                 f"believed cam: {self.believed_cam}",
                 f"HID origin anchor: "
                 f"{self.hid_origin_cam.tolist() if self.hid_origin_cam is not None else None}",
                 f"S<->C homography: "
                 f"{'set' if self.H_sc is not None else 'UNSET'} "
                 f"(conf {self.geometry_confidence:.2f})",
                 f"screen_wh: {self.screen_wh}",
                 f"outliers rejected: {self.outliers_rejected}, "
                 f"geometry jumps: {self.geometry_jumps}",
                 f"cursor: {'LOST -- reanchor required' if self.cursor_lost else 'tracked'}",
                 f"viewport epoch: {self.viewport_epoch}",
                 f"elements: {len(self.elements)}"]
        for name, rec in self.elements.items():
            hid = rec["hid"]
            lines.append(
                f"  {name}: hid="
                f"{(round(float(hid[0]),1), round(float(hid[1]),1)) if hid is not None else None} "
                f"obs={rec['observations']} src={rec['source']} "
                f"stale_cam={rec['cam_stale']}")
        return "\n".join(lines)
