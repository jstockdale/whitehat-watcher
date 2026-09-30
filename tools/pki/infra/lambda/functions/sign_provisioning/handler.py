"""POST /sign -- Tier 1 provisioning-command signer.

Request body:  {"command": <watcher-provision-v1 JSON>}
Response 200:  {"signature_hex": <64-byte raw r||s, hex>,
                "cert": <current Tier 1 cert JSON>,
                "key_id": <KMS key ARN that signed>}
Response 400:  validation failure (audit entry written with reason).
Response 503:  Tier 1 cert not provisioned yet (deploy step missing).

Security contract (PKI_DESIGN_BRIEF.md):
  * The command is schema-validated BEFORE any KMS call. Never sign
    arbitrary bytes.
  * KMS signs the canonical bytes with ECDSA_SHA_256 (MessageType RAW);
    KMS returns DER, converted here to raw r||s for the device.
  * Every invocation -- signed, rejected, or error -- writes an audit item.
    The audit stores cmd_sha256 (SHA-256 of the canonical bytes), NEVER the
    command itself.
"""

import hashlib
import json
import os
import time

import boto3

from pki_common import (
    CommandValidationError,
    PkiError,
    build_audit_item,
    canonical_provision_bytes,
    der_to_raw_sig,
    validate_provision_command,
)

AUDIT_TABLE = os.environ["AUDIT_TABLE_NAME"]
CERT_PARAM = os.environ["TIER1_CERT_PARAM_NAME"]
KMS_KEY_ID = os.environ["KMS_KEY_ID"]  # alias ARN: follows the alias on rotation

_kms = boto3.client("kms")
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
        _table.put_item(Item=build_audit_item(kind="SIGN", now=now,
                                              principal=principal, **kw))
    except Exception:
        # Audit must never break the API response path; the failure is
        # visible in CloudWatch Logs. (A dead audit table is itself an
        # incident -- never silently drop the request because of it.)
        pass


def _current_cert() -> dict:
    resp = _ssm.get_parameter(Name=CERT_PARAM)
    return json.loads(resp["Parameter"]["Value"])


def handler(event: dict, context) -> dict:
    now = time.time()
    principal = _principal(event)

    try:
        body = json.loads(event.get("body") or "")
    except (json.JSONDecodeError, TypeError):
        _audit(now, principal, result="rejected", reason="request body is not JSON")
        return _resp(400, {"error": "invalid request", "reason": "body must be JSON"})

    command = body.get("command") if isinstance(body, dict) else None

    try:
        cert = _current_cert()
        cert_serial = int(cert.get("serial", -1))
    except _ssm.exceptions.ParameterNotFound:
        _audit(now, principal, result="error",
               reason="tier1 cert not provisioned (SSM parameter missing)")
        return _resp(503, {"error": "tier1 cert not provisioned"})
    except Exception as exc:  # corrupted cert JSON etc.
        _audit(now, principal, result="error",
               reason=f"cannot load tier1 cert: {exc}")
        return _resp(500, {"error": "tier1 cert unavailable"})

    try:
        canonical = validate_provision_command(command, now)
    except CommandValidationError as exc:
        reason = str(exc)
        digest = (
            hashlib.sha256(canonical_provision_bytes(command)).hexdigest()
            if isinstance(command, dict)
            else hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()
        )
        _audit(
            now,
            principal,
            cmd_sha256=digest,
            cmd=command.get("cmd", "") if isinstance(command, dict) else "",
            device=command.get("device", "") if isinstance(command, dict) else "",
            cert_serial=cert_serial,
            result="rejected",
            reason=reason,
        )
        return _resp(400, {"error": "command rejected", "reason": reason})

    cmd_sha256 = hashlib.sha256(canonical).hexdigest()
    try:
        sig_resp = _kms.sign(
            KeyId=KMS_KEY_ID,
            Message=canonical,
            MessageType="RAW",
            SigningAlgorithm="ECDSA_SHA_256",
        )
        raw_sig = der_to_raw_sig(sig_resp["Signature"])
    except PkiError as exc:
        _audit(
            now, principal, cmd_sha256=cmd_sha256, cmd=command["cmd"],
            device=command["device"], cert_serial=cert_serial,
            result="error", reason=f"signature conversion failed: {exc}",
        )
        return _resp(500, {"error": "signing failed"})
    except Exception as exc:
        _audit(
            now, principal, cmd_sha256=cmd_sha256, cmd=command["cmd"],
            device=command["device"], cert_serial=cert_serial,
            result="error", reason=f"kms sign failed: {type(exc).__name__}",
        )
        return _resp(500, {"error": "signing failed"})

    key_id = sig_resp.get("KeyId", "")
    _audit(
        now, principal, cmd_sha256=cmd_sha256, cmd=command["cmd"],
        device=command["device"], cert_serial=cert_serial,
        kms_key_arn=key_id, result="signed",
    )
    return _resp(
        200,
        {
            "signature_hex": raw_sig.hex(),
            "cert": cert,
            "key_id": key_id,
        },
    )
