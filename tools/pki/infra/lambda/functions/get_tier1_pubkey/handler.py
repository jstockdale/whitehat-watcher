"""GET /tier1/pubkey -- current Tier 1 certificate + P-256 public key.

Response 200: {"p256_pubkey_hex": <65-byte uncompressed, hex>, "cert": {...}}
Response 503: Tier 1 cert not provisioned yet (SSM parameter missing).

The cert lives in ONE place (SSM Parameter Store); rotation updates that
single parameter, so this endpoint (and /sign) always serve the current
key without a redeploy. SSM parameter versioning keeps the history.
No IAM principal needed beyond the API's AWS_IAM auth.
"""

import json
import os

import boto3

_ssm = boto3.client("ssm")
CERT_PARAM = os.environ["TIER1_CERT_PARAM_NAME"]


def _resp(status: int, body: dict) -> dict:
    return {
        "statusCode": status,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }


def handler(event: dict, context) -> dict:
    try:
        resp = _ssm.get_parameter(Name=CERT_PARAM)
        cert = json.loads(resp["Parameter"]["Value"])
    except _ssm.exceptions.ParameterNotFound:
        return _resp(503, {"error": "tier1 cert not provisioned"})
    except Exception:
        return _resp(500, {"error": "tier1 cert unavailable"})

    pubkey_hex = cert.get("p256_pubkey_hex")
    if not isinstance(pubkey_hex, str):
        return _resp(500, {"error": "tier1 cert malformed"})
    return _resp(200, {"p256_pubkey_hex": pubkey_hex, "cert": cert})
