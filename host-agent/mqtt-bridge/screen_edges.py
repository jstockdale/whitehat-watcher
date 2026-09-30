#!/usr/bin/env python3
"""
Screen edge detection for the Watcher digital twin.

The display is a bright rectangle on a darker background (bezel/desk).
Finding its four edges in camera pixels gives, per frame:

  - 4 screen corners in camera coordinates
  - a full 3x3 homography mapping screen pixels <-> camera pixels
  - continuous, passive geometric calibration (no cursor moves needed)

Why edges beat point landmarks: each edge is fit through hundreds of
pixels, giving sub-pixel edge position -- far more precise than any blob
centroid. Four edges -> four corners -> DLT homography from one frame.

Graceful degradation:
  - all 4 edges  -> full homography
  - 2 adjacent   -> one corner + orientation (partial constraint)
  - <2 usable    -> low confidence, caller keeps the previous homography

No OpenCV: implemented with numpy/PIL/scipy only.

Usage:
    from screen_edges import find_display_rectangle, solve_homography

    corners, conf, detail = find_display_rectangle("frame.jpg")
    # corners: dict with 'tl','tr','br','bl' in camera pixels (or None)
    if conf > 0.5:
        H = solve_homography(screen_corners_wh(1920, 1080),
                             [corners['tl'], corners['tr'],
                              corners['br'], corners['bl']])
"""
import math
from typing import Dict, List, Optional, Tuple

import numpy as np
from PIL import Image

Point = Tuple[float, float]


# ---------------------------------------------------------------------------
# Homography math (DLT)
# ---------------------------------------------------------------------------

def solve_homography(src: np.ndarray, dst: np.ndarray) -> np.ndarray:
    """
    Direct Linear Transform: 3x3 H mapping src -> dst.

    src, dst: Nx2 arrays of corresponding points (N >= 4).
    Returns H with H[2,2] = 1.
    """
    src = np.asarray(src, dtype=float)
    dst = np.asarray(dst, dtype=float)
    if len(src) < 4 or len(dst) < 4:
        raise ValueError(f"need >= 4 correspondences, got {len(src)}")
    A = []
    for (x, y), (u, v) in zip(src, dst):
        A.append([-x, -y, -1, 0, 0, 0, u * x, u * y, u])
        A.append([0, 0, 0, -x, -y, -1, v * x, v * y, v])
    _, _, Vt = np.linalg.svd(np.array(A))
    H = Vt[-1].reshape(3, 3)
    return H / H[2, 2]


def apply_homography(H: np.ndarray, pts) -> np.ndarray:
    """Apply 3x3 homography to Nx2 points. Returns Nx2."""
    pts = np.atleast_2d(np.asarray(pts, dtype=float))
    homo = np.hstack([pts, np.ones((len(pts), 1))])
    out = (np.asarray(H) @ homo.T).T
    return out[:, :2] / out[:, 2:3]


def homography_reprojection_error(H: np.ndarray,
                                  src: np.ndarray,
                                  dst: np.ndarray) -> float:
    """Mean Euclidean reprojection error of src -> dst under H."""
    pred = apply_homography(H, src)
    return float(np.mean(np.linalg.norm(pred - np.asarray(dst), axis=1)))


def screen_corners_wh(w: float, h: float) -> np.ndarray:
    """Screen corners in screen pixels: TL, TR, BR, BL."""
    return np.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=float)


# ---------------------------------------------------------------------------
# Robust line fitting (RANSAC)
# ---------------------------------------------------------------------------

def _fit_line_ls(pts: np.ndarray) -> Optional[Tuple[float, float, float]]:
    """
    Least-squares line fit. Returns (a, b, c) for ax + by + c = 0,
    normalized so sqrt(a^2+b^2) = 1. None if degenerate.
    """
    pts = np.asarray(pts, dtype=float)
    if len(pts) < 2:
        return None
    centroid = pts.mean(axis=0)
    centered = pts - centroid
    _, _, Vt = np.linalg.svd(centered)
    # normal = smallest singular vector
    nx, ny = Vt[-1]
    c = -(nx * centroid[0] + ny * centroid[1])
    return (float(nx), float(ny), float(c))


def _point_line_dist(pts: np.ndarray, line: Tuple[float, float, float]) -> np.ndarray:
    a, b, c = line
    pts = np.asarray(pts, dtype=float)
    return np.abs(a * pts[:, 0] + b * pts[:, 1] + c)  # already normalized


def ransac_line(pts: np.ndarray,
                thresh: float = 2.0,
                iters: int = 60,
                min_inliers: int = 12,
                seed: int = 0) -> Tuple[Optional[Tuple[float, float, float]], float]:
    """
    RANSAC line fit. Returns (line (a,b,c) or None, inlier_ratio).
    inlier_ratio is measured against all input points.
    """
    pts = np.asarray(pts, dtype=float)
    n = len(pts)
    if n < min_inliers:
        return None, 0.0
    rng = np.random.default_rng(seed)
    best_line, best_inliers = None, np.zeros(n, dtype=bool)
    for _ in range(iters):
        i, j = rng.choice(n, 2, replace=False)
        p1, p2 = pts[i], pts[j]
        d = p2 - p1
        norm = math.hypot(d[0], d[1])
        if norm < 1e-6:
            continue
        # normal form
        nx, ny = -d[1] / norm, d[0] / norm
        c = -(nx * p1[0] + ny * p1[1])
        dists = np.abs(nx * pts[:, 0] + ny * pts[:, 1] + c)
        inliers = dists < thresh
        if inliers.sum() > best_inliers.sum():
            best_inliers = inliers
            best_line = (nx, ny, c)
    if best_line is None or best_inliers.sum() < min_inliers:
        return None, 0.0
    refined = _fit_line_ls(pts[best_inliers])
    ratio = float(best_inliers.sum()) / n
    return refined, ratio


def intersect_lines(l1: Tuple[float, float, float],
                    l2: Tuple[float, float, float]) -> Optional[Point]:
    """Intersection of two lines in normal form. None if near-parallel."""
    a1, b1, c1 = l1
    a2, b2, c2 = l2
    det = a1 * b2 - a2 * b1
    if abs(det) < 1e-9:
        return None
    x = (b1 * c2 - b2 * c1) / det
    y = (c1 * a2 - c2 * a1) / det
    return (float(x), float(y))


# ---------------------------------------------------------------------------
# Display rectangle detection
# ---------------------------------------------------------------------------

def _to_gray(image) -> np.ndarray:
    if isinstance(image, str):
        img = Image.open(image).convert("L")
    elif isinstance(image, Image.Image):
        img = image.convert("L")
    else:
        arr = np.asarray(image)
        img = Image.fromarray(arr).convert("L")
    return np.array(img, dtype=float)


def _edge_candidates(gray: np.ndarray, side: str,
                     grad_thresh: float) -> np.ndarray:
    """
    Candidate edge points for one side of the display rectangle.

    The display is brighter than its surround. Scanning inward:
      top:    dark -> bright going down  => +gy maximum, search top half
      bottom: bright -> dark going down  => -gy maximum, search bottom half
      left:   dark -> bright going right => +gx maximum, search left half
      right:  bright -> dark going right => -gx maximum, search right half

    For each column (top/bottom) or row (left/right) we take the strongest
    gradient peak above threshold -- the display edge spans the full width,
    so it dominates over window chrome inside the screen.
    Returns Nx2 candidate points (may be empty).
    """
    h, w = gray.shape
    gy, gx = np.gradient(gray)
    pts: List[Tuple[float, float]] = []

    if side == "top":
        band = gy[:h // 2, :]
        for x in range(w):
            col = band[:, x]
            y = int(np.argmax(col))
            if col[y] > grad_thresh:
                # sub-pixel refine with neighbors
                if 0 < y < len(col) - 1:
                    denom = col[y - 1] - 2 * col[y] + col[y + 1]
                    dy = 0.5 * (col[y - 1] - col[y + 1]) / denom if abs(denom) > 1e-9 else 0.0
                    y = y + max(-1.0, min(1.0, dy))
                pts.append((float(x), float(y)))
    elif side == "bottom":
        band = -gy[h // 2:, :]
        for x in range(w):
            col = band[:, x]
            y = int(np.argmax(col))
            if col[y] > grad_thresh:
                if 0 < y < len(col) - 1:
                    denom = col[y - 1] - 2 * col[y] + col[y + 1]
                    dy = 0.5 * (col[y - 1] - col[y + 1]) / denom if abs(denom) > 1e-9 else 0.0
                    y = y + max(-1.0, min(1.0, dy))
                pts.append((float(x), float(h // 2 + y)))
    elif side == "left":
        band = gx[:, :w // 2]
        for y in range(h):
            row = band[y, :]
            x = int(np.argmax(row))
            if row[x] > grad_thresh:
                if 0 < x < len(row) - 1:
                    denom = row[x - 1] - 2 * row[x] + row[x + 1]
                    dx = 0.5 * (row[x - 1] - row[x + 1]) / denom if abs(denom) > 1e-9 else 0.0
                    x = x + max(-1.0, min(1.0, dx))
                pts.append((float(x), float(y)))
    elif side == "right":
        band = -gx[:, w // 2:]
        for y in range(h):
            row = band[y, :]
            x = int(np.argmax(row))
            if row[x] > grad_thresh:
                if 0 < x < len(row) - 1:
                    denom = row[x - 1] - 2 * row[x] + row[x + 1]
                    dx = 0.5 * (row[x - 1] - row[x + 1]) / denom if abs(denom) > 1e-9 else 0.0
                    x = x + max(-1.0, min(1.0, dx))
                pts.append((float(w // 2 + x), float(y)))
    else:
        raise ValueError(f"unknown side {side!r}")
    return np.array(pts, dtype=float).reshape(-1, 2)


def find_display_rectangle(image,
                           grad_thresh: float = 25.0,
                           min_support: float = 0.35,
                           seed: int = 0,
                           expected_aspect: Optional[float] = None,
                           aspect_tol: float = 0.4
                           ) -> Tuple[Dict[str, Optional[Point]], float, dict]:
    """
    Find the display rectangle in a camera frame.

    Returns (corners, confidence, detail) where corners is a dict
    {'tl','tr','br','bl'} of camera-pixel points (None for missing),
    confidence in [0,1], and detail has per-side line/inlier info.

    min_support: a side is usable only if gradient candidates cover at
    least this fraction of the image width/height -- rejects sides where
    the edge is mostly occluded or out of frame.

    expected_aspect: known screen width/height (e.g. from telemetry).
    If all 4 corners are found but the quad's aspect disagrees by more
    than aspect_tol, the rectangle is rejected (confidence 0): it is
    probably an occluder/window edge, not the display boundary. A
    wrong-but-confident homography is worse than no homography.
    """
    gray = _to_gray(image)
    h, w = gray.shape
    detail: dict = {"sides": {}}
    lines: Dict[str, Optional[Tuple[float, float, float]]] = {}
    side_ratios: Dict[str, float] = {}

    for side in ("top", "bottom", "left", "right"):
        cands = _edge_candidates(gray, side, grad_thresh)
        span = w if side in ("top", "bottom") else h
        support = len(cands) / span if span else 0.0
        line, inlier_ratio = ransac_line(cands, seed=seed)
        usable = (line is not None and support >= min_support
                  and inlier_ratio >= 0.5)
        detail["sides"][side] = {
            "candidates": int(len(cands)),
            "support": round(support, 3),
            "inlier_ratio": round(inlier_ratio, 3),
            "usable": bool(usable),
        }
        lines[side] = line if usable else None
        side_ratios[side] = inlier_ratio if usable else 0.0

    corners: Dict[str, Optional[Point]] = {"tl": None, "tr": None,
                                           "br": None, "bl": None}
    pairs = {"tl": ("top", "left"), "tr": ("top", "right"),
             "br": ("bottom", "right"), "bl": ("bottom", "left")}
    for name, (s1, s2) in pairs.items():
        if lines[s1] is not None and lines[s2] is not None:
            pt = intersect_lines(lines[s1], lines[s2])
            # sanity: corner must be inside (or near) the image
            if pt is not None and -20 <= pt[0] <= w + 20 and -20 <= pt[1] <= h + 20:
                corners[name] = pt

    n_corners = sum(1 for v in corners.values() if v is not None)
    n_sides = sum(1 for v in lines.values() if v is not None)
    mean_inlier = (sum(side_ratios.values()) / max(n_sides, 1)) if n_sides else 0.0
    # aspect guard: a quad that doesn't look like the known screen is
    # an occluder or window edge, not the display boundary
    if n_corners == 4 and expected_aspect is not None:
        wt = math.dist(corners["tl"], corners["tr"])
        wb = math.dist(corners["bl"], corners["br"])
        hl = math.dist(corners["tl"], corners["bl"])
        hr = math.dist(corners["tr"], corners["br"])
        quad_aspect = ((wt + wb) / 2) / max((hl + hr) / 2, 1e-9)
        mismatch = abs(quad_aspect - expected_aspect) / expected_aspect
        detail["quad_aspect"] = round(quad_aspect, 3)
        detail["aspect_mismatch"] = round(mismatch, 3)
        if mismatch > aspect_tol:
            detail["rejected"] = (
                f"quad aspect {quad_aspect:.2f} != expected "
                f"{expected_aspect:.2f} (mismatch {mismatch:.0%} > "
                f"tol {aspect_tol:.0%}); probably occlusion, not the display")
            detail["n_corners"] = n_corners
            detail["n_sides"] = n_sides
            detail["image_wh"] = (w, h)
            return corners, 0.0, detail
    # confidence: need all 4 corners for full marks; partial credit for sides
    if n_corners == 4:
        confidence = 0.5 + 0.5 * mean_inlier
    elif n_sides >= 2:
        confidence = 0.25 * (n_sides / 4) + 0.25 * mean_inlier
    else:
        confidence = 0.0
    detail["n_corners"] = n_corners
    detail["n_sides"] = n_sides
    detail["image_wh"] = (w, h)
    return corners, round(float(confidence), 3), detail


def rectangle_homography(corners: Dict[str, Optional[Point]],
                         screen_w: float,
                         screen_h: float) -> Optional[np.ndarray]:
    """
    Build the screen->camera homography from detected corners.
    Returns None unless all 4 corners are present.
    """
    if any(corners[k] is None for k in ("tl", "tr", "br", "bl")):
        return None
    src = screen_corners_wh(screen_w, screen_h)
    dst = np.array([corners["tl"], corners["tr"],
                    corners["br"], corners["bl"]], dtype=float)
    return solve_homography(src, dst)
