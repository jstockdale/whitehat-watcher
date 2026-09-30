#!/usr/bin/env python3
"""
Vision helpers for the Watcher eyes-on-screen cursor pipeline.

Covers the lessons from the 2026-09-24 Claude task:
- Never trust a raw visual-grounding point: verify it against the image
  (verify_target_crop) and aim for the CENTER of the UI element, not an
  unverified point (find_button_bounds).
- Report cursor localization confidence honestly; re-hunt when it is low
  (localize_cursor).
"""
import numpy as np
from pathlib import Path
from PIL import Image, ImageDraw
from scipy import ndimage
from typing import Dict, List, Optional, Tuple

from cursor_calibration import diff_cursor_positions, order_cursor_blobs

# 416x416 Himax snapshots
IMG_W, IMG_H = 416, 416


def verify_target_crop(image_path: str, x: float, y: float,
                       box: int = 120, zoom: int = 3) -> Image.Image:
    """
    Crop a region around (x, y) and magnify it for visual confirmation.

    Draws a crosshair at the exact point so you can see what a
    visual-grounding coordinate actually refers to before committing
    a move to it. Returns the zoomed PIL image (also useful to embed
    in reports).

    box: crop size in original pixels. zoom: magnification factor.
    """
    img = Image.open(image_path).convert("RGB")
    w, h = img.size
    x0 = int(max(0, x - box // 2))
    y0 = int(max(0, y - box // 2))
    x1 = int(min(w, x0 + box))
    y1 = int(min(h, y0 + box))
    crop = img.crop((x0, y0, x1, y1)).resize(
        ((x1 - x0) * zoom, (y1 - y0) * zoom), Image.NEAREST)

    draw = ImageDraw.Draw(crop)
    cx, cy = (x - x0) * zoom, (y - y0) * zoom
    r = 10
    draw.line([cx - r, cy, cx + r, cy], fill="red", width=2)
    draw.line([cx, cy - r, cx, cy + r], fill="red", width=2)
    draw.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], outline="red", width=2)
    return crop


def find_button_bounds(image_path: str, x: float, y: float,
                       search_radius: int = 60,
                       threshold: int = 150,
                       min_area: int = 20,
                       max_area: int = 1200) -> Optional[Dict]:
    """
    Find the bright UI element (button) nearest to (x, y) and return its
    bounding box so the move can aim for its CENTER.

    This is the fix for the Claude-task submit-button miss: visual
    grounding returned (236, 217) but the actual orange button was at
    (242, 250). Aiming at the blob center instead of the raw point
    would have landed the click correctly.

    Only compact blobs (area <= max_area) are considered, so large
    bright regions like an open white window do not win.

    Returns {"cx", "cy", "x0", "y0", "x1", "y1", "area"} in original
    image coordinates, or None if nothing bright is found nearby.
    """
    img = Image.open(image_path).convert("L")
    arr = np.array(img)
    h, w = arr.shape

    x0 = max(0, int(x - search_radius))
    y0 = max(0, int(y - search_radius))
    x1 = min(w, int(x + search_radius))
    y1 = min(h, int(y + search_radius))
    region = arr[y0:y1, x0:x1]

    binary = region > threshold
    labeled, n_feat = ndimage.label(binary)
    best = None
    best_dist = None
    for i in range(1, n_feat + 1):
        ys, xs = np.where(labeled == i)
        n = len(xs)
        if not (min_area <= n <= max_area):
            continue
        bx0, bx1 = xs.min() + x0, xs.max() + x0
        by0, by1 = ys.min() + y0, ys.max() + y0
        cx, cy = (bx0 + bx1) / 2, (by0 + by1) / 2
        dist = ((cx - x) ** 2 + (cy - y) ** 2) ** 0.5
        if best_dist is None or dist < best_dist:
            best_dist = dist
            best = {"cx": float(cx), "cy": float(cy),
                    "x0": int(bx0), "y0": int(by0),
                    "x1": int(bx1), "y1": int(by1), "area": int(n)}
    return best


def localize_cursor(before_path: str, after_path: str,
                    prior_pos: Optional[Tuple[float, float]] = None,
                    **diff_kwargs) -> Tuple[Optional[Tuple[int, int]],
                                           float, Dict]:
    """
    Localize the cursor in after_path by differencing against before_path.

    Returns (position, confidence, detail) where position is (x, y) of the
    cursor in after_path (or None if nothing usable was found) and
    confidence is 0.0-1.0. The caller should re-hunt when confidence < 0.5
    instead of trusting the position.

    Confidence rules (from the Claude task: at 416x416 the diff has about
    +/-10px of uncertainty, and extra blobs mean UI changed under us):
      2 blobs, similar size, sane separation  -> 0.9
      2 blobs, one much bigger (area ratio>2) -> 0.7
      2 blobs, separation > 200px             -> 0.6 (unexpected jump)
      2 blobs, separation < 3px               -> 0.3 (probably didn't move)
      3-4 blobs                               -> 0.4 (ambiguous)
      1 blob                                  -> 0.3 (before/after merged)
      0 or >4 blobs                           -> 0.1 (unusable)

    prior_pos: last known cursor position. Used to label which blob is
    "after" (the one farther from prior_pos). Without it the after-blob
    is the smaller one, which is only a guess.
    """
    blobs = diff_cursor_positions(before_path, after_path, **diff_kwargs)
    detail = {"n_blobs": len(blobs),
              "blobs": [(b[0], b[1], b[2]) for b in blobs[:6]]}

    if len(blobs) == 0 or len(blobs) > 4:
        return None, 0.1, detail

    if len(blobs) == 1:
        b = blobs[0]
        detail["note"] = "single blob: before/after merged or no motion"
        return (b[0], b[1]), 0.3, detail

    if len(blobs) in (3, 4):
        # Ambiguous: pick the blob farthest from prior_pos as "after".
        if prior_pos is not None:
            after_b = max(blobs[:4],
                          key=lambda b: (b[0] - prior_pos[0]) ** 2 +
                                        (b[1] - prior_pos[1]) ** 2)
        else:
            after_b = max(blobs[:4], key=lambda b: b[2])
        detail["note"] = "3-4 blobs: ambiguous, took farthest/largest"
        return (after_b[0], after_b[1]), 0.4, detail

    # Exactly 2 blobs: the normal case.
    b1, b2 = blobs[0], blobs[1]
    if prior_pos is not None:
        d1 = (b1[0] - prior_pos[0]) ** 2 + (b1[1] - prior_pos[1]) ** 2
        d2 = (b2[0] - prior_pos[0]) ** 2 + (b2[1] - prior_pos[1]) ** 2
        before_b, after_b = (b1, b2) if d1 <= d2 else (b2, b1)
        detail["labeled_by"] = "prior_pos"
    else:
        # Guess: smaller blob is the after-image (cursor freshly drawn).
        before_b, after_b = (b1, b2) if b1[2] >= b2[2] else (b2, b1)
        detail["labeled_by"] = "area_guess"

    sep = ((after_b[0] - before_b[0]) ** 2 +
           (after_b[1] - before_b[1]) ** 2) ** 0.5
    area_ratio = max(b1[2], b2[2]) / max(min(b1[2], b2[2]), 1)
    detail.update({"separation_px": round(sep, 1),
                   "area_ratio": round(area_ratio, 2)})

    conf = 0.9
    if area_ratio > 2:
        conf = 0.7
        detail["note"] = "blobs differ a lot in size"
    if sep > 200:
        conf = min(conf, 0.6)
        detail["note"] = "unexpectedly large jump"
    if sep < 3:
        conf = 0.3
        detail["note"] = "blobs nearly coincide: cursor may not have moved"

    return (after_b[0], after_b[1]), conf, detail


def annotate_detections(image_path: str,
                        points: List[Tuple[float, float, str, str]]) -> Image.Image:
    """
    Draw labeled circles on an image for a report.

    points: list of (x, y, label, color). Returns the annotated PIL image.
    """
    img = Image.open(image_path).convert("RGB")
    draw = ImageDraw.Draw(img)
    for x, y, label, color in points:
        r = 10
        draw.ellipse([x - r, y - r, x + r, y + r], outline=color, width=2)
        draw.text((x + r + 2, y - 8), label, fill=color)
    return img
