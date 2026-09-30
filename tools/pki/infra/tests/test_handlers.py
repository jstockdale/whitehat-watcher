"""Handler-level tests with a stubbed boto3 (no AWS, no moto).

Covers: sign happy path + 400/503 paths, pubkey endpoint, audit query
paths, rotation happy path + rejections. The fake KMS asserts it receives
exactly the canonical signing bytes.
"""

import base64
import hashlib
import importlib.util
import json
import os
import sys
import time
import types

import pytest
from nacl.bindings import crypto_sign, crypto_sign_keypair

INFRA = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FUNC = os.path.join(INFRA, "lambda", "functions")


# Fake boto3.dynamodb.conditions (Key/Attr) for the audit_log handler.
# Supports the `&` composition the handler uses; FakeTable only records kwargs.
class _Cond:
    def __init__(self, *parts):
        self.parts = parts

    def __and__(self, other):
        return _Cond(self, "&", other)

    def __repr__(self):
        return f"Cond{self.parts!r}"


class _KeyOrAttr:
    _kind = "?"

    def __init__(self, name):
        self.name = name

    def eq(self, v):
        return _Cond(self._kind, self.name, "eq", v)

    def gte(self, v):
        return _Cond(self._kind, self.name, "gte", v)


class Key(_KeyOrAttr):
    _kind = "key"


class Attr(_KeyOrAttr):
    _kind = "attr"


_fake_conditions = types.ModuleType("boto3.dynamodb.conditions")
_fake_conditions.Key = Key
_fake_conditions.Attr = Attr
_fake_dynamodb_pkg = types.ModuleType("boto3.dynamodb")
_fake_dynamodb_pkg.conditions = _fake_conditions
sys.modules["boto3.dynamodb"] = _fake_dynamodb_pkg
sys.modules["boto3.dynamodb.conditions"] = _fake_conditions


# ---------------------------------------------------------------------------
# Fake AWS
# ---------------------------------------------------------------------------


class ParameterNotFound(Exception):
    pass


class FakeExceptions:
    ParameterNotFound = ParameterNotFound


def _der(r: int, s: int) -> bytes:
    def enc(v: int) -> bytes:
        b = v.to_bytes((v.bit_length() + 7) // 8 or 1, "big")
        if b[0] & 0x80:
            b = b"\x00" + b
        return b"\x02" + bytes([len(b)]) + b

    body = enc(r) + enc(s)
    return b"\x30" + bytes([len(body)]) + body


CANNED_R = int("aa" * 32, 16)
CANNED_S = int("bb" * 32, 16)
CANNED_DER = _der(CANNED_R, CANNED_S)
CANNED_KEY_ID = "arn:aws:kms:us-west-2:123456789012:key/aaaa-bbbb"


class FakeKMS:
    exceptions = FakeExceptions()

    def __init__(self):
        self.sign_calls = []

    def sign(self, KeyId, Message, MessageType, SigningAlgorithm):
        assert MessageType == "RAW"
        assert SigningAlgorithm == "ECDSA_SHA_256"
        self.sign_calls.append({"KeyId": KeyId, "Message": Message})
        return {"Signature": CANNED_DER, "KeyId": CANNED_KEY_ID}


class FakeSSM:
    exceptions = FakeExceptions()

    def __init__(self, params=None):
        self.params = dict(params or {})
        self.put_calls = []

    def get_parameter(self, Name, WithDecryption=False):
        if Name not in self.params:
            raise ParameterNotFound(Name)
        return {"Parameter": {"Value": self.params[Name]}}

    def put_parameter(self, Name, Value, Type, Overwrite):
        self.put_calls.append({"Name": Name, "Value": Value, "Type": Type})
        self.params[Name] = Value
        return {"Version": 2}


class FakeTable:
    def __init__(self, query_result=None, scan_result=None):
        self.items = []
        self.query_result = query_result
        self.scan_result = scan_result
        self.last_query = None
        self.last_scan = None

    def put_item(self, Item):
        self.items.append(Item)

    def query(self, **kw):
        self.last_query = kw
        return self.query_result or {"Items": []}

    def scan(self, **kw):
        self.last_scan = kw
        return self.scan_result or {"Items": []}


class FakeDynamo:
    def __init__(self, table):
        self._table = table

    def Table(self, name):
        return self._table


class FakeBoto3:
    def __init__(self, kms=None, ssm=None, table=None):
        self._kms = kms or FakeKMS()
        self._ssm = ssm or FakeSSM()
        self._table = table or FakeTable()

    def client(self, name):
        return {"kms": self._kms, "ssm": self._ssm}[name]

    def resource(self, name):
        assert name == "dynamodb"
        return FakeDynamo(self._table)


def load_handler(mod_name: str, func_dir: str, fake: FakeBoto3, env: dict):
    for k, v in env.items():
        os.environ[k] = v
    sys.modules["boto3"] = fake  # stub before import
    path = os.path.join(FUNC, func_dir, "handler.py")
    spec = importlib.util.spec_from_file_location(mod_name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[mod_name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def cert_json():
    return json.dumps(
        {
            "format": "watcher-tier1-cert-v1",
            "tier": 1,
            "subject": "watcher-tier1-provisioning",
            "algo": "ecdsa-p256-sha256",
            "p256_pubkey_hex": "04" + "11" * 64,
            "issued_at": "2026-01-01T00:00:00Z",
            "expires_at": "2027-01-01T00:00:00Z",
            "serial": 4,
            "tier0_sig_hex": "00" * 64,
        }
    )


def iso_now_plus(seconds: float) -> str:
    from datetime import datetime, timezone

    return (
        datetime.fromtimestamp(time.time() + seconds, tz=timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )
    )


def valid_command():
    return {
        "format": "watcher-provision-v1",
        "cmd": "install_commander_key",
        "device": "watcher-dev-01",
        "nonce": "ab" * 16,
        "issued_at": iso_now_plus(-60),
        "expires_at": iso_now_plus(840),
        "params": {"pubkey_hex": "cd" * 32},
    }


SIGN_ENV = {
    "AUDIT_TABLE_NAME": "watcher-pki-audit",
    "TIER1_CERT_PARAM_NAME": "/watcher/pki/tier1/current-cert",
    "KMS_KEY_ID": "arn:aws:kms:us-west-2:123456789012:alias/watcher-tier1-provisioning",
}


# ---------------------------------------------------------------------------
# sign_provisioning
# ---------------------------------------------------------------------------


def test_sign_happy_path(cert_json):
    fake = FakeBoto3(ssm=FakeSSM({SIGN_ENV["TIER1_CERT_PARAM_NAME"]: cert_json}))
    mod = load_handler("h_sign_ok", "sign_provisioning", fake, SIGN_ENV)

    event = {
        "body": json.dumps({"command": valid_command()}),
        "requestContext": {"identity": {"userArn": "arn:aws:iam::123:user/john"}},
    }
    resp = mod.handler(event, None)
    assert resp["statusCode"] == 200
    body = json.loads(resp["body"])
    assert body["signature_hex"] == (CANNED_R.to_bytes(32, "big") + CANNED_S.to_bytes(32, "big")).hex()
    assert body["cert"]["serial"] == 4
    assert body["key_id"] == CANNED_KEY_ID

    # KMS received exactly the canonical bytes (asserted precisely in
    # test_sign_kms_gets_canonical_bytes below).

    # Audit entry: signed, with canonical hash (not the command).
    assert len(fake._table.items) == 1
    item = fake._table.items[0]
    assert item["result"] == "signed"
    assert item["cmd"] == "install_commander_key"
    assert item["device"] == "watcher-dev-01"
    assert item["cert_serial"] == 4
    assert item["kms_key_arn"] == CANNED_KEY_ID
    assert item["principal"] == "arn:aws:iam::123:user/john"
    assert item["cmd_sha256"] == hashlib.sha256(
        fake._kms.sign_calls[0]["Message"]
    ).hexdigest()
    assert "pubkey_hex" not in json.dumps(item)  # command itself never stored
    assert "ttl" not in item  # no TTL: audit entries are kept forever


def test_sign_kms_gets_canonical_bytes(cert_json):
    """Explicit check that KMS signs the canonical bytes, nothing else."""
    fake = FakeBoto3(ssm=FakeSSM({SIGN_ENV["TIER1_CERT_PARAM_NAME"]: cert_json}))
    mod = load_handler("h_sign_canon", "sign_provisioning", fake, SIGN_ENV)
    command = valid_command()
    mod.handler({"body": json.dumps({"command": command})}, None)
    from pki_common import canonical_provision_bytes

    assert fake._kms.sign_calls[0]["Message"] == canonical_provision_bytes(command)


def test_sign_rejects_bad_cmd_without_touching_kms(cert_json):
    fake = FakeBoto3(ssm=FakeSSM({SIGN_ENV["TIER1_CERT_PARAM_NAME"]: cert_json}))
    mod = load_handler("h_sign_bad", "sign_provisioning", fake, SIGN_ENV)
    command = valid_command()
    command["cmd"] = "reboot"  # not a provisioning command
    resp = mod.handler({"body": json.dumps({"command": command})}, None)
    assert resp["statusCode"] == 400
    assert "cmd must be one of" in json.loads(resp["body"])["reason"]
    assert fake._kms.sign_calls == []  # KMS never called
    item = fake._table.items[0]
    assert item["result"] == "rejected"
    assert item["kms_key_arn"] == ""


def test_sign_rejects_expired(cert_json):
    fake = FakeBoto3(ssm=FakeSSM({SIGN_ENV["TIER1_CERT_PARAM_NAME"]: cert_json}))
    mod = load_handler("h_sign_exp", "sign_provisioning", fake, SIGN_ENV)
    command = valid_command()
    command["issued_at"] = iso_now_plus(-1200)
    command["expires_at"] = iso_now_plus(-300)
    resp = mod.handler({"body": json.dumps({"command": command})}, None)
    assert resp["statusCode"] == 400
    assert fake._kms.sign_calls == []


def test_sign_rejects_non_json_body(cert_json):
    fake = FakeBoto3(ssm=FakeSSM({SIGN_ENV["TIER1_CERT_PARAM_NAME"]: cert_json}))
    mod = load_handler("h_sign_njson", "sign_provisioning", fake, SIGN_ENV)
    resp = mod.handler({"body": "this is not json"}, None)
    assert resp["statusCode"] == 400
    assert fake._kms.sign_calls == []


def test_sign_503_when_cert_not_provisioned():
    fake = FakeBoto3(ssm=FakeSSM({}))
    mod = load_handler("h_sign_nocert", "sign_provisioning", fake, SIGN_ENV)
    resp = mod.handler({"body": json.dumps({"command": valid_command()})}, None)
    assert resp["statusCode"] == 503
    assert fake._kms.sign_calls == []


# ---------------------------------------------------------------------------
# get_tier1_pubkey
# ---------------------------------------------------------------------------


def test_pubkey_happy_path(cert_json):
    fake = FakeBoto3(ssm=FakeSSM({"/watcher/pki/tier1/current-cert": cert_json}))
    mod = load_handler(
        "h_pubkey_ok", "get_tier1_pubkey", fake,
        {"TIER1_CERT_PARAM_NAME": "/watcher/pki/tier1/current-cert"},
    )
    resp = mod.handler({}, None)
    assert resp["statusCode"] == 200
    body = json.loads(resp["body"])
    assert body["p256_pubkey_hex"] == "04" + "11" * 64
    assert body["cert"]["serial"] == 4


def test_pubkey_503_when_missing():
    fake = FakeBoto3(ssm=FakeSSM({}))
    mod = load_handler(
        "h_pubkey_missing", "get_tier1_pubkey", fake,
        {"TIER1_CERT_PARAM_NAME": "/watcher/pki/tier1/current-cert"},
    )
    assert mod.handler({}, None)["statusCode"] == 503


# ---------------------------------------------------------------------------
# audit_log
# ---------------------------------------------------------------------------


def _audit_mod(fake, result):
    return load_handler(
        f"h_audit_{id(fake)}", "audit_log", fake, {"AUDIT_TABLE_NAME": "watcher-pki-audit"}
    ), result


def test_audit_query_by_cmd_uses_gsi():
    items = [{"pk": "SIGN#2026-09-27", "cmd": "install_commander_key"}]
    table = FakeTable(query_result={"Items": items})
    fake = FakeBoto3(table=table)
    mod, _ = _audit_mod(fake, None)
    resp = mod.handler(
        {"queryStringParameters": {"cmd": "install_commander_key", "since": "2026-09-01T00:00:00Z"}},
        None,
    )
    assert resp["statusCode"] == 200
    assert json.loads(resp["body"])["entries"] == items
    assert table.last_query["IndexName"] == "by-cmd"
    assert table.last_query["ScanIndexForward"] is False


def test_audit_scan_without_cmd():
    table = FakeTable(scan_result={"Items": [{"a": 1}], "LastEvaluatedKey": {"pk": "x"}})
    fake = FakeBoto3(table=table)
    mod, _ = _audit_mod(fake, None)
    resp = mod.handler({"queryStringParameters": {"limit": "10"}}, None)
    body = json.loads(resp["body"])
    assert resp["statusCode"] == 200
    assert body["entries"] == [{"a": 1}]
    assert body["next_token"] is not None
    # token round-trips
    resp2 = mod.handler(
        {"queryStringParameters": {"next_token": body["next_token"]}}, None
    )
    assert resp2["statusCode"] == 200
    assert table.last_scan["ExclusiveStartKey"] == {"pk": "x"}


def test_audit_bad_limit_and_token():
    fake = FakeBoto3()
    mod, _ = _audit_mod(fake, None)
    # oversized limit is clamped to MAX_LIMIT (200), not an error
    assert mod.handler({"queryStringParameters": {"limit": "9999"}}, None)["statusCode"] == 200
    assert fake._table.last_scan["Limit"] == 200
    assert mod.handler({"queryStringParameters": {"limit": "0"}}, None)["statusCode"] == 400
    assert mod.handler({"queryStringParameters": {"limit": "abc"}}, None)["statusCode"] == 400
    assert mod.handler({"queryStringParameters": {"next_token": "!!!"}} , None)["statusCode"] == 400


# ---------------------------------------------------------------------------
# rotation
# ---------------------------------------------------------------------------

ROT_ENV = {
    "AUDIT_TABLE_NAME": "watcher-pki-audit",
    "TIER1_CERT_PARAM_NAME": "/watcher/pki/tier1/current-cert",
    "TIER0_PUBKEY_PARAM_NAME": "/watcher/pki/tier0/pubkey-hex",
}


def _tier0_signed_cert(serial: int, tier0_priv: bytes, tamper: bool = False):
    from pki_common import canonical_cert_bytes

    cert = {
        "format": "watcher-tier1-cert-v1",
        "tier": 1,
        "subject": "watcher-tier1-provisioning",
        "algo": "ecdsa-p256-sha256",
        "p256_pubkey_hex": "04" + "33" * 64,
        "issued_at": "2026-01-01T00:00:00Z",
        "expires_at": "2027-06-01T00:00:00Z",
        "serial": serial,
        "tier0_sig_hex": "",
    }
    sig = crypto_sign(canonical_cert_bytes(cert), tier0_priv)[:64]
    if tamper:
        sig = bytearray(sig)
        sig[5] ^= 1
        sig = bytes(sig)
    cert["tier0_sig_hex"] = sig.hex()
    return cert


def test_rotation_happy_path():
    tier0_pub, tier0_priv = crypto_sign_keypair()
    current = {"serial": 4, "p256_pubkey_hex": "04" + "11" * 64}
    fake = FakeBoto3(
        ssm=FakeSSM(
            {
                ROT_ENV["TIER0_PUBKEY_PARAM_NAME"]: tier0_pub.hex(),
                ROT_ENV["TIER1_CERT_PARAM_NAME"]: json.dumps(current),
            }
        )
    )
    mod = load_handler("h_rot_ok", "rotation", fake, ROT_ENV)
    new_cert = _tier0_signed_cert(serial=5, tier0_priv=tier0_priv)
    resp = mod.handler({"body": json.dumps({"tier1_cert": new_cert})}, None)
    assert resp["statusCode"] == 200
    body = json.loads(resp["body"])
    assert body["serial"] == 5 and body["previous_serial"] == 4
    # stored as current
    assert len(fake._ssm.put_calls) == 1
    stored = json.loads(fake._ssm.put_calls[0]["Value"])
    assert stored["serial"] == 5
    assert fake._ssm.put_calls[0]["Type"] == "String"
    # audit
    item = fake._table.items[0]
    assert item["result"] == "rotated"
    assert item["cmd"] == "rotate_tier1"
    assert item["cert_serial"] == 5
    assert item["kms_key_arn"] == ""  # rotation touches no KMS key


def test_rotation_rejects_bad_tier0_signature():
    tier0_pub, tier0_priv = crypto_sign_keypair()
    fake = FakeBoto3(
        ssm=FakeSSM({ROT_ENV["TIER0_PUBKEY_PARAM_NAME"]: tier0_pub.hex()})
    )
    mod = load_handler("h_rot_badsig", "rotation", fake, ROT_ENV)
    cert = _tier0_signed_cert(serial=5, tier0_priv=tier0_priv, tamper=True)
    resp = mod.handler({"body": json.dumps({"tier1_cert": cert})}, None)
    assert resp["statusCode"] == 400
    assert fake._ssm.put_calls == []
    assert fake._table.items[0]["result"] == "rejected"


def test_rotation_rejects_non_increasing_serial():
    tier0_pub, tier0_priv = crypto_sign_keypair()
    fake = FakeBoto3(
        ssm=FakeSSM(
            {
                ROT_ENV["TIER0_PUBKEY_PARAM_NAME"]: tier0_pub.hex(),
                ROT_ENV["TIER1_CERT_PARAM_NAME"]: json.dumps({"serial": 9}),
            }
        )
    )
    mod = load_handler("h_rot_serial", "rotation", fake, ROT_ENV)
    cert = _tier0_signed_cert(serial=9, tier0_priv=tier0_priv)  # not > 9
    resp = mod.handler({"body": json.dumps({"tier1_cert": cert})}, None)
    assert resp["statusCode"] == 400
    assert "serial" in json.loads(resp["body"])["reason"]
    assert fake._ssm.put_calls == []


def test_rotation_500_when_tier0_pubkey_missing():
    fake = FakeBoto3(ssm=FakeSSM({}))
    mod = load_handler("h_rot_notier0", "rotation", fake, ROT_ENV)
    cert = _tier0_signed_cert(serial=1, tier0_priv=crypto_sign_keypair()[1])
    resp = mod.handler({"body": json.dumps({"tier1_cert": cert})}, None)
    assert resp["statusCode"] == 500
    assert fake._ssm.put_calls == []
