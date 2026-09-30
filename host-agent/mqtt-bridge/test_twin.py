#!/usr/bin/env python3
"""
Thorough offline tests for the digital twin stack:
  screen_edges.py, fiducial.py, screen_twin.py

Run:  python3 test_twin.py
All synthetic tests must pass. Real-frame tests are informational
(they print what the detectors see in actual Himax captures).
"""
import math
import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from screen_edges import (find_display_rectangle, solve_homography,
                          apply_homography, homography_reprojection_error,
                          screen_corners_wh)
from fiducial import find_fiducial
from screen_twin import ScreenTwin

PASS, FAIL = "PASS", "FAIL"
results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(f"[{PASS if cond else FAIL}] {name}" + (f" -- {detail}" if detail and not cond else ""))


def make_screen_frame(size=416, rect=None, bg=18, fg=210, noise=0, seed=1):
    """Synthetic camera frame: dark background, bright screen quad."""
    rng = np.random.default_rng(seed)
    img = Image.new("L", (size, size), bg)
    d = ImageDraw.Draw(img)
    if rect is None:
        # rotated rectangle (perspective-ish view of a screen)
        cx = cy = size / 2
        hw, hh, ang = 150, 110, math.radians(8)
        pts = []
        for sx, sy in [(-1, -1), (1, -1), (1, 1), (-1, 1)]:
            x = sx * hw * math.cos(ang) - sy * hh * math.sin(ang) + cx
            y = sx * hw * math.sin(ang) + sy * hh * math.cos(ang) + cy
            pts.append((x, y))
        rect = pts
    d.polygon(rect, fill=fg)
    if noise:
        arr = np.array(img).astype(float) + rng.normal(0, noise, (size, size))
        img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
    return img, rect


def make_fiducial_frame(size=416, sym_px=28, pos=None, bg=18,
                        notch="tr", n_marks=1):
    """
    Synthetic frame with calibration fiducial(s), proportions faithful
    to the page CSS: 72px box, 10px black border (13.9%), 28px black
    core (38.9%) with a 14px white notch in the `notch` quadrant
    ("tl"/"tr"/"bl"/"br"). n_marks=4 renders one per corner.
    """
    from fiducial import FIDUCIAL_CORNER_IDS
    img = Image.new("L", (size, size), bg)
    d = ImageDraw.Draw(img)
    positions = []
    if n_marks == 4:
        m = size * 0.12
        spots = {"tl": (m, m), "tr": (size - m, m),
                 "bl": (m, size - m), "br": (size - m, size - m)}
        items = [(cid, spots[cid], cid) for cid in FIDUCIAL_CORNER_IDS]
    else:
        items = [(notch, pos or (size * 0.78, size * 0.80), notch)]
    for cid, (cx, cy), nq in items:
        h = sym_px / 2
        b = max(1, int(round(sym_px * 0.139)))      # black border ring
        d.rectangle([cx - h, cy - h, cx + h, cy + h], fill=235)
        d.rectangle([cx - h, cy - h, cx + h, cy + h], outline=20, width=b)
        ch = max(2, int(round(sym_px * 0.389 / 2)))
        d.rectangle([cx - ch, cy - ch, cx + ch, cy + ch], fill=20)
        # notch: white quadrant of the core facing the symbol's corner
        nx0, nx1 = (cx - ch, cx) if "l" in nq else (cx, cx + ch)
        ny0, ny1 = (cy - ch, cy) if "t" in nq else (cy, cy + ch)
        d.rectangle([nx0, ny0, nx1, ny1], fill=235)
        positions.append((cid, (cx, cy)))
    return img, positions


# ---------------------------------------------------------------- 1. DLT ---
print("--- homography DLT ---")
rng = np.random.default_rng(0)
true_H = np.array([[1.2, 0.15, 30.0], [-0.1, 1.1, 20.0], [0.0008, -0.0004, 1.0]])
src = rng.uniform(0, 1920, (8, 2))
dst = apply_homography(true_H, src)
H = solve_homography(src, dst)
err = homography_reprojection_error(H, src, dst)
check("DLT round-trip < 1e-6 px", err < 1e-6, f"err={err:.2e}")
check("DLT recovers true H", np.allclose(H / H[2, 2], true_H / true_H[2, 2], atol=1e-6))

# ------------------------------------------------- 2. edge detection synth ---
print("--- edge detection (synthetic) ---")
img, rect = make_screen_frame(noise=4)
corners, conf, detail = find_display_rectangle(img)
found = [corners[k] for k in ("tl", "tr", "br", "bl")]
# rect order from make_screen_frame: [TL, TR, BR, BL] (rotated)
ok = all(f is not None for f in found)
max_err = 0.0
if ok:
    for f, t in zip(found, rect):
        max_err = max(max_err, math.dist(f, t))
check("synthetic: all 4 corners found", ok, str(detail["sides"]))
check("synthetic: corners within 3px", ok and max_err < 3.0, f"max_err={max_err:.2f}")
check("synthetic: confidence > 0.7", conf > 0.7, f"conf={conf}")
Hsyn = None
if ok:
    from screen_edges import rectangle_homography
    Hsyn = rectangle_homography(corners, 1920, 1080)
    check("synthetic: homography from corners", Hsyn is not None)
    # center of screen should map near center of quad
    c = apply_homography(Hsyn, [(960, 540)])[0]
    exp = (sum(p[0] for p in rect) / 4, sum(p[1] for p in rect) / 4)
    check("synthetic: screen center maps to quad center < 3px",
          math.dist(c, exp) < 3.0, f"mapped={c}, expected={exp}")

# partial occlusion: cover the right half -> the visible "right edge" is now
# the occluder's boundary, giving a quad with the wrong aspect ratio.
# With the known screen aspect, the detector must REJECT it (conf 0)
# rather than return a wrong-but-confident homography.
img2 = img.copy()
d2 = ImageDraw.Draw(img2)
d2.rectangle([208, 0, 416, 416], fill=18)
true_aspect = (150 * 2) / (110 * 2)  # the synthetic screen's aspect
corners2, conf2, detail2 = find_display_rectangle(
    img2, expected_aspect=true_aspect)
check("occluded: wrong-aspect quad rejected (conf 0)",
      conf2 == 0.0 and "rejected" in detail2,
      f"conf={conf2} detail={detail2.get('rejected', '')[:60]}")
# and the twin refuses low-confidence geometry updates
twin_occ = ScreenTwin(J=np.array([[0.205862, 0.003448], [-0.007586, 0.194828]]))
r_occ = twin_occ.update_geometry(corners2, (1920, 1080), conf2)
check("occluded: twin refuses low-confidence geometry",
      not r_occ["updated"] and twin_occ.H_sc is None, r_occ["reason"])

# ------------------------------------------------- 3. real Himax frames ----
print("--- edge detection (real Himax captures, informational) ---")
capdir = os.path.expanduser("~/workspace/projects/watcher-input-assistant/"
                            "himax-captures/calib-20260924")
if os.path.isdir(capdir):
    for fn in sorted(os.listdir(capdir))[:4]:
        p = os.path.join(capdir, fn)
        try:
            c, cf, dt = find_display_rectangle(p)
            n = dt["n_corners"]
            print(f"  {fn}: corners={n} conf={cf}")
        except Exception as e:  # noqa: BLE001 -- informational only
            print(f"  {fn}: ERROR {e}")
else:
    print("  (capture dir missing, skipped)")

# ------------------------------------------------- 4. fiducial synth -------
print("--- fiducial detection (synthetic) ---")
from fiducial import find_fiducial, find_fiducials
fimg, positions = make_fiducial_frame()
fx, fy = positions[0][1]
found_xy, fconf, fdetail = find_fiducial(fimg)
okf = found_xy is not None and math.dist(found_xy, (fx, fy)) < 3.0
check("synthetic: fiducial found within 3px", okf,
      f"found={found_xy} conf={fconf} cands={len(fdetail['candidates'])}")
check("synthetic: fiducial confidence > 0.45", fconf > 0.45, f"conf={fconf}")
# no fiducial present -> None, gracefully
blank = Image.new("L", (416, 416), 18)
nf, nfc, _ = find_fiducial(blank)
check("blank frame: no false positive", nf is None, f"conf={nfc}")

# four fiducials, one per corner, notch identifies each
print("--- four-fiducial detection (synthetic) ---")
fimg4, pos4 = make_fiducial_frame(sym_px=28, n_marks=4)
hits, d4 = find_fiducials(fimg4)
by_id = {h["corner_id"]: h for h in hits if h["corner_id"]}
check("4 fiducials: all four detected", len(hits) == 4,
      f"hits={len(hits)}")
check("4 fiducials: all four identified by notch",
      set(by_id) == {"tl", "tr", "bl", "br"},
      f"ids={sorted(by_id)}")
all_close = True
for cid, (ex, ey) in pos4:
    h = by_id.get(cid)
    if h is None or math.dist(h["center"], (ex, ey)) >= 3.0:
        all_close = False
check("4 fiducials: positions within 3px", all_close)

# twin: 4 fiducial marks -> full homography, no edges needed
print("--- Kalman cursor tracking (simulated) ---")
J = np.array([[0.205862, 0.003448], [-0.007586, 0.194828]])

print("--- twin: 4-fiducial homography ---")
twinF = ScreenTwin(J=J)
rngF = np.random.default_rng(3)
true_H = np.array([[1.35, 0.12, 40.0],
                   [-0.08, 1.28, 25.0],
                   [0.0006, -0.0003, 1.0]])
screen_marks = {"tl": (0, 0), "tr": (1920, 0),
                "br": (1920, 1080), "bl": (0, 1080)}
marks = []
for cid, sxy in screen_marks.items():
    cxy = apply_homography(true_H, [sxy])[0]
    cxy = cxy + rngF.normal(0, 1.5, 2)  # detection noise
    marks.append({"corner_id": cid, "screen_xy": sxy,
                  "cam_xy": (float(cxy[0]), float(cxy[1])),
                  "confidence": 0.9})
resF = twinF.update_geometry_fiducials(marks, (1920, 1080))
check("4 fiducials: homography adopted", resF["updated"],
      resF.get("reason", ""))
errF = homography_reprojection_error(
    twinF.H_sc,
    np.array([screen_marks[k] for k in ("tl", "tr", "br", "bl")]),
    apply_homography(true_H,
                     np.array([screen_marks[k]
                               for k in ("tl", "tr", "br", "bl")])))
check("4 fiducials: homography accurate < 3px", errF < 3.0, f"err={errF:.2f}")
# a later camera bump is still caught
marks2 = []
for cid, sxy in screen_marks.items():
    cxy = apply_homography(true_H, [sxy])[0] + np.array([60.0, 40.0])
    marks2.append({"corner_id": cid, "screen_xy": sxy,
                   "cam_xy": (float(cxy[0]), float(cxy[1])),
                   "confidence": 0.9})
resF2 = twinF.update_geometry_fiducials(marks2, (1920, 1080))
check("4 fiducials: camera bump flagged as jump", resF2["jumped"])
# 2 marks -> consistency check only, no adoption
twinF2 = ScreenTwin(J=J)
resF3 = twinF2.update_geometry_fiducials(marks[:2], (1920, 1080))
check("2 fiducials: no homography without prior",
      not resF3["updated"], resF3.get("reason", "")[:50])

# ------------------------------------------------- 5. Kalman filter -------
twin = ScreenTwin(J=J)
# ground truth: a few HID moves; camera sees cursor with ~10px noise
true_hid = np.zeros(2)
moves = [(200, 0), (0, 200), (-120, 80), (300, -150), (50, 50)]
rng = np.random.default_rng(7)
raw_errs, filt_errs = [], []
# anchor: first detection
true_cam = J @ true_hid + np.array([100.0, 200.0])  # origin at (100,200)
twin.observe_cursor(tuple(true_cam + rng.normal(0, 10, 2)), confidence=0.7)
for dx, dy in moves:
    twin.command(dx, dy)
    true_hid = true_hid + np.array([dx, dy])
    true_cam = J @ true_hid + np.array([100.0, 200.0])
    noisy = true_cam + rng.normal(0, 10, 2)
    raw_errs.append(float(np.linalg.norm(noisy - true_cam)))
    twin.observe_cursor(tuple(noisy), confidence=0.7)
    est_cam = np.array(twin.hid_to_cam(twin.believed_hid))
    filt_errs.append(float(np.linalg.norm(est_cam - true_cam)))
check("Kalman: filtered RMSE < raw RMSE",
      np.mean(filt_errs) < np.mean(raw_errs),
      f"raw={np.mean(raw_errs):.2f} filt={np.mean(filt_errs):.2f}")

# 5b. systematic bias: truth moves 10% further than commanded (a hot
# Jacobian). Dead reckoning compounds it; the filter must substantially
# correct it. (A gross model error is a recalibration matter -- the
# filter's job is drift, which it demonstrably halves.)
print("--- Kalman bias correction (simulated miscalibration) ---")
twinb = ScreenTwin(J=J)
true_hid = np.zeros(2)
dead_hid = np.zeros(2)  # what pure dead-reckoning believes
origin = np.array([100.0, 200.0])
twinb.observe_cursor(tuple(origin + rng.normal(0, 10, 2)), confidence=0.7)
for _ in range(12):
    dx, dy = 150, 0
    twinb.command(dx, dy)
    dead_hid = dead_hid + np.array([dx, dy])
    true_hid = true_hid + np.array([dx, dy]) * 1.10  # 10% hot
    true_cam = J @ true_hid + origin
    twinb.observe_cursor(tuple(true_cam + rng.normal(0, 10, 2)),
                         confidence=0.7)
filt_px = float(np.linalg.norm(
    np.array(twinb.hid_to_cam(twinb.believed_hid)) - (J @ true_hid + origin)))
dead_px = float(np.linalg.norm(
    np.array(J @ dead_hid + origin) - (J @ true_hid + origin)))
check("bias: filter corrects systematic move bias",
      filt_px < dead_px * 0.65,
      f"dead-reckoning err={dead_px:.1f}px filtered err={filt_px:.1f}px")
check("bias: filter absolute error sane (<25px)",
      filt_px < 25.0, f"err={filt_px:.1f}px")

# 5c. predict_dt coasts the belief on learned velocity
print("--- predict_dt ---")
twind = ScreenTwin(J=J)
twind.observe_cursor((100.0, 200.0), confidence=0.9)
twind.x[2:] = [50.0, -20.0]  # 50 HID/s right, 20 HID/s up
p_before = twind.P[0, 0]
x0 = twind.believed_hid
twind.predict_dt(2.0)
x1 = twind.believed_hid
check("predict_dt: coasts along velocity",
      abs((x1[0] - x0[0]) - 100.0) < 1e-9 and abs((x1[1] - x0[1]) + 40.0) < 1e-9,
      f"moved {x1[0]-x0[0]:.1f},{x1[1]-x0[1]:.1f}")
check("predict_dt: grows uncertainty", twind.P[0, 0] > p_before)

# ------------------------------------------------- 6. outlier gate --------
print("--- outlier rejection ---")
twin2 = ScreenTwin(J=J)
twin2.observe_cursor((100.0, 200.0), confidence=0.9)
twin2.command(100, 0)  # predicted cam ~ (120.6, 199.2)
before = twin2.believed_hid
res = twin2.observe_cursor((180.0, 260.0), confidence=0.9)  # 60px off
after = twin2.believed_hid
check("outlier: 60px-off detection rejected", not res["accepted"],
      res["reason"])
check("outlier: belief unchanged by rejected detection",
      math.dist(before, after) < 0.5, f"moved {math.dist(before, after):.2f}")
check("outlier: counter incremented", twin2.outliers_rejected == 1)

# 6b. repeated disagreement -> LOST -> refuse moves -> reanchor recovers
print("--- lost-state / relocalization ---")
twinL = ScreenTwin(J=J)
twinL.observe_cursor((100.0, 200.0), confidence=0.9)
twinL.command(100, 0)
twinL.add_element("btn", hid_xy=(400.0, 100.0), source="camera",
                  confidence=0.9)
for _ in range(5):
    r = twinL.observe_cursor((300.0, 300.0), confidence=0.9)
    assert not r["accepted"]
check("lost: 5 consecutive outliers -> cursor_lost", twinL.cursor_lost)
try:
    twinL.move_to_element("btn")
    check("lost: move_to_element refuses while LOST", False)
except RuntimeError as e:
    check("lost: move_to_element refuses while LOST", True, str(e)[:60])
# a good detection still can't pass the gate while belief is far off...
r = twinL.observe_cursor((120.6, 199.2), confidence=0.9)
# ...so relocalization is explicit:
ra = twinL.reanchor((120.6, 199.2), confidence=0.9)
check("lost: reanchor clears LOST",
      not twinL.cursor_lost and ra["reanchored"])
dx, dy = twinL.move_to_element("btn")
check("lost: moves plannable after reanchor",
      isinstance(dx, int) and isinstance(dy, int), f"({dx},{dy})")

# ------------------------------------------- 7. sanity check / elements ---
print("--- sanity check + element map ---")
twin3 = ScreenTwin(J=J)
twin3.observe_cursor((100.0, 200.0), confidence=0.9)
twin3.command(200, 0)
pred = twin3.believed_cam
sc = twin3.sanity_check((pred[0] + 3, pred[1] - 2))
check("sanity: small residual passes", sc["ok"], sc["message"])
sc2 = twin3.sanity_check((pred[0] + 40, pred[1]))
check("sanity: 40px residual flagged", not sc2["ok"], sc2["message"])

# geometry from synthetic homography, then element via screen coords
twin3.update_geometry(
    {"tl": (50, 40), "tr": (366, 40), "br": (366, 376), "bl": (50, 376)},
    (1920, 1080), 0.9)
twin3.add_element("submit", screen_xy=(960, 540), source="screenshot",
                  confidence=0.9)
el = twin3.get_element("submit")
check("element: stored with HID address", el is not None and el["hid"] is not None)
dx, dy = twin3.move_to_element("submit")
# believed hid after command(200,0) fused with anchor... just check it returns ints
check("element: move_to_element returns int HID delta",
      isinstance(dx, int) and isinstance(dy, int), f"({dx},{dy})")
# re-observation refines
n0 = twin3.elements["submit"]["observations"]
twin3.add_element("submit", screen_xy=(962, 541), source="camera", confidence=0.6)
check("element: re-observation increments count",
      twin3.elements["submit"]["observations"] == n0 + 1)
try:
    twin3.move_to_element("nope")
    check("element: unknown name raises KeyError", False)
except KeyError:
    check("element: unknown name raises KeyError", True)

# 7b. viewport epoch: scroll/resize invalidates stored positions
print("--- viewport epoch ---")
twinV = ScreenTwin(J=J)
twinV.note_viewport(1920, 1080)
twinV.add_element("btn", hid_xy=(500.0, 500.0), source="screenshot",
                  confidence=0.9)
check("viewport: element usable in its epoch",
      twinV.get_element("btn") is not None)
vp = twinV.note_viewport(1920, 1080, scroll_y=200)  # user scrolled
check("viewport: scroll bumps epoch", vp["changed"] and vp["epoch"] == 1)
check("viewport: element stale after scroll",
      twinV.get_element("btn") is None)
twinV.add_element("btn", hid_xy=(500.0, 300.0), source="camera",
                  confidence=0.8)
elV = twinV.get_element("btn")
check("viewport: re-observed element usable again",
      elV is not None and abs(elV["hid"][1] - 300.0) < 1.0)
vp2 = twinV.note_viewport(1920, 1080, scroll_y=200)  # same viewport
check("viewport: unchanged viewport keeps epoch", not vp2["changed"])

# ------------------------------------------------- 8. geometry jump -------
print("--- camera-disturbance detection ---")
twin4 = ScreenTwin(J=J)
twin4.observe_cursor((100.0, 200.0), confidence=0.9)
r1 = twin4.update_geometry(
    {"tl": (50, 40), "tr": (366, 40), "br": (366, 376), "bl": (50, 376)},
    (1920, 1080), 0.9)
# same geometry again: refine, no jump
r2 = twin4.update_geometry(
    {"tl": (51, 41), "tr": (367, 41), "br": (367, 377), "bl": (51, 377)},
    (1920, 1080), 0.9)
check("geometry: small shift = refine, no jump", r2["updated"] and not r2["jumped"])
# big shift: camera bumped
r3 = twin4.update_geometry(
    {"tl": (90, 80), "tr": (406, 80), "br": (406, 416), "bl": (90, 416)},
    (1920, 1080), 0.9)
check("geometry: big shift flagged as jump", r3["jumped"], r3["reason"])
check("geometry: jump counter incremented", twin4.geometry_jumps == 1)

# ------------------------------------------------- 9. save / load ---------
print("--- persistence ---")
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, "twin.json")
    twin3.save(p)
    t5 = ScreenTwin.load(p)
    check("save/load: believed HID survives",
          np.allclose(t5.x, twin3.x), f"{t5.x} vs {twin3.x}")
    check("save/load: homography survives",
          np.allclose(t5.H_sc, twin3.H_sc))
    check("save/load: elements survive",
          set(t5.elements) == set(twin3.elements))
    check("save/load: anchor survives",
          np.allclose(t5.hid_origin_cam, twin3.hid_origin_cam))

print()
n_pass = sum(1 for _, ok, _ in results if ok)
print(f"{n_pass}/{len(results)} tests passed")
sys.exit(0 if n_pass == len(results) else 1)
