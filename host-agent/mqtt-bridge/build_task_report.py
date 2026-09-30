#!/usr/bin/env python3
"""Build the Claude task execution report."""
import base64
import io
from pathlib import Path
from PIL import Image

OUT = Path.home() / "workspace/visual_grounding/claude-task-report-20260924.html"

def img_b64(path, max_w=600):
    im = Image.open(path).convert("RGB")
    if im.width > max_w:
        h = int(im.height * max_w / im.width)
        im = im.resize((max_w, h), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, format="JPEG", quality=80)
    return base64.b64encode(buf.getvalue()).decode()

# Snapshots from the run
typed_b64 = img_b64("/tmp/claude-typed.jpg")
final_b64 = img_b64("/tmp/claude-final.jpg")

html = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Claude Task Report 2026-09-24</title>
<style>
body {{ font-family: system-ui, sans-serif; max-width: 900px; margin: 20px auto; padding: 0 16px; background: #f5f5f5; }}
h1 {{ color: #1a1a1a; }} h2 {{ color: #2a2a2a; border-bottom: 2px solid #ddd; padding-bottom: 6px; }}
.card {{ background: white; border-radius: 8px; padding: 16px; margin: 16px 0; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }}
.card img {{ max-width: 100%; border: 1px solid #ddd; border-radius: 4px; }}
table {{ border-collapse: collapse; width: 100%; margin: 12px 0; }}
th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
th {{ background: #f0f0f0; }}
.ok {{ color: green; font-weight: bold; }} .warn {{ color: #b8860b; font-weight: bold; }}
pre {{ background: #f0f0f0; padding: 12px; border-radius: 4px; overflow-x: auto; }}
.step-num {{ display: inline-block; background: #2563eb; color: white; width: 28px; height: 28px; border-radius: 50%; text-align: center; line-height: 28px; font-weight: bold; margin-right: 8px; }}
</style></head><body>
<h1>Claude Task Execution Report — 2026-09-24</h1>
<div class="card">
<h2>Task</h2>
<p>Type <code>hello world! hi claude from muse :)</code> in Claude's text box, then hover over the orange submit button. <strong>Do NOT click submit.</strong></p>
<p><strong>Calibration used:</strong> cursor_calibration_20260924_v2.json<br>
J = [[0.2059, 0.0034], [-0.0076, 0.1948]] px/HID | RMS 1.55 px</p>
</div>

<div class="card">
<h2><span class="step-num">1</span>Locate cursor (hunt)</h2>
<table>
<tr><th>Planned</th><td>Glide +100 HID X (~20.6 px) to create a detectable diff</td></tr>
<tr><th>Executed</th><td><code>mouse_glide dx=100 dy=0 dur=1000ms</code></td></tr>
<tr><th>Measured</th><td>Before (102,213) → After (127,212). Delta (25,-1) px vs predicted (20.6,-0.8).</td></tr>
<tr><th>Accuracy</th><td class="ok">Good — 4.4 px overshoot, cursor found</td></tr>
<tr><th>Moves</th><td>1</td></tr>
</table>
</div>

<div class="card">
<h2><span class="step-num">2</span>Move to text box</h2>
<table>
<tr><th>Planned</th><td>Text box at (123,223). Cursor at (127,212). Delta (-4,+11) px → HID (-20,+56) via J<sup>-1</sup>.</td></tr>
<tr><th>Executed</th><td><code>mouse_glide dx=-20 dy=56 dur=800ms</code></td></tr>
<tr><th>Measured</th><td>Landed at (122,224). Target (123,223). Error (-1,+1).</td></tr>
<tr><th>Accuracy</th><td class="ok">1.4 px error — excellent</td></tr>
<tr><th>Moves</th><td>1</td></tr>
</table>
</div>

<div class="card">
<h2><span class="step-num">3</span>Click to focus</h2>
<table>
<tr><th>Planned</th><td>Left-click on the text box to focus it</td></tr>
<tr><th>Executed</th><td><code>mouse_click button=left</code> → <code>ok</code> reply confirmed</td></tr>
<tr><th>Accuracy</th><td class="ok">Correlated OK</td></tr>
<tr><th>Moves</th><td>1 click</td></tr>
</table>
</div>

<div class="card">
<h2><span class="step-num">4</span>Type the message</h2>
<table>
<tr><th>Planned</th><td>Type <code>hello world! hi claude from muse :)</code></td></tr>
<tr><th>Executed</th><td><code>type text="hello world! hi claude from muse :)"</code> → <code>ok</code> reply confirmed</td></tr>
<tr><th>Measured</th><td>Snapshot shows text visible in the input box</td></tr>
<tr><th>Accuracy</th><td class="ok">Text confirmed visually</td></tr>
<tr><th>Moves</th><td>1 type command</td></tr>
</table>
<img src="data:image/jpeg;base64,{typed_b64}" alt="Text typed in Claude box">
</div>

<div class="card">
<h2><span class="step-num">5</span>Move to submit button (coarse)</h2>
<table>
<tr><th>Planned</th><td>Submit button at (236,217). Cursor at (122,224). Delta (114,-7) px → HID (554,-14) via J<sup>-1</sup>.</td></tr>
<tr><th>Executed</th><td><code>mouse_glide dx=554 dy=-14 dur=1200ms</code></td></tr>
<tr><th>Measured</th><td>Landed at (222,216). Target (236,217). Error (-14,-1).</td></tr>
<tr><th>Accuracy</th><td class="warn">14 px error — needed correction (largest error of the run)</td></tr>
<tr><th>Moves</th><td>1</td></tr>
</table>
</div>

<div class="card">
<h2><span class="step-num">6</span>Fine correction to submit button</h2>
<table>
<tr><th>Planned</th><td>Delta (14,1) px → HID (68,8) via J<sup>-1</sup>.</td></tr>
<tr><th>Executed</th><td><code>mouse_glide dx=68 dy=8 dur=600ms</code></td></tr>
<tr><th>Measured</th><td>Landed at (232,217). Target (236,217). Error (-4,0).</td></tr>
<tr><th>Accuracy</th><td class="ok">4 px error — hovering over the button</td></tr>
<tr><th>Moves</th><td>1</td></tr>
</table>
<img src="data:image/jpeg;base64,{final_b64}" alt="Cursor hovering over submit button">
</div>

<div class="card">
<h2>Summary</h2>
<table>
<tr><th>Goal step</th><th>Moves</th><th>Final accuracy</th></tr>
<tr><td>Locate cursor</td><td>1 glide</td><td class="ok">Found</td></tr>
<tr><td>Move to text box</td><td>1 glide</td><td class="ok">1.4 px</td></tr>
<tr><td>Click to focus</td><td>1 click</td><td class="ok">OK reply</td></tr>
<tr><td>Type message</td><td>1 type</td><td class="ok">Visually confirmed</td></tr>
<tr><td>Hover submit button</td><td>2 glides</td><td class="ok">4 px</td></tr>
<tr><td><strong>Total</strong></td><td><strong>4 glides + 1 click + 1 type</strong></td><td></td></tr>
</table>
<p><strong>Submit was NOT clicked.</strong> Cursor is hovering over the orange submit button awaiting your approval.</p>
<p><strong>Notes:</strong> The coarse submit move had the largest error (14 px), likely due to the longer distance (114 px) amplifying Jacobian residual. The fine correction brought it to 4 px. All other moves were within 5 px.</p>
</div>
</body></html>"""

OUT.write_text(html)
print(f"Report: {OUT}")
