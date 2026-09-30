#!/usr/bin/env python3
"""
Reusable task report builder for Watcher eyes-on-screen runs.

V3 style: black background, full Jacobian derivation, per-step move math.
Produces the annotated before/after style:
  PRE:  cursor (lime) + target (cyan) + move vector (yellow)
  POST: target (cyan, same position) + cursor (red) + miss vector (orange)

Usage:
    from task_report import TaskReport

    r = TaskReport(
        title="Claude Task Execution Report",
        intro="Type hello ... Hover over submit. <strong>Do NOT click.</strong>",
    )
    r.set_jacobian(
        J=[[0.205862, 0.003448], [-0.007586, 0.194828]],
        Jinv=[[4.854455, -0.085920], [0.189023, 5.129398]],
        correspondences=[("200X", (200,0), (41,-1)), ...],
        rms_px=1.55, source="cursor_calibration_20260924_v2.json",
        note="500Y sign-flip corrected via position continuity.",
    )
    r.add_step(label="2. Move to text box",
               before_img="/tmp/a.jpg", after_img="/tmp/b.jpg",
               cursor_before=(127, 212), cursor_after=(122, 224),
               target=(123, 223),
               hid=( -20, 56), hid_label="mouse_glide dx=-20 dy=56",
               notes="Landed at top edge; aim deeper next time.")
    r.add_summary_row("Move to text box", "1.4px", "ok",
                      "Cursor at top edge of box; click was luck.")
    r.add_root_cause("Blind trust in VG point",
                     "verify_target_crop + find_button_bounds")
    r.generate_html("/path/to/report.html")
"""
import base64
import io
from pathlib import Path
from typing import List, Optional, Tuple

from PIL import Image, ImageDraw

Point = Tuple[float, float]

_CSS = """
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
.notes{background:#3a2f00;border-left:3px solid #ffc107;padding:10px 14px;margin:10px 0;font-size:14px;border-radius:0 4px 4px 0;color:#fff}
figcaption{font-size:13px;color:#aaa;margin-top:6px}
code{background:#111;padding:2px 6px;border-radius:3px}
"""

_LEGEND = """
<div class="legend">
<span><span class="dot" style="background:lime"></span>Cursor (pre-move)</span>
<span><span class="dot" style="background:cyan"></span>Target</span>
<span><span class="dot" style="background:red"></span>Cursor (post-move)</span>
<span><span class="dot" style="background:#ff0"></span>Move vector</span>
<span><span class="dot" style="background:orange"></span>Miss vector</span>
</div>
"""


def _img_b64(path: str, max_w: int = 640) -> str:
    img = Image.open(path).convert("RGB")
    if img.width > max_w:
        img = img.resize((max_w, int(img.height * max_w / img.width)),
                         Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=85)
    return base64.b64encode(buf.getvalue()).decode()


def _annotate_before(path: str, cursor: Optional[Point],
                     target: Optional[Point]) -> Image.Image:
    img = Image.open(path).convert("RGB")
    draw = ImageDraw.Draw(img)
    if cursor:
        cx, cy = cursor
        r = 10
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline="lime", width=2)
        draw.text((cx + r + 2, cy - 8), "cursor", fill="lime")
    if target:
        tx, ty = target
        r = 12
        draw.ellipse([tx - r, ty - r, tx + r, ty + r], outline="cyan", width=2)
        draw.text((tx + r + 2, ty - 8), "target", fill="cyan")
        if cursor:
            draw.line([cx, cy, tx, ty], fill="yellow", width=2)
            draw.polygon([(tx - 6, ty - 3), (tx - 6, ty + 3), (tx, ty)],
                         fill="yellow")
    return img


def _annotate_after(path: str, cursor: Optional[Point],
                    target: Optional[Point]) -> Tuple[Image.Image, Optional[float]]:
    img = Image.open(path).convert("RGB")
    draw = ImageDraw.Draw(img)
    miss = None
    if target:
        tx, ty = target
        r = 12
        draw.ellipse([tx - r, ty - r, tx + r, ty + r], outline="cyan", width=2)
        draw.text((tx + r + 2, ty - 8), "target", fill="cyan")
    if cursor:
        cx, cy = cursor
        r = 10
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline="red", width=2)
        draw.text((cx + r + 2, cy - 8), "cursor", fill="red")
        if target:
            draw.line([tx, ty, cx, cy], fill="orange", width=2)
            miss = round(((cx - tx) ** 2 + (cy - ty) ** 2) ** 0.5, 1)
    return img, miss


def _pil_b64(img: Image.Image) -> str:
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=85)
    return base64.b64encode(buf.getvalue()).decode()


class TaskReport:
    """Accumulates annotated steps and renders a self-contained HTML report."""

    def __init__(self, title: str, intro: str = ""):
        self.title = title
        self.intro = intro
        self._jac = None
        self._steps: List[dict] = []
        self._summary_rows: List[dict] = []
        self._root_causes: List[dict] = []

    def set_jacobian(self, J, Jinv, correspondences=None, rms_px=None,
                     source="", note=""):
        """
        Add the full Jacobian derivation section.
        J: 2x2 HID->pixel. Jinv: 2x2 pixel->HID.
        correspondences: list of (name, (dhid_x,dhid_y), (dpx,dpy)).
        """
        self._jac = {"J": J, "Jinv": Jinv,
                     "correspondences": correspondences or [],
                     "rms_px": rms_px, "source": source, "note": note}

    def add_step(self, label: str,
                 before_img: Optional[str] = None,
                 after_img: Optional[str] = None,
                 cursor_before: Optional[Point] = None,
                 cursor_after: Optional[Point] = None,
                 target: Optional[Point] = None,
                 hid: Optional[Tuple[int, int]] = None,
                 hid_label: str = "",
                 notes: str = "") -> None:
        """
        Add one move/action step. If hid=(dx,dy) and a Jacobian was set,
        the per-step move math table (Jinv computation, predicted landing,
        error vector) is generated automatically.
        """
        self._steps.append({
            "label": label, "before_img": before_img, "after_img": after_img,
            "cursor_before": cursor_before, "cursor_after": cursor_after,
            "target": target, "hid": hid, "hid_label": hid_label,
            "notes": notes,
        })

    def add_summary_row(self, step: str, result: str,
                        cls: str = "", note: str = "") -> None:
        """Add a row to the closing summary table. cls in {"ok","warn","bad"}."""
        self._summary_rows.append({"step": step, "result": result,
                                  "cls": cls, "note": note})

    def add_root_cause(self, failure: str, fix: str) -> None:
        """Add a row to the root-cause -> pipeline-fix table."""
        self._root_causes.append({"failure": failure, "fix": fix})

    def _render_jacobian(self) -> str:
        j = self._jac
        J, Jinv = j["J"], j["Jinv"]
        html = "<h2>Non-Orthonormal Jacobian — Full Derivation</h2>\n"
        html += '<div class="card">\n'
        if j["correspondences"]:
            html += "<h3>Calibration Correspondences</h3>\n"
            html += ("<table><tr><th>Move</th><th>&Delta;HID (commanded)</th>"
                     "<th>&Delta;Pixel (measured)</th></tr>\n")
            for name, dhid, dpx in j["correspondences"]:
                html += (f"<tr><td>{name}</td><td>{dhid}</td>"
                         f"<td>{dpx}</td></tr>\n")
            html += "</table>\n"
        if j["note"]:
            html += f"<p><strong>Note:</strong> {j['note']}</p>\n"
        html += "<h3>Jacobian J (HID &rarr; Pixel)</h3>\n"
        html += ('<div class="metric">[ &Delta;px ] &nbsp;&nbsp; '
                 f'[ {J[0][0]:.6f} &nbsp;&nbsp; {J[0][1]:.6f} ] [ &Delta;hid_x ]<br>\n'
                 '[ &Delta;py ] &nbsp;=&nbsp; '
                 f'[{J[1][0]:.6f} &nbsp;&nbsp; {J[1][1]:.6f} ] [ &Delta;hid_y ]</div>\n')
        html += "<h3>Inverse Jacobian J&#8315;&#185; (Pixel &rarr; HID)</h3>\n"
        html += ('<div class="metric">[ &Delta;hid_x ] &nbsp;&nbsp; '
                 f'[ {Jinv[0][0]:.6f} &nbsp; {Jinv[0][1]:.6f} ] [ &Delta;px ]<br>\n'
                 '[ &Delta;hid_y ] &nbsp;=&nbsp; '
                 f'[ {Jinv[1][0]:.6f} &nbsp;&nbsp; {Jinv[1][1]:.6f} ] [ &Delta;py ]</div>\n')
        # Derived metrics
        sx = (J[0][0]**2 + J[1][0]**2) ** 0.5
        sy = (J[0][1]**2 + J[1][1]**2) ** 0.5
        import math
        dot = J[0][0]*J[0][1] + J[1][0]*J[1][1]
        angle = math.degrees(math.acos(dot / (sx*sy)))
        html += (f"<p>Scale X: <strong>{sx:.3f} px/HID</strong> | "
                 f"Scale Y: <strong>{sy:.3f} px/HID</strong> | "
                 f"Inter-axis angle: <strong>{angle:.1f}&deg;</strong>")
        if j["rms_px"] is not None:
            html += f' | RMS residual: <strong>{j["rms_px"]} px</strong>'
        html += "</p>\n"
        if j["source"]:
            html += f"<p>Source: <code>{j['source']}</code></p>\n"
        html += "</div>\n"
        return html

    def _move_math_rows(self, s: dict) -> str:
        """Per-step parameter table with J math. Returns '' if not applicable."""
        if not s["hid"]:
            return ""
        J, Jinv = self._jac["J"], self._jac["Jinv"] if self._jac else (None, None)
        hdx, hdy = s["hid"]
        rows = ""
        cb, ca = s["cursor_before"], s["cursor_after"]
        if cb:
            rows += (f"<tr><td>Start (cursor)</td><td>({cb[0]}, "
                     f"{cb[1]})</td></tr>\n")
        if s["target"]:
            rows += (f"<tr><td>Target</td><td>({s['target'][0]}, "
                     f"{s['target'][1]})</td></tr>\n")
        if cb and s["target"] and Jinv:
            dpx = s["target"][0] - cb[0]
            dpy = s["target"][1] - cb[1]
            chx = Jinv[0][0]*dpx + Jinv[0][1]*dpy
            chy = Jinv[1][0]*dpx + Jinv[1][1]*dpy
            rows += f"<tr><td>&Delta;Pixel desired</td><td>({dpx}, {dpy})</td></tr>\n"
            rows += (f"<tr><td>&Delta;HID computed (J&#8315;&#185;&middot;&Delta;px)</td>"
                     f"<td>({chx:.1f}, {chy:.1f}) &rarr; rounded ({hdx}, {hdy})</td></tr>\n")
            rows += (f'<tr><td>HID computation</td><td><span style="font-family:monospace;font-size:13px">'
                     f'&Delta;hid_x = {Jinv[0][0]:.4f}&times;{dpx} + ({Jinv[0][1]:.4f})&times;{dpy} = {chx:.1f}<br>'
                     f'&Delta;hid_y = {Jinv[1][0]:.4f}&times;{dpx} + {Jinv[1][1]:.4f}&times;{dpy} = {chy:.1f}'
                     f'</span></td></tr>\n')
        rows += f"<tr><td>&Delta;HID commanded</td><td>({hdx}, {hdy})</td></tr>\n"
        if J:
            ppx = J[0][0]*hdx + J[0][1]*hdy
            ppy = J[1][0]*hdx + J[1][1]*hdy
            rows += (f"<tr><td>Predicted &Delta;pixel (J&middot;&Delta;hid)</td>"
                     f"<td>({ppx:.1f}, {ppy:.1f})</td></tr>\n")
            if cb:
                rows += (f"<tr><td>Predicted landing</td>"
                         f"<td>({cb[0]+ppx:.1f}, {cb[1]+ppy:.1f})</td></tr>\n")
        if ca:
            rows += (f"<tr><td>Actual landing</td><td>({ca[0]}, "
                     f"{ca[1]})</td></tr>\n")
        if ca and s["target"]:
            ex = ca[0] - s["target"][0]
            ey = ca[1] - s["target"][1]
            dist = round((ex**2 + ey**2) ** 0.5, 1)
            cls = "success" if dist < 6 else ("warn" if dist < 20 else "error")
            rows += f"<tr><td>Error vector</td><td>({ex}, {ey})</td></tr>\n"
            rows += (f'<tr><td>Error distance</td>'
                     f'<td class="{cls}">{dist} px</td></tr>\n')
        elif ca and cb and J:
            adx = ca[0] - cb[0]
            ady = ca[1] - cb[1]
            ppx = J[0][0]*hdx + J[0][1]*hdy
            ppy = J[1][0]*hdx + J[1][1]*hdy
            dist = round(((adx-ppx)**2 + (ady-ppy)**2) ** 0.5, 1)
            rows += f"<tr><td>Actual &Delta;pixel</td><td>({adx}, {ady})</td></tr>\n"
            rows += (f'<tr><td>Prediction error</td>'
                     f'<td class="success">{dist} px</td></tr>\n')
        return (f"<table><tr><th>Parameter</th><th>Value</th></tr>\n{rows}</table>\n")

    def _render_step(self, s: dict) -> str:
        html = f'<div class="card">\n<h3>{s["label"]}</h3>\n'
        if s["hid_label"]:
            html += f'<p><code>{s["hid_label"]}</code></p>\n'
        html += self._move_math_rows(s)
        if s["before_img"] and s["after_img"]:
            b64_before = _pil_b64(_annotate_before(
                s["before_img"], s["cursor_before"], s["target"]))
            after_img, miss = _annotate_after(
                s["after_img"], s["cursor_after"], s["target"])
            html += ('<div class="grid">\n'
                     f'<div class="card" style="margin:0">'
                     f'<img src="data:image/jpeg;base64,{b64_before}">'
                     '<figcaption>PRE &mdash; cursor (lime), target (cyan), '
                     'move vector (yellow)</figcaption></div>\n'
                     f'<div class="card" style="margin:0">'
                     f'<img src="data:image/jpeg;base64,{_pil_b64(after_img)}">'
                     '<figcaption>POST &mdash; target (cyan, same position), '
                     'cursor (red), miss (orange)</figcaption></div>\n'
                     '</div>\n')
            if miss is not None:
                cls = "ok" if miss < 6 else ("warn" if miss < 20 else "bad")
                # CSS classes are success/warn/error in v3 theme
                cls = {"ok": "success", "warn": "warn",
                       "bad": "error"}[cls]
                html += (f'<p>Miss distance: '
                         f'<span class="{cls}">{miss} px</span></p>\n')
        elif s["after_img"]:
            html += ('<div class="card" style="margin:0">'
                     f'<img src="data:image/jpeg;base64,{_img_b64(s["after_img"])}">'
                     '<figcaption>Post-action snapshot</figcaption>'
                     '</div>\n')
        if s["notes"]:
            html += f'<div class="notes">{s["notes"]}</div>\n'
        html += '</div>\n'
        return html

    def generate_html(self, output_path: str) -> str:
        """Render the report and write it to output_path. Returns the path."""
        html = (f'<!DOCTYPE html>\n<html><head><meta charset="utf-8">'
                f'<title>{self.title}</title>\n<style>{_CSS}</style>'
                f'</head><body>\n<h1>{self.title}</h1>\n'
                f'<div class="card">\n<p>{self.intro}</p>\n'
                + _LEGEND + '</div>\n')
        if self._jac:
            html += self._render_jacobian()
        if self._steps:
            html += "<h2>Task Execution &mdash; Per-Step Analysis</h2>\n"
            for s in self._steps:
                html += self._render_step(s)
        if self._summary_rows:
            html += '<h2>Summary</h2>\n<div class="card">\n<table>\n'
            html += '<tr><th>Step</th><th>Result</th><th>Note</th></tr>\n'
            for r in self._summary_rows:
                cls = {"ok": "success", "bad": "error"}.get(r["cls"], r["cls"])
                html += (f'<tr><td>{r["step"]}</td>'
                         f'<td class="{cls}">{r["result"]}</td>'
                         f'<td>{r["note"]}</td></tr>\n')
            html += '</table>\n</div>\n'
        if self._root_causes:
            html += '<h2>Root Causes &rarr; Pipeline Fixes</h2>\n<div class="card">\n<table>\n'
            html += '<tr><th>#</th><th>Failure</th><th>Fix</th></tr>\n'
            for i, rc in enumerate(self._root_causes, 1):
                html += (f'<tr><td>{i}</td><td>{rc["failure"]}</td>'
                         f'<td><code>{rc["fix"]}</code></td></tr>\n')
            html += '</table>\n</div>\n'
        html += '</body></html>'
        Path(output_path).write_text(html)
        return output_path
