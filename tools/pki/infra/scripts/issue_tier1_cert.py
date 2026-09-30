#!/usr/bin/env python3
"""Tier 0 ceremony helper: issue (or verify) a Tier 1 certificate.

############################################################################
#  WARNING: Tier 0 private key handling.
#
#  * Run this ONLY on John's offline/secure machine. NEVER on a shared host,
#    NEVER in CI, NEVER with the key in the repo / shell history / chat.
#  * The Tier 0 private key file must be mode 0600 and must never leave
#    John's custody (brief: "NEVER in AWS, NEVER in repo").
#  * This script reads the key, signs ONE canonical cert, and forgets it.
#    It never prints, logs, or stores key material.
############################################################################

Key file format: 32-byte Ed25519 seed as 64 hex chars (whitespace stripped),
or a 64-byte secret key as 128 hex chars (seed || pubkey; seed is used).

Requires PyNaCl (already a project dependency: host-agent/mqtt-bridge/).

Usage -- issue:
    ./issue_tier1_cert.py issue \\
        --tier0-key-file ~/.ssh/watcher_tier0_seed.hex \\
        --p256-pubkey-hex 04<128 hex> \\
        --serial 2 --valid-days 365 \\
        --out tier1-cert-s2.json

Usage -- verify (self-check a cert against a Tier 0 pubkey):
    ./issue_tier1_cert.py verify \\
        --cert tier1-cert-s2.json \\
        --tier0-pubkey-hex <64 hex chars>

After issuing: POST the cert to /rotation (see ../DEPLOY.md), then archive
this cert JSON with the ceremony notes. The device/firmware side bakes the
Tier 0 pubkey + Tier 1 pubkey + expiry via the firmware build pipeline.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timedelta, timezone

sys.path.insert(
    0, os.path.join(os.path.dirname(__file__), "..", "lambda", "layer", "python")
)

import pki_common  # noqa: E402
from pki_common import (  # noqa: E402
    CERT_FORMAT,
    canonical_cert_bytes,
    verify_tier1_cert,
)

SUBJECT = "watcher-tier1-provisioning"


def _read_tier0_seed(path: str) -> bytes:
    with open(path) as fh:
        hexed = "".join(fh.read().split())
    raw = bytes.fromhex(hexed)
    if len(raw) == 32:
        return raw
    if len(raw) == 64:
        return raw[:32]  # secret key = seed || pubkey
    raise SystemExit(f"key file must decode to 32 or 64 bytes, got {len(raw)}")


def cmd_issue(args: argparse.Namespace) -> None:
    from nacl.bindings import crypto_sign, crypto_sign_seed_keypair

    seed = _read_tier0_seed(args.tier0_key_file)
    pub, priv = crypto_sign_seed_keypair(seed)
    print(f"Tier 0 pubkey: {pub.hex()}", file=sys.stderr)
    print("KEEP THIS MACHINE OFFLINE. Do not paste the key anywhere.",
          file=sys.stderr)

    now = datetime.now(tz=timezone.utc).replace(microsecond=0)
    issued_at = args.issued_at or now.strftime("%Y-%m-%dT%H:%M:%SZ")
    expires_at = (now + timedelta(days=args.valid_days)).strftime("%Y-%m-%dT%H:%M:%SZ")

    cert = {
        "format": CERT_FORMAT,
        "tier": 1,
        "subject": SUBJECT,
        "algo": "ecdsa-p256-sha256",
        "p256_pubkey_hex": args.p256_pubkey_hex.lower(),
        "issued_at": issued_at,
        "expires_at": expires_at,
        "serial": args.serial,
        "tier0_sig_hex": "",
    }
    canonical = canonical_cert_bytes(cert)  # validates shape before signing
    sig = crypto_sign(canonical, priv)[:64]
    cert["tier0_sig_hex"] = sig.hex()

    # Self-check through the same verifier the Lambda uses.
    verify_tier1_cert(cert, pub.hex(), now.timestamp())

    out = json.dumps(cert, indent=2) + "\n"
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(out)
        os.chmod(args.out, 0o600)
        print(f"wrote {args.out} (mode 0600)", file=sys.stderr)
    else:
        sys.stdout.write(out)


def cmd_verify(args: argparse.Namespace) -> None:
    with open(args.cert) as fh:
        cert = json.load(fh)
    now = datetime.now(tz=timezone.utc).timestamp()
    verified = verify_tier1_cert(cert, args.tier0_pubkey_hex, now)
    print(f"OK: Tier 0 signature valid; serial={verified['serial']}; "
          f"valid {verified['issued_at']} .. {verified['expires_at']}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Tier 0 ceremony: issue/verify Tier 1 certs")
    sub = ap.add_subparsers(dest="cmd", required=True)

    i = sub.add_parser("issue", help="sign a new Tier 1 cert with the Tier 0 key")
    i.add_argument("--tier0-key-file", required=True)
    i.add_argument("--p256-pubkey-hex", required=True,
                   help="65-byte uncompressed P-256 pubkey, 130 hex chars (from the KMS key)")
    i.add_argument("--serial", type=int, required=True,
                   help="must be greater than the currently deployed cert serial")
    i.add_argument("--valid-days", type=int, default=365)
    i.add_argument("--issued-at", default=None, help="ISO-8601 UTC (default: now)")
    i.add_argument("--out", default=None, help="output path (default: stdout)")
    i.set_defaults(fn=cmd_issue)

    v = sub.add_parser("verify", help="verify a cert against a Tier 0 pubkey")
    v.add_argument("--cert", required=True)
    v.add_argument("--tier0-pubkey-hex", required=True)
    v.set_defaults(fn=cmd_verify)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
