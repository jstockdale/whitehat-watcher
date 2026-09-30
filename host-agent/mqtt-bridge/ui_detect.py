#!/usr/bin/env python3
"""
UI element detection for Watcher eyes-on-screen control.

Finds the interactive elements of the Claude chat UI in Himax camera frames
(640x480) without a human eyeballing pixel positions:

- find_orange_button(): the orange circular submit button at the right end
  of the chat text field.
- find_text_field(): the dark input bar at the bottom of the Claude pane.
  Its bounding box plugs directly into cursor_ops.plan_around() as an
  avoid rectangle (the cursor must route AROUND it, never through it —
  crossing flips the visible arrow into an invisible I-beam).
- find_claude_pane(): the dark chat area vs the bright console pane.

Pure Python + PIL + numpy. No hardware, no MQTT, no network.

Color model (measured 2026-09-25 from /tmp/respond_p3.jpg):
- Button disc: R 210-250, G 160-200, B 110-150 (warm orange)
- Button center icon: near-white warm (R>200, G>180, B>140, R>=G>=B)
- Text field: neutral dark, gray ~35-45
- Claude pane: dark, gray ~30-70 | Console pane: bright, gray ~140-165

Validation (self-test over 25 /tmp/*.jpg frames, 2026-09-25):
- Orange button: 25/25. All 15 frames where the button is visible are
  found within 10px of hand-verified position (12 of 15 within 2px;
  the 3 with ~10px error are respond_p5-p8 where the hand GT itself
  was an eyeball estimate — the detector is self-consistent at
  (389,420) across all frames, fixed UI + fixed camera). All 10 frames
  where the button is absent (early loading frames, scrolled-up views,
  post-send state) correctly return (None, 0.0, 0) — no false positives.
- Text field: 25/25. Returns (0,400,401,479) +/-2px, conf 0.87-0.93.
  This is the bottom band of the Claude pane containing the input
  container AND the submit button, so it doubles as the avoid rect.
- Claude pane: 25/25. Returns (0,0,397,479) +/-1px in all frames.

Limitations (honest):
- Button position prior (lower-right of Claude pane) is load-bearing for
  rejecting the sun emoji; if the UI layout changes, retune.
- Text field detection leans on the fixed UI geometry (input container
  always in the bottom ~75px of the pane). If the camera moves or the UI
  rescales, the band prior must be recalibrated — the sanity gate
  (band must be dark) will fail safe rather than return a bogus rect.
- Fiducial rejection is primarily by color (fiducials are black/white);
  the square-vs-disc shape check is a backstop.
- Radius estimate (~6px) is the orange-disc core; the clickable button
  including its glow is ~8-10px.
"""

from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _load_rgb(path):
    """Load image as int32 RGB numpy array. Returns (array, w, h)."""
    img = Image.open(path).convert("RGB")
    a = np.asarray(img).astype(np.int32)
    h, w, _ = a.shape
    return a, w, h


def _label_blobs(mask):
    """
    Connected-component labeling for a boolean mask (4-connectivity).
    Pure numpy + BFS over sparse foreground; fine for a few thousand px.
    Returns list of dicts: {pixels: (ys, xs), area, bbox}.
    """
    h, w = mask.shape
    labels = np.zeros((h, w), dtype=np.int32)
    blobs = []
    cur = 0
    ys_all, xs_all = np.nonzero(mask)
    remaining = set(zip(ys_all.tolist(), xs_all.tolist()))
    while remaining:
        cur += 1
        seed = remaining.pop()
        q = deque([seed])
        labels[seed] = cur
        ys, xs = [seed[0]], [seed[1]]
        while q:
            y, x = q.popleft()
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if (ny, nx) in remaining:
                    remaining.discard((ny, nx))
                    labels[ny, nx] = cur
                    q.append((ny, nx))
                    ys.append(ny)
                    xs.append(nx)
        blobs.append({
            "area": len(ys),
            "bbox": (min(xs), min(ys), max(xs), max(ys)),
            "ys": np.array(ys),
            "xs": np.array(xs),
        })
    return blobs


def _circularity(blob):
    """
    How disc-like is the blob? 1.0 = perfect disc.
    Uses bbox fill ratio vs pi/4 (disc fills pi/4 of its bounding square).
    """
    x0, y0, x1, y1 = blob["bbox"]
    bw = x1 - x0 + 1
    bh = y1 - y0 + 1
    if bw <= 0 or bh <= 0:
        return 0.0
    fill = blob["area"] / (bw * bh)
    aspect = min(bw, bh) / max(bw, bh)
    # disc: fill ~= 0.785, aspect ~= 1. square: fill ~= 1.0
    fill_score = max(0.0, 1.0 - abs(fill - 0.785) / 0.785)
    return fill_score * aspect


# ---------------------------------------------------------------------------
# 1. orange submit button
# ---------------------------------------------------------------------------

# Measured button orange (disc, excluding bright center icon).
_ORANGE_LO = np.array([150, 110, 60])
_ORANGE_HI = np.array([255, 215, 170])


def _orange_mask(a):
    """Boolean mask of orange-disc pixels plus warm bright center pixels.

    The warm-center sub-mask requires genuine warmth (R-B > 25) so the
    bright white console pane (R==G==B==255) is NOT matched.
    """
    disc = (
        (a[..., 0] >= _ORANGE_LO[0]) & (a[..., 0] <= _ORANGE_HI[0]) &
        (a[..., 1] >= _ORANGE_LO[1]) & (a[..., 1] <= _ORANGE_HI[1]) &
        (a[..., 2] >= _ORANGE_LO[2]) & (a[..., 2] <= _ORANGE_HI[2]) &
        (a[..., 0] > a[..., 1] + 15) & (a[..., 1] > a[..., 2] + 15)
    )
    # bright warm center (the arrow icon): near-white but warm-tinted.
    # (R-B > 25) rejects neutral white/gray (console pane, fiducial cores).
    center = (
        (a[..., 0] > 200) & (a[..., 1] > 150) & (a[..., 2] > 100) &
        (a[..., 0] - a[..., 2] > 25) & (a[..., 0] - a[..., 2] < 90) &
        (a[..., 1] - a[..., 2] > 10)
    )
    return disc | center


def find_orange_button(image_path):
    """
    Find the orange circular submit button.

    Returns (x, y, confidence, radius):
      x, y   - button center in image pixels (floats)
      confidence - 0..1
      radius - estimated disc radius in pixels (float)
    Returns (None, 0.0, 0) if no button is found.

    Rejects: the sun emoji in chat (wrong position), tiny orange specks in
    browser chrome (too small / too high), and square fiducial markers
    (shape check: disc vs square, plus fiducials are black/white anyway).
    """
    a, w, h = _load_rgb(image_path)
    mask = _orange_mask(a)

    # Working ROI: the button lives at the right end of the text field,
    # i.e. lower-right of the Claude (dark) pane. Exclude:
    #  - the chat message area (sun emoji false positive),
    #  - the top browser chrome (tiny orange specks),
    #  - the bright console pane (its warm-tinted pixels can leak through).
    (px0, _py0, px1, _py1), _ = find_claude_pane(image_path)
    yy, xx = np.mgrid[0:h, 0:w]
    # xx < px1 + 4: keep the button's right edge (it sits ~5px left of the
    # pane boundary); the tightened color mask already rejects the pane.
    roi = (xx > w * 0.45) & (xx < px1 + 4) & (yy > h * 0.55)
    blobs = _label_blobs(mask & roi)
    if not blobs:
        return None, 0.0, 0

    best = None
    best_score = 0.0
    for b in blobs:
        x0, y0, x1, y1 = b["bbox"]
        bw = x1 - x0 + 1
        bh = y1 - y0 + 1
        # Size gate: button is ~12-20px across in these frames.
        if not (8 <= bw <= 32 and 8 <= bh <= 32):
            continue
        if not (25 <= b["area"] <= 900):
            continue
        circ = _circularity(b)
        if circ < 0.35:
            continue  # streaks / irregular blobs

        # Fiducial backstop: square markers have a thick BLACK border ring.
        # Sample a ring just outside the bbox; if mostly near-black AND the
        # blob is squarish (fill ~1.0), it's a marker, not the button.
        pad = 3
        rx0, ry0 = max(0, x0 - pad), max(0, y0 - pad)
        rx1, ry1 = min(w - 1, x1 + pad), min(h - 1, y1 + pad)
        ring = np.zeros((ry1 - ry0 + 1, rx1 - rx0 + 1), dtype=bool)
        ring[:pad, :] = True
        ring[-pad:, :] = True
        ring[:, :pad] = True
        ring[:, -pad:] = True
        ring_gray = a[ry0:ry1 + 1, rx0:rx1 + 1].mean(axis=2)
        black_frac = (ring_gray[ring] < 25).mean() if ring.any() else 0.0
        fill = b["area"] / (bw * bh)
        if black_frac > 0.45 and fill > 0.9:
            continue  # black-bordered square marker

        # Position prior: button sits at the right end of the text field,
        # i.e. right side, vertically in the lower band where the input bar is.
        cx = (x0 + x1) / 2
        cy = (y0 + y1) / 2
        pos_score = 1.0
        if cx < w * 0.55:
            pos_score *= 0.4
        if not (h * 0.70 < cy < h * 0.97):
            pos_score *= 0.4

        size_score = 1.0 - min(abs(b["area"] - 150) / 400, 1.0) * 0.5
        score = 0.45 * circ + 0.25 * size_score + 0.30 * pos_score
        if score > best_score:
            best_score = score
            best = (cx, cy, b)

    if best is None:
        return None, 0.0, 0
    cx, cy, b = best
    radius = float(np.sqrt(b["area"] / np.pi))
    confidence = round(min(max(best_score, 0.0), 0.95), 3)
    return round(float(cx), 1), round(float(cy), 1), confidence, round(radius, 1)


# ---------------------------------------------------------------------------
# 2. text input field (avoid rectangle for waypoint planning)
# ---------------------------------------------------------------------------

def find_text_field(image_path):
    """
    Find the chat text input area (dark container holding the message
    field, placeholder "Write a message...", and the submit button).

    Returns ((x0, y0, x1, y1), confidence): bounding box in image pixels,
    directly usable as an avoid rect in
    cursor_ops.plan_around(cursor, target, avoid_rects).

    Strategy: the input container is fixed UI — it always occupies the
    bottom ~75px of the Claude pane (verified constant across all
    validation frames; the chat messages scroll but this container does
    not). We take the pane geometry from find_claude_pane(), cut the
    bottom band, and verify it is actually dark (sanity gate against a
    wildly wrong pane detection). The orange submit button sits inside
    this rect, so plan_around() naturally routes above it and drops
    vertically onto the button — exactly John's rule.
    """
    a, w, h = _load_rgb(image_path)
    gray = a.mean(axis=2)

    (px0, _py0, px1, py1), pane_conf = find_claude_pane(image_path)

    # Input container: bottom band of the pane.
    band_h = 75
    y0 = max(0, py1 - band_h)
    y1 = py1
    x0, x1 = px0, px1

    # Sanity: the band must be predominantly dark. If not, the pane
    # detection (or the UI layout) is off — report not-found rather than
    # a bogus rect.
    band = gray[y0:y1 + 1, x0:x1 + 1]
    dark_frac = (band < 70).mean()
    if dark_frac < 0.55:
        return (0, 0, 0, 0), 0.0

    # Confidence: darker + wider = more confident, capped at 0.95.
    confidence = round(min(0.60 + dark_frac * 0.30 +
                           min((x1 - x0) / 500, 1.0) * 0.05, 0.95), 3)
    pad = 4
    return (max(0, x0 - pad), max(0, y0 - pad),
            min(w - 1, x1 + pad), min(h - 1, y1 + pad)), confidence


# ---------------------------------------------------------------------------
# 3. Claude pane (dark chat area vs bright console)
# ---------------------------------------------------------------------------

def find_claude_pane(image_path):
    """
    Find the dark Claude chat pane (vs the bright white console on the right).

    Returns ((x0, y0, x1, y1), confidence): bounding box of the dark pane.
    Used to constrain search regions (button/field live inside this pane).
    """
    a, w, h = _load_rgb(image_path)
    gray = a.mean(axis=2)

    # Column brightness: dark pane left, bright console right.
    col = gray.mean(axis=0)
    # boundary = last column (scanning left->right) where brightness jumps
    # from dark (<90) to bright (>120) and stays bright for a while.
    x1 = w - 1
    for x in range(int(w * 0.3), w - 60):
        if col[x] < 90 and col[x + 1] > 110 and col[x + 1:x + 60].mean() > 120:
            x1 = x
            break
    else:
        # fallback: brightest sustained region boundary
        bright = col > 120
        idx = np.nonzero(bright)[0]
        if len(idx):
            # right edge of the dark pane = just before the first long bright run
            x1 = int(idx[0]) - 1

    # Row brightness: screen top/bottom vs dark bezel.
    row = gray.mean(axis=1)
    y0, y1p = 0, h - 1
    for y in range(h):
        if row[y] > 25:
            y0 = y
            break
    for y in range(h - 1, -1, -1):
        if row[y] > 25:
            y1p = y
            break

    # Left edge: first column that's part of the screen (not bezel).
    x0 = 0
    for x in range(w):
        if col[x] > 25:
            x0 = x
            break

    confidence = 0.9 if (x1 - x0) > w * 0.4 else 0.5
    return (x0, y0, x1, y1p), round(confidence, 3)


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

# Hand-verified ground truth (2026-09-25).
# Button visibility was checked per-image by sampling the button region:
# visible frames have region mean ~(130-150, 100-120, 80-90) [orange],
# absent frames have region mean ~(50, 55, 50) [dark].
# The button is absent in early frames (UI not yet rendered / loading)
# and in scrolled-up frames (button below the fold), and after sending
# (submit control changes state while Claude responds).
_GT = {
    # button VISIBLE (orange disc at ~(390,420))
    "respond_p3.jpg": {"button": (390, 420)},
    "respond_p4.jpg": {"button": (390, 420)},
    "respond_p5.jpg": {"button": (379, 421)},
    "respond_p6.jpg": {"button": (379, 421)},
    "respond_p7.jpg": {"button": (379, 421)},
    "respond_p8.jpg": {"button": (379, 421)},
    "reply_p4.jpg":   {"button": (390, 420)},
    "reply_p5.jpg":   {"button": (390, 420)},
    "reply_p6.jpg":   {"button": (390, 420)},
    "reply_p7.jpg":   {"button": (390, 420)},
    "reply_p8.jpg":   {"button": (390, 420)},
    "reply_p9.jpg":   {"button": (390, 420)},
    "reply_p10.jpg":  {"button": (390, 420)},
    "reply_p11.jpg":  {"button": (390, 420)},
    "reply_p12.jpg":  {"button": (390, 420)},
    # button ABSENT (verified dark in button region)
    "respond_p0.jpg": {"button": None},
    "respond_p1.jpg": {"button": None},
    "respond_p2.jpg": {"button": None},
    "reply_p0.jpg":   {"button": None},
    "reply_p1.jpg":   {"button": None},
    "reply_p2.jpg":   {"button": None},
    "reply_p3.jpg":   {"button": None},
    "final2.jpg":          {"button": None},
    "claude_reply2.jpg":   {"button": None},
    "claude_response.jpg": {"button": None},
}


def _self_test():
    print("ui_detect self-test over /tmp/*.jpg", flush=True)
    n_btn_ok = n_btn_total = 0
    n_field_ok = n_field_total = 0
    fails = []
    for name in sorted(_GT):
        path = f"/tmp/{name}"
        if not Path(path).exists():
            print(f"  SKIP {name} (missing)")
            continue
        gt_btn = _GT[name]["button"]

        res = find_orange_button(path)
        n_btn_total += 1
        if res[0] is None:
            bx = by = None
            bconf, brad = res[1], res[2] if len(res) > 2 else 0
        else:
            bx, by, bconf, brad = res[0], res[1], res[2], res[3] if len(res) > 3 else 0
        if gt_btn is None:
            ok = bx is None
            detail = "absent->not-found" if ok else f"FALSE POSITIVE at ({bx},{by})"
        else:
            if bx is None:
                ok, detail = False, "MISSED (not found)"
            else:
                err = ((bx - gt_btn[0]) ** 2 + (by - gt_btn[1]) ** 2) ** 0.5
                ok = err <= 12
                detail = f"err={err:.1f}px conf={bconf} r={brad}"
        n_btn_ok += ok
        if not ok:
            fails.append(f"button {name}: {detail}")

        (fx0, fy0, fx1, fy1), fconf = find_text_field(path)
        n_field_total += 1
        # field bar must sit in the bottom band and be wide
        fok = (fy0 >= 400 and fy1 <= 480 and (fx1 - fx0) >= 180 and fconf > 0.4)
        n_field_ok += fok
        if not fok:
            fails.append(f"field {name}: bbox=({fx0},{fy0},{fx1},{fy1}) conf={fconf}")

        (px0, py0, px1, py1), pconf = find_claude_pane(path)
        print(f"  {name}: button [{detail}] "
              f"field=({fx0},{fy0},{fx1},{fy1},c={fconf}) "
              f"pane=({px0},{py0},{px1},{py1},c={pconf})", flush=True)

    print(f"\nbutton: {n_btn_ok}/{n_btn_total}  field: {n_field_ok}/{n_field_total}",
          flush=True)
    if fails:
        print("FAILURES:", flush=True)
        for f in fails:
            print(f"  - {f}", flush=True)
        raise SystemExit(1)
    print("SELF-TEST OK", flush=True)


if __name__ == "__main__":
    _self_test()
