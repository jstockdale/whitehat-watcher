"""POST /rotation -- install a new Tier 1 certificate (Tier 0-signed).

Request body:  {"tier1_cert": <watcher-tier1-cert-v1 JSON>}
Response 200:  {"cert": {...}, "serial": N, "previous_serial": M}
Response 400:  cert invalid (bad Tier 0 signature, bad window, serial not
               increasing, malformed). Audit entry with reason.
Response 500:  Tier 0 pubkey not provisioned out-of-band.

Verification order per the brief: parse -> recompute canonical bytes ->
Ed25519 verify against the Tier 0 pubkey (SSM SecureString, deployed
out-of-band by John) -> validity window. Serial must strictly increase
over the current cert (replay protection for old certs).

This does NOT create a new KMS key. Full KMS key rotation is a runbook
(see DEPLOY.md): new KMS key -> Tier 0 ceremony issues cert for the new
pubkey -> POST /rotation -> move the alias -> rebake firmware anchors.
"""

import json
import os
import time

import boto3

from pki_common import (
    CertValidationError,
    build_audit_item,
    verify_tier1_cert,
)

AUDIT_TABLE = os.environ["AUDIT_TABLE_NAME"]
CERT_PARAM = os.environ["TIER1_CERT_PARAM_NAME"]
TIER0_PUBKEY_PARAM = os.environ["TIER0_PUBKEY_PARAM_NAME"]

_ssm = boto3.client("ssm")
_table = boto3.resource("dynamodb").Table(AUDIT_TABLE)


def _resp(status: int, body: dict) -> dict:
    return {
        "statusCode": status,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }


def _principal(event: dict) -> str:
    try:
        return event["requestContext"]["identity"]["userArn"]
    except (KeyError, TypeError):
        return "unknown"


def _audit(now: float, principal: str, **kw) -> None:
    try:
        _table.put_item(Item=build_audit_item(kind="ROTATION", now=now,
                                              principal=principal, **kw))
    except Exception:
        pass


def _get_tier0_pubkey_hex() -> str | None:
    try:
        resp = _ssm.get_parameter(Name=TIER0_PUBKEY_PARAM, WithDecryption=True)
        return resp["Parameter"]["Value"].strip()
    except _ssm.exceptions.ParameterNotFound:
        return None


def _get_current_cert() -> dict | None:
    try:
        resp = _ssm.get_parameter(Name=CERT_PARAM)
        return json.loads(resp["Parameter"]["Value"])
    except _ssm.exceptions.ParameterNotFound:
        return None


def handler(event: dict, context) -> dict:
    now = time.time()
    principal = _principal(event)

    try:
        body = json.loads(event.get("body") or "")
    except (json.JSONDecodeError, TypeError):
        _audit(now, principal, cmd="rotate_tier1", device="n/a",
               result="rejected", reason="request body is not JSON")
        return _resp(400, {"error": "invalid request", "reason": "body must be JSON"})

    cert = body.get("tier1_cert") if isinstance(body, dict) else None

    tier0_pubkey_hex = _get_tier0_pubkey_hex()
    if not tier0_pubkey_hex:
        _audit(now, principal, cmd="rotate_tier1", device="n/a",
               result="error", reason="tier0 pubkey not provisioned out-of-band")
        return _resp(500, {"error": "tier0 trust anchor not provisioned"})

    try:
        verified = verify_tier1_cert(cert, tier0_pubkey_hex, now)
    except CertValidationError as exc:
        _audit(now, principal, cmd="rotate_tier1", device="n/a",
               result="rejected", reason=str(exc))
        return _resp(400, {"error": "cert rejected", "reason": str(exc)})

    current = _get_current_cert()
    previous_serial = int(current["serial"]) if current else -1
    if verified["serial"] <= previous_serial:
        reason = (
            f"cert serial {verified['serial']} does not increase over "
            f"current serial {previous_serial} (replay protection)"
        )
        _audit(now, principal, cmd="rotate_tier1", device="n/a",
               cert_serial=verified["serial"], result="rejected", reason=reason)
        return _resp(400, {"error": "cert rejected", "reason": reason})

    _ssm.put_parameter(
        Name=CERT_PARAM,
        Value=json.dumps(verified),
        Type="String",
        Overwrite=True,
    )
    _audit(now, principal, cmd="rotate_tier1", device="n/a",
           cert_serial=verified["serial"], result="rotated",
           reason=f"serial {previous_serial} -> {verified['serial']}")
    return _resp(
        200,
        {"cert": verified, "serial": verified["serial"],
         "previous_serial": previous_serial},
    )
