"""GET /audit -- query the signing audit log.

Query params:
  since   ISO-8601 timestamp; only entries with timestamp >= since
  cmd     filter to one command (uses the by-cmd GSI)
  device  filter to one device id (post-filter)
  limit   page size, default 50, max 200
  next_token  opaque pagination cursor from a previous response

Response 200: {"entries": [...], "next_token": "<opaque>" | null}

Entries are newest-first. Without ?cmd= the table is scanned with a
timestamp filter (fine at dev scale; add a GSI if the table grows).
"""

import base64
import json
import os

import boto3
from boto3.dynamodb.conditions import Attr, Key

_table = boto3.resource("dynamodb").Table(os.environ["AUDIT_TABLE_NAME"])
GSI_BY_CMD = "by-cmd"
DEFAULT_LIMIT = 50
MAX_LIMIT = 200


def _resp(status: int, body: dict) -> dict:
    return {
        "statusCode": status,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }


def _decode_token(token: str):
    try:
        return json.loads(base64.urlsafe_b64decode(token.encode()).decode())
    except Exception:
        return None


def _encode_token(key) -> str | None:
    if not key:
        return None
    return base64.urlsafe_b64encode(json.dumps(key).encode()).decode()


def handler(event: dict, context) -> dict:
    params = (event.get("queryStringParameters") or {}) if event else {}
    since = params.get("since")
    cmd = params.get("cmd")
    device = params.get("device")
    try:
        limit = min(int(params.get("limit", DEFAULT_LIMIT)), MAX_LIMIT)
        if limit < 1:
            raise ValueError
    except (ValueError, TypeError):
        return _resp(400, {"error": "limit must be an integer 1..200"})

    start_key = _decode_token(params["next_token"]) if params.get("next_token") else None
    if params.get("next_token") and start_key is None:
        return _resp(400, {"error": "next_token is malformed"})

    kwargs: dict = {"Limit": limit, "ScanIndexForward": False}
    if start_key:
        kwargs["ExclusiveStartKey"] = start_key

    if cmd:
        # GSI path: pk = cmd, sk = timestamp.
        key_cond = Key("cmd").eq(cmd)
        if since:
            key_cond = key_cond & Key("timestamp").gte(since)
        kwargs["IndexName"] = GSI_BY_CMD
        kwargs["KeyConditionExpression"] = key_cond
        if device:
            kwargs["FilterExpression"] = Attr("device").eq(device)
        result = _table.query(**kwargs)
    else:
        filt = None
        if since:
            filt = Attr("timestamp").gte(since)
        if device:
            dev = Attr("device").eq(device)
            filt = dev if filt is None else (filt & dev)
        if filt is not None:
            kwargs["FilterExpression"] = filt
        result = _table.scan(**kwargs)

    return _resp(
        200,
        {
            "entries": result.get("Items", []),
            "next_token": _encode_token(result.get("LastEvaluatedKey")),
        },
    )
