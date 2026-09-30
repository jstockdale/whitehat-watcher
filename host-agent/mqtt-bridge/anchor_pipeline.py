#!/usr/bin/env python3
"""Anchor pipeline: camera frame -> detected fiducials -> Screen<->Camera.

Stage 1 (this script): detect fiducial cores, de-duplicate, geometrically
    assign TL/TR/BR/BL in camera coords.
Stage 2 (needs page telemetry): pair with screen marks -> DLT homography
    -> ScreenTwin.update_geometry_fiducials.

The corner notch is NOT used for primary ID: at 640x480 the notch is
2-4px and its image-axis quadrant is unreliable under perspective.
Primary ID is geometric (camera sees the screen roughly front-on).
The notch is kept as a verification hint only.

Usage:
    python3 anchor_pipeline.py <snapshot.jpg> [--show]
"""
import json
import math
import sys

import numpy as np

from fiducial import find_fiducials_v2

NMS_RADIUS_PX = 18.0  # detections closer than this are one symbol


def nms(cands, radius=NMS_RADIUS_PX):
    """Merge detections within `radius` px, keeping the best score."""
    cands = sorted(cands, key=lambda c: -c["score"])
    kept = []
    for c in cands:
        x, y = c["center"]
        if all(math.dist((x, y), k["center"]) > radius for k in kept):
            kept.append(c)
    return kept


def geometric_assign(points):
    """Assign TL/TR/BR/BL by camera position.

    Assumes the camera views the screen roughly front-on (no 90-degree+
    roll): the topmost two are TL/TR, the bottommost two BL/BR.
    Returns dict corner_id -> (x, y). Needs 3+ points; with exactly 3,
    returns the three found (no parallelogram completion -- invalid
    under perspective; use predict_missing() with screen telemetry).
    """
    if len(points) < 3:
        return {}
    pts = sorted(points, key=lambda p: p[1])  # top to bottom (y grows down)
    top_two = sorted(pts[:2], key=lambda p: p[0])
    out = {"tl": tuple(top_two[0]), "tr": tuple(top_two[1])}
    rest = sorted(pts[2:], key=lambda p: p[0])
    if len(rest) >= 2:
        out["bl"] = tuple(rest[0])
        out["br"] = tuple(rest[1])
    elif len(rest) == 1:
        # one bottom point: it's bl if it's left of center, else br
        midx = (out["tl"][0] + out["tr"][0]) / 2
        key = "bl" if rest[0][0] < midx else "br"
        out[key] = tuple(rest[0])
        out["_missing"] = "br" if key == "bl" else "bl"
    return out


def predict_missing(assign, screen_marks):
    """Predict the missing 4th camera point via an affine fit.

    With 3 camera<->screen correspondences, fit an affine transform
    (6 DOF, exact for 3 points) and use it to predict where the 4th
    screen mark should appear in camera coords. Returns (x, y) or None.
    This is only a search seed -- the pipeline must then verify a real
    core detection at the predicted location.
    """
    missing = assign.get("_missing")
    if not missing:
        return None
    have = [k for k in ("tl", "tr", "bl", "br") if k in assign]
    if len(have) != 3 or missing not in screen_marks:
        return None
    # affine: solve [x y 1] @ A = [u v] for 3 correspondences
    src = np.array([screen_marks[k] for k in have])   # screen
    dst = np.array([assign[k] for k in have])          # camera
    M = np.column_stack([src, np.ones(3)])
    try:
        A, *_ = np.linalg.lstsq(M, dst, rcond=None)
    except np.linalg.LinAlgError:
        return None
    pred = np.append(screen_marks[missing], 1.0) @ A
    return (float(pred[0]), float(pred[1]))


def main():
    path = sys.argv[1]
    show = "--show" in sys.argv
    hits, detail = find_fiducials_v2(path)
    cands = detail["candidates"]
    # keep everything above a lenient floor for NMS, then re-threshold
    cands = [c for c in cands if c["score"] >= 0.25]
    dedup = nms(cands)
    # final: keep the 4 best (the symbols) -- real scenes have <= 4
    dedup = sorted(dedup, key=lambda c: -c["score"])[:4]
    pts = [c["center"] for c in dedup]
    assign = geometric_assign(pts)

    print(f"raw candidates: {len(detail['candidates'])}, "
          f"after NMS: {len(dedup)}")
    for c in dedup:
        x, y = c["center"]
        print(f"  cam=({x:.0f},{y:.0f}) notch_hint={c['corner_id']} "
              f"score={c['score']} area={c['area']}")
    print("assignment:")
    for k in ("tl", "tr", "br", "bl"):
        v = assign.get(k)
        tag = " (missing)" if assign.get("_missing") == k else ""
        print(f"  {k}: ({v[0]:.0f},{v[1]:.0f}){tag}" if v else f"  {k}: --")

    if show:
        from PIL import Image, ImageDraw
        im = Image.open(path).convert("RGB")
        d = ImageDraw.Draw(im)
        colors = {"tl": "red", "tr": "lime", "br": "cyan", "bl": "yellow"}
        for k in ("tl", "tr", "br", "bl"):
            if k not in assign:
                continue
            x, y = assign[k]
            d.ellipse([x-8, y-8, x+8, y+8], outline=colors[k], width=3)
            d.text((x+10, y-10), k.upper(), fill=colors[k])
        im.resize((1280, 960), Image.LANCZOS).save("/tmp/anchors_assigned.png")
        print("annotated: /tmp/anchors_assigned.png")


if __name__ == "__main__":
    main()
