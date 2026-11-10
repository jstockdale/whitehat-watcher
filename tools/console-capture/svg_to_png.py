#!/usr/bin/env python3
"""Render an SVG asset to a PNG review copy with headless Chromium.

Usage: .venv/bin/python svg_to_png.py <in.svg> [out.png] [--scale 2]

On this box Chromium lives under ~/workspace/.cache-playwright (PLAYWRIGHT_BROWSERS_PATH);
the default below keeps the tool self-contained here.
"""

import os
import pathlib
import sys

os.environ.setdefault(
    "PLAYWRIGHT_BROWSERS_PATH",
    str(pathlib.Path.home() / "workspace" / ".cache-playwright"),
)

from playwright.sync_api import sync_playwright


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    scale = 2.0
    if "--scale" in sys.argv:
        scale = float(sys.argv[sys.argv.index("--scale") + 1])
    if not args:
        print(__doc__)
        return 1
    src = pathlib.Path(args[0]).expanduser().resolve()
    out = pathlib.Path(args[1]).expanduser().resolve() if len(args) > 1 else src.with_suffix(".png")
    if not src.exists():
        print(f"not found: {src}", file=sys.stderr)
        return 1
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(device_scale_factor=scale)
        page.goto(src.as_uri(), wait_until="load")
        page.wait_for_timeout(400)
        page.locator("svg").first.screenshot(path=str(out))
        browser.close()
    print(f"{src.name} -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
