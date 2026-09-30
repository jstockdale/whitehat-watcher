#!/usr/bin/env python3
"""cert_to_qr.py — render a Tier 1 cert JSON as a QR code for airgap transfer.

The cert is public by design; the QR code is just a reliable way to move it
off an airgapped machine without retyping hex. Display the image, scan with
your phone, paste the text wherever it's needed.

Usage:
    ./cert_to_qr.py --cert tier1-s1.tier1cert.json
    ./cert_to_qr.py --cert tier1-s1.tier1cert.json --format png --out tier1-s1.png

SVG is the default: it needs nothing beyond the vendored qrcode package.
PNG needs Pillow. No pip access required — qrcode is vendored under vendor/.
"""
import argparse
import json
import os
import sys

# Vendored qrcode (pure Python) so this works on airgapped machines with
# no pip access. System-installed qrcode takes precedence if present.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "vendor"))


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Render a Tier 1 cert JSON as a QR code for airgap transfer.")
    ap.add_argument("--cert", required=True,
                    help="path to the .tier1cert.json file")
    ap.add_argument("--out", default=None,
                    help="output path (default: <cert basename>.<fmt>)")
    ap.add_argument("--format", choices=["png", "svg"], default="svg",
                    help="image format (default: svg — needs no Pillow; "
                         "png needs Pillow installed)")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    try:
        import qrcode
    except ImportError:
        print("ERROR: needs the 'qrcode' package (pip install qrcode).",
              file=sys.stderr)
        return 2

    try:
        with open(args.cert) as f:
            cert = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: cannot read cert: {e}", file=sys.stderr)
        return 2

    # Sanity: it should look like a tier1 cert before we encode it.
    if cert.get("format") != "watcher-tier1-cert-v1":
        print("ERROR: not a watcher-tier1-cert-v1 JSON.", file=sys.stderr)
        return 2

    out = args.out
    if not out:
        base = os.path.splitext(os.path.basename(args.cert))[0]
        out = base + "." + args.format

    compact = json.dumps(cert, separators=(",", ":"))
    if args.format == "svg":
        from qrcode.image.svg import SvgImage
        qrcode.make(compact, image_factory=SvgImage).save(out)
    else:
        try:
            qrcode.make(compact).save(out)
        except ImportError as e:
            print(f"ERROR: PNG output needs Pillow ({e}); "
                  f"use --format svg instead.", file=sys.stderr)
            return 2
    print(f"wrote: {out} ({len(compact)} bytes encoded)")
    print(f"serial {cert.get('serial')}, subject {cert.get('subject')}")
    print("Display the image on this machine, scan with your phone to "
          "move the cert off the airgap.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
