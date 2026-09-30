#!/usr/bin/env python3
"""Build annotated calibration report with before/after images.

For each move shows:
- Green: where cursor was (before)
- Blue: where we wanted it to go (predicted via Jacobian)
- Red: where it actually went (after)
- Error vector and distance
"""
import base64
import io
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

CAP_DIR = Path.home() / "workspace/projects/watcher-input-assistant/himax-captures/calib-20260924"
OUT_HTML = Path.home() / "workspace/visual_grounding/calib-20260924-glide-report.html"

# Measured data (corrected 500Y)
MOVES = [
    {"label": "200X", "hid": (200, 0), "before": (89, 215), "after": (130, 214)},
    {"label": "200Y", "hid": (0, 200), "before": (87, 215), "after": (87, 250)},
    {"label": "500X", "hid": (500, 0), "before": (87, 215), "after": (190, 211)},
    {"label": "500Y", "hid": (0, 500), "before": (88, 215), "after": (90, 314)},
]

# Jacobian (pixels/HID)
J = [[0.20586207, 0.00344828], [-0.00758621, 0.19482759]]
J_INV = [[4.85445523, -0.08591956], [0.18902304, 5.12939782]]

def predict(before, hid):
    dx, dy = hid
    px = before[0] + J[0][0]*dx + J[0][1]*dy
    py = before[1] + J[1][0]*dx + J[1][1]*dy
    return (px, py)

def annotate(move):
    label = move["label"]
    before_img = Image.open(CAP_DIR / f"calib-{label}-before.jpg").convert("RGB")
    after_img = Image.open(CAP_DIR / f"calib-{label}-after.jpg").convert("RGB")
    bx, by = move["before"]
    ax, ay = move["after"]
    px, py = predict((bx, by), move["hid"])
    # Error
    ex, ey = ax - px, ay - py
    err_dist = (ex**2 + ey**2) ** 0.5
    # Annotate after image (show all three points)
    draw = ImageDraw.Draw(after_img)
    r = 8
    # Before (green)
    draw.ellipse([bx-r, by-r, bx+r, by+r], outline="green", width=3)
    draw.text((bx+r+2, by-10), "before", fill="green")
    # Predicted (blue)
    draw.ellipse([px-r, py-r, px+r, py+r], outline="blue", width=3)
    draw.text((px+r+2, py-10), "wanted", fill="blue")
    # After (red)
    draw.ellipse([ax-r, ay-r, ax+r, ay+r], outline="red", width=3)
    draw.text((ax+r+2, ay-10), "actual", fill="red")
    # Error arrow from predicted to actual
    draw.line([px, py, ax, ay], fill="orange", width=2)
    # Encode
    buf = io.BytesIO()
    # Scale up 2x for visibility
    after_img = after_img.resize((after_img.width*2, after_img.height*2), Image.NEAREST)
    # Re-draw on scaled (simpler: just encode the original annotated)
    buf = io.BytesIO()
    after_img.save(buf, format="JPEG", quality=85)
    b64 = base64.b64encode(buf.getvalue()).decode()
    return {
        "label": label,
        "hid": move["hid"],
        "before": (bx, by),
        "predicted": (round(px, 1), round(py, 1)),
        "actual": (ax, ay),
        "error": (round(ex, 1), round(ey, 1)),
        "error_dist": round(err_dist, 1),
        "img_b64": b64,
    }

def main():
    results = [annotate(m) for m in MOVES]
    html = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Cursor Calibration Report 2026-09-24 (Glide)</title>
<style>
body {{ font-family: system-ui, sans-serif; max-width: 900px; margin: 20px auto; padding: 0 16px; background: #f5f5f5; }}
h1 {{ color: #1a1a1a; }}
.card {{ background: white; border-radius: 8px; padding: 16px; margin: 16px 0; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }}
.card img {{ max-width: 100%; border: 1px solid #ddd; border-radius: 4px; }}
table {{ border-collapse: collapse; width: 100%; margin: 12px 0; }}
th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
th {{ background: #f0f0f0; }}
.legend {{ display: flex; gap: 16px; margin: 8px 0; }}
.dot {{ display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 4px; }}
pre {{ background: #f0f0f0; padding: 12px; border-radius: 4px; overflow-x: auto; }}
</style></head><body>
<h1>Cursor Calibration Report — 2026-09-24 (Mouse Glide)</h1>
<div class="card">
<h2>Jacobian (pixels / HID unit)</h2>
<pre>J = [[{J[0][0]:.6f}, {J[0][1]:.6f}],
     [{J[1][0]:.6f}, {J[1][1]:.6f}]]

J_inv (HID / pixel) = [[{J_INV[0][0]:.4f}, {J_INV[0][1]:.4f}],
                       [{J_INV[1][0]:.4f}, {J_INV[1][1]:.4f}]]

Scale X: 0.206 px/HID | Scale Y: 0.195 px/HID
Inter-axis angle: 91.1° | RMS residual: 1.55 px
N correspondences: 4</pre>
<p><strong>Method:</strong> mouse_glide commands (200 and 500 HID units, X and Y axes).
Each move: baseline snapshot → glide → snapshot → glide back → verify.
Cursor positions measured via before/after image differencing.</p>
<p><strong>Note:</strong> 500Y raw delta was sign-flipped by blob area ordering;
corrected to (+2, +99) based on position continuity (before=(88,215), after=(90,314)).</p>
</div>
<div class="card">
<div class="legend">
<span><span class="dot" style="background:green"></span>Before (where cursor was)</span>
<span><span class="dot" style="background:blue"></span>Wanted (predicted)</span>
<span><span class="dot" style="background:red"></span>Actual (where it went)</span>
</div>
</div>
"""
    for r in results:
        html += f"""
<div class="card">
<h2>{r['label']}: HID {r['hid']}</h2>
<table>
<tr><th></th><th>X</th><th>Y</th></tr>
<tr><td>Before</td><td>{r['before'][0]}</td><td>{r['before'][1]}</td></tr>
<tr><td>Predicted</td><td>{r['predicted'][0]}</td><td>{r['predicted'][1]}</td></tr>
<tr><td>Actual</td><td>{r['actual'][0]}</td><td>{r['actual'][1]}</td></tr>
<tr><td><strong>Error</strong></td><td>{r['error'][0]}</td><td>{r['error'][1]}</td></tr>
</table>
<p><strong>Error distance:</strong> {r['error_dist']} pixels</p>
<img src="data:image/jpeg;base64,{r['img_b64']}" alt="{r['label']} annotated">
</div>
"""
    html += "</body></html>"
    OUT_HTML.write_text(html)
    print(f"Report written to {OUT_HTML}")

if __name__ == "__main__":
    main()
