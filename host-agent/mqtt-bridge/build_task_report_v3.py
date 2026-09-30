#!/usr/bin/env python3
"""Claude task report v3 — black background, full Jacobian derivation, rich per-step data."""
import base64
from pathlib import Path

OUT = Path.home() / "workspace/visual_grounding/claude-task-report-20260924-v3.html"
IMG_DIR = Path("/tmp")

def img_b64(name):
    return base64.b64encode(open(IMG_DIR / name, "rb").read()).decode()

# Jacobian from cursor_calibration_20260924_v2.json
J = [[0.205862, 0.003448], [-0.007586, 0.194828]]
JINV = [[4.854455, -0.085920], [0.189023, 5.129398]]

# Calibration correspondences (from run_calibration.py / MEMORY.md)
CALIB = [
    ("200X", (200, 0), (41, -1)),
    ("200Y", (0, 200), (0, 35)),
    ("500X", (500, 0), (103, -4)),
    ("500Y", (0, 500), (2, 99)),
]

# Task steps: (label, hid_dx, hid_dy, cursor_before, target, cursor_after_true, notes, img_before_idx, img_after_idx)
# Images: 0=hunt-before, 1=hunt-after, 2=textbox-before, 3=textbox-after, 4=type-after,
#         5=submit-before, 6=submit-after, 7=correction-before, 8=correction-after
STEPS = [
    {
        "n": 1, "title": "Hunt — locate cursor",
        "cmd": "mouse_glide dx=100 dy=0 dur=1000ms",
        "before": (102, 213), "target": None, "after": (127, 212),
        "hid": (100, 0),
        "img_b": 0, "img_a": 1,
        "notes": "No target — purpose was to create a detectable diff. Measured (25,-1)px vs predicted (20.6,-0.8)px.",
    },
    {
        "n": 2, "title": "Move to text box",
        "cmd": "mouse_glide dx=-20 dy=56 dur=1000ms",
        "before": (127, 212), "target": (123, 223), "after": (122, 224),
        "hid": (-20, 56),
        "img_b": 2, "img_a": 3,
        "notes": "Target was top-center of text box — TOO HIGH. Should have aimed 20px deeper at ~(123,243), the vertical center of the clickable region. Click landed on the top edge; it worked but was luck.",
    },
    {
        "n": 3, "title": "Click to focus",
        "cmd": "mouse_click button=left",
        "before": None, "target": None, "after": None,
        "hid": None,
        "img_b": None, "img_a": None,
        "notes": "Correlated OK reply. No image — click does not move cursor.",
    },
    {
        "n": 4, "title": "Type message",
        "cmd": 'type "hello world! hi claude from muse :)"',
        "before": None, "target": None, "after": None,
        "hid": None,
        "img_b": None, "img_a": 4,
        "notes": "Correlated OK reply. Text visually confirmed in input box.",
    },
    {
        "n": 5, "title": "Move to submit (coarse) — MISSED BADLY",
        "cmd": "mouse_glide dx=554 dy=-14 dur=1000ms",
        "before": (122, 224), "target": (242, 250), "after": (218, 215),
        "hid": (554, -14),
        "img_b": 5, "img_a": 6,
        "notes": "CATASTROPHIC TARGET ERROR. Visual grounding returned (236,217) — I trusted it blindly. True button center is (242,250), 33px off in Y. Cursor landed at (218,215): 42px from the true target, sitting on the top edge of the text frame. The v1 '4px error' claim was measured against the wrong target. ADDITIONALLY: I circled (230,205) in v2 — the real cursor (white arrow) is at (218,215), 7 o'clock outside my circle. Two independent localization failures in one step.",
    },
    {
        "n": 6, "title": "Correction to button",
        "cmd": "mouse_glide dx=54 dy=259 dur=1000ms",
        "before": (230, 205), "target": (242, 250), "after": (247, 266),
        "hid": (54, 259),
        "img_b": 7, "img_a": 8,
        "notes": "Moved toward true button position (242,250). Diff blob at (247,266): ~17px from button center. Cursor may be on/near button but localization is ±10px at 416px resolution. Submit NOT clicked.",
    },
]

def hid_for(dpx, dpy):
    hx = JINV[0][0]*dpx + JINV[0][1]*dpy
    hy = JINV[1][0]*dpx + JINV[1][1]*dpy
    return hx, hy

def pred_for(hdx, hdy):
    px = J[0][0]*hdx + J[0][1]*hdy
    py = J[1][0]*hdx + J[1][1]*hdy
    return px, py

html = """<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Claude Task Report v3 — 2026-09-24</title>
<style>
body{margin:20px;background:#1a1a1a;color:#fff;font-family:sans-serif;max-width:1200px;margin:0 auto;padding:20px}
h1{color:#fff} h2{color:#4fc3f7} h3{color:#81c784}
.grid{display:flex;gap:20px;flex-wrap:wrap;margin:20px 0}
.card{background:#2a2a2a;padding:15px;border-radius:8px;flex:1;min-width:300px;margin:16px 0}
.card img{width:100%;border:1px solid #555;border-radius:4px}
.metric{background:#111;padding:15px;margin:8px 0;border-radius:4px;font-family:monospace;font-size:14px;line-height:1.6;overflow-x:auto}
.success{color:#81c784;font-weight:bold} .error{color:#ff8a65;font-weight:bold} .warn{color:#ffb74d;font-weight:bold}
pre{background:#111;padding:15px;border-radius:4px;overflow-x:auto}
table{border-collapse:collapse;width:100%;margin:10px 0}
th,td{border:1px solid #555;padding:8px;text-align:left}
th{background:#333}
.legend{display:flex;gap:16px;margin:8px 0;flex-wrap:wrap}
.dot{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:4px;vertical-align:middle}
.notes{background:#3a2f00;border-left:3px solid #ffc107;padding:10px 14px;margin:10px 0;font-size:14px;border-radius:0 4px 4px 0}
figcaption{font-size:13px;color:#aaa;margin-top:6px}
code{background:#111;padding:2px 6px;border-radius:3px}
</style></head><body>
<h1>Claude Task Execution Report v3 — 2026-09-24</h1>
<p>Type <code>hello world! hi claude from muse :)</code> in Claude's text box, then hover over the orange submit button. <strong>Submit NOT clicked.</strong></p>
<div class="legend">
<span><span class="dot" style="background:lime"></span>Cursor (pre-move)</span>
<span><span class="dot" style="background:cyan"></span>Target</span>
<span><span class="dot" style="background:red"></span>Cursor (post-move)</span>
<span><span class="dot" style="background:#ff0"></span>Move vector</span>
<span><span class="dot" style="background:orange"></span>Miss vector</span>
</div>
"""

# --- Jacobian derivation ---
html += """
<h2>Non-Orthonormal Jacobian — Full Derivation</h2>
<div class="card">
<h3>Calibration Correspondences (mouse_glide, 2026-09-24)</h3>
<table><tr><th>Move</th><th>ΔHID (commanded)</th><th>ΔPixel (measured)</th><th>Scale</th></tr>
"""
for name, dhid, dpx in CALIB:
    sx = dpx[0]/dhid[0] if dhid[0] else dpx[0]/dhid[1]
    sy = dpx[1]/dhid[1] if dhid[1] else dpx[1]/dhid[0]
    html += f"<tr><td>{name}</td><td>{dhid}</td><td>{dpx}</td><td>{abs(sx):.3f} / {abs(sy):.3f} px/HID</td></tr>\n"
html += """</table>
<p><strong>Note:</strong> 500Y raw delta was sign-flipped by blob area ordering; corrected to (+2,+99) via position continuity (before=(88,215), after=(90,314)). This exact bug class recurred in the task — see Step 5.</p>
<h3>Least-Squares Fit (4 correspondences, 2 unknowns per row)</h3>
<div class="metric">We solve for J in &nbsp; Δpixel = J · Δhid &nbsp; (2×2, non-orthonormal: axes need not be perpendicular or equally scaled).<br><br>
Each correspondence gives 2 equations:<br>
&nbsp;&nbsp;Δpx<sub>i</sub> = J<sub>00</sub>·Δhid_x<sub>i</sub> + J<sub>01</sub>·Δhid_y<sub>i</sub><br>
&nbsp;&nbsp;Δpy<sub>i</sub> = J<sub>10</sub>·Δhid_x<sub>i</sub> + J<sub>11</sub>·Δhid_y<sub>i</sub><br><br>
8 equations, 4 unknowns → overdetermined least-squares via normal equations (A<sup>T</sup>A)<sup>-1</sup>A<sup>T</sup>b.</div>
<h3>Jacobian J (HID → Pixel)</h3>
<div class="metric">[ Δpx ] &nbsp;&nbsp; [ 0.205862 &nbsp;&nbsp; 0.003448 ] [ Δhid_x ]<br>
[ Δpy ] &nbsp;=&nbsp; [-0.007586 &nbsp;&nbsp; 0.194828 ] [ Δhid_y ]</div>
<p>Off-diagonal terms (0.0034, -0.0076) = perspective shear. A pure X move drags Y by -7.6px per 1000 HID. Inter-axis angle: <strong>91.1°</strong> (not 90°).</p>
<h3>Inverse Jacobian J⁻¹ (Pixel → HID)</h3>
<div class="metric">[ Δhid_x ] &nbsp;&nbsp; [ 4.854455 &nbsp; -0.085920 ] [ Δpx ]<br>
[ Δhid_y ] &nbsp;=&nbsp; [ 0.189023 &nbsp;&nbsp; 5.129398 ] [ Δpy ]</div>
<p>Scale X: <strong>0.206 px/HID</strong> | Scale Y: <strong>0.195 px/HID</strong> | RMS residual: <strong>1.55 px</strong> | N=4</p>
<p>Source: <code>cursor_calibration_20260924_v2.json</code></p>
</div>
"""

# --- Per-step ---
html += "<h2>Task Execution — Per-Step Analysis</h2>\n"
for s in STEPS:
    html += f'<div class="card">\n<h3>Step {s["n"]}: {s["title"]}</h3>\n'
    html += f'<p><code>{s["cmd"]}</code></p>\n'
    if s["hid"]:
        hdx, hdy = s["hid"]
        ppx, ppy = pred_for(hdx, hdy)
        html += "<table><tr><th>Parameter</th><th>Value</th></tr>\n"
        if s["before"]:
            html += f"<tr><td>Start (cursor)</td><td>({s['before'][0]}, {s['before'][1]})</td></tr>\n"
        if s["target"]:
            html += f"<tr><td>Target</td><td>({s['target'][0]}, {s['target'][1]})</td></tr>\n"
            dpx = s["target"][0] - s["before"][0]
            dpy = s["target"][1] - s["before"][1]
            chx, chy = hid_for(dpx, dpy)
            html += f"<tr><td>ΔPixel desired</td><td>({dpx}, {dpy})</td></tr>\n"
            html += f"<tr><td>ΔHID computed (J⁻¹·Δpx)</td><td>({chx:.1f}, {chy:.1f}) → rounded ({hdx}, {hdy})</td></tr>\n"
            html += f"""<tr><td>HID computation</td><td><span style="font-family:monospace;font-size:13px">Δhid_x = 4.8545×{dpx} + (-0.0859)×{dpy} = {chx:.1f}<br>Δhid_y = 0.1890×{dpx} + 5.1294×{dpy} = {chy:.1f}</span></td></tr>\n"""
        html += f"<tr><td>ΔHID commanded</td><td>({hdx}, {hdy})</td></tr>\n"
        html += f"<tr><td>Predicted Δpixel (J·Δhid)</td><td>({ppx:.1f}, {ppy:.1f})</td></tr>\n"
        if s["before"] and s["target"]:
            pred_land = (s["before"][0]+ppx, s["before"][1]+ppy)
            html += f"<tr><td>Predicted landing</td><td>({pred_land[0]:.1f}, {pred_land[1]:.1f})</td></tr>\n"
        if s["after"]:
            html += f"<tr><td>Actual landing</td><td>({s['after'][0]}, {s['after'][1]})</td></tr>\n"
        if s["after"] and s["target"]:
            ex = s["after"][0] - s["target"][0]
            ey = s["after"][1] - s["target"][1]
            dist = (ex**2 + ey**2)**0.5
            cls = "success" if dist < 6 else ("warn" if dist < 20 else "error")
            html += f'<tr><td>Error vector</td><td>({ex}, {ey})</td></tr>\n'
            html += f'<tr><td>Error distance</td><td class="{cls}">{dist:.1f} px</td></tr>\n'
        elif s["after"] and s["before"]:
            # hunt step: compare actual delta to predicted
            adx = s["after"][0] - s["before"][0]
            ady = s["after"][1] - s["before"][1]
            html += f"<tr><td>Actual Δpixel</td><td>({adx}, {ady})</td></tr>\n"
            ex, ey = adx - ppx, ady - ppy
            dist = (ex**2 + ey**2)**0.5
            html += f'<tr><td>Prediction error</td><td class="success">{dist:.1f} px</td></tr>\n'
        html += "</table>\n"
    if s["img_b"] is not None and s["img_a"] is not None:
        html += '<div class="grid">\n'
        html += f'<div class="card" style="margin:0"><img src="data:image/jpeg;base64,{img_b64(f"report-img-{s["img_b"]}.jpg")}"><figcaption>PRE — cursor (lime), target (cyan), move vector (yellow)</figcaption></div>\n'
        html += f'<div class="card" style="margin:0"><img src="data:image/jpeg;base64,{img_b64(f"report-img-{s["img_a"]}.jpg")}"><figcaption>POST — target (cyan, same position), cursor (red), miss (orange)</figcaption></div>\n'
        html += "</div>\n"
    elif s["img_a"] is not None:
        html += f'<div class="card" style="margin:0"><img src="data:image/jpeg;base64,{img_b64(f"report-img-{s["img_a"]}.jpg")}"><figcaption>Post-action snapshot</figcaption></div>\n'
    html += f'<div class="notes">{s["notes"]}</div>\n'
    html += "</div>\n"

# --- Honest summary ---
html += """
<h2>Honest Summary</h2>
<div class="card">
<table>
<tr><th>Step</th><th>What I claimed (v1)</th><th>What actually happened</th></tr>
<tr><td>2. Text box</td><td class="success">1.4px — "excellent"</td><td class="warn">Aimed at top edge (123,223). Should have aimed 20px deeper at (123,243). Click worked by luck.</td></tr>
<tr><td>5. Submit coarse</td><td class="success">4px — "on the button"</td><td class="error">Two failures: (a) wrong target — VG said (236,217), true button (242,250), 33px off; (b) wrong cursor circle — drew (230,205), true cursor (218,215). Real miss: 42px. Cursor sat on text-frame top edge.</td></tr>
<tr><td>6. Correction</td><td>—</td><td class="warn">Cursor ~(247,266), ~17px from button center. ±10px localization uncertainty at 416px.</td></tr>
</table>
<h3>Root Causes</h3>
<table>
<tr><th>#</th><th>Failure</th><th>Fix (pipeline agent built)</th></tr>
<tr><td>1</td><td>Blind trust in single visual-grounding point</td><td><code>verify_target_crop</code> + <code>find_button_bounds</code> — zoom the VG point, confirm it's the target, aim for region center</td></tr>
<tr><td>2</td><td>Aimed at edges instead of clickable-region centers</td><td>Button/region targeting: find bounds, aim for center, never an unverified point</td></tr>
<tr><td>3</td><td>False precision (±10px reported as 1.4px)</td><td><code>localize_cursor</code> returns confidence 0–1; re-hunt below 0.5; report uncertainty honestly</td></tr>
<tr><td>4</td><td>One-shot moves, no correction loop</td><td><code>move_to_target</code> — iterate until within tolerance or max iters</td></tr>
<tr><td>5</td><td>Blob area-ordering sign flip (calib 500Y AND task step 5)</td><td><code>order_cursor_blobs</code> — position continuity / expected-direction hints</td></tr>
<tr><td>6</td><td>Hand-built reports, inconsistent format</td><td><code>task_report.py</code> — reusable TaskReport API with this annotation style</td></tr>
<tr><td>7</td><td>No pre-move sanity check</td><td><code>check_move_sane</code> — target on-image? HID reasonable? predicted landing on-image?</td></tr>
</table>
<p>Pipeline code: <code>~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/</code> — <code>cursor_vision.py</code>, <code>cursor_moves.py</code>, <code>task_report.py</code>, <code>cursor_calibration.py</code> (extended).</p>
<p><strong>Submit was NOT clicked. Total: 5 glides + 1 click + 1 type.</strong></p>
</div>
</body></html>
"""
OUT.write_text(html)
print(f"Report: {OUT} ({OUT.stat().st_size} bytes)")
