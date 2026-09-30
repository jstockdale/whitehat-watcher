#!/usr/bin/env python3
"""verify_tier1_cert.py — verify a Tier 1 cert against a Tier 0 pubkey.

This is the check the firmware build pipeline runs BEFORE baking any
Tier 1 key into a firmware image: never bake a bare pubkey, only a
Tier 0-verified cert.

Checks, in order (per PKI_DESIGN_BRIEF.md):
  1. parse JSON, schema/format/tier/subject/algo match the brief
  2. P-256 pubkey is 65-byte uncompressed and a valid curve point
  3. issued_at / expires_at parse and issued_at <= now <= expires_at
  4. recompute canonical bytes from the cert fields (never trust a
     canonical blob carried inside the cert — there isn't one, by design)
  5. Ed25519-verify tier0_sig_hex against the Tier 0 pubkey

Exit 0 = VALID (loud), exit 1 = INVALID (loud reason), exit 2 = usage /
I/O error.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nacl.exceptions import BadSignatureError
from nacl.signing import VerifyKey

import pki_common as P


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Verify a Tier 1 certificate against a Tier 0 public key.",
        epilog="Used by the firmware build pipeline before baking the Tier 1 "
               "key. Exit 0 = valid, 1 = invalid, 2 = usage error.")
    ap.add_argument("--cert", required=True,
                    help="path to the .tier1cert.json file")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--tier0-pubkey-hex",
                   help="Tier 0 Ed25519 pubkey, 64 hex chars")
    g.add_argument("--tier0-pubkey-file",
                   help="path to a file containing the Tier 0 pubkey hex "
                        "(e.g. tier0_public.pub)")
    return ap.parse_args(argv)


def fail(msg):
    print(f"FAIL: {msg}")
    print("RESULT: INVALID")
    return 1


def main(argv=None):
    args = parse_args(argv)

    print("=== Tier 1 certificate verification ===")
    print(f"cert: {args.cert}")

    # ---- Tier 0 pubkey ---------------------------------------------------
    if args.tier0_pubkey_hex:
        t0_hex = args.tier0_pubkey_hex.strip().lower()
    else:
        try:
            with open(args.tier0_pubkey_file) as f:
                t0_hex = f.read().strip().lower()
        except OSError as e:
            print(f"ERROR: cannot read Tier 0 pubkey file: {e}", file=sys.stderr)
            return 2
    try:
        t0_pub = bytes.fromhex(t0_hex)
    except ValueError:
        print("ERROR: Tier 0 pubkey is not valid hex.", file=sys.stderr)
        return 2
    if len(t0_pub) != P.ED25519_PUBKEY_LEN:
        print(f"ERROR: Tier 0 pubkey must be 32 bytes, got {len(t0_pub)}.",
              file=sys.stderr)
        return 2
    verify_key = VerifyKey(t0_pub)
    print(f"tier0 fingerprint: {P.fp16(t0_pub)}")

    # ---- 1. parse + schema ----------------------------------------------
    try:
        with open(args.cert) as f:
            cert = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: cannot read/parse cert: {e}", file=sys.stderr)
        return 2
    if not isinstance(cert, dict):
        return fail("cert JSON is not an object")
    required = ["format", "tier", "subject", "algo", "p256_pubkey_hex",
                "issued_at", "expires_at", "serial", "tier0_sig_hex"]
    missing = [k for k in required if k not in cert]
    if missing:
        return fail(f"missing fields: {', '.join(missing)}")
    checks = [
        ("format", cert["format"] == P.CERT_FORMAT, P.CERT_FORMAT),
        ("tier", cert["tier"] == P.CERT_TIER, str(P.CERT_TIER)),
        ("subject", cert["subject"] == P.CERT_SUBJECT, P.CERT_SUBJECT),
        ("algo", cert["algo"] == P.CERT_ALGO, P.CERT_ALGO),
    ]
    for name, ok, want in checks:
        if not ok:
            return fail(f"{name} mismatch: got {cert[name]!r}, want {want!r}")
    print("schema: format/tier/subject/algo match the brief ... OK")

    # ---- 2. P-256 pubkey --------------------------------------------------
    try:
        p256 = bytes.fromhex(cert["p256_pubkey_hex"])
    except (ValueError, TypeError):
        return fail("p256_pubkey_hex is not valid hex")
    try:
        P.validate_p256_uncompressed(p256)
    except (ValueError, RuntimeError) as e:
        return fail(f"invalid P-256 public key: {e}")
    print(f"P-256 pubkey: 65-byte uncompressed, valid curve point ... OK "
          f"(fp {P.fp16(p256)})")

    # ---- 3. timestamps ----------------------------------------------------
    try:
        issued_unix = P.iso_to_unix(cert["issued_at"])
        expires_unix = P.iso_to_unix(cert["expires_at"])
    except (ValueError, TypeError) as e:
        return fail(f"bad timestamp: {e}")
    if not isinstance(cert["serial"], int) or isinstance(cert["serial"], bool) \
            or not (0 <= cert["serial"] < 2 ** 32):
        return fail(f"serial must be a u32 integer, got {cert['serial']!r}")
    now = P.now_unix()
    if issued_unix > now:
        return fail(f"cert not yet valid (issued_at {cert['issued_at']} is in the future)")
    if expires_unix < now:
        return fail(f"cert EXPIRED (expires_at {cert['expires_at']} < now)")
    print(f"validity: {cert['issued_at']} <= now <= {cert['expires_at']} ... OK")

    # ---- 4+5. canonical bytes + Ed25519 verify -----------------------------
    canonical = P.cert_canonical_bytes(
        cert["subject"], p256, issued_unix, expires_unix, cert["serial"])
    try:
        sig = bytes.fromhex(cert["tier0_sig_hex"])
    except (ValueError, TypeError):
        return fail("tier0_sig_hex is not valid hex")
    if len(sig) != P.ED25519_SIG_LEN:
        return fail(f"tier0_sig_hex must be 64 bytes, got {len(sig)}")
    try:
        verify_key.verify(canonical, sig)
    except BadSignatureError:
        return fail("Ed25519 signature does NOT verify against the Tier 0 pubkey "
                    "(wrong key, or cert was modified after signing)")
    print("Ed25519 signature over canonical bytes ... OK")

    print()
    print(f"subject: {cert['subject']}")
    print(f"serial:  {cert['serial']}")
    print(f"expiry:  {cert['expires_at']}")
    print("RESULT: VALID")
    return 0


if __name__ == "__main__":
    sys.exit(main())
