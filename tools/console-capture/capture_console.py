#!/usr/bin/env python3
"""Headless screenshot of the Watcher Console — replicable README asset capture.

Renders the locally-hosted console HTML file in headless Chromium (Playwright)
with a FRESH browser profile: no localStorage, so no broker credentials and no
serial port — the page renders its disconnected state and never touches the
bench. Tab selection uses the page's own hash routing (#muse, #console, ...).

Usage:
    .venv/bin/python capture_console.py [--tab muse] [--out PATH]
                                        [--html PATH] [--width 1480] [--height 940]

Defaults: --tab muse, --out ../../docs/assets/console.png,
--html ~/workspace/your_files/watcher-console/watcher-console.html

Setup (once):
    python3 -m venv .venv
    .venv/bin/pip install playwright
    .venv/bin/python -m playwright install chromium

The script prints the CONSOLE_VERSION it captured so the shot is attributable
to an exact console build.
"""

import argparse
import os
import pathlib
import sys

# On this box Chromium lives under ~/workspace/.cache-playwright.
os.environ.setdefault(
    "PLAYWRIGHT_BROWSERS_PATH",
    str(pathlib.Path.home() / "workspace" / ".cache-playwright"),
)

from playwright.sync_api import sync_playwright

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_HTML = pathlib.Path.home() / "workspace/your_files/watcher-console/watcher-console.html"
DEFAULT_OUT = HERE / "../../docs/assets/console.png"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tab", default="muse", help="console tab to show (monitor|console|commands|muse|privacy|flash|setup)")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="output PNG path")
    ap.add_argument("--html", default=str(DEFAULT_HTML), help="console HTML file to render")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=960)
    ap.add_argument("--scale", type=float, default=2.0, help="device scale factor (2 = retina-crisp)")
    ap.add_argument("--settle-ms", type=int, default=2500, help="extra settle wait after load (page timers/animations)")
    args = ap.parse_args()

    html = pathlib.Path(args.html).expanduser().resolve()
    if not html.exists():
        print(f"console HTML not found: {html}", file=sys.stderr)
        return 1
    out = pathlib.Path(args.out).expanduser().resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    url = html.as_uri() + "#" + args.tab

    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(
            viewport={"width": args.width, "height": args.height},
            device_scale_factor=args.scale,
        )
        page.goto(url, wait_until="load")
        # The target tab section must be the active one before we shoot.
        page.wait_for_selector(f"section#{args.tab}.active, div#{args.tab}.active, #{args.tab}.active",
                               timeout=10000)
        page.wait_for_timeout(args.settle_ms)
        version = page.evaluate(
            "() => (document.body.innerText.match(/0\\.4\\.\\d+(?:\\.\\d+)?/) || ['unknown'])[0]"
        )
        page.screenshot(path=str(out))
        browser.close()

    print(f"tab={args.tab} console_version~{version} -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
