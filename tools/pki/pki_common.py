#!/usr/bin/env python3
"""pki_common.py — shared helpers for the Watcher PKI tooling.

Single home for the normative values from PKI_DESIGN_BRIEF.md so the
three ceremony scripts (gen_tier0, issue_tier1_cert, verify_tier1_cert)
cannot drift out of agreement with each other — or with the brief.

NOT secret-bearing: this module is safe to commit.
"""

import hashlib
import struct
from datetime import datetime, timezone

# ---------------------------------------------------------------- normative
# From PKI_DESIGN_BRIEF.md — "Tier 1 certificate format (JSON)".
CERT_FORMAT = "watcher-tier1-cert-v1"
CERT_TIER = 1
CERT_SUBJECT = "watcher-tier1-provisioning"
CERT_ALGO = "ecdsa-p256-sha256"

# From PKI_DESIGN_BRIEF.md — "Canonical bytes signed by Tier 0".
# u8 algo id 0x01 == ecdsa-p256-sha256 (the only Tier 1 algo).
CERT_MAGIC = b"WATCHER-TIER1-CERT-v1"
CERT_ALGO_ID = 0x01

P256_UNCOMPRESSED_LEN = 65          # 04 || x(32) || y(32)
ED25519_SIG_LEN = 64
ED25519_PUBKEY_LEN = 32
ED25519_SEED_LEN = 32

ISO_FMT = "%Y-%m-%dT%H:%M:%SZ"


# ------------------------------------------------------------------ helpers
def unix_to_iso(ts: int) -> str:
    """Unix seconds -> 'YYYY-MM-DDTHH:MM:SSZ' (the cert timestamp format)."""
    return datetime.fromtimestamp(int(ts), tz=timezone.utc).strftime(ISO_FMT)


def iso_to_unix(s: str) -> int:
    """'YYYY-MM-DDTHH:MM:SSZ' -> unix seconds. Strict: rejects anything else."""
    try:
        dt = datetime.strptime(s, ISO_FMT).replace(tzinfo=timezone.utc)
    except (ValueError, TypeError) as e:
        raise ValueError(
            f"bad timestamp {s!r}: must be UTC 'YYYY-MM-DDTHH:MM:SSZ'") from e
    return int(dt.timestamp())


def now_unix() -> int:
    return int(datetime.now(tz=timezone.utc).timestamp())


def fp16(pubkey_bytes: bytes) -> str:
    """Project fingerprint convention (same as seal_keygen.py):
    sha256(pubkey).hexdigest()[:16]."""
    return hashlib.sha256(bytes(pubkey_bytes)).hexdigest()[:16]


def cert_canonical_bytes(subject: str, p256_pubkey: bytes,
                         issued_at_unix: int, expires_at_unix: int,
                         serial: int) -> bytes:
    """Build the exact canonical bytes Tier 0 signs (per the brief).

    b"WATCHER-TIER1-CERT-v1"
    || u16(len(subject)) || subject.encode()
    || u8(0x01)                       # algo id: ecdsa-p256-sha256
    || 65-byte uncompressed P-256 pubkey
    || u64(issued_at unix) || u64(expires_at unix)
    || u32(serial)

    All integers big-endian. Any change here invalidates every cert —
    this function is the single source of truth for all three scripts
    AND the firmware build-pipeline verifier must reproduce it byte-exact.
    """
    if not isinstance(p256_pubkey, (bytes, bytearray)) or \
            len(p256_pubkey) != P256_UNCOMPRESSED_LEN:
        raise ValueError("P-256 pubkey must be exactly 65 bytes uncompressed")
    if p256_pubkey[0] != 0x04:
        raise ValueError("P-256 pubkey must start with 0x04 (uncompressed)")
    if not (0 <= serial < 2 ** 32):
        raise ValueError("serial must fit in u32")
    subject_b = subject.encode("utf-8")
    return (CERT_MAGIC
            + struct.pack(">H", len(subject_b)) + subject_b
            + struct.pack(">B", CERT_ALGO_ID)
            + bytes(p256_pubkey)
            + struct.pack(">Q", issued_at_unix)
            + struct.pack(">Q", expires_at_unix)
            + struct.pack(">I", serial))


def validate_p256_uncompressed(p256_pubkey: bytes):
    """Raise unless the 65 bytes are a valid point on NIST P-256.

    Requires the `cryptography` package.
    """
    if len(p256_pubkey) != P256_UNCOMPRESSED_LEN:
        raise ValueError(f"P-256 pubkey must be 65 bytes, got {len(p256_pubkey)}")
    if p256_pubkey[0] != 0x04:
        raise ValueError("P-256 pubkey must be uncompressed (first byte 0x04)")
    try:
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError as e:
        raise RuntimeError(
            "the 'cryptography' package is required to validate P-256 keys "
            "(pip install cryptography)") from e
    # Raises ValueError if the point is not on the curve.
    ec.EllipticCurvePublicKey.from_encoded_point(
        ec.SECP256R1(), bytes(p256_pubkey))


def load_ed25519_seed(path: str) -> bytes:
    """Load a Tier 0 private key file.

    Accepts the 32-byte seed hex this tooling writes (libsodium layout:
    seed(32) || pk(32) — the seed alone is sufficient and is what we
    store). Also accepts a full 64-byte secret hex (seed||pubkey) for
    compatibility, using the seed half.
    """
    with open(path, "r") as f:
        raw = f.read().strip()
    try:
        key = bytes.fromhex(raw)
    except ValueError as e:
        raise ValueError(f"{path}: not hex: {e}") from e
    if len(key) == ED25519_SEED_LEN:
        return key
    if len(key) == 2 * ED25519_SEED_LEN:      # seed || pubkey
        return key[:ED25519_SEED_LEN]
    raise ValueError(
        f"{path}: expected 32-byte seed hex (64 chars) or 64-byte secret "
        f"hex (128 chars), got {len(key)} bytes")
