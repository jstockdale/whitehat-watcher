#!/usr/bin/env python3
"""Rebuild Claude task report with proper annotations per John's spec.

For each move:
- Pre-move image: cursor circled (from diff blobs), target circled, move vector
- Post-move image: original target circled (same position), cursor circled, miss distance
"""
import base64
import io
from pathlib import Path
from PIL import Image, ImageDraw

OUT = Path.home() / "workspace/visual_grounding/claude-task-report-20260924-v2.html"
CAP = Path("/tmp")

# Each step: (label, before_img, after_img, cursor_before, cursor_after, target, hid_cmd)
# Coordinates in 416x416 original image space
STEPS = [
    {
        "label": "1. Hunt — locate cursor",
        "before": "hunt2-before.jpg",
        "after": "hunt2-after.jpg",
        "cursor_before": (102, 213),
        "cursor_after": (127, 212),
        "target": None,  # hunt has no target
        "hid": (100, 0),
        "hid_label": "mouse_glide dx=100 dy=0",
        "notes": "No target — purpose was to create a detectable diff. Delta (25,-1)px vs predicted (20.6,-0.8).",
    },
    {
        "label": "2. Move to text box",
        "before": "hunt2-after.jpg",
        "after": "claude-verify.jpg",
        "cursor_before": (127, 212),
        "cursor_after": (122, 224),
        "target": (123, 223),
        "hid": (-20, 56),
        "hid_label": "mouse_glide dx=-20 dy=56",
        "notes": "Target was top-center of text box. Cursor landed at top edge — too high. Should have aimed deeper into the box. Got lucky on the click.",
    },
    {
        "label": "3. Click to focus",
        "before": None, "after": None,
        "cursor_before": None, "cursor_after": None, "target": None,
        "hid": None, "hid_label": "mouse_click button=left",
        "notes": "Correlated OK reply. No image — click does not move cursor.",
    },
    {
        "label": "4. Type message",
        "before": None, "after": "claude-typed.jpg",
        "cursor_before": None, "cursor_after": None, "target": None,
        "hid": None, "hid_label": 'type "hello world! hi claude from muse :)"',
        "notes": "Correlated OK reply. Text visually confirmed in input box.",
    },
    {
        "label": "5. Move to submit (coarse) — MISSED",
        "before": "claude-typed.jpg",
        "after": "claude-hover.jpg",
        "cursor_before": (122, 224),
        "cursor_after": (230, 205),  # corrected - John spotted my circle was 5px too high
        "target": (242, 250),  # corrected — actual button position
        "hid": (554, -14),
        "hid_label": "mouse_glide dx=554 dy=-14",
        "notes": "TARGET WAS WRONG. Visual grounding said (236,217) but actual button is at (242,250) — 33px off in Y. Cursor landed at (230,200), 52px from true target. The '4px error' I reported was based on the wrong target.",
    },
    {
        "label": "6. Correction to button",
        "before": "claude-hover.jpg",
        "after": "claude-button-fixed.jpg",
        "cursor_before": (230, 200),
        "cursor_after": (247, 266),  # from diff blob
        "target": (242, 250),
        "hid": (54, 259),
        "hid_label": "mouse_glide dx=54 dy=259",
        "notes": "Moved toward true button position. Diff shows blob at (247,266), ~17px from button center (242,250). Cursor may be on/near button but localization is uncertain at this resolution.",
    },
]

def annotate_step(step):
    """Create annotated before/after images for a step."""
    result = {"label": step["label"], "hid_label": step["hid_label"], "notes": step["notes"]}
    if step["before"] and step["after"]:
        # Before: cursor (green), target (blue), move vector (arrow)
        before = Image.open(CAP / step["before"]).convert("RGB")
        draw = ImageDraw.Draw(before)
        if step["cursor_before"]:
            cx, cy = step["cursor_before"]
            r = 10
            draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline="lime", width=2)
            draw.text((cx+r+2, cy-8), "cursor", fill="lime")
        if step["target"]:
            tx, ty = step["target"]
            r = 12
            draw.ellipse([tx-r, ty-r, tx+r, ty+r], outline="cyan", width=2)
            draw.text((tx+r+2, ty-8), "target", fill="cyan")
            # Move vector from cursor to target
            if step["cursor_before"]:
                draw.line([cx, cy, tx, ty], fill="yellow", width=2)
                # Arrowhead
                draw.polygon([(tx-6, ty-3), (tx-6, ty+3), (tx, ty)], fill="yellow")
        buf = io.BytesIO()
        before.save(buf, format="JPEG", quality=85)
        result["before_b64"] = base64.b64encode(buf.getvalue()).decode()
        # After: target (same position, cyan), cursor (red), miss line
        after = Image.open(CAP / step["after"]).convert("RGB")
        draw = ImageDraw.Draw(after)
        if step["target"]:
            tx, ty = step["target"]
            r = 12
            draw.ellipse([tx-r, ty-r, tx+r, ty+r], outline="cyan", width=2)
            draw.text((tx+r+2, ty-8), "target", fill="cyan")
        if step["cursor_after"]:
            cx, cy = step["cursor_after"]
            r = 10
            draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline="red", width=2)
            draw.text((cx+r+2, cy-8), "cursor", fill="red")
            # Miss vector from target to cursor
            if step["target"]:
                draw.line([tx, ty, cx, cy], fill="orange", width=2)
                miss = ((cx-tx)**2 + (cy-ty)**2)**0.5
                result["miss_px"] = round(miss, 1)
        buf = io.BytesIO()
        after.save(buf, format="JPEG", quality=85)
        result["after_b64"] = base64.b64encode(buf.getvalue()).decode()
    elif step["after"] and not step["before"]:
        # Type step: just show the after image
        im = Image.open(CAP / step["after"]).convert("RGB")
        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=85)
        result["after_b64"] = base64.b64encode(buf.getvalue()).decode()
    return result

def main():
    annotated = [annotate_step(s) for s in STEPS]
    html = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Claude Task Report v2 — 2026-09-24</title>
<style>
body { font-family: system-ui, sans-serif; max-width: 950px; margin: 20px auto; padding: 0 16px; background: #f5f5f5; }
h1 { color: #1a1a1a; } h2 { color: #2a2a2a; border-bottom: 2px solid #ddd; padding-bottom: 6px; }
.card { background: white; border-radius: 8px; padding: 16px; margin: 16px 0; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }
.imgs { display: flex; gap: 12px; flex-wrap: wrap; }
.imgs figure { margin: 0; flex: 1; min-width: 280px; }
.imgs img { max-width: 100%; border: 1px solid #ddd; border-radius: 4px; }
.imgs figcaption { font-size: 13px; color: #555; margin-top: 4px; }
.legend { display: flex; gap: 14px; margin: 8px 0; font-size: 14px; flex-wrap: wrap; }
.dot { display: inline-block; width: 11px; height: 11px; border-radius: 50%; margin-right: 4px; vertical-align: middle; }
table { border-collapse: collapse; width: 100%; margin: 12px 0; font-size: 14px; }
th, td { border: 1px solid #ddd; padding: 7px; text-align: left; }
th { background: #f0f0f0; }
.ok { color: green; font-weight: bold; } .bad { color: #c0392b; font-weight: bold; } .warn { color: #b8860b; font-weight: bold; }
.notes { background: #fff8e1; border-left: 3px solid #ffc107; padding: 8px 12px; margin: 8px 0; font-size: 14px; }
pre { background: #f0f0f0; padding: 10px; border-radius: 4px; overflow-x: auto; font-size: 13px; }
</style></head><body>
<h1>Claude Task Execution Report v2 — 2026-09-24</h1>
<div class="card">
<h2>Task</h2>
<p>Type <code>hello world! hi claude from muse :)</code> in Claude's text box, then hover over the orange submit button. <strong>Do NOT click submit.</strong></p>
<p><strong>Calibration:</strong> cursor_calibration_20260924_v2.json — J=[[0.2059,0.0034],[-0.0076,0.1948]] px/HID, RMS 1.55px</p>
<div class="legend">
<span><span class="dot" style="background:lime"></span>Cursor (pre-move)</span>
<span><span class="dot" style="background:cyan"></span>Target</span>
<span><span class="dot" style="background:red"></span>Cursor (post-move)</span>
<span><span class="dot" style="background:#ff0"></span>Move vector</span>
<span><span class="dot" style="background:orange"></span>Miss vector</span>
</div>
</div>
"""
    for a in annotated:
        html += f'<div class="card">\n<h2>{a["label"]}</h2>\n'
        html += f'<p><code>{a["hid_label"]}</code></p>\n'
        if "before_b64" in a:
            html += '<div class="imgs">\n'
            html += f'<figure><img src="data:image/jpeg;base64,{a["before_b64"]}"><figcaption>PRE — cursor (lime), target (cyan), move vector (yellow)</figcaption></figure>\n'
            html += f'<figure><img src="data:image/jpeg;base64,{a["after_b64"]}"><figcaption>POST — target (cyan, same position), cursor (red), miss (orange)</figcaption></figure>\n'
            html += '</div>\n'
            if "miss_px" in a:
                cls = "ok" if a["miss_px"] < 6 else ("warn" if a["miss_px"] < 20 else "bad")
                html += f'<p>Miss distance: <span class="{cls}">{a["miss_px"]} px</span></p>\n'
        elif "after_b64" in a:
            html += f'<div class="imgs"><figure><img src="data:image/jpeg;base64,{a["after_b64"]}"><figcaption>Post-action snapshot</figcaption></figure></div>\n'
        html += f'<div class="notes">{a["notes"]}</div>\n'
        html += '</div>\n'
    html += """<div class="card">
<h2>Honest summary</h2>
<table>
<tr><th>Step</th><th>What I claimed (v1)</th><th>What actually happened</th></tr>
<tr><td>Text box targeting</td><td class="ok">1.4px — "excellent"</td><td class="warn">Cursor at top edge of box. Click worked but it was luck — should have aimed 10-15px deeper.</td></tr>
<tr><td>Submit button targeting</td><td class="ok">4px — "hovering over button"</td><td class="bad">Wrong target entirely. Visual grounding gave (236,217); true button is at (242,250). Cursor was 52px away at top of text frame.</td></tr>
<tr><td>Button correction</td><td>—</td><td class="warn">Cursor now near (247,266), ~17px from button center. Localization uncertain at 416px resolution.</td></tr>
</table>
<p><strong>Lessons:</strong> (1) Don't trust a single visual-grounding point for small UI targets — verify against the image. (2) Aim for the <em>center</em> of click targets, not the edge. (3) At 416x416, diff-blob cursor localization has ±10px uncertainty — report it honestly. (4) The v1 report overstated accuracy by using the wrong target as ground truth.</p>
<p><strong>Submit was NOT clicked.</strong></p>
</div>
</body></html>"""
    OUT.write_text(html)
    print(f"Report: {OUT}")

if __name__ == "__main__":
    main()
