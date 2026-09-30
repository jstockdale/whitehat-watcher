#!/usr/bin/env python3
"""
Non-orthonormal cursor calibration for Watcher eyes-on-screen.

Solves for the full 2x2 affine Jacobian mapping HID units to camera pixels,
accounting for perspective distortion (shear, non-uniform scale, rotation).

    [Δpx]   [a b] [Δhid_x]
    [Δpy] = [c d] [Δhid_y]

The off-diagonal terms (b, c) capture the non-orthonormality:
- b: how much X pixel shifts during a pure Y HID move
- c: how much Y pixel shifts during a pure X HID move

Usage:
    from cursor_calibration import CursorCalibrator

    cal = CursorCalibrator()
    # Add correspondences from calibration moves
    cal.add_correspondence(dhid_x=200, dhid_y=0, dpx=125, dpy=11)
    cal.add_correspondence(dhid_x=0, dhid_y=200, dpx=-11, dpy=119)
    cal.solve()  # Least squares fit

    # Forward: HID -> pixel
    dpx, dpy = cal.hid_to_pixel(100, 50)

    # Inverse: pixel -> HID (for targeting)
    dhid_x, dhid_y = cal.pixel_to_hid(100, 60)

    cal.save('calibration.json')
"""

import json
import numpy as np
from pathlib import Path
from typing import List, Tuple, Optional


class CursorCalibrator:
    """
    Solves for the 2x2 affine Jacobian between HID units and camera pixels.

    Uses least squares over N>=2 correspondences. With exactly 2 orthogonal
    moves, gives the exact solution. With more, gives the best fit.
    """

    def __init__(self):
        self.correspondences: List[Tuple[float, float, float, float]] = []
        # Each: (dhid_x, dhid_y, dpx, dpy)
        self.J: Optional[np.ndarray] = None  # 2x2 forward Jacobian
        self.J_inv: Optional[np.ndarray] = None  # 2x2 inverse
        self.residual_rms: Optional[float] = None
        self.n_points: int = 0

    def add_correspondence(self, dhid_x: float, dhid_y: float,
                           dpx: float, dpy: float) -> None:
        """Add one measured (HID delta -> pixel delta) pair."""
        self.correspondences.append((dhid_x, dhid_y, dpx, dpy))
        self.J = None  # Invalidate solved matrices
        self.J_inv = None

    def solve(self) -> np.ndarray:
        """
        Solve for J via least squares.

        We solve: P = H @ J.T
        where H is Nx2 (HID deltas), P is Nx2 (pixel deltas).

        Returns the 2x2 Jacobian J.
        """
        if len(self.correspondences) < 2:
            raise ValueError(
                f"Need at least 2 correspondences, have {len(self.correspondences)}"
            )

        H = np.array([[c[0], c[1]] for c in self.correspondences])  # Nx2
        P = np.array([[c[2], c[3]] for c in self.correspondences])  # Nx2

        # Solve for J.T: H @ J.T = P
        # Using lstsq for robustness with overdetermined systems
        JT, residuals, rank, sv = np.linalg.lstsq(H, P, rcond=None)
        self.J = JT.T  # 2x2

        # Compute inverse
        det = np.linalg.det(self.J)
        if abs(det) < 1e-9:
            raise ValueError(f"Jacobian is singular (det={det:.2e})")
        self.J_inv = np.linalg.inv(self.J)

        # Compute RMS residual
        P_pred = H @ JT
        residuals_vec = P - P_pred
        self.residual_rms = float(np.sqrt(np.mean(residuals_vec ** 2)))
        self.n_points = len(self.correspondences)

        return self.J.copy()

    def hid_to_pixel(self, dhid_x: float, dhid_y: float) -> Tuple[float, float]:
        """Convert HID delta to predicted pixel delta."""
        if self.J is None:
            raise RuntimeError("Must call solve() first")
        v = self.J @ np.array([dhid_x, dhid_y])
        return float(v[0]), float(v[1])

    def pixel_to_hid(self, dpx: float, dpy: float) -> Tuple[float, float]:
        """Convert desired pixel delta to required HID delta (for targeting)."""
        if self.J_inv is None:
            raise RuntimeError("Must call solve() first")
        v = self.J_inv @ np.array([dpx, dpy])
        return float(v[0]), float(v[1])

    def get_orthonormality_metrics(self) -> dict:
        """
        Quantify how non-orthonormal the mapping is.

        Returns:
            scale_x: pixels per HID in X (diagonal)
            scale_y: pixels per HID in Y (diagonal)
            shear_x: pixels of X-shift per HID Y (off-diagonal)
            shear_y: pixels of Y-shift per HID X (off-diagonal)
            shear_angle_deg: angle between the transformed axes
            scale_ratio: scale_x / scale_y (1.0 = uniform)
        """
        if self.J is None:
            raise RuntimeError("Must call solve() first")

        a, b = self.J[0, 0], self.J[0, 1]
        c, d = self.J[1, 0], self.J[1, 1]

        # Column vectors = images of HID basis vectors
        vx = np.array([a, c])  # HID X-axis in pixel space
        vy = np.array([b, d])  # HID Y-axis in pixel space

        scale_x = float(np.linalg.norm(vx))
        scale_y = float(np.linalg.norm(vy))

        # Angle between axes (90deg = orthonormal)
        cos_angle = np.dot(vx, vy) / (scale_x * scale_y)
        cos_angle = np.clip(cos_angle, -1.0, 1.0)
        angle_deg = float(np.degrees(np.arccos(cos_angle)))

        return {
            'scale_x': scale_x,
            'scale_y': scale_y,
            'shear_x': float(b),  # X-shift per unit HID Y
            'shear_y': float(c),  # Y-shift per unit HID X
            'inter_axis_angle_deg': angle_deg,
            'scale_ratio': scale_x / scale_y if scale_y > 0 else float('inf'),
            'residual_rms_px': self.residual_rms,
            'n_correspondences': self.n_points,
        }

    def predict_landing(self, start_px: Tuple[float, float],
                        dhid_x: float, dhid_y: float) -> Tuple[float, float]:
        """Predict where the cursor will land in pixels after a HID move."""
        dpx, dpy = self.hid_to_pixel(dhid_x, dhid_y)
        return (start_px[0] + dpx, start_px[1] + dpy)

    def compute_move_to_target(self,
                               current_px: Tuple[float, float],
                               target_px: Tuple[float, float]) -> Tuple[int, int]:
        """
        Compute the integer HID move to go from current to target pixel.

        Returns (dhid_x, dhid_y) as integers, rounded.
        """
        dpx = target_px[0] - current_px[0]
        dpy = target_px[1] - current_px[1]
        dhid_x, dhid_y = self.pixel_to_hid(dpx, dpy)
        return (int(round(dhid_x)), int(round(dhid_y)))

    def save(self, path: str) -> None:
        """Save calibration to JSON."""
        if self.J is None:
            raise RuntimeError("Must call solve() first")
        data = {
            'jacobian': self.J.tolist(),
            'jacobian_inv': self.J_inv.tolist(),
            'correspondences': self.correspondences,
            'residual_rms': self.residual_rms,
            'n_points': self.n_points,
            'metrics': self.get_orthonormality_metrics(),
        }
        Path(path).write_text(json.dumps(data, indent=2))

    @classmethod
    def load(cls, path: str) -> 'CursorCalibrator':
        """Load calibration from JSON."""
        data = json.loads(Path(path).read_text())
        cal = cls()
        cal.correspondences = [tuple(c) for c in data['correspondences']]
        cal.J = np.array(data['jacobian'])
        cal.J_inv = np.array(data['jacobian_inv'])
        cal.residual_rms = data['residual_rms']
        cal.n_points = data['n_points']
        return cal

    def __repr__(self) -> str:
        if self.J is None:
            return f"CursorCalibrator(unsolved, {len(self.correspondences)} correspondences)"
        m = self.get_orthonormality_metrics()
        return (
            f"CursorCalibrator(\n"
            f"  J=[[{self.J[0,0]:.4f}, {self.J[0,1]:.4f}],\n"
            f"     [{self.J[1,0]:.4f}, {self.J[1,1]:.4f}]],\n"
            f"  scale=({m['scale_x']:.3f}, {m['scale_y']:.3f}) px/HID,\n"
            f"  inter_axis_angle={m['inter_axis_angle_deg']:.1f}deg,\n"
            f"  residual_rms={m['residual_rms_px']:.2f}px,\n"
            f"  n={m['n_correspondences']}\n"
            f")"
        )


def detect_cursor_blobs(image_path: str,
                         threshold: int = 180,
                         min_area: int = 5,
                         max_area: int = 150,
                         max_aspect: float = 5.0) -> List[Tuple[int, int, int, int, int]]:
    """
    Detect bright cursor-like blobs in a single image.

    Returns list of (cx, cy, area, w, h) sorted by area descending.
    Assumes white cursor on dark background.
    """
    from PIL import Image
    from scipy import ndimage

    img = Image.open(image_path).convert('L')
    arr = np.array(img)
    w_img, h_img = img.size

    binary = arr > threshold
    labeled, n_feat = ndimage.label(binary)

    blobs = []
    for i in range(1, n_feat + 1):
        ys, xs = np.where(labeled == i)
        n = len(xs)
        if min_area <= n <= max_area:
            bw = int(xs.max() - xs.min() + 1)
            bh = int(ys.max() - ys.min() + 1)
            aspect = max(bw, bh) / max(min(bw, bh), 1)
            cx, cy = int(xs.mean()), int(ys.mean())
            # Exclude edges (likely UI chrome)
            if aspect < max_aspect and 10 < cx < w_img - 10 and 10 < cy < h_img - 10:
                blobs.append((cx, cy, n, bw, bh))

    return sorted(blobs, key=lambda b: b[2], reverse=True)


def diff_cursor_positions(before_path: str, after_path: str,
                          diff_threshold: int = 40,
                          blur_radius: float = 1.0,
                          min_area: int = 8,
                          max_area: int = 200) -> List[Tuple[int, int, int, int, int]]:
    """
    Find cursor positions by differencing before/after images.

    Returns list of (cx, cy, area, w, h) for changed blobs.
    For a cursor move, expect 2 blobs: before-position and after-position.
    """
    from PIL import Image, ImageChops, ImageFilter
    from scipy import ndimage

    before = Image.open(before_path).convert('L')
    after = Image.open(after_path).convert('L')

    if blur_radius > 0:
        before = before.filter(ImageFilter.GaussianBlur(blur_radius))
        after = after.filter(ImageFilter.GaussianBlur(blur_radius))

    diff = ImageChops.difference(before, after)
    diff_arr = np.array(diff)
    binary = diff_arr > diff_threshold
    labeled, n_feat = ndimage.label(binary)

    blobs = []
    for i in range(1, n_feat + 1):
        ys, xs = np.where(labeled == i)
        n = len(xs)
        if min_area <= n <= max_area:
            bw = int(xs.max() - xs.min() + 1)
            bh = int(ys.max() - ys.min() + 1)
            aspect = max(bw, bh) / max(min(bw, bh), 1)
            if aspect < 4:
                blobs.append((int(xs.mean()), int(ys.mean()), n, bw, bh))

    return sorted(blobs, key=lambda b: b[2], reverse=True)


def order_cursor_blobs(blobs: List[Tuple[int, int, int, int, int]],
                       before_pos: Optional[Tuple[float, float]] = None,
                       expected_delta: Optional[Tuple[float, float]] = None
                       ) -> Tuple[Tuple[int, int, int, int, int],
                                  Tuple[int, int, int, int, int]]:
    """
    Decide which of two diff blobs is the before-position and which is after.

    The naive approach (blobs[0] = largest area = "before") flips the sign
    whenever the after-blob happens to be bigger. That is exactly what
    corrupted the 500Y calibration measurement on 2026-09-24
    (blobs were (90,314) area=50 and (88,215) area=46; area order gave
    delta (-2,-99) instead of the true (+2,+99)).

    Disambiguation, in priority order:
      1. before_pos: known (x, y) cursor position before the move.
         The blob closest to it is labeled "before".
      2. expected_delta: expected (dx, dy) pixel delta. The ordering whose
         delta has the largest dot product with it wins.

    Returns (before_blob, after_blob). With no hint available it falls back
    to area order (largest first) and the caller should treat the sign as
    untrusted.
    """
    if len(blobs) < 2:
        raise ValueError(f"need 2 blobs to order, got {len(blobs)}")
    b1, b2 = blobs[0], blobs[1]

    if before_pos is not None:
        d1 = (b1[0] - before_pos[0]) ** 2 + (b1[1] - before_pos[1]) ** 2
        d2 = (b2[0] - before_pos[0]) ** 2 + (b2[1] - before_pos[1]) ** 2
        return (b1, b2) if d1 <= d2 else (b2, b1)

    if expected_delta is not None:
        ex, ey = expected_delta
        # ordering A: b1 -> b2 ; ordering B: b2 -> b1
        dot_a = (b2[0] - b1[0]) * ex + (b2[1] - b1[1]) * ey
        dot_b = -dot_a
        return (b1, b2) if dot_a >= dot_b else (b2, b1)

    return (b1, b2)  # area order; sign untrusted


def diff_cursor_delta(before_path: str, after_path: str,
                      before_pos: Optional[Tuple[float, float]] = None,
                      expected_delta: Optional[Tuple[float, float]] = None,
                      **kwargs) -> Tuple[int, int,
                                         Tuple[int, int, int, int, int],
                                         Tuple[int, int, int, int, int]]:
    """
    Diff two frames and return the signed cursor pixel delta.

    before_pos / expected_delta are passed through to order_cursor_blobs
    to disambiguate which blob is before vs after. Any extra kwargs go to
    diff_cursor_positions.

    Returns (dpx, dpy, before_blob, after_blob).
    Raises ValueError if fewer than 2 blobs are found.
    """
    blobs = diff_cursor_positions(before_path, after_path, **kwargs)
    before_b, after_b = order_cursor_blobs(blobs, before_pos, expected_delta)
    return (after_b[0] - before_b[0], after_b[1] - before_b[1],
            before_b, after_b)
    # Demo with the 2026-09-24 tripod calibration data
    cal = CursorCalibrator()
    cal.add_correspondence(dhid_x=200, dhid_y=0, dpx=125, dpy=11)
    cal.add_correspondence(dhid_x=0, dhid_y=200, dpx=-11, dpy=119)
    cal.add_correspondence(dhid_x=-200, dhid_y=0, dpx=-125, dpy=-12)
    J = cal.solve()

    print("Jacobian (HID -> pixel):")
    print(J)
    print()
    print("Inverse (pixel -> HID):")
    print(cal.J_inv)
    print()
    print("Orthonormality metrics:")
    for k, v in cal.get_orthonormality_metrics().items():
        print(f"  {k}: {v}")
    print()
    print("Test: move to target 100px right, 60px down from (287,239)")
    move = cal.compute_move_to_target((287, 239), (387, 299))
    print(f"  HID move: {move}")
    pred = cal.predict_landing((287, 239), *move)
    print(f"  Predicted landing: ({pred[0]:.1f}, {pred[1]:.1f})")
