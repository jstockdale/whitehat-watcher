"""Watcher PKI shared core: validation, canonical encodings, cert verification.

Ships to every PKI Lambda as a Lambda layer (``lambda/layer/python/``).
Pure Python standard library on purpose:

* The Lambda deployment packages need no vendored native dependencies
  (no PyNaCl/libsodium wheels), so ``cdk synth``/``cdk deploy`` work with
  plain ``Code.from_asset`` and no Docker bundling step.
* The exact same file runs under ``pytest`` locally, so the
  security-critical schema validation is unit-tested against the code
  that actually signs.

Implements the encodings from PKI_DESIGN_BRIEF.md exactly:

* Tier 1 cert canonical bytes (Tier 0 Ed25519 signs these).
* Provisioning command canonical bytes (Tier 1 ECDSA P-256 signs these).

Ed25519 verification is implemented in pure Python (RFC 8032). It is
cross-checked against PyNaCl in the unit tests (``tests/``); PyNaCl is a
*test-only* dependency and is never bundled into a Lambda.
"""

from __future__ import annotations

import hashlib
import json
import re
import secrets
import struct
from datetime import datetime, timezone

# ---------------------------------------------------------------------------
# Constants (from PKI_DESIGN_BRIEF.md)
# ---------------------------------------------------------------------------

PROVISION_FORMAT = "watcher-provision-v1"
CERT_FORMAT = "watcher-tier1-cert-v1"

PROVISION_DOMAIN = b"WATCHER-PROVISION-v1"
CERT_DOMAIN = b"WATCHER-TIER1-CERT-v1"

ALLOWED_CMDS = (
    "install_commander_key",
    "install_telemetry_seed",
    "rotate_tier1",
    "factory_reset_keys",
)

TOP_LEVEL_FIELDS = frozenset(
    {"format", "cmd", "device", "nonce", "issued_at", "expires_at", "params"}
)

CERT_FIELDS = frozenset(
    {
        "format",
        "tier",
        "subject",
        "algo",
        "p256_pubkey_hex",
        "issued_at",
        "expires_at",
        "serial",
        "tier0_sig_hex",
    }
)

# Provisioning command lifetime: expiry window MUST be <= 15 minutes.
MAX_WINDOW_S = 15 * 60
# Tolerated clock skew for issued_at slightly in the future (device has no
# SNTP; the Lambda accepts a small skew and documents it).
CLOCK_SKEW_S = 60

# Algo id registry for the Tier 1 cert canonical bytes (brief, section cert).
ALGO_IDS = {"ecdsa-p256-sha256": 0x01}

# device: "*" (any unkeyed device) or a device id.
DEVICE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
NONCE_RE = re.compile(r"^[0-9a-fA-F]{32}$")
HEX_RE = re.compile(r"^[0-9a-fA-F]+$")
# Ed25519 pubkey: 32 bytes -> 64 hex chars.
ED25519_PUBKEY_HEX_LEN = 64
# P-256 uncompressed point: 0x04 || x || y, 65 bytes -> 130 hex chars.
P256_UNCOMPRESSED_HEX_LEN = 130
# Telemetry-seed fingerprint: hex, 8..64 bytes (existing fp is 16 hex chars).
SEED_FP_MIN_HEX = 16
SEED_FP_MAX_HEX = 128

TIER1_CERT_SUBJECT = "watcher-tier1-provisioning"


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class PkiError(Exception):
    """Base class for PKI validation failures."""


class CommandValidationError(PkiError):
    """A provisioning command failed pre-sign validation."""


class CertValidationError(PkiError):
    """A Tier 1 certificate failed verification."""


# ---------------------------------------------------------------------------
# Time helpers
# ---------------------------------------------------------------------------


def parse_iso8601(value: object, field: str) -> datetime:
    """Parse an ISO-8601 timestamp. Naive datetimes are rejected: the brief
    uses ``Z``-suffixed UTC everywhere, and implicit-local-time bugs in a
    signing service are a classic expiry-bypass vector."""
    if not isinstance(value, str):
        raise CommandValidationError(f"{field} must be a string")
    try:
        dt = datetime.fromisoformat(value)
    except ValueError:
        raise CommandValidationError(f"{field} is not valid ISO-8601: {value!r}")
    if dt.tzinfo is None:
        raise CommandValidationError(f"{field} must carry a timezone (use Z/UTC)")
    return dt.astimezone(timezone.utc)


def unix_seconds(dt: datetime) -> int:
    return int(dt.timestamp())


# ---------------------------------------------------------------------------
# Canonical encodings (byte-exact per the brief)
# ---------------------------------------------------------------------------


def canonical_json(obj: object) -> bytes:
    """Deterministic JSON: sorted keys, no whitespace, ASCII."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("ascii")


def canonical_provision_bytes(cmd: dict) -> bytes:
    """Canonical bytes the Tier 1 KMS key signs (``ECDSA_SHA_256``).

    b"WATCHER-PROVISION-v1"
    || u16(len(cmd)) || cmd || u16(len(device)) || device
    || 16-byte nonce (raw) || u64(issued_at) || u64(expires_at)
    || canonical-JSON(params)
    """
    nonce_raw = bytes.fromhex(cmd["nonce"])
    assert len(nonce_raw) == 16
    parts = [
        PROVISION_DOMAIN,
        struct.pack(">H", len(cmd["cmd"].encode("utf-8"))),
        cmd["cmd"].encode("utf-8"),
        struct.pack(">H", len(cmd["device"].encode("utf-8"))),
        cmd["device"].encode("utf-8"),
        nonce_raw,
        struct.pack(">Q", unix_seconds(parse_iso8601(cmd["issued_at"], "issued_at"))),
        struct.pack(">Q", unix_seconds(parse_iso8601(cmd["expires_at"], "expires_at"))),
        canonical_json(cmd["params"]),
    ]
    return b"".join(parts)


def canonical_cert_bytes(cert: dict) -> bytes:
    """Canonical bytes the Tier 0 key signs over a Tier 1 certificate.

    b"WATCHER-TIER1-CERT-v1"
    || u16(len(subject)) || subject || u8(algo_id)
    || 65-byte uncompressed P-256 pubkey
    || u64(issued_at) || u64(expires_at) || u32(serial)
    """
    try:
        algo_id = ALGO_IDS[cert["algo"]]
    except KeyError:
        raise CertValidationError(f"unknown cert algo: {cert.get('algo')!r}")
    subject = cert["subject"].encode("utf-8")
    pubkey = bytes.fromhex(cert["p256_pubkey_hex"])
    if len(pubkey) != 65 or pubkey[0] != 0x04:
        raise CertValidationError("p256_pubkey_hex must be 65-byte uncompressed (04||x||y)")
    parts = [
        CERT_DOMAIN,
        struct.pack(">H", len(subject)),
        subject,
        struct.pack(">B", algo_id),
        pubkey,
        struct.pack(">Q", unix_seconds(parse_iso8601(cert["issued_at"], "issued_at"))),
        struct.pack(">Q", unix_seconds(parse_iso8601(cert["expires_at"], "expires_at"))),
        struct.pack(">I", cert["serial"]),
    ]
    return b"".join(parts)


# ---------------------------------------------------------------------------
# Provisioning command validation (MANDATORY pre-sign gate)
# ---------------------------------------------------------------------------


def _require_hex(value: object, field: str, hex_len: int) -> None:
    if not isinstance(value, str) or len(value) != hex_len or not HEX_RE.fullmatch(value):
        raise CommandValidationError(
            f"{field} must be {hex_len} hex chars ({hex_len // 2} bytes)"
        )


def _validate_params(cmd: str, params: object, now: float) -> None:
    if not isinstance(params, dict):
        raise CommandValidationError("params must be an object")
    if cmd == "install_commander_key":
        if set(params) != {"pubkey_hex"}:
            raise CommandValidationError(
                "install_commander_key params must be exactly {pubkey_hex}"
            )
        _require_hex(params["pubkey_hex"], "params.pubkey_hex", ED25519_PUBKEY_HEX_LEN)
    elif cmd == "install_telemetry_seed":
        if set(params) != {"seed_fp"}:
            raise CommandValidationError(
                "install_telemetry_seed params must be exactly {seed_fp}"
            )
        fp = params["seed_fp"]
        if (
            not isinstance(fp, str)
            or not (SEED_FP_MIN_HEX <= len(fp) <= SEED_FP_MAX_HEX)
            or not HEX_RE.fullmatch(fp)
        ):
            raise CommandValidationError(
                "params.seed_fp must be hex, "
                f"{SEED_FP_MIN_HEX}..{SEED_FP_MAX_HEX} chars "
                "(fingerprint only -- the seed itself must NEVER appear here)"
            )
    elif cmd == "rotate_tier1":
        if set(params) != {"p256_pubkey_hex", "expires_at"}:
            raise CommandValidationError(
                "rotate_tier1 params must be exactly {p256_pubkey_hex, expires_at}"
            )
        _require_hex(params["p256_pubkey_hex"], "params.p256_pubkey_hex",
                     P256_UNCOMPRESSED_HEX_LEN)
        if not params["p256_pubkey_hex"][:2].lower() == "04":
            raise CommandValidationError(
                "params.p256_pubkey_hex must start with 04 (uncompressed point)"
            )
        exp = parse_iso8601(params["expires_at"], "params.expires_at")
        if exp.timestamp() <= now:
            raise CommandValidationError("params.expires_at must be in the future")
    elif cmd == "factory_reset_keys":
        if set(params) != {"confirm"}:
            raise CommandValidationError(
                "factory_reset_keys params must be exactly {confirm}"
            )
        if params["confirm"] is not True:
            raise CommandValidationError("params.confirm must be boolean true")
    else:  # pragma: no cover -- cmd membership checked before dispatch
        raise CommandValidationError(f"unknown cmd: {cmd!r}")


def validate_provision_command(cmd: object, now: float) -> bytes:
    """Schema-validate a provisioning command per the brief.

    Returns the canonical signing bytes on success. Raises
    :class:`CommandValidationError` with a human-readable reason on any
    failure -- the caller turns that into ``400 + audit entry`` and NEVER
    touches KMS.
    """
    if not isinstance(cmd, dict):
        raise CommandValidationError("command must be a JSON object")

    unknown = set(cmd) - TOP_LEVEL_FIELDS
    if unknown:
        raise CommandValidationError(
            f"unknown top-level field(s): {sorted(unknown)} "
            "(strict schema: no extra fields are signed)"
        )
    missing = TOP_LEVEL_FIELDS - set(cmd)
    if missing:
        raise CommandValidationError(f"missing required field(s): {sorted(missing)}")

    if cmd["format"] != PROVISION_FORMAT:
        raise CommandValidationError(
            f"format must be {PROVISION_FORMAT!r}, got {cmd['format']!r}"
        )

    if cmd["cmd"] not in ALLOWED_CMDS:
        raise CommandValidationError(
            f"cmd must be one of {list(ALLOWED_CMDS)}, got {cmd['cmd']!r}"
        )

    device = cmd["device"]
    if device != "*" and not (isinstance(device, str) and DEVICE_RE.fullmatch(device)):
        raise CommandValidationError(
            "device must be '*' or a device id ([A-Za-z0-9._-], max 64 chars)"
        )

    nonce = cmd["nonce"]
    if not isinstance(nonce, str) or not NONCE_RE.fullmatch(nonce):
        raise CommandValidationError("nonce must be 32 hex chars (16 random bytes)")

    issued = parse_iso8601(cmd["issued_at"], "issued_at")
    expires = parse_iso8601(cmd["expires_at"], "expires_at")
    window = expires.timestamp() - issued.timestamp()
    if window <= 0:
        raise CommandValidationError("expires_at must be after issued_at")
    if window > MAX_WINDOW_S:
        raise CommandValidationError(
            f"expiry window {window:.0f}s exceeds the 15-minute maximum"
        )
    if issued.timestamp() > now + CLOCK_SKEW_S:
        raise CommandValidationError(
            f"issued_at is more than {CLOCK_SKEW_S}s in the future"
        )
    if expires.timestamp() <= now:
        raise CommandValidationError("command is already expired")

    _validate_params(cmd["cmd"], cmd["params"], now)

    return canonical_provision_bytes(cmd)


# ---------------------------------------------------------------------------
# DER (KMS) -> raw r||s signature conversion
# ---------------------------------------------------------------------------


def _der_read_length(buf: bytes, pos: int) -> tuple[int, int]:
    first = buf[pos]
    pos += 1
    if first & 0x80 == 0:
        return first, pos
    nbytes = first & 0x7F
    if nbytes == 0 or nbytes > 4:
        raise PkiError("unsupported DER length form")
    length = int.from_bytes(buf[pos : pos + nbytes], "big")
    return length, pos + nbytes


def der_to_raw_sig(der: bytes) -> bytes:
    """Convert a DER-encoded ECDSA signature (as KMS returns) to the raw
    64-byte ``r || s`` form the device expects. Rejects malformed DER --
    a truncated/injected signature must never reach the device."""
    if not isinstance(der, (bytes, bytearray)) or len(der) < 8:
        raise PkiError("DER signature too short")
    buf = bytes(der)
    pos = 0
    if buf[pos] != 0x30:
        raise PkiError("DER signature must start with SEQUENCE")
    pos += 1
    seq_len, pos = _der_read_length(buf, pos)
    if seq_len != len(buf) - pos:
        raise PkiError("DER SEQUENCE length mismatch")

    def read_int() -> int:
        nonlocal pos
        if buf[pos] != 0x02:
            raise PkiError("DER signature must contain INTEGERs")
        pos += 1
        ilen, pos = _der_read_length(buf, pos)
        if ilen == 0 or pos + ilen > len(buf):
            raise PkiError("DER INTEGER length out of range")
        val = int.from_bytes(buf[pos : pos + ilen], "big")
        pos += ilen
        return val

    r = read_int()
    s = read_int()
    if pos != len(buf):
        raise PkiError("trailing bytes after DER signature")
    if not (0 < r < 1 << 256 and 0 < s < 1 << 256):
        raise PkiError("DER signature r/s out of range for P-256")
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


# ---------------------------------------------------------------------------
# Audit item builder (DynamoDB watcher-pki-audit)
# ---------------------------------------------------------------------------

# Audit retention: forever (no DynamoDB TTL -- John 2026-09-27).
# NOTE: AUDIT_TTL_SECONDS / the "ttl" item field were removed 2026-09-27;
# the table has no TTL configured, so audit entries are never auto-deleted.


def build_audit_item(
    *,
    kind: str,
    now: float,
    principal: str,
    cmd_sha256: str = "",
    cmd: str = "",
    device: str = "",
    cert_serial: int = -1,
    kms_key_arn: str = "",
    result: str,
    reason: str = "",
) -> dict:
    """Build one audit-table item. ``kind`` is ``"SIGN"`` or ``"ROTATION"``.

    Field set follows PKI_DESIGN_BRIEF.md exactly (timestamp, principal,
    cmd_sha256, cmd, device, cert_serial, kms_key_arn, result, reason)
    plus the storage fields ``pk``/``sk``.
    """
    ts = datetime.fromtimestamp(now, tz=timezone.utc)
    ts_iso = ts.strftime("%Y-%m-%dT%H:%M:%S.%fZ")
    return {
        "pk": f"{kind}#{ts.strftime('%Y-%m-%d')}",
        "sk": f"{ts_iso}#{secrets.token_hex(4)}",
        "timestamp": ts_iso,
        "principal": principal,
        "cmd_sha256": cmd_sha256,
        "cmd": cmd,
        "device": device,
        "cert_serial": cert_serial,
        "kms_key_arn": kms_key_arn,
        "result": result,
        "reason": reason,
    }


# ---------------------------------------------------------------------------
# Ed25519 verification (RFC 8032), pure Python
# ---------------------------------------------------------------------------
# Used ONLY by the rotation path (verify Tier 0 signature over a Tier 1
# cert). Cross-checked against PyNaCl in tests/test_pki_common.py.

_Q = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _Q - 2, _Q)) % _Q
_I = pow(2, (_Q - 1) // 4, _Q)  # sqrt(-1) mod q


def _xrecover(y: int) -> int:
    xx = (y * y - 1) * pow(_D * y * y + 1, _Q - 2, _Q) % _Q
    x = pow(xx, (_Q + 3) // 8, _Q)
    if (x * x - xx) % _Q != 0:
        x = (x * _I) % _Q
    if x & 1:
        x = _Q - x
    return x


def _edwards_add(P: tuple[int, int, int, int], Q: tuple[int, int, int, int]):
    x1, y1, z1, t1 = P
    x2, y2, z2, t2 = Q
    a = (y1 - x1) * (y2 - x2) % _Q
    b = (y1 + x1) * (y2 + x2) % _Q
    c = t1 * (2 * _D) % _Q * t2 % _Q
    d = (2 * z1 % _Q) * z2 % _Q
    e = (b - a) % _Q
    f = (d - c) % _Q
    g = (d + c) % _Q
    h = (b + a) % _Q
    return (e * f % _Q, g * h % _Q, f * g % _Q, e * h % _Q)


def _scalarmult(P: tuple[int, int, int, int], e: int):
    if e < 0:
        raise ValueError("negative scalar")
    Q = (0, 1, 1, 0)  # identity in extended coordinates
    while e:
        if e & 1:
            Q = _edwards_add(Q, P)
        P = _edwards_add(P, P)
        e >>= 1
    return Q


def _encodepoint(P: tuple[int, int, int, int]) -> bytes:
    x, y, z, _t = P
    inv_z = pow(z, _Q - 2, _Q)
    x = x * inv_z % _Q
    y = y * inv_z % _Q
    return ((y | ((x & 1) << 255))).to_bytes(32, "little")


def _decodepoint(s: bytes):
    if len(s) != 32:
        raise ValueError("point encoding must be 32 bytes")
    y = int.from_bytes(s, "little")
    sign = (y >> 255) & 1
    y &= (1 << 255) - 1
    if y >= _Q:
        raise ValueError("y coordinate out of range")
    x = _xrecover(y)
    if (x & 1) != sign:
        x = _Q - x
    return (x, y, 1, x * y % _Q)


_Bx = _xrecover(4 * pow(5, _Q - 2, _Q) % _Q)
_By = 4 * pow(5, _Q - 2, _Q) % _Q
_BASE = (_Bx, _By, 1, _Bx * _By % _Q)


def ed25519_verify(sig: bytes, msg: bytes, pubkey: bytes) -> bool:
    """Return True iff ``sig`` is a valid Ed25519 signature over ``msg``."""
    if len(sig) != 64 or len(pubkey) != 32:
        return False
    try:
        a_pt = _decodepoint(pubkey)
        r_pt = _decodepoint(sig[:32])
    except ValueError:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= _L:
        return False
    h = int.from_bytes(
        hashlib.sha512(sig[:32] + pubkey + msg).digest(), "little"
    )
    lhs = _scalarmult(_BASE, s)
    rhs = _edwards_add(r_pt, _scalarmult(a_pt, h))
    return _encodepoint(lhs) == _encodepoint(rhs)


# ---------------------------------------------------------------------------
# Tier 1 certificate verification (rotation path)
# ---------------------------------------------------------------------------


def verify_tier1_cert(cert: object, tier0_pubkey_hex: str, now: float) -> dict:
    """Verify a Tier 1 certificate against the Tier 0 Ed25519 pubkey.

    Order per the brief: parse -> recompute canonical bytes -> Ed25519
    verify -> check ``issued_at <= now <= expires_at``. Returns the parsed
    cert dict on success; raises :class:`CertValidationError` otherwise.
    """
    if not isinstance(cert, dict):
        raise CertValidationError("tier1_cert must be a JSON object")
    unknown = set(cert) - CERT_FIELDS
    if unknown:
        raise CertValidationError(f"unknown cert field(s): {sorted(unknown)}")
    missing = CERT_FIELDS - {"tier0_sig_hex"} - set(cert)
    # tier0_sig_hex presence is checked explicitly below for a better error.
    if missing:
        raise CertValidationError(f"missing cert field(s): {sorted(missing)}")

    if cert.get("format") != CERT_FORMAT:
        raise CertValidationError(f"cert format must be {CERT_FORMAT!r}")
    if cert.get("tier") != 1:
        raise CertValidationError("cert tier must be 1")
    if cert.get("subject") != TIER1_CERT_SUBJECT:
        raise CertValidationError(f"cert subject must be {TIER1_CERT_SUBJECT!r}")
    if not isinstance(cert.get("serial"), int) or cert["serial"] < 0:
        raise CertValidationError("cert serial must be a non-negative integer")

    sig_hex = cert.get("tier0_sig_hex")
    if not isinstance(sig_hex, str) or len(sig_hex) != 128 or not HEX_RE.fullmatch(sig_hex):
        raise CertValidationError("tier0_sig_hex must be 128 hex chars (64 bytes)")
    if not isinstance(tier0_pubkey_hex, str) or len(tier0_pubkey_hex) != 64 or not HEX_RE.fullmatch(
        tier0_pubkey_hex
    ):
        raise CertValidationError("Tier 0 pubkey misconfigured (must be 64 hex chars)")

    canonical = canonical_cert_bytes(cert)  # raises on bad algo/pubkey/timestamps
    if not ed25519_verify(
        bytes.fromhex(sig_hex), canonical, bytes.fromhex(tier0_pubkey_hex)
    ):
        raise CertValidationError("Tier 0 Ed25519 signature verification failed")

    issued = parse_iso8601(cert["issued_at"], "issued_at")
    expires = parse_iso8601(cert["expires_at"], "expires_at")
    if not (issued.timestamp() <= now <= expires.timestamp()):
        raise CertValidationError("certificate is not currently valid (issued_at/expiry window)")

    return cert
