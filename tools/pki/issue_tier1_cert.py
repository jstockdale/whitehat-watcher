#!/usr/bin/env python3
"""issue_tier1_cert.py — Tier 0 signs a Tier 1 (provisioning authority) cert.

Builds the exact Tier 1 certificate JSON from PKI_DESIGN_BRIEF.md,
computes the canonical bytes, Ed25519-signs them with the Tier 0
private key, and writes <out>.tier1cert.json (also printed to stdout).

The P-256 pubkey comes from AWS KMS (key spec ECC_NIST_P256); this
script takes the 65-byte uncompressed point as hex (e.g. from
`aws kms get-public-key`). The point is validated to lie on the
P-256 curve before signing — Tier 0 never signs a bogus key.

The Tier 0 private key is read transiently and never printed, logged,
or written anywhere.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nacl.signing import SigningKey

import pki_common as P


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Tier 0 signs a Tier 1 certificate (watcher-tier1-cert-v1).",
        epilog="Example: issue_tier1_cert.py --tier0-key ~/.config/watcher-pki/tier0_private.key "
               "--p256-pubkey-hex 04a1b2... --serial 2 --expires-days 90 "
               "--out ./tier1-s2.tier1cert.json")
    ap.add_argument("--tier0-key", required=True,
                    help="path to the Tier 0 private key file (32-byte seed hex)")
    ap.add_argument("--p256-pubkey-hex", required=True,
                    help="Tier 1 P-256 public key: 65-byte uncompressed 04||x||y, hex "
                         "(130 hex chars)")
    ap.add_argument("--serial", required=True, type=int,
                    help="certificate serial number (u32; must increase on rotation)")
    ap.add_argument("--expires-days", type=int, default=365,
                    help="cert lifetime in days from issuance (default: %(default)s)")
    ap.add_argument("--issued-at",
                    help="override issuance time as 'YYYY-MM-DDTHH:MM:SSZ' "
                         "(default: now; for testing only)")
    ap.add_argument("--out", default=None,
                    help="output path for the cert JSON "
                         "(default: ./tier1-s<SERIAL>.tier1cert.json)")
    ap.add_argument("--qr", default=None, metavar="PATH",
                    help="also write a QR code of the cert JSON for airgap "
                         "transfer (scan with phone, paste the text). "
                         "Extension picks the format: .svg (default, no "
                         "extra deps) or .png (needs Pillow).")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    # ---- validate inputs before touching the Tier 0 key ----------------
    if not (0 <= args.serial < 2 ** 32):
        print(f"ERROR: --serial {args.serial} must fit in u32 (0..4294967295).",
              file=sys.stderr)
        return 2
    if args.expires_days <= 0:
        print("ERROR: --expires-days must be positive.", file=sys.stderr)
        return 2

    p256_hex = args.p256_pubkey_hex.strip().lower()
    if p256_hex.startswith("0x"):
        p256_hex = p256_hex[2:]
    try:
        p256 = bytes.fromhex(p256_hex)
    except ValueError:
        print("ERROR: --p256-pubkey-hex is not valid hex.", file=sys.stderr)
        return 2
    try:
        P.validate_p256_uncompressed(p256)
    except (ValueError, RuntimeError) as e:
        print(f"ERROR: invalid P-256 public key: {e}", file=sys.stderr)
        return 2

    if args.issued_at:
        try:
            issued_unix = P.iso_to_unix(args.issued_at)
        except ValueError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 2
    else:
        issued_unix = P.now_unix()
    expires_unix = issued_unix + args.expires_days * 86400

    # ---- load Tier 0 key (transient; never printed) --------------------
    try:
        seed = P.load_ed25519_seed(args.tier0_key)
    except (OSError, ValueError) as e:
        print(f"ERROR: cannot load Tier 0 key: {e}", file=sys.stderr)
        return 2
    tier0 = SigningKey(seed)
    tier0_pub_hex = tier0.verify_key.encode().hex()

    # ---- build canonical bytes + sign ----------------------------------
    canonical = P.cert_canonical_bytes(
        P.CERT_SUBJECT, p256, issued_unix, expires_unix, args.serial)
    sig = tier0.sign(canonical).signature          # 64-byte Ed25519

    cert = {
        "format": P.CERT_FORMAT,
        "tier": P.CERT_TIER,
        "subject": P.CERT_SUBJECT,
        "algo": P.CERT_ALGO,
        "p256_pubkey_hex": p256.hex(),
        "issued_at": P.unix_to_iso(issued_unix),
        "expires_at": P.unix_to_iso(expires_unix),
        "serial": args.serial,
        "tier0_sig_hex": sig.hex(),
    }
    cert_json = json.dumps(cert, indent=2, sort_keys=False) + "\n"

    out_path = args.out or f"tier1-s{args.serial}.tier1cert.json"
    with open(out_path, "w") as f:
        f.write(cert_json)

    print(cert_json, end="")
    print(f"wrote: {out_path}")
    print(f"tier0 fingerprint (signer): {P.fp16(bytes.fromhex(tier0_pub_hex))}")
    print(f"tier1 P-256 fingerprint:    {P.fp16(p256)}")

    if args.qr:
        # Vendored qrcode (pure Python) so this works on airgapped machines.
        sys.path.insert(0, os.path.join(os.path.dirname(
            os.path.abspath(__file__)), "vendor"))
        try:
            import qrcode
        except ImportError:
            print("ERROR: vendored qrcode not found under vendor/.",
                  file=sys.stderr)
            return 2
        # Compact JSON (no whitespace) keeps the QR version small.
        compact = json.dumps(cert, separators=(",", ":"))
        ext = os.path.splitext(args.qr)[1].lower()
        if ext == ".png":
            try:
                qrcode.make(compact).save(args.qr)
            except ImportError as e:
                print(f"ERROR: PNG output needs Pillow ({e}); "
                      f"use a .svg path instead.", file=sys.stderr)
                return 2
        else:
            from qrcode.image.svg import SvgImage
            qrcode.make(compact, image_factory=SvgImage).save(args.qr)
        print(f"wrote QR: {args.qr} ({len(compact)} bytes encoded)")
        print("Display it on this machine and scan with your phone to move "
              "the cert off the airgap — no typing, no photos.")

    print("Next: publish this cert via POST /rotation, then point the "
          "firmware build pipeline at it (see PKI_RUNBOOK.md).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
