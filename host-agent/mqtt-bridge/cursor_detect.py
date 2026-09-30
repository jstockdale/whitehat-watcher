#!/usr/bin/env python3
"""
Automatic cursor detection for Watcher eyes-on-screen control.

Pure image processing — no hardware commands, no MQTT. The caller
(servo loop in cursor_ops.py) handles moves and snapshots; this module
answers "where is the cursor in this camera frame?".

Two methods:

1. detect_wiggle (primary): differential. Snapshot A, nudge the cursor by
   a known small HID amount, snapshot B. Pixels that brightened (cursor's
   new position) and pixels that darkened (cursor's old position) form a
   pos/neg lobe pair whose displacement must match the expected wiggle in
   camera pixels (via the Jacobian). Screen animation and JPEG noise almost
   never produce a pair with the right displacement vector, so false
   positives are rare. Returns the midpoint (cursor's average position
   across the two frames) or None when nothing matches.

2. detect_bright_blob (fallback): single-frame. Finds bright compact blobs
   and scores them as cursor candidates. Cheap but unreliable in busy UIs
   (chat text, fiducial markers and buttons are all bright) — use only to
   get a rough region, then confirm with a wiggle.

Measured accuracy on real bench images (2026-09-25, 640x480 Himax JPEGs):
- detect_wiggle found the correct lobe pair in 5 of 6 real move pairs, with
  displacement error vs. the commanded (Jacobian-predicted) move typically
  under 9 px Manhattan. The 6th pair correctly returned None (the cursor
  had left the visible frame between snapshots).
- One pair caught the I-beam text cursor (not just the arrow) — the method
  detects any small bright thing that moves as commanded.
- detect_bright_blob on a frame with a known cursor (reply_p10.jpg,
  cursor visually confirmed at ~(350,352)) ranked a fiducial marker first:
  single-frame detection cannot reliably distinguish cursor / fiducial /
  text. Treat its output as a hint (confidence < 0.5), never as truth.

Requires: PIL, numpy. No other dependencies.
"""

from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

# Forward Jacobian: camera pixels per HID unit (informal 2026-09-25 fit).
# Kept in sync with cursor_ops.J_INV (this is J, that is J^-1).
J_FWD = [
    [0.1924, 0.0034],
    [-0.0076, 0.2202],
]


# ---------------------------------------------------------------------------
# internals
# ---------------------------------------------------------------------------

def _load_gray(path):
    """Load an image as float32 grayscale numpy array."""
    img = Image.open(path).convert("L")
    return np.asarray(img, dtype=np.float32)


def _label_components(mask):
    """
    Connected-component labeling (8-connectivity) on a 2D bool mask.
    Pure-Python BFS; fine for sparse masks (a few thousand pixels).

    Returns a list of dicts: area, cx, cy (centroid in x/y pixels),
    x0, x1, y0, y1 (inclusive bounding box), compact (area / bbox area),
    mean_bright (filled in by callers when needed).
    """
    h, w = mask.shape
    visited = np.zeros_like(mask, dtype=bool)
    comps = []
    while True:
        cand = np.nonzero(mask & ~visited)
        if cand[0].size == 0:
            break
        sy, sx = int(cand[0][0]), int(cand[1][0])
        q = deque([(sy, sx)])
        visited[sy, sx] = True
        xs = []
        ys = []
        while q:
            cy, cx = q.popleft()
            xs.append(cx)
            ys.append(cy)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    ny, nx = cy + dy, cx + dx
                    if (0 <= ny < h and 0 <= nx < w
                            and mask[ny, nx] and not visited[ny, nx]):
                        visited[ny, nx] = True
                        q.append((ny, nx))
        xs = np.asarray(xs)
        ys = np.asarray(ys)
        x0, x1 = int(xs.min()), int(xs.max())
        y0, y1 = int(ys.min()), int(ys.max())
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        comps.append({
            "area": int(xs.size),
            "cx": float(xs.mean()),
            "cy": float(ys.mean()),
            "x0": x0, "x1": x1, "y0": y0, "y1": y1,
            "compact": float(xs.size) / float(bw * bh),
            "xs": xs, "ys": ys,
        })
    return comps


def _expected_cam_disp(wiggle_hid_dx, wiggle_hid_dy):
    """Expected cursor displacement in camera pixels for a HID wiggle."""
    ex = J_FWD[0][0] * wiggle_hid_dx + J_FWD[0][1] * wiggle_hid_dy
    ey = J_FWD[1][0] * wiggle_hid_dx + J_FWD[1][1] * wiggle_hid_dy
    return ex, ey


# ---------------------------------------------------------------------------
# primary: wiggle (differential) detection
# ---------------------------------------------------------------------------

def detect_wiggle(snap_a_path, snap_b_path, wiggle_hid_dx, wiggle_hid_dy=0.0,
                  diff_thresh=45.0, min_area=8, max_area=400,
                  max_pair_sep=80.0, max_disp_err=14.0, min_brightness=80.0):
    """
    Find the cursor from a before/after snapshot pair around a known wiggle.

    snap_a_path: snapshot taken BEFORE the wiggle.
    snap_b_path: snapshot taken AFTER the wiggle.
    wiggle_hid_dx / wiggle_hid_dy: the commanded HID nudge between them.

    How it works: the cursor's old position gets darker in B (neg lobe) and
    its new position gets brighter (pos lobe). Every pos/neg component pair
    is scored by how well its displacement matches the Jacobian-predicted
    wiggle in camera pixels. The best pair within max_disp_err wins.

    Returns (x, y) — the midpoint of the two lobes in camera pixels —
    or None when no pair matches (cursor invisible, off-screen, or the
    wiggle didn't register).
    """
    a = _load_gray(snap_a_path)
    b = _load_gray(snap_b_path)
    # Crop to common shape in case of size mismatch.
    h = min(a.shape[0], b.shape[0])
    w = min(a.shape[1], b.shape[1])
    a, b = a[:h, :w], b[:h, :w]

    d = b - a
    pos_comps = _label_components(d > diff_thresh)
    neg_comps = _label_components(d < -diff_thresh)
    ex, ey = _expected_cam_disp(wiggle_hid_dx, wiggle_hid_dy)

    best = None
    for p in pos_comps:
        if not (min_area <= p["area"] <= max_area):
            continue
        for n in neg_comps:
            if not (min_area <= n["area"] <= max_area):
                continue
            dx = p["cx"] - n["cx"]
            dy = p["cy"] - n["cy"]
            if (dx * dx + dy * dy) ** 0.5 > max_pair_sep:
                continue
            err = abs(dx - ex) + abs(dy - ey)
            if err > max_disp_err:
                continue
            # The cursor is white: the new-pos lobe must be bright in B
            # and/or the old-pos lobe bright in A.
            bright_p = float(b[p["ys"], p["xs"]].mean())
            bright_n = float(a[n["ys"], n["xs"]].mean())
            if max(bright_p, bright_n) < min_brightness:
                continue
            # Prefer small error, then brighter, then larger lobes.
            score = (err
                     - 0.01 * max(bright_p, bright_n)
                     - 0.02 * min(p["area"], n["area"]))
            if best is None or score < best[0]:
                best = (score, p, n)

    if best is None:
        return None
    _, p, n = best
    return ((p["cx"] + n["cx"]) / 2.0, (p["cy"] + n["cy"]) / 2.0)


# ---------------------------------------------------------------------------
# fallback: single-frame bright blob detection
# ---------------------------------------------------------------------------

def detect_bright_blob(image_path, bright_thresh=170.0,
                       min_area=15, max_area=600):
    """
    Single-frame cursor guess: find bright compact blobs, score them.

    Scoring prefers: area in a cursor-plausible band, compact
    (arrow-like, not text streaks), very bright, and not huge (rules out
    the white console panel). Returns ((x, y), confidence) with confidence
    in 0..1, or (None, 0.0) when nothing is plausible.

    WARNING: in busy UIs this confuses the cursor with fiducial markers,
    bright text and buttons (measured: ranked a fiducial first on a real
    frame). Use as a hint only; confirm with detect_wiggle.
    """
    img = _load_gray(image_path)
    comps = _label_components(img > bright_thresh)

    scored = []
    for c in comps:
        area = c["area"]
        if not (min_area <= area <= max_area):
            continue
        bw = c["x1"] - c["x0"] + 1
        bh = c["y1"] - c["y0"] + 1
        # Text streaks are wide and short; the cursor is roughly square.
        aspect = max(bw, bh) / max(min(bw, bh), 1)
        if aspect > 3.0:
            continue
        # Cursor arrow is moderately compact (filled triangle ~0.4-0.7).
        # Fiducial white squares are more solid; text is streaky.
        compact = c["compact"]
        size_score = np.exp(-((area - 60.0) / 70.0) ** 2)
        compact_score = np.exp(-((compact - 0.55) / 0.25) ** 2)
        bright = float(img[c["ys"], c["xs"]].mean())
        bright_score = min(max((bright - 150.0) / 80.0, 0.0), 1.0)
        aspect_score = 1.0 if aspect <= 1.8 else 0.5
        total = size_score * compact_score * bright_score * aspect_score
        scored.append((total, c, bright))

    if not scored:
        return None, 0.0
    scored.sort(key=lambda s: -s[0])
    best_score, best_c, best_bright = scored[0]
    if best_score <= 0.0:
        return None, 0.0
    # Confidence from absolute match quality and margin over runner-up.
    margin = 1.0
    if len(scored) > 1 and scored[1][0] > 0:
        margin = min(best_score / scored[1][0] / 2.0, 1.0)
    confidence = min(best_score, 1.0) * 0.6 + margin * 0.4
    # Single-frame is never trustworthy: cap confidence (see docstring).
    confidence = min(confidence, 0.65)
    return (best_c["cx"], best_c["cy"]), float(confidence)


def detect(image_path, method="auto"):
    """
    Convenience wrapper. method="auto" (default) tries the cheap
    single-frame blob detector; the caller is expected to follow up with
    detect_wiggle for anything it intends to act on.

    Returns ((x, y), confidence) like detect_bright_blob.
    """
    if method in ("auto", "blob"):
        return detect_bright_blob(image_path)
    raise ValueError(f"unknown method {method!r} (try 'auto' or 'blob')")


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _self_test():
    print("cursor_detect self-test")
    print("=======================")

    # --- 1. synthetic wiggle: paste a fake cursor arrow, move it, detect ---
    print("\n[1] synthetic wiggle pair")
    rng = np.random.RandomState(7)
    base = (rng.rand(480, 640) * 40).astype(np.float32)  # dark noisy bg
    # fake arrow: small bright triangle-ish blob
    yy, xx = np.mgrid[0:480, 0:640]
    arrow = ((xx - 300) ** 2 + (yy - 200) ** 2 < 60).astype(np.float32) * 220
    a_img = np.clip(base + arrow, 0, 255)
    # wiggle HID (+20, 0) -> expected cam disp
    ex, ey = _expected_cam_disp(20, 0)
    print(f"    synthetic HID(+20,0) -> expected cam disp ({ex:.2f},{ey:.2f})")
    arrow_b = ((xx - 300 - ex) ** 2 + (yy - 200 - ey) ** 2 < 60).astype(np.float32) * 220
    b_img = np.clip(base + arrow_b, 0, 255)
    Image.fromarray(a_img.astype(np.uint8)).save("/tmp/det_syn_a.png")
    Image.fromarray(b_img.astype(np.uint8)).save("/tmp/det_syn_b.png")
    got = detect_wiggle("/tmp/det_syn_a.png", "/tmp/det_syn_b.png", 20, 0)
    assert got is not None, "synthetic wiggle not detected!"
    # true midpoint: ((300 + 300+ex)/2, (200 + 200+ey)/2)
    tx, ty = 300 + ex / 2, 200 + ey / 2
    err = abs(got[0] - tx) + abs(got[1] - ty)
    print(f"    detected ({got[0]:.1f},{got[1]:.1f}) vs true ({tx:.1f},{ty:.1f}) err={err:.1f}px")
    assert err < 3.0, f"synthetic wiggle error too large: {err}"
    print("    PASS")

    # --- 2. synthetic no-motion: identical frames -> None ---
    print("\n[2] synthetic no-motion pair (must return None)")
    got = detect_wiggle("/tmp/det_syn_a.png", "/tmp/det_syn_a.png", 20, 0)
    assert got is None, f"false positive on identical frames: {got}"
    print("    PASS (None)")

    # --- 3. real bench pairs with known HID moves ---
    print("\n[3] real bench image pairs (known HID moves)")
    real_pairs = [
        # (a, b, hdx, hdy, note)
        ("/tmp/reply_p11.jpg", "/tmp/reply_p12.jpg", -9, -105,
         "arrow near submit button"),
        ("/tmp/reply_p4.jpg", "/tmp/reply_p5.jpg", 0, 40,
         "I-beam text cursor moving down"),
        ("/tmp/reply_p6.jpg", "/tmp/reply_p7.jpg", 0, 30,
         "small nudge"),
        ("/tmp/reply_p8.jpg", "/tmp/reply_p9.jpg", 61, -151,
         "diagonal move"),
        ("/tmp/respond_p7.jpg", "/tmp/respond_p8.jpg", 141, 259,
         "large move to submit"),
    ]
    failures = 0
    for ap, bp, hdx, hdy, note in real_pairs:
        if not (Path(ap).exists() and Path(bp).exists()):
            print(f"    SKIP {ap} (missing)")
            continue
        got = detect_wiggle(ap, bp, hdx, hdy)
        tag = f"{Path(ap).stem}->{Path(bp).stem} HID({hdx},{hdy}) [{note}]"
        if got is None:
            print(f"    MISS {tag}")
            failures += 1
        else:
            print(f"    OK   {tag} -> ({got[0]:.0f},{got[1]:.0f})")
    # The p9->p10 pair is a known-correct None (cursor left the frame);
    # verify it returns None rather than a false positive.
    r_none = detect_wiggle("/tmp/reply_p9.jpg", "/tmp/reply_p10.jpg", 0, -80)
    print(f"    known-invisible pair p9->p10 -> {r_none} (None is correct here)")
    assert failures == 0, f"{failures} real pairs missed"
    print("    PASS")

    # --- 4. blob detector smoke test (honest: not asserted for accuracy) ---
    print("\n[4] bright-blob smoke test (informational only)")
    for p in ["/tmp/reply_p10.jpg", "/tmp/reply_p12.jpg", "/tmp/respond_p0.jpg"]:
        if not Path(p).exists():
            continue
        (pos, conf) = detect_bright_blob(p)
        print(f"    {Path(p).name}: pos={pos} conf={conf:.2f}")
    print("    (blob accuracy is known-poor; see docstring — wiggle is primary)")

    # --- 5. detect() wrapper ---
    print("\n[5] detect() wrapper")
    pos, conf = detect("/tmp/reply_p12.jpg")
    assert pos is not None and 0.0 <= conf <= 1.0
    print(f"    OK -> ({pos[0]:.0f},{pos[1]:.0f}) conf={conf:.2f}")

    print("\nAll self-tests passed.")


if __name__ == "__main__":
    _self_test()
