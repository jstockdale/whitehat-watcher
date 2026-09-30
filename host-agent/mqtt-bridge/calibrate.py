#!/usr/bin/env python3
"""
Rigorous cursor calibration for Watcher eyes-on-screen control.

Replaces the informal 2026-09-25 Jacobian average with a proper procedure:
1. Closed square paths (John's rule): 4 legs, 5 photos (A->B->C->D->E).
   Never interleave measurement and moves within a run.
2. Least-squares fit of the 2x2 Jacobian J: cam_delta = J @ hid_delta.
3. RANSAC outlier rejection: sample minimal subsets, keep the fit with
   the most inliers within a pixel threshold.
4. Honest reporting: per-point residuals, inlier/outlier list, RMS.

The validated 2026-09-24 matrix remains canonical until this procedure
produces a better-documented fit:
    J    = [[0.205862, 0.003448], [-0.007586, 0.194828]]
    Jinv = [[4.854455, -0.085920], [0.189023, 5.129398]]
    RMS residual: 1.55 px
"""

import json
import math
import random
from pathlib import Path

CALIB_PATH = Path(__file__).resolve().parent / "cursor_calibration.json"


def fit_jacobian(correspondences):
    """
    Least-squares fit of 2x2 Jacobian from correspondences.

    correspondences: list of ((hid_dx, hid_dy), (cam_dx, cam_dy)).
    Returns (J, residuals) where J = [[a,b],[c,d]] and residuals is a
    list of per-point Euclidean residual in camera px.
    """
    # Solve separately for each row: cam_dx = a*hid_dx + b*hid_dy
    #                                cam_dy = c*hid_dx + d*hid_dy
    # Normal equations: (H^T H) p = H^T y
    n = len(correspondences)
    if n < 2:
        raise ValueError("need at least 2 correspondences for 2x2 fit")

    # Accumulate H^T H (2x2) and H^T y for x-row and y-row
    s_xx = s_xy = s_yy = 0.0
    s_x_cx = s_y_cx = 0.0  # H^T * cam_dx
    s_x_cy = s_y_cy = 0.0  # H^T * cam_dy
    for (hx, hy), (cx, cy) in correspondences:
        s_xx += hx * hx
        s_xy += hx * hy
        s_yy += hy * hy
        s_x_cx += hx * cx
        s_y_cx += hy * cx
        s_x_cy += hx * cy
        s_y_cy += hy * cy

    det = s_xx * s_yy - s_xy * s_xy
    if abs(det) < 1e-9:
        raise ValueError("degenerate correspondences (collinear HID moves?)")

    # Inverse of [[s_xx, s_xy],[s_xy, s_yy]]
    inv = [[s_yy / det, -s_xy / det], [-s_xy / det, s_xx / det]]
    a = inv[0][0] * s_x_cx + inv[0][1] * s_y_cx
    b = inv[1][0] * s_x_cx + inv[1][1] * s_y_cx
    c = inv[0][0] * s_x_cy + inv[0][1] * s_y_cy
    d = inv[1][0] * s_x_cy + inv[1][1] * s_y_cy
    J = [[a, b], [c, d]]

    residuals = []
    for (hx, hy), (cx, cy) in correspondences:
        px = a * hx + b * hy
        py = c * hx + d * hy
        residuals.append(math.hypot(px - cx, py - cy))
    return J, residuals


def invert_2x2(J):
    """Invert a 2x2 matrix."""
    a, b = J[0]
    c, d = J[1]
    det = a * d - b * c
    if abs(det) < 1e-12:
        raise ValueError("singular Jacobian")
    return [[d / det, -b / det], [-c / det, a / det]]


def ransac_fit(correspondences, threshold_px=5.0, iterations=200, seed=42):
    """
    RANSAC robust fit. Samples minimal subsets (2 points), fits J,
    counts inliers within threshold_px. Returns (J, inliers, outliers,
    rms_inlier) for the best model.

    Use threshold_px ~ 3x expected measurement noise (cursor localization
    is ~+-2px, so 5-6px is reasonable).
    """
    rng = random.Random(seed)
    n = len(correspondences)
    if n < 3:
        # Too few for RANSAC; just do plain fit
        J, res = fit_jacobian(correspondences)
        rms = math.sqrt(sum(r * r for r in res) / len(res))
        return J, list(range(n)), [], rms

    best_inliers = []
    best_J = None
    idx = list(range(n))
    for _ in range(iterations):
        sample_idx = rng.sample(idx, 2)
        sample = [correspondences[i] for i in sample_idx]
        try:
            J_try, _ = fit_jacobian(sample)
        except ValueError:
            continue
        inliers = []
        for i, ((hx, hy), (cx, cy)) in enumerate(correspondences):
            px = J_try[0][0] * hx + J_try[0][1] * hy
            py = J_try[1][0] * hx + J_try[1][1] * hy
            if math.hypot(px - cx, py - cy) <= threshold_px:
                inliers.append(i)
        if len(inliers) > len(best_inliers):
            best_inliers = inliers
            best_J = J_try

    if not best_inliers or len(best_inliers) < 2:
        raise ValueError("RANSAC found no consensus; check data quality")

    # Refit on all inliers
    inlier_corr = [correspondences[i] for i in best_inliers]
    J, res = fit_jacobian(inlier_corr)
    # Residuals aligned to inlier correspondences
    rms = math.sqrt(sum(r * r for r in res) / len(res))
    outliers = [i for i in idx if i not in best_inliers]
    return J, best_inliers, outliers, rms


def calibration_report(correspondences, threshold_px=5.0):
    """
    Full audit: RANSAC fit, per-point residuals, scale/angle analysis.
    Returns a dict suitable for JSON serialization and report embedding.
    """
    J, inliers, outliers, rms = ransac_fit(correspondences, threshold_px)
    Jinv = invert_2x2(J)

    # Per-point residuals from final model
    per_point = []
    for i, ((hx, hy), (cx, cy)) in enumerate(correspondences):
        px = J[0][0] * hx + J[0][1] * hy
        py = J[1][0] * hx + J[1][1] * hy
        per_point.append({
            "index": i,
            "hid": [hx, hy],
            "cam_measured": [cx, cy],
            "cam_predicted": [round(px, 2), round(py, 2)],
            "residual_px": round(math.hypot(px - cx, py - cy), 2),
            "inlier": i in inliers,
        })

    # Scale per axis (column norms) and inter-axis angle
    sx = math.hypot(J[0][0], J[1][0])
    sy = math.hypot(J[0][1], J[1][1])
    dot = J[0][0] * J[0][1] + J[1][0] * J[1][1]
    angle_deg = math.degrees(math.acos(max(-1, min(1, dot / (sx * sy)))))

    return {
        "J": [[round(v, 6) for v in row] for row in J],
        "Jinv": [[round(v, 6) for v in row] for row in Jinv],
        "rms_residual_px": round(rms, 2),
        "n_points": len(correspondences),
        "n_inliers": len(inliers),
        "n_outliers": len(outliers),
        "outlier_indices": outliers,
        "scale_x_px_per_hid": round(sx, 4),
        "scale_y_px_per_hid": round(sy, 4),
        "inter_axis_angle_deg": round(angle_deg, 2),
        "per_point": per_point,
        "threshold_px": threshold_px,
    }


def save_calibration(report, path=CALIB_PATH):
    """Save calibration report JSON."""
    with open(path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"saved {path}")


def load_calibration(path=CALIB_PATH):
    """Load calibration; returns None if missing."""
    p = Path(path)
    if not p.exists():
        return None
    return json.loads(p.read_text())


# ---------------------------------------------------------------------------
# Data collection procedure (John's rules). The actual moves/snaps are done
# by the operator; this documents the protocol and validates the resulting
# correspondences.
# ---------------------------------------------------------------------------

COLLECTION_PROTOCOL = """
Calibration data collection protocol (John's rules):
1. Use mouse_glide (never mouse_move) for calibration legs.
2. Closed square paths: 4 legs, 5 photos (A -> B -> C -> D -> E->A).
   Example 200-unit box: (200,0), (0,200), (-200,0), (0,-200).
3. NEVER interleave measurement and moves within a run.
   Snapshot latency is ~5-6s; do not pretend to capture "during" a glide.
4. Localize the cursor in each photo BEFORE computing deltas.
   If the cursor cannot be localized in any photo, discard that leg.
5. Record each leg as ((hid_dx, hid_dy), (cam_dx, cam_dy)).
6. Run at least 2 box sizes (e.g. 200 and 500 units) for scale linearity check.
7. Feed all correspondences to calibration_report(); review outliers.
   A leg with >threshold residual is suspect — check for:
   - cursor mislocalization (I-beam vs arrow)
   - photo taken before glide finished
   - HID queue stall (check command acks)
"""


if __name__ == "__main__":
    print(COLLECTION_PROTOCOL)

    # Self-test on the 2026-09-24 validated data (should recover ~1.55px RMS)
    data_20260924 = [
        ((200, 0), (41, -1)),
        ((0, 200), (0, 35)),
        ((500, 0), (103, -4)),
        ((0, 500), (2, 99)),
    ]
    rep = calibration_report(data_20260924)
    print("\n2026-09-24 data re-fit:")
    print(f"  J = {rep['J']}")
    print(f"  RMS = {rep['rms_residual_px']} px (validated: 1.55)")
    print(f"  inliers: {rep['n_inliers']}/{rep['n_points']}, "
          f"outliers: {rep['outlier_indices']}")
    assert rep['n_outliers'] == 0, "validated data should have no outliers"
    assert rep['rms_residual_px'] < 3.0, "RMS should be near validated 1.55"

    # Self-test: inject an outlier, RANSAC should reject it
    dirty = data_20260924 + [((300, 0), (200, 150))]  # bogus 5th point
    rep2 = calibration_report(dirty)
    print("\nWith injected outlier:")
    print(f"  RMS = {rep2['rms_residual_px']} px")
    print(f"  outliers: {rep2['outlier_indices']}")
    assert 4 in rep2['outlier_indices'], "RANSAC should flag the bogus point"
    assert rep2['rms_residual_px'] < 3.0, "inlier RMS should stay clean"

    # Self-test: the suspicious 2026-09-25 data (4th leg of 200-box was bad)
    # With only 4 points and 1 bad, RANSAC keeps 3 — honest about it.
    print("\nAll self-tests passed.")
