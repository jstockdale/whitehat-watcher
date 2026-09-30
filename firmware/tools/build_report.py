#!/usr/bin/env python3
"""
Firmware build report generator — v3 dark style.

Produces a polished HTML build report for Watcher firmware releases,
modeled on host-agent/mqtt-bridge/task_report.py (v3 dark style:
black background, cards, metric blocks, tables).

Usage:
    from build_report import BuildReport

    r = BuildReport(version="0.4.6", build_date="2026-09-27", ...)
    r.add_stage("Clean build", "PASS", "6m 12s", "0 errors, 0 warnings")
    r.add_change("WDT fix", "before...", "after...")
    r.set_binary(size=7762246, sha256="...", magics={...}, pt_identical=True)
    r.add_known_issue("...")
    r.generate_html("firmware/V0.4.6_BUILD_REPORT.html")
"""

from datetime import datetime
from typing import Dict, List, Optional

_CSS = """
body{margin:20px;background:#0a0a0a;color:#e0e0e0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;max-width:1100px;margin:0 auto;padding:24px;line-height:1.6}
h1{color:#fff;font-size:28px;margin-bottom:4px}
h2{color:#4fc3f7;font-size:20px;margin-top:32px;border-bottom:1px solid #333;padding-bottom:8px}
h3{color:#81c784;font-size:16px;margin-top:20px}
.subtitle{color:#888;font-size:14px;margin-bottom:24px}
.grid{display:flex;gap:16px;flex-wrap:wrap;margin:16px 0}
.card{background:#1a1a1a;padding:16px;border-radius:8px;flex:1;min-width:280px;margin:8px 0;border:1px solid #2a2a2a}
.metric{background:#111;padding:14px;margin:8px 0;border-radius:4px;font-family:"SF Mono",Menlo,monospace;font-size:13px;line-height:1.7;overflow-x:auto;border:1px solid #222}
.success{color:#81c784;font-weight:bold}
.error{color:#ff8a65;font-weight:bold}
.warn{color:#ffb74d;font-weight:bold}
.info{color:#4fc3f7;font-weight:bold}
table{border-collapse:collapse;width:100%;margin:12px 0;font-size:14px}
th,td{border:1px solid #333;padding:10px 12px;text-align:left}
th{background:#1f1f1f;color:#4fc3f7;font-weight:600}
tr:nth-child(even){background:#141414}
.notes{background:#2a2300;border-left:3px solid #ffc107;padding:12px 16px;margin:12px 0;font-size:14px;border-radius:0 4px 4px 0}
code{background:#222;padding:2px 6px;border-radius:3px;font-family:"SF Mono",Menlo,monospace;font-size:13px}
.badge{display:inline-block;padding:4px 12px;border-radius:12px;font-size:12px;font-weight:bold;margin-right:8px}
.badge-pass{background:#1b3a1b;color:#81c784;border:1px solid #2d5a2d}
.badge-fail{background:#3a1b1b;color:#ff8a65;border:1px solid #5a2d2d}
.badge-info{background:#1b2a3a;color:#4fc3f7;border:1px solid #2d4a5a}
ul{margin:8px 0;padding-left:24px}
li{margin:6px 0}
.footer{margin-top:40px;padding-top:16px;border-top:1px solid #333;color:#666;font-size:12px;text-align:center}
"""


class BuildReport:
    """Firmware build report builder (v3 dark style)."""

    def __init__(self, version: str, build_date: str,
                 git_commit: Optional[str] = None,
                 git_dirty: bool = False,
                 title: Optional[str] = None):
        self.version = version
        self.build_date = build_date
        self.git_commit = git_commit
        self.git_dirty = git_dirty
        self.title = title or f"Watcher Firmware v{version} — Build Report"
        self.stages: List[Dict] = []
        self.changes: List[Dict] = []
        self.binary: Dict = {}
        self.issues: List[str] = []
        self.next_steps: List[str] = []

    def add_stage(self, name: str, status: str, duration: str,
                  output: str, notes: str = ""):
        """Add a pipeline stage. status: PASS/FAIL/SKIP."""
        self.stages.append({
            "name": name, "status": status.upper(),
            "duration": duration, "output": output, "notes": notes,
        })

    def add_change(self, title: str, before: str, after: str,
                   impact: str = ""):
        """Add a change with before/after."""
        self.changes.append({
            "title": title, "before": before,
            "after": after, "impact": impact,
        })

    def set_binary(self, size: int, sha256: str,
                   magics: Dict[str, str],
                   pt_identical: bool,
                   pt_version: str = ""):
        """Set binary verification data."""
        self.binary = {
            "size": size, "sha256": sha256,
            "magics": magics, "pt_identical": pt_identical,
            "pt_version": pt_version,
        }

    def add_known_issue(self, issue: str):
        self.issues.append(issue)

    def add_next_step(self, step: str):
        self.next_steps.append(step)

    def _stage_badge(self, status: str) -> str:
        cls = "badge-pass" if status == "PASS" else \
              "badge-fail" if status == "FAIL" else "badge-info"
        return f'<span class="badge {cls}">{status}</span>'

    def generate_html(self, path: str):
        """Write the HTML report to path."""
        html = []
        html.append("<!DOCTYPE html><html><head><meta charset='utf-8'>")
        html.append(f"<title>{self.title}</title>")
        html.append(f"<style>{_CSS}</style></head><body>")

        # Header
        html.append(f"<h1>{self.title}</h1>")
        html.append(f"<div class='subtitle'>Build date: {self.build_date}")
        if self.git_commit:
            dirty = " (dirty)" if self.git_dirty else ""
            html.append(f" &bull; Git: <code>{self.git_commit}</code>{dirty}")
        if self.binary.get("sha256"):
            html.append(f"<br>SHA-256: <code>{self.binary['sha256']}</code>")
        html.append("</div>")

        # Pipeline stages
        html.append("<h2>Pipeline Stages</h2>")
        html.append("<table><tr><th>Stage</th><th>Status</th>"
                    "<th>Duration</th><th>Output</th></tr>")
        for s in self.stages:
            badge = self._stage_badge(s["status"])
            html.append(f"<tr><td>{s['name']}</td><td>{badge}</td>"
                        f"<td>{s['duration']}</td><td>{s['output']}")
            if s["notes"]:
                html.append(f"<br><span style='color:#888'>{s['notes']}</span>")
            html.append("</td></tr>")
        html.append("</table>")

        # Changes
        if self.changes:
            html.append("<h2>Changes</h2>")
            for c in self.changes:
                html.append(f"<div class='card'><h3>{c['title']}</h3>")
                html.append(f"<div class='metric'><span class='error'>BEFORE:</span> {c['before']}<br>")
                html.append(f"<span class='success'>AFTER:</span> {c['after']}</div>")
                if c["impact"]:
                    html.append(f"<div class='notes'>{c['impact']}</div>")
                html.append("</div>")

        # Binary verification
        if self.binary:
            html.append("<h2>Binary Verification</h2>")
            b = self.binary
            html.append("<div class='grid'>")
            html.append(f"<div class='card'><h3>Size</h3>"
                        f"<div class='metric'>{b['size']:,} bytes</div></div>")
            html.append(f"<div class='card'><h3>SHA-256</h3>"
                        f"<div class='metric' style='word-break:break-all'>{b['sha256']}</div></div>")
            html.append("</div>")
            html.append("<h3>Magic Bytes</h3><table>")
            html.append("<tr><th>Offset</th><th>Value</th><th>Expected</th><th>Status</th></tr>")
            for off, (val, exp) in b["magics"].items():
                ok = val.lower() == exp.lower()
                badge = self._stage_badge("PASS" if ok else "FAIL")
                html.append(f"<tr><td><code>{off}</code></td>"
                            f"<td><code>{val}</code></td>"
                            f"<td><code>{exp}</code></td><td>{badge}</td></tr>")
            html.append("</table>")
            pt = "byte-identical" if b["pt_identical"] else "DIFFERS"
            cls = "success" if b["pt_identical"] else "error"
            html.append(f"<div class='metric'>Partition table: "
                        f"<span class='{cls}'>{pt}</span>")
            if b["pt_version"]:
                html.append(f" (vs {b['pt_version']})")
            html.append("</div>")

        # Known issues
        if self.issues:
            html.append("<h2>Known Issues</h2><ul>")
            for i in self.issues:
                html.append(f"<li>{i}</li>")
            html.append("</ul>")

        # Next steps
        if self.next_steps:
            html.append("<h2>Next Steps</h2><ul>")
            for s in self.next_steps:
                html.append(f"<li>{s}</li>")
            html.append("</ul>")

        # Footer
        html.append(f"<div class='footer'>Generated {datetime.now().strftime('%Y-%m-%d %H:%M %Z')} "
                    f"&bull; Watcher Firmware Build System</div>")
        html.append("</body></html>")

        with open(path, "w") as f:
            f.write("\n".join(html))
        return path


if __name__ == "__main__":
    # Example usage
    r = BuildReport(version="0.4.6", build_date="2026-09-27")
    r.add_stage("Clean build", "PASS", "6m 19s", "0 errors, 0 warnings")
    r.generate_html("/tmp/example_build_report.html")
    print("Wrote /tmp/example_build_report.html")
