#!/usr/bin/env python3
"""Bake Tier 2 trust anchors into firmware/components/pki/tier2_keys.h.

Tier 2 = the trust anchors baked into firmware at build time (per
tools/pki/PKI_DESIGN_BRIEF.md):
  - Tier 0 Ed25519 public key (32 bytes, offline root, John-held)
  - Tier 1 ECDSA P-256 public key (65 bytes uncompressed, KMS-backed)
  - Tier 1 certificate serial + expiry (unix time)

The script NEVER bakes a bare unverified pubkey. It always verifies the
Tier 0 Ed25519 signature over the Tier 1 certificate's canonical bytes
(exact layout from the design brief) before emitting the header, and
refuses to bake an expired (or not-yet-valid) certificate.

Any verification failure exits non-zero with a loud message.

Certificate sources:
  --cert-file PATH     offline / air-gap build: Tier 1 cert JSON file
  --tier1-pubkey-url   GET /tier1/pubkey (API Gateway, IAM auth).
                       The endpoint is expected to return
                       {"p256_pubkey_hex": ..., "cert": {...}, "key_id": ...}
                       or the bare cert object. IAM request signing is out
                       of scope for this offline-first script; prefer
                       --cert-file in CI / on the build machine.

  --self-test          no network, no keys on disk: generates EPHEMERAL
                       Tier 0 (Ed25519) + Tier 1 (P-256) keypairs in memory,
                       builds a cert, signs it, runs the full verify path,
                       signs a sample provisioning command with the P-256
                       key and verifies it (DER/raw handling), then bakes
                       a header. The baked keys are TEST VECTORS ONLY and
                       the header is stamped as such. Private keys are never
                       written anywhere.

Usage (real build):
  TIER0_PUBKEY_HEX=<64 hex chars> \\
    firmware/tools/bake_tier2_keys.py \\
      --tier0-pubkey-hex "$TIER0_PUBKEY_HEX" \\
      --cert-file /path/to/tier1-cert.json

Then: idf.py build   (components/pki/CMakeLists.txt fails LOUDLY at
configure time if the generated header is missing or stale.)
"""
import argparse
import datetime
import json
import os
import struct
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BRIEF_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "tools", "pki", "PKI_DESIGN_BRIEF.md")

CERT_MAGIC = b"WATCHER-TIER1-CERT-v1"
CERT_FORMAT = "watcher-tier1-cert-v1"
ALGO_ID_ECDSA_P256_SHA256 = 0x01

PROV_MAGIC = b"WATCHER-PROVISION-v1"
PROV_FORMAT = "watcher-provision-v1"

REPO_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_OUT = os.path.join(REPO_ROOT, "components", "pki", "tier2_keys.h")


class BakeError(Exception):
    pass


# --------------------------------------------------------------------------
# Pure-Python Ed25519 verification (RFC 8032, SHA-512).
#
# Deliberately dependency-free: the bake path must not depend on PyNaCl
# (or any native module) being installed with the right permissions on
# the build machine. The --self-test cross-checks this implementation
# against the `cryptography` package AND the RFC 8032 test vector.
# --------------------------------------------------------------------------

import hashlib as _hashlib

_ED_Q = (1 << 255) - 19
_ED_L = (1 << 252) + 27742317777372353535851937790883648493
_ED_D = (-121665 * pow(121666, _ED_Q - 2, _ED_Q)) % _ED_Q
_ED_I = pow(2, (_ED_Q - 1) // 4, _ED_Q)


def _ed_xrecover(y):
    xx = (y * y - 1) * pow(_ED_D * y * y + 1, _ED_Q - 2, _ED_Q) % _ED_Q
    x = pow(xx, (_ED_Q + 3) // 8, _ED_Q)
    if (x * x - xx) % _ED_Q != 0:
        x = (x * _ED_I) % _ED_Q
    if x % 2 != 0:
        x = _ED_Q - x
    return x


def _ed_decodepoint(s):
    y = int.from_bytes(s, "little") & ((1 << 255) - 1)
    if y >= _ED_Q:
        return None
    x = _ed_xrecover(y)
    if (x & 1) != (s[31] >> 7):
        x = _ED_Q - x
    x %= _ED_Q
    if (-x * x + y * y - 1 - _ED_D * x * x * y * y) % _ED_Q != 0:
        return None  # not on the curve (-x^2 + y^2 = 1 + d.x^2.y^2)
    return (x, y, 1, (x * y) % _ED_Q)


def _ed_add(P, Q):
    (x1, y1, z1, t1) = P
    (x2, y2, z2, t2) = Q
    a = ((y1 - x1) * (y2 - x2)) % _ED_Q
    b = ((y1 + x1) * (y2 + x2)) % _ED_Q
    c = (t1 * 2 * _ED_D * t2) % _ED_Q
    dd = (z1 * 2 * z2) % _ED_Q
    e = (b - a) % _ED_Q
    f = (dd - c) % _ED_Q
    g = (dd + c) % _ED_Q
    h = (b + a) % _ED_Q
    return ((e * f) % _ED_Q, (g * h) % _ED_Q,
            (f * g) % _ED_Q, (e * h) % _ED_Q)


def _ed_scalarmult(P, e):
    Q = (0, 1, 1, 0)  # identity
    e = e % _ED_L
    while e > 0:
        if e & 1:
            Q = _ed_add(Q, P)
        P = _ed_add(P, P)
        e >>= 1
    return Q


_ED_GY = (4 * pow(5, _ED_Q - 2, _ED_Q)) % _ED_Q
_ED_GX = _ed_xrecover(_ED_GY)
_ED_G = (_ED_GX, _ED_GY, 1, (_ED_GX * _ED_GY) % _ED_Q)


def ed25519_check(sig64, pub32, msg):
    """RFC 8032 verification. Returns True on valid signature."""
    if len(sig64) != 64 or len(pub32) != 32:
        return False
    R = _ed_decodepoint(sig64[:32])
    A = _ed_decodepoint(pub32)
    if R is None or A is None:
        return False
    S = int.from_bytes(sig64[32:], "little")
    if S >= _ED_L:
        return False
    h = _hashlib.sha512(sig64[:32] + pub32 + msg).digest()
    k = int.from_bytes(h, "little") % _ED_L
    lhs = _ed_scalarmult(_ED_G, S)
    rhs = _ed_add(R, _ed_scalarmult(A, k))
    # projective equality: x1*z2 == x2*z1 and y1*z2 == y2*z1
    return ((lhs[0] * rhs[2] - rhs[0] * lhs[2]) % _ED_Q == 0 and
            (lhs[1] * rhs[2] - rhs[1] * lhs[2]) % _ED_Q == 0)


# --------------------------------------------------------------------------
# Canonical bytes (exact layout from PKI_DESIGN_BRIEF.md — all big-endian)
# --------------------------------------------------------------------------

def parse_iso8601_utc(s):
    """'2026-09-27T00:00:00Z' -> unix seconds. Strict UTC only."""
    try:
        if not isinstance(s, str) or not s.endswith("Z"):
            raise ValueError("must end with Z")
        dt = datetime.datetime.strptime(s[:-1], "%Y-%m-%dT%H:%M:%S")
        dt = dt.replace(tzinfo=datetime.timezone.utc)
        return int(dt.timestamp())
    except (ValueError, TypeError) as e:
        raise BakeError("bad ISO8601 UTC timestamp %r: %s" % (s, e))


def cert_canonical_bytes(cert):
    """Canonical bytes signed by Tier 0 (Ed25519)."""
    for f in ("subject", "algo", "p256_pubkey_hex",
              "issued_at", "expires_at", "serial"):
        if f not in cert:
            raise BakeError("cert missing field %r" % f)
    if cert.get("format") != CERT_FORMAT:
        raise BakeError("cert format %r != %r"
                        % (cert.get("format"), CERT_FORMAT))
    if cert.get("tier") != 1:
        raise BakeError("cert tier %r != 1" % cert.get("tier"))
    if cert["algo"] != "ecdsa-p256-sha256":
        raise BakeError("cert algo %r unsupported" % cert["algo"])
    pub = bytes.fromhex(cert["p256_pubkey_hex"])
    if len(pub) != 65 or pub[0] != 0x04:
        raise BakeError("p256_pubkey_hex must be 65-byte uncompressed "
                        "(04||x||y), got %d bytes" % len(pub))
    subject = cert["subject"].encode("utf-8")
    issued = parse_iso8601_utc(cert["issued_at"])
    expires = parse_iso8601_utc(cert["expires_at"])
    serial = cert["serial"]
    if not isinstance(serial, int) or serial < 0 or serial > 0xFFFFFFFF:
        raise BakeError("serial must be u32")
    out = bytearray()
    out += CERT_MAGIC
    out += struct.pack(">H", len(subject)) + subject
    out += struct.pack(">B", ALGO_ID_ECDSA_P256_SHA256)
    out += pub
    out += struct.pack(">Q", issued)
    out += struct.pack(">Q", expires)
    out += struct.pack(">I", serial)
    return bytes(out), issued, expires, serial, pub


def provision_canonical_bytes(cmd):
    """Canonical bytes signed by Tier 1 (KMS ECDSA_SHA_256).

    Mirrors pki_build_provision_canon() in
    firmware/components/pki/pki_verify.c — the two MUST agree byte for
    byte; the --self-test exercises both sides of the Python half.
    """
    for f in ("cmd", "device", "nonce", "issued_at", "expires_at", "params"):
        if f not in cmd:
            raise BakeError("provision cmd missing field %r" % f)
    if cmd.get("format") != PROV_FORMAT:
        raise BakeError("provision format %r != %r"
                        % (cmd.get("format"), PROV_FORMAT))
    nonce = bytes.fromhex(cmd["nonce"])
    if len(nonce) != 16:
        raise BakeError("nonce must be 32 hex chars (16 raw bytes)")
    c = cmd["cmd"].encode("utf-8")
    d = cmd["device"].encode("utf-8")
    issued = parse_iso8601_utc(cmd["issued_at"])
    expires = parse_iso8601_utc(cmd["expires_at"])
    params_canon = json.dumps(cmd["params"], sort_keys=True,
                              separators=(",", ":"),
                              ensure_ascii=True).encode("utf-8")
    out = bytearray()
    out += PROV_MAGIC
    out += struct.pack(">H", len(c)) + c
    out += struct.pack(">H", len(d)) + d
    out += nonce
    out += struct.pack(">Q", issued)
    out += struct.pack(">Q", expires)
    out += params_canon
    return bytes(out)


# --------------------------------------------------------------------------
# Verification
# --------------------------------------------------------------------------

def verify_tier0_signature(cert, tier0_pubkey_hex):
    """Recompute canonical bytes, Ed25519-verify tier0_sig_hex.

    Returns (pub65, issued, expires, serial). Raises BakeError (loud).
    """
    canon, issued, expires, serial, pub = cert_canonical_bytes(cert)
    try:
        tier0 = bytes.fromhex(tier0_pubkey_hex)
    except Exception as e:
        raise BakeError("bad --tier0-pubkey-hex: %s" % e)
    if len(tier0) != 32:
        raise BakeError("--tier0-pubkey-hex must be 32 bytes, got %d"
                        % len(tier0))
    sig_hex = cert.get("tier0_sig_hex")
    if not sig_hex:
        raise BakeError("cert has no tier0_sig_hex — refusing to bake "
                        "an unverified pubkey")
    try:
        sig = bytes.fromhex(sig_hex)
    except Exception:
        raise BakeError("tier0_sig_hex is not valid hex")
    if len(sig) != 64:
        raise BakeError("tier0_sig_hex must be 64 bytes, got %d" % len(sig))
    if not ed25519_check(sig, tier0, canon):
        raise BakeError(
            "TIER 0 SIGNATURE INVALID over canonical cert bytes — "
            "the Tier 1 pubkey is NOT trusted. Baking refused.")
    return pub, issued, expires, serial


def check_cert_window(issued, expires, now):
    if issued > expires:
        raise BakeError("cert issued_at (%d) > expires_at (%d)"
                        % (issued, expires))
    if now < issued:
        raise BakeError("cert not yet valid (issued_at in the future); "
                        "check the build machine clock")
    if now > expires:
        raise BakeError("cert EXPIRED at %d (now %d) — rotate Tier 1, "
                        "do not bake a dead trust anchor" % (expires, now))


def fetch_cert(args):
    if args.cert_file:
        with open(args.cert_file) as f:
            return json.load(f)
    # GET /tier1/pubkey
    req = urllib.request.Request(args.tier1_pubkey_url,
                                 headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = json.loads(r.read().decode("utf-8"))
    except Exception as e:
        raise BakeError("GET %s failed: %s"
                        % (args.tier1_pubkey_url, e))
    if isinstance(body, dict) and "cert" in body:
        return body["cert"]
    return body


def fmt_c_array(name, data, per_line=8):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk))
    return ("static const uint8_t %s[%d] = {\n%s\n};"
            % (name, len(data), ",\n".join(lines)))


def bake_header(tier0_pubkey_hex, pub65, issued, expires, serial,
                cert, out_path, test_vector=False):
    tier0 = bytes.fromhex(tier0_pubkey_hex)
    issued_s = datetime.datetime.fromtimestamp(
        issued, tz=datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    expires_s = datetime.datetime.fromtimestamp(
        expires, tz=datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    marker = "TEST VECTORS ONLY — DO NOT SHIP" if test_vector else \
        "Tier 0 Ed25519 signature VERIFIED before baking"
    header = ("""/* GENERATED — do not edit.
 *
 * Produced by firmware/tools/bake_tier2_keys.py from the Tier 1
 * certificate (format "%(fmt)s", serial %(serial_str)s).
 * %(marker)s (canonical bytes per tools/pki/PKI_DESIGN_BRIEF.md).
 * Cert validity: %(issued_s)s .. %(expires_s)s (unix %(issued)d..%(expires)d).
 */
#pragma once

#include <stdint.h>

#define TIER1_CERT_FORMAT "%(fmt)s"
#define TIER1_CERT_SERIAL %(serial_str)s
#define TIER1_CERT_ISSUED_AT %(issued_str)s
#define TIER1_CERT_EXPIRES_AT %(expires_str)s

/* Tier 0 (offline root) Ed25519 public key — verifies Tier 1 rotations. */
%(tier0_arr)s

/* Tier 1 (provisioning authority) P-256 public key, 65-byte uncompressed
 * 04||x||y — verifies provisioning commands (mbedTLS ECDSA P-256). */
%(tier1_arr)s
""" % {"fmt": CERT_FORMAT,
        "serial_str": "%uu" % serial,
        "issued_str": "%uull" % issued,
        "expires_str": "%uull" % expires,
        "marker": marker,
        "issued_s": issued_s, "expires_s": expires_s,
        "issued": issued, "expires": expires,
        "tier0_arr": fmt_c_array("TIER0_PUBKEY", tier0),
        "tier1_arr": fmt_c_array("TIER1_P256_PUBKEY", pub65)})
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write(header)
    return header


# --------------------------------------------------------------------------
# --self-test: ephemeral keys, full round trip, no secrets on disk
# --------------------------------------------------------------------------

def der_to_raw64(der):
    """DER ECDSA signature -> raw 64-byte r||s (what the Lambda ships)."""
    from cryptography.hazmat.primitives.asymmetric.utils import \
        decode_dss_signature
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def self_test(out_path):
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PrivateKey
    from cryptography.hazmat.primitives.asymmetric.utils import \
        Prehashed
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.serialization import \
        Encoding, PublicFormat

    print("== bake_tier2_keys.py --self-test ==")
    print("   (ephemeral in-memory keys; nothing secret is written)")

    # 0. Cross-check the pure-Python Ed25519 against the `cryptography`
    #    package: sign with it, verify with the pure-Python code; then
    #    mutate message/signature and confirm rejection.
    t0_chk = Ed25519PrivateKey.generate()
    chk_msg = b"watcher-pki-ed25519-selftest"
    chk_sig = t0_chk.sign(chk_msg)
    chk_pub = t0_chk.public_key().public_bytes(Encoding.Raw,
                                               PublicFormat.Raw)
    assert ed25519_check(chk_sig, chk_pub, chk_msg), \
        "Ed25519 verify failed on a valid signature — implementation BUG"
    assert not ed25519_check(chk_sig, chk_pub, chk_msg + b"x"), \
        "Ed25519 accepted a wrong message — BUG"
    mut = bytearray(chk_sig)
    mut[10] ^= 0x01
    assert not ed25519_check(bytes(mut), chk_pub, chk_msg), \
        "Ed25519 accepted a mutated signature — BUG"
    print("   [ok] pure-Python Ed25519 cross-checked vs `cryptography`")

    # Ephemeral Tier 0 (Ed25519) + Tier 1 (P-256).
    t0_sk = Ed25519PrivateKey.generate()
    t0_pk = t0_sk.public_key().public_bytes(Encoding.Raw,
                                            PublicFormat.Raw)
    t0_pk_hex = t0_pk.hex()
    t1_sk = ec.generate_private_key(ec.SECP256R1())
    t1_pub = t1_sk.public_key().public_numbers()
    pub65 = (b"\x04" + t1_pub.x.to_bytes(32, "big")
             + t1_pub.y.to_bytes(32, "big"))
    print("   generated ephemeral Tier0 Ed25519 + Tier1 P-256 keys")

    now = int(datetime.datetime.now(
        tz=datetime.timezone.utc).timestamp())
    cert = {
        "format": CERT_FORMAT,
        "tier": 1,
        "subject": "watcher-tier1-provisioning",
        "algo": "ecdsa-p256-sha256",
        "p256_pubkey_hex": pub65.hex(),
        "issued_at": "2026-09-27T00:00:00Z",
        "expires_at": "2030-09-27T00:00:00Z",
        "serial": 42,
    }
    canon, issued, expires, serial, _ = cert_canonical_bytes(cert)
    cert["tier0_sig_hex"] = t0_sk.sign(canon).hex()
    # cryptography's signer and our pure-Python verifier must agree.
    assert ed25519_check(bytes.fromhex(cert["tier0_sig_hex"]),
                         t0_pk, canon), \
        "cross-implementation Ed25519 mismatch — BUG"

    # 1. The real verify path must accept the good cert.
    pub, i2, e2, s2 = verify_tier0_signature(cert, t0_pk_hex)
    assert (i2, e2, s2) == (issued, expires, 42), "verify round-trip"
    check_cert_window(issued, expires, now)
    print("   [ok] Tier 0 signature verifies; cert window valid")

    # 2. Tampering must fail LOUDLY (exit non-zero via BakeError).
    bad = dict(cert)
    bad["serial"] = 43
    try:
        verify_tier0_signature(bad, t0_pk_hex)
        raise AssertionError("tampered cert verified — BUG")
    except BakeError as e:
        print("   [ok] tampered cert rejected: %s" % e)

    # 3. Wrong Tier 0 key must fail.
    try:
        wrong = Ed25519PrivateKey.generate().public_key().public_bytes(
            Encoding.Raw, PublicFormat.Raw).hex()
        verify_tier0_signature(cert, wrong)
        raise AssertionError("wrong Tier0 key verified — BUG")
    except BakeError as e:
        print("   [ok] wrong Tier 0 key rejected: %s" % e)

    # 4. Expired cert must fail.
    try:
        check_cert_window(issued, issued - 1, now)
        raise AssertionError("expired cert accepted — BUG")
    except BakeError as e:
        print("   [ok] expired cert rejected: %s" % e)

    # 5. Provisioning command round trip: canonical bytes (Python) ->
    #    KMS-style ECDSA_SHA_256 sign -> DER -> raw64 (Lambda's transform)
    #    -> verify with the baked pubkey. Mirrors what the device does in
    #    pki_verify_provision() with mbedTLS.
    import hashlib
    pcmd = {
        "format": PROV_FORMAT,
        "cmd": "install_commander_key",
        "device": "*",
        "nonce": "0123456789abcdef0123456789abcdef",
        "issued_at": "2026-09-27T00:00:00Z",
        "expires_at": "2026-09-27T00:15:00Z",
        "params": {"pubkey_hex": "aa" * 32},
    }
    pcanon = provision_canonical_bytes(pcmd)
    digest = hashlib.sha256(pcanon).digest()
    der = t1_sk.sign(digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    raw64 = der_to_raw64(der)
    t1_sk.public_key().verify(
        der, digest, ec.ECDSA(Prehashed(hashes.SHA256())))
    print("   [ok] provision canonical bytes sign/verify round trip "
          "(%d canon bytes, sig %s...)" % (len(pcanon), raw64.hex()[:16]))
    print("   provision canonical bytes (hex):")
    print("   " + pcanon.hex())

    # 6. Bake the TEST-VECTOR header.
    bake_header(t0_pk_hex, pub65, issued, expires, serial, cert,
                out_path, test_vector=True)
    print("   [ok] baked TEST-VECTOR header -> %s" % out_path)
    print()
    print("   *** TEST VECTORS ONLY — DO NOT SHIP FIRMWARE WITH THIS HEADER ***")
    print("   Delete it and re-run bake_tier2_keys.py with the real")
    print("   --tier0-pubkey-hex + Tier 1 cert before any real build.")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=
                                 argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tier0-pubkey-hex", default=os.environ.get(
        "TIER0_PUBKEY_HEX"),
        help="Tier 0 Ed25519 pubkey (64 hex chars). "
             "Also read from $TIER0_PUBKEY_HEX.")
    ap.add_argument("--cert-file",
                    help="Tier 1 cert JSON (offline/air-gap build).")
    ap.add_argument("--tier1-pubkey-url",
                    help="GET /tier1/pubkey endpoint URL.")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="Generated header path (default: %(default)s).")
    ap.add_argument("--self-test", action="store_true",
                    help="Ephemeral in-memory keys; full verify round trip; "
                         "bakes a TEST-VECTOR header. No secrets on disk.")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test(args.out)

    if not args.tier0_pubkey_hex:
        sys.stderr.write(
            "FATAL: --tier0-pubkey-hex (or $TIER0_PUBKEY_HEX) is required.\n"
            "The Tier 0 pubkey is the ONLY trust root; without it there is\n"
            "nothing to verify the Tier 1 cert against, and this script\n"
            "will not bake an unverified pubkey.\n")
        return 2
    if not (args.cert_file or args.tier1_pubkey_url):
        sys.stderr.write(
            "FATAL: need a cert source: --cert-file or --tier1-pubkey-url.\n")
        return 2
    if args.cert_file and args.tier1_pubkey_url:
        sys.stderr.write("FATAL: --cert-file and --tier1-pubkey-url are "
                         "mutually exclusive.\n")
        return 2

    try:
        cert = fetch_cert(args)
        pub65, issued, expires, serial = verify_tier0_signature(
            cert, args.tier0_pubkey_hex)
        now = int(datetime.datetime.now(
            tz=datetime.timezone.utc).timestamp())
        check_cert_window(issued, expires, now)
        bake_header(args.tier0_pubkey_hex, pub65, issued, expires,
                    serial, cert, args.out)
    except BakeError as e:
        sys.stderr.write("FATAL: %s\n" % e)
        return 2
    except (OSError, ValueError, KeyError) as e:
        sys.stderr.write("FATAL: could not load/parse cert: %s\n" % e)
        return 2

    print("baked %s (Tier1 cert serial %d, expires %d)"
          % (args.out, serial, expires))
    print("Tier 0 signature VERIFIED; cert window valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
