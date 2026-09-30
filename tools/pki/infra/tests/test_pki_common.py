"""Unit tests for pki_common: the security-critical pre-sign validation.

Valid + invalid payloads per the task spec:
  bad cmd, expired, oversized window, malformed params, plus canonical-byte
  vectors, DER conversion, Tier 0 cert verification.

PyNaCl is used ONLY here (test oracle for the pure-Python Ed25519 in
pki_common). It is never bundled into a Lambda.
"""

import hashlib
import json
import struct
import time

import pytest
from nacl.bindings import crypto_sign, crypto_sign_keypair
from nacl.exceptions import BadSignatureError

import pki_common
from pki_common import (
    CommandValidationError,
    CertValidationError,
    PkiError,
    canonical_cert_bytes,
    canonical_provision_bytes,
    der_to_raw_sig,
    ed25519_verify,
    validate_provision_command,
    verify_tier1_cert,
)

NOW = 1_789_000_000.0  # fixed clock: 2026-09-09T... (see iso helpers)


def iso(ts: float) -> str:
    from datetime import datetime, timezone

    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def base_cmd(**over):
    cmd = {
        "format": "watcher-provision-v1",
        "cmd": "install_commander_key",
        "device": "watcher-dev-01",
        "nonce": "ab" * 16,
        "issued_at": iso(NOW - 60),
        "expires_at": iso(NOW + 840),  # 15-min window exactly
        "params": {"pubkey_hex": "cd" * 32},
    }
    cmd.update(over)
    return cmd


# ---------------------------------------------------------------------------
# Happy paths: one valid command per cmd value
# ---------------------------------------------------------------------------


def test_valid_install_commander_key():
    canon = validate_provision_command(base_cmd(), NOW)
    assert canon.startswith(b"WATCHER-PROVISION-v1")


def test_valid_install_telemetry_seed():
    canon = validate_provision_command(
        base_cmd(cmd="install_telemetry_seed", params={"seed_fp": "6be828a38270d363"}),
        NOW,
    )
    assert isinstance(canon, bytes)


def test_valid_rotate_tier1():
    canon = validate_provision_command(
        base_cmd(
            cmd="rotate_tier1",
            params={
                "p256_pubkey_hex": "04" + "11" * 64,
                "expires_at": iso(NOW + 30 * 86400),
            },
        ),
        NOW,
    )
    assert isinstance(canon, bytes)


def test_valid_factory_reset_keys():
    canon = validate_provision_command(
        base_cmd(cmd="factory_reset_keys", params={"confirm": True}), NOW
    )
    assert isinstance(canon, bytes)


def test_device_wildcard_ok():
    validate_provision_command(base_cmd(device="*"), NOW)


# ---------------------------------------------------------------------------
# Canonical byte vector: hand-computed layout
# ---------------------------------------------------------------------------


def test_canonical_bytes_exact_layout():
    cmd = base_cmd()
    canon = canonical_provision_bytes(cmd)
    nonce_raw = bytes.fromhex("ab" * 16)
    params_json = json.dumps(
        {"pubkey_hex": "cd" * 32}, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    expected = (
        b"WATCHER-PROVISION-v1"
        + struct.pack(">H", len(b"install_commander_key"))
        + b"install_commander_key"
        + struct.pack(">H", len(b"watcher-dev-01"))
        + b"watcher-dev-01"
        + nonce_raw
        + struct.pack(">Q", int(NOW - 60))
        + struct.pack(">Q", int(NOW + 840))
        + params_json
    )
    assert canon == expected


def test_canonical_json_key_order_and_separators():
    params = {"z": 1, "a": [1, 2], "m": {"y": True}}
    assert pki_common.canonical_json(params) == b'{"a":[1,2],"m":{"y":true},"z":1}'


# ---------------------------------------------------------------------------
# Rejections: envelope
# ---------------------------------------------------------------------------


def test_reject_non_dict():
    with pytest.raises(CommandValidationError):
        validate_provision_command("not a dict", NOW)


def test_reject_unknown_top_level_field():
    with pytest.raises(CommandValidationError, match="unknown top-level"):
        validate_provision_command(base_cmd(extra=1), NOW)


def test_reject_missing_field():
    cmd = base_cmd()
    del cmd["nonce"]
    with pytest.raises(CommandValidationError, match="missing required"):
        validate_provision_command(cmd, NOW)


def test_reject_bad_format():
    with pytest.raises(CommandValidationError, match="format"):
        validate_provision_command(base_cmd(format="watcher-provision-v9"), NOW)


def test_reject_bad_cmd():
    with pytest.raises(CommandValidationError, match="cmd must be one of"):
        validate_provision_command(base_cmd(cmd="reboot"), NOW)


def test_reject_empty_cmd():
    with pytest.raises(CommandValidationError):
        validate_provision_command(base_cmd(cmd=""), NOW)


# ---------------------------------------------------------------------------
# Rejections: device / nonce
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "device", ["", "has space", "semi;colon", "x" * 65, "watcher/dev", None, 42]
)
def test_reject_bad_device(device):
    with pytest.raises(CommandValidationError, match="device"):
        validate_provision_command(base_cmd(device=device), NOW)


@pytest.mark.parametrize("nonce", ["", "zz" * 16, "ab" * 15, "ab" * 17, None, 123])
def test_reject_bad_nonce(nonce):
    with pytest.raises(CommandValidationError, match="nonce"):
        validate_provision_command(base_cmd(nonce=nonce), NOW)


# ---------------------------------------------------------------------------
# Rejections: time window
# ---------------------------------------------------------------------------


def test_reject_expired():
    with pytest.raises(CommandValidationError, match="expired"):
        validate_provision_command(
            base_cmd(issued_at=iso(NOW - 1200), expires_at=iso(NOW - 300)), NOW
        )


def test_reject_oversized_window():
    with pytest.raises(CommandValidationError, match="15-minute"):
        validate_provision_command(
            base_cmd(issued_at=iso(NOW - 60), expires_at=iso(NOW + 901)), NOW
        )


def test_reject_inverted_window():
    with pytest.raises(CommandValidationError, match="after issued_at"):
        validate_provision_command(
            base_cmd(issued_at=iso(NOW), expires_at=iso(NOW - 10)), NOW
        )


def test_reject_issued_far_future():
    with pytest.raises(CommandValidationError, match="future"):
        validate_provision_command(
            base_cmd(issued_at=iso(NOW + 3600), expires_at=iso(NOW + 3600 + 600)),
            NOW,
        )


def test_accept_issued_slightly_future_within_skew():
    validate_provision_command(
        base_cmd(issued_at=iso(NOW + 30), expires_at=iso(NOW + 30 + 600)), NOW
    )


def test_reject_naive_timestamp():
    with pytest.raises(CommandValidationError, match="timezone"):
        validate_provision_command(
            base_cmd(issued_at="2026-09-09T00:00:00", expires_at=iso(NOW + 600)), NOW
        )


def test_reject_garbage_timestamp():
    with pytest.raises(CommandValidationError, match="ISO-8601"):
        validate_provision_command(
            base_cmd(issued_at="yesterday", expires_at=iso(NOW + 600)), NOW
        )


# ---------------------------------------------------------------------------
# Rejections: params shape per cmd
# ---------------------------------------------------------------------------


def test_reject_params_not_dict():
    with pytest.raises(CommandValidationError, match="params must be an object"):
        validate_provision_command(base_cmd(params=["x"]), NOW)


@pytest.mark.parametrize(
    "params",
    [
        {},
        {"pubkey_hex": "cd" * 32, "extra": 1},  # extra key
        {"pubkey_hex": "cd" * 31},  # too short
        {"pubkey_hex": "cd" * 33},  # too long
        {"pubkey_hex": "zz" * 32},  # not hex
        {"pubkey_hex": 123},
        {"wrong": "cd" * 32},  # missing key
    ],
)
def test_reject_bad_commander_key_params(params):
    with pytest.raises(CommandValidationError):
        validate_provision_command(base_cmd(params=params), NOW)


@pytest.mark.parametrize(
    "params",
    [
        {},
        {"seed_fp": "xyz"},  # not hex
        {"seed_fp": "ab" * 4},  # too short
        {"seed_fp": "ab" * 65},  # too long
        {"seed_fp": "6be828a38270d363", "seed": "deadbeef"},  # extra: seed itself!
        {"other": "6be828a38270d363"},
    ],
)
def test_reject_bad_telemetry_seed_params(params):
    with pytest.raises(CommandValidationError):
        validate_provision_command(
            base_cmd(cmd="install_telemetry_seed", params=params), NOW
        )


@pytest.mark.parametrize(
    "params",
    [
        {"p256_pubkey_hex": "04" + "11" * 64},  # missing expires_at
        {"expires_at": iso(NOW + 1000)},  # missing pubkey
        {
            "p256_pubkey_hex": "05" + "11" * 64,  # not 04-prefixed
            "expires_at": iso(NOW + 1000),
        },
        {
            "p256_pubkey_hex": "04" + "11" * 63,  # too short
            "expires_at": iso(NOW + 1000),
        },
        {
            "p256_pubkey_hex": "04" + "11" * 64,
            "expires_at": iso(NOW - 1000),  # already expired
        },
        {
            "p256_pubkey_hex": "04" + "11" * 64,
            "expires_at": iso(NOW + 1000),
            "extra": 1,
        },
    ],
)
def test_reject_bad_rotate_tier1_params(params):
    with pytest.raises(CommandValidationError):
        validate_provision_command(base_cmd(cmd="rotate_tier1", params=params), NOW)


@pytest.mark.parametrize(
    "params",
    [
        {},
        {"confirm": False},
        {"confirm": 1},  # truthy but not boolean True
        {"confirm": "true"},
        {"confirm": True, "extra": 1},
    ],
)
def test_reject_bad_factory_reset_params(params):
    with pytest.raises(CommandValidationError):
        validate_provision_command(
            base_cmd(cmd="factory_reset_keys", params=params), NOW
        )


# ---------------------------------------------------------------------------
# DER -> raw r||s
# ---------------------------------------------------------------------------


def _der_encode(r: int, s: int) -> bytes:
    def enc_int(v: int) -> bytes:
        b = v.to_bytes((v.bit_length() + 7) // 8 or 1, "big")
        if b[0] & 0x80:
            b = b"\x00" + b
        return b"\x02" + bytes([len(b)]) + b

    body = enc_int(r) + enc_int(s)
    return b"\x30" + bytes([len(body)]) + body


def test_der_to_raw_roundtrip():
    r = int("9f" * 32, 16)
    s = int("01" + "00" * 31, 16)  # exercises leading-zero INTEGER padding
    raw = der_to_raw_sig(_der_encode(r, s))
    assert raw == r.to_bytes(32, "big") + s.to_bytes(32, "big")
    assert len(raw) == 64


@pytest.mark.parametrize(
    "der",
    [
        b"",
        b"\x30\x06\x02\x01\x01\x02\x01\x02extra",  # trailing bytes
        b"\x31\x06\x02\x01\x01\x02\x01\x02",  # not a SEQUENCE
        b"\x30\x06\x03\x01\x01\x02\x01\x02",  # first elem not INTEGER
        b"\x30\x02\x02\x01\x01",  # truncated (missing s)
    ],
)
def test_der_malformed_rejected(der):
    with pytest.raises(PkiError):
        der_to_raw_sig(der)


# ---------------------------------------------------------------------------
# Ed25519 pure-Python implementation vs PyNaCl oracle
# ---------------------------------------------------------------------------


def test_ed25519_matches_pynacl():
    import os

    for _ in range(5):
        pub, priv = crypto_sign_keypair()
        msg = os.urandom(64)
        signed = crypto_sign(msg, priv)
        sig = signed[:64]
        assert ed25519_verify(sig, msg, pub) is True
        # tampered signature fails
        bad = bytearray(sig)
        bad[10] ^= 1
        assert ed25519_verify(bytes(bad), msg, pub) is False
        # wrong message fails
        assert ed25519_verify(sig, msg + b"x", pub) is False
        # wrong pubkey fails
        pub2, _ = crypto_sign_keypair()
        assert ed25519_verify(sig, msg, pub2) is False


def test_ed25519_rejects_bad_lengths():
    pub, priv = crypto_sign_keypair()
    sig = crypto_sign(b"m", priv)[:64]
    assert ed25519_verify(sig[:63], b"m", pub) is False
    assert ed25519_verify(sig, b"m", pub[:31]) is False


def test_ed25519_rejects_small_order_point_pubkey():
    # Low-order public key must not verify (RFC 8032 malleability guard).
    assert ed25519_verify(b"\x00" * 64, b"msg", b"\x00" * 32) is False


# ---------------------------------------------------------------------------
# Tier 1 cert verification
# ---------------------------------------------------------------------------


def _make_cert(serial=7, **over):
    pub, priv = crypto_sign_keypair()
    cert = {
        "format": "watcher-tier1-cert-v1",
        "tier": 1,
        "subject": "watcher-tier1-provisioning",
        "algo": "ecdsa-p256-sha256",
        "p256_pubkey_hex": "04" + "22" * 64,
        "issued_at": iso(NOW - 86400),
        "expires_at": iso(NOW + 365 * 86400),
        "serial": serial,
        "tier0_sig_hex": "",  # filled below
    }
    cert.update(over)
    canon = canonical_cert_bytes(cert)
    sig = crypto_sign(canon, priv)[:64]
    cert["tier0_sig_hex"] = sig.hex()
    return cert, pub.hex(), priv


def test_cert_verify_ok():
    cert, pub_hex, _ = _make_cert()
    out = verify_tier1_cert(cert, pub_hex, NOW)
    assert out["serial"] == 7


def test_cert_canonical_bytes_exact_layout():
    cert, _, _ = _make_cert(serial=3)
    canon = canonical_cert_bytes(cert)
    expected = (
        b"WATCHER-TIER1-CERT-v1"
        + struct.pack(">H", len(b"watcher-tier1-provisioning"))
        + b"watcher-tier1-provisioning"
        + struct.pack(">B", 0x01)
        + bytes.fromhex("04" + "22" * 64)
        + struct.pack(">Q", int(NOW - 86400))
        + struct.pack(">Q", int(NOW + 365 * 86400))
        + struct.pack(">I", 3)
    )
    assert canon == expected


def test_cert_reject_tampered_sig():
    cert, pub_hex, _ = _make_cert()
    bad = bytearray.fromhex(cert["tier0_sig_hex"])
    bad[0] ^= 1
    cert["tier0_sig_hex"] = bytes(bad).hex()
    with pytest.raises(CertValidationError, match="signature verification failed"):
        verify_tier1_cert(cert, pub_hex, NOW)


def test_cert_reject_wrong_tier0_key():
    cert, _, _ = _make_cert()
    other_pub, _ = crypto_sign_keypair()
    with pytest.raises(CertValidationError, match="signature verification failed"):
        verify_tier1_cert(cert, other_pub.hex(), NOW)


def test_cert_reject_expired():
    cert, pub_hex, _ = _make_cert()
    cert["issued_at"] = iso(NOW - 2 * 365 * 86400)
    cert["expires_at"] = iso(NOW - 86400)
    # Signature no longer matches the mutated window AND the window is past;
    # either way verification must fail.
    with pytest.raises(CertValidationError):
        verify_tier1_cert(cert, pub_hex, NOW)


@pytest.mark.parametrize(
    "field,value",
    [
        ("format", "watcher-tier1-cert-v0"),
        ("tier", 2),
        ("subject", "someone-else"),
        ("serial", -1),
        ("serial", "7"),
        ("algo", "ed25519"),
    ],
)
def test_cert_reject_bad_fields(field, value):
    cert, pub_hex, _ = _make_cert()
    cert[field] = value
    with pytest.raises(CertValidationError):
        verify_tier1_cert(cert, pub_hex, NOW)


def test_cert_reject_unknown_field():
    cert, pub_hex, _ = _make_cert()
    cert["extra"] = 1
    with pytest.raises(CertValidationError, match="unknown cert field"):
        verify_tier1_cert(cert, pub_hex, NOW)


def test_cert_reject_bad_pubkey_hex():
    cert, pub_hex, _ = _make_cert()
    cert["p256_pubkey_hex"] = "05" + "22" * 64  # wrong point marker
    with pytest.raises(CertValidationError):
        verify_tier1_cert(cert, pub_hex, NOW)


def test_cert_hash_is_stable():
    """cmd/cert canonical hashing (audit cmd_sha256) is deterministic."""
    cmd = base_cmd()
    h1 = hashlib.sha256(canonical_provision_bytes(cmd)).hexdigest()
    h2 = hashlib.sha256(canonical_provision_bytes(base_cmd())).hexdigest()
    assert h1 == h2 and len(h1) == 64
