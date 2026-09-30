#!/usr/bin/env python3
"""
Four unique fiducial markers for the Watcher digital twin.

PAGE-SIDE SPEC (implemented in watcher-console.html v0.4.39+):
  - Four <div>s, one fixed in each page corner (24px inset):
      #twinFidTL  #twinFidTR  #twinFidBL  #twinFidBR
  - Each: 72x72 CSS px white square, 10px black border ring
    (box-sizing: border-box), 28px black core centered.
  - Uniqueness via NOTCH position: the white notch cut from the black
    core sits in the quadrant facing the symbol's own corner:
      TL -> notch top-left      TR -> notch top-right
      BL -> notch bottom-left   BR -> notch bottom-right
  - Telemetry: the console's MQTT status payload includes
      "fiducials": {"vw": w, "vh": h, "sx": sx, "sy": sy,
                    "marks": {"tl": {"x":..,"y":..,"size":..}, ...}}
    measured via getBoundingClientRect() at publish time, so it stays
    correct across scroll/resize/zoom.

WHY FOUR: the camera often sees only part of the screen (in the current
bench setup the screen OVERFILLS the frame), so one symbol is a gamble.
Four corners maximize the chance at least one is visible, and four
correspondences solve the full screen<->camera homography from fiducials
alone. A single visible symbol still gives position + identity (the notch
quadrant identifies which corner it is).

HOST-SIDE: find_fiducials() locates all symbols in a camera frame and
returns each center in camera pixels plus its corner id from the notch.
Detections + telemetry = absolute screen<->camera correspondences.

Detection strategy (416x416, symbol ~8-30 px across):
  1. bright blobs in a plausible area range
  2. squareness filter
  3. dark border ring check (the 10px black ring survives as a dark halo)
  4. dark core check (inner black square)
  5. notch quadrant: brightest quadrant of the core region -> corner id
Confidence blends all cues; nothing is trusted on brightness alone.
"""
from typing import Dict, Optional, Tuple

import numpy as np
from PIL import Image

Point = Tuple[float, float]

# Page-side constants (must match watcher-console.html)
FIDUCIAL_DIV_IDS = ("twinFidTL", "twinFidTR", "twinFidBL", "twinFidBR")
FIDUCIAL_CORNER_IDS = ("tl", "tr", "bl", "br")
FIDUCIAL_CSS_SIZE = 72          # px on the page
FIDUCIAL_MIN_VERSION = "0.4.39"  # first console version with four fiducials


def _to_gray(image) -> np.ndarray:
    if isinstance(image, str):
        img = Image.open(image).convert("L")
    elif isinstance(image, Image.Image):
        img = image.convert("L")
    else:
        img = Image.fromarray(np.asarray(image)).convert("L")
    return np.array(img)


def find_fiducial(image,
                  bright_thresh: int = 200,
                  dark_thresh: int = 100,
                  min_area: int = 20,
                  max_area: int = 2000) -> Tuple[Optional[Point], float, dict]:
    """
    Locate the calibration fiducial in a camera frame.

    Returns (center (x,y) or None, confidence in [0,1], detail dict).
    detail includes all candidates scored, so callers can inspect
    near-misses when tuning thresholds.
    """
    from scipy import ndimage

    gray = _to_gray(image)
    h, w = gray.shape
    detail: dict = {"candidates": []}

    binary = gray > bright_thresh
    labeled, n_feat = ndimage.label(binary)

    best = None
    best_score = 0.0

    for i in range(1, n_feat + 1):
        ys, xs = np.where(labeled == i)
        area = len(xs)
        if not (min_area <= area <= max_area):
            continue
        x0, x1 = xs.min(), xs.max()
        y0, y1 = ys.min(), ys.max()
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        aspect = max(bw, bh) / max(min(bw, bh), 1)
        if aspect > 1.6:  # fiducial is square; perspective skew is mild
            continue
        cx, cy = float(xs.mean()), float(ys.mean())

        # --- dark border ring check: sample just outside the bbox ---
        pad = max(2, int(round(0.35 * max(bw, bh))))
        rx0, rx1 = max(0, x0 - pad), min(w, x1 + pad + 1)
        ry0, ry1 = max(0, y0 - pad), min(h, y1 + pad + 1)
        ring = gray[ry0:ry1, rx0:rx1].copy()
        # mask out the blob itself (approx by bbox interior)
        ix0, ix1 = x0 - rx0, x1 - rx0 + 1
        iy0, iy1 = y0 - ry0, y1 - ry0 + 1
        ring[iy0:iy1, ix0:ix1] = 255  # exclude -> treated as bright
        ring_px = ring[ring < 255]
        dark_frac = float(np.mean(ring_px < dark_thresh)) if ring_px.size else 0.0

        # --- dark core check: inner 45% should contain a dark blob ---
        cx0 = int(x0 + bw * 0.275)
        cx1 = int(x0 + bw * 0.725)
        cy0 = int(y0 + bh * 0.275)
        cy1 = int(y0 + bh * 0.725)
        core = gray[cy0:cy1 + 1, cx0:cx1 + 1]
        core_dark_frac = float(np.mean(core < dark_thresh)) if core.size else 0.0
        # the core square is ~54% of the symbol's bright width with a white
        # notch cut from one quadrant -> inner window is mostly dark.
        # allow a wide band for perspective/blur/notch variation.
        core_ok = 0.05 <= core_dark_frac <= 0.85

        # --- notch quadrant: which quadrant of the core is brightest? ---
        # The white notch is cut from the core quadrant facing the
        # symbol's own corner: TL->"tl", TR->"tr", BL->"bl", BR->"br".
        notch_id = _notch_quadrant(gray, cx, cy, max(bw, bh) / 2,
                                   dark_thresh)

        # --- score ---
        # squareness in [0,1], dark ring in [0,1], core gate 0/1
        square_score = max(0.0, 1.0 - (aspect - 1.0) / 0.6)
        score = (0.35 * square_score + 0.45 * dark_frac
                 + 0.20 * (1.0 if core_ok else 0.0))
        # hard gates: need a real dark ring and a plausible core
        if dark_frac < 0.35 or not core_ok:
            score *= 0.3

        cand = {"center": (round(cx, 1), round(cy, 1)),
                "corner_id": notch_id,
                "area": int(area), "aspect": round(aspect, 2),
                "dark_ring_frac": round(dark_frac, 2),
                "core_dark_frac": round(core_dark_frac, 2),
                "score": round(score, 3)}
        detail["candidates"].append(cand)
        if score > best_score:
            best_score = score
            best = (cx, cy)

    detail["candidates"].sort(key=lambda c: c["score"], reverse=True)
    hits = [c for c in detail["candidates"] if c["score"] >= 0.45]
    if best is None or best_score < 0.45:
        return None, round(float(best_score), 3), detail
    return (round(best[0], 1), round(best[1], 1)), round(float(best_score), 3), detail


def _notch_quadrant(gray: np.ndarray, cx: float, cy: float,
                    half: float, dark_thresh: float) -> Optional[str]:
    """
    Identify the notch by the centroid of bright pixels in the core.
    The core is black except the white notch, so the bright centroid's
    quadrant IS the notch quadrant. Uses a small central window (stays
    inside the core despite 1-2px center error) and the notch's actual
    location -- not a fixed grid split -- so it is robust to the blob
    center being slightly off.
    Returns "tl"/"tr"/"bl"/"br", or None if ambiguous.
    """
    r = max(2, int(round(half * 0.35)))
    h, w = gray.shape
    x0, x1 = max(0, int(cx) - r), min(w, int(cx) + r + 1)
    y0, y1 = max(0, int(cy) - r), min(h, int(cy) + r + 1)
    win = gray[y0:y1, x0:x1]
    if win.size < 9:
        return None
    bright = win > (dark_thresh + 40)
    n_bright = int(bright.sum())
    # the notch is a small bright patch in a dark core: a few pixels,
    # but not most of the window (else we're not looking at the core)
    if n_bright < 3 or n_bright > 0.5 * win.size:
        return None
    ys, xs = np.nonzero(bright)
    qx = "l" if float(xs.mean()) < win.shape[1] / 2 else "r"
    qy = "t" if float(ys.mean()) < win.shape[0] / 2 else "b"
    return qy + qx


def find_fiducials(image,
                   bright_thresh: int = 200,
                   dark_thresh: int = 100,
                   min_area: int = 20,
                   max_area: int = 2000) -> Tuple[list, dict]:
    """
    Find ALL fiducial symbols in a frame.

    Returns (hits, detail) where hits is a list of
    {"center": (x, y), "corner_id": "tl"|"tr"|"bl"|"br"|None,
     "score": float, ...}, sorted by score descending.
    corner_id comes from the notch quadrant; None means the symbol was
    found but its identity is ambiguous (don't trust it for anchoring).
    """
    _, _, detail = find_fiducial(image, bright_thresh=bright_thresh,
                                 dark_thresh=dark_thresh,
                                 min_area=min_area, max_area=max_area)
    hits = [c for c in detail["candidates"] if c["score"] >= 0.45]
    return hits, detail


def parse_fiducial_telemetry(status: dict) -> Optional[Dict]:
    """
    Extract the fiducial report from a console MQTT status payload.

    New format (console v0.4.39+):
      {"fiducials": {"vw": w, "vh": h, "sx": sx, "sy": sy,
                     "marks": {"tl": {"x":..,"y":..,"size":..}, ...}}}
    Returns {"marks": {"tl": {"screen_xy": (x,y), "size": s}, ...},
             "viewport": (vw, vh), "scroll": (sx, sy)}
    or None if the payload has no fiducials. The legacy single-fiducial
    format (v0.4.37/0.4.38) is also accepted, mapped to corner "br".
    """
    status = status or {}
    fids = status.get("fiducials")
    if fids and isinstance(fids.get("marks"), dict):
        marks = {}
        for cid, m in fids["marks"].items():
            if "x" in m and "y" in m:
                marks[cid] = {"screen_xy": (float(m["x"]), float(m["y"])),
                              "size": float(m.get("size", FIDUCIAL_CSS_SIZE))}
        if not marks:
            return None
        return {
            "marks": marks,
            "viewport": (float(fids.get("vw", 0)), float(fids.get("vh", 0))),
            "scroll": (float(fids.get("sx", 0)), float(fids.get("sy", 0))),
        }
    # legacy single-fiducial format
    fid = status.get("fiducial")
    if not fid or "x" not in fid or "y" not in fid:
        return None
    return {
        "marks": {"br": {
            "screen_xy": (float(fid["x"]), float(fid["y"])),
            "size": float(fid.get("size", FIDUCIAL_CSS_SIZE))}},
        "viewport": (float(fid.get("vw", 0)), float(fid.get("vh", 0))),
        "scroll": (float(fid.get("sx", 0)), float(fid.get("sy", 0))),
    }


def find_fiducials_v2(image,
                      dark_thresh: int = 110,
                      bright_thresh: int = 170,
                      min_area: int = 8,
                      max_area: int = 500) -> Tuple[list, dict]:
    """
    Core-first fiducial detector (v2).

    v1 keyed on the symbol's WHITE body, which merges with light
    backgrounds (and the BLACK border merges with dark backgrounds) --
    both fail depending on what's behind the symbol. v2 keys on the
    one feature that is background-invariant: the BLACK CORE, which
    always sits inside the symbol's own white interior.

    Symbol ground truth (console CSS .twinFid): 72px white square,
    10px black border, 28px black core centered, 14px white notch in
    the core quadrant facing the symbol's corner.

    Returns (hits, detail) like find_fiducials(); hits have
    {"center", "corner_id" ("tl"/"tr"/"bl"/"br"/None), "score", ...}.
    """
    from scipy import ndimage

    gray = _to_gray(image)
    h, w = gray.shape
    detail: dict = {"candidates": []}

    dark = gray < dark_thresh
    labeled, n_feat = ndimage.label(dark)

    for i in range(1, n_feat + 1):
        ys, xs = np.where(labeled == i)
        area = len(xs)
        if not (min_area <= area <= max_area):
            continue
        x0, x1 = xs.min(), xs.max()
        y0, y1 = ys.min(), ys.max()
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        aspect = max(bw, bh) / max(min(bw, bh), 1)
        if aspect > 1.7:  # core is square; allow perspective slant
            continue
        cx, cy = float(xs.mean()), float(ys.mean())

        # --- white surround: the core must sit in the symbol's white
        # interior. Sample a ring just outside the core bbox; most of it
        # must be bright. (Tight ring: the border may merge with a dark
        # background, so don't reach that far.)
        pad = max(2, int(round(0.6 * max(bw, bh))))
        rx0, rx1 = max(0, x0 - pad), min(w, x1 + pad + 1)
        ry0, ry1 = max(0, y0 - pad), min(h, y1 + pad + 1)
        ring = gray[ry0:ry1, rx0:rx1].copy()
        ix0, ix1 = x0 - rx0, x1 - rx0 + 1
        iy0, iy1 = y0 - ry0, y1 - ry0 + 1
        ring[iy0:iy1, ix0:ix1] = 0  # exclude the core itself
        ring_px = ring[ring > 0]
        surround_bright = (float(np.mean(ring_px > bright_thresh))
                           if ring_px.size else 0.0)

        # --- notch quadrant -> corner identity ---
        notch_id = _notch_quadrant(gray, cx, cy, max(bw, bh) / 2,
                                   dark_thresh)

        # --- score ---
        square_score = max(0.0, 1.0 - (aspect - 1.0) / 0.7)
        score = (0.30 * square_score
                 + 0.30 * surround_bright
                 + 0.40 * (1.0 if notch_id else 0.0))
        # hard gate: without a white surround this isn't a symbol core
        if surround_bright < 0.45:
            score *= 0.3

        cand = {"center": (round(cx, 1), round(cy, 1)),
                "corner_id": notch_id,
                "area": int(area), "aspect": round(aspect, 2),
                "surround_bright": round(surround_bright, 2),
                "score": round(score, 3)}
        detail["candidates"].append(cand)

    detail["candidates"].sort(key=lambda c: c["score"], reverse=True)
    hits = [c for c in detail["candidates"] if c["score"] >= 0.45]
    return hits, detail
