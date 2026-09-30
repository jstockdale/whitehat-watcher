"""Phase 0 JSON line protocol between the host agent (watcherd) and Watcher firmware.

Wire format: UTF-8 JSON object per line, terminated by ``\\n``.
Any received line that is not valid JSON, or is not a JSON object with a
string ``"t"`` field, MUST be ignored by both sides.

This module is the single source of truth for the Phase 0 schema. The
COMMANDS / EVENTS dicts below drive validation and are reused to render
docs (see protocol.md).
"""

from __future__ import annotations

import json
from typing import Any, Dict, Optional, Union

# ---------------------------------------------------------------------------
# Shared vocabularies
# ---------------------------------------------------------------------------

KEY_CODES = (
    tuple(chr(c) for c in range(ord("a"), ord("z") + 1))
    + tuple(str(d) for d in range(10))
    + (
        "enter", "tab", "esc", "space",
        "backspace", "delete", "home", "end", "pgup", "pgdn",
        "up", "down", "left", "right",
        "f1", "f2", "f3", "f4", "f5", "f6",
        "f7", "f8", "f9", "f10", "f11", "f12",
    )
)
KEY_MODIFIERS = ("ctrl", "shift", "alt", "gui")  # combinable with "+", e.g. "ctrl+shift"
CONSUMER_CODES = ("vol_up", "vol_down", "mute", "play_pause", "next", "prev")
MOUSE_BUTTONS = ("left", "right", "middle")
BLE_STATES = ("connected", "disconnected")

PROTOCOL_VERSION = "0"  # Phase 0 PoC


class ProtocolError(ValueError):
    """Raised when a message violates the Phase 0 schema."""


# ---------------------------------------------------------------------------
# Command spec (host -> device).
# ---------------------------------------------------------------------------

COMMANDS: Dict[str, Dict[str, Any]] = {
    "ping": {
        "desc": "Liveness check. Device replies {\"t\": \"pong\"}.",
        "required": {},
        "optional": {},
    },
    "type": {
        "desc": "Type text via BLE HID as if typed on a keyboard.",
        "required": {"text": str},
        "optional": {},
    },
    "key": {
        "desc": 'Press a key, optionally with modifiers (e.g. mod="ctrl+shift").',
        "required": {"code": str},
        "optional": {"mod": str},
    },
    "mouse_move": {
        "desc": "Relative mouse movement in HID units (device-defined scale).",
        "required": {"dx": int, "dy": int},
        "optional": {},
    },
    "mouse_wheel": {
        "desc": "Scroll wheel; positive delta scrolls up, negative scrolls down (notches).",
        "required": {"delta": int},
        "optional": {},
    },
    "mouse_click": {
        "desc": "Click a mouse button (defaults to left when omitted).",
        "required": {},
        "optional": {"button": str},
    },
    "consumer": {
        "desc": "Media / consumer-control key (volume, playback).",
        "required": {"code": str},
        "optional": {},
    },
    "wifi": {
        "desc": "Store Wi-Fi credentials in NVS and connect (needed before snap).",
        "required": {"ssid": str, "pass": str},
        "optional": {},
    },
    "snap": {
        "desc": "Capture a JPEG via the Himax camera and HTTP POST it to "
                "http://host:port/still?id=N (port defaults to 8099).",
        "required": {"host": str},
        "optional": {"port": int},
    },
}

# ---------------------------------------------------------------------------
# Event spec (device -> host).
# ---------------------------------------------------------------------------

EVENTS: Dict[str, Dict[str, Any]] = {
    "ready": {
        "desc": "Sent once when the firmware is up and the serial link is live.",
        "required": {"fw": str, "name": str},
        "optional": {},
    },
    "pong": {
        "desc": "Reply to a ping command.",
        "required": {},
        "optional": {},
    },
    "ble": {
        "desc": "BLE HID link state changed.",
        "required": {"state": str},
        "optional": {},
    },
    "still_sent": {
        "desc": "A snap capture finished POSTing: still id, byte count, elapsed ms.",
        "required": {"id": int, "bytes": int, "ms": (int, float)},
        "optional": {},
    },
    "ok": {
        "desc": "A command completed successfully (cmd = the command's t value).",
        "required": {"cmd": str},
        "optional": {},
    },
    "err": {
        "desc": "A command failed (cmd = the command's t value).",
        "required": {"cmd": str, "msg": str},
        "optional": {},
    },
}


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _check_field(cmd: str, field: str, value: Any, ftype: Union[type, tuple]) -> None:
    if ftype is int:
        ok = _is_int(value)
    elif isinstance(ftype, tuple):
        ok = any(
            (_is_int(value) if t is int else isinstance(value, t)) for t in ftype
        )
    else:
        ok = isinstance(value, ftype)
    if not ok:
        want = getattr(ftype, "__name__", repr(ftype))
        raise ProtocolError(
            f"message {cmd!r}: field {field!r} must be {want}, got {value!r}"
        )


def _check_spec(obj: Dict[str, Any], spec: Dict[str, Any], kind: str) -> None:
    t = obj["t"]
    for field, ftype in spec["required"].items():
        if field not in obj:
            raise ProtocolError(f"{kind} {t!r} missing required field {field!r}")
        _check_field(t, field, obj[field], ftype)
    for field, ftype in spec["optional"].items():
        if field in obj:
            _check_field(t, field, obj[field], ftype)


def validate_command(obj: Dict[str, Any]) -> bool:
    """Validate a host->device command. Raises ProtocolError on violation."""
    if not isinstance(obj, dict):
        raise ProtocolError("command must be a JSON object")
    t = obj.get("t")
    if t not in COMMANDS:
        raise ProtocolError(f"unknown command {t!r}")
    _check_spec(obj, COMMANDS[t], "command")

    if t == "key":
        if obj["code"] not in KEY_CODES:
            raise ProtocolError(f"unknown key code {obj['code']!r}")
        if "mod" in obj:
            for mod in obj["mod"].split("+"):
                if mod not in KEY_MODIFIERS:
                    raise ProtocolError(
                        f"unknown modifier {mod!r} in mod={obj['mod']!r}"
                    )
    elif t == "consumer":
        if obj["code"] not in CONSUMER_CODES:
            raise ProtocolError(f"unknown consumer code {obj['code']!r}")
    elif t == "mouse_click" and "button" in obj:
        if obj["button"] not in MOUSE_BUTTONS:
            raise ProtocolError(f"unknown mouse button {obj['button']!r}")
    elif t == "type":
        if not obj["text"]:
            raise ProtocolError("type requires non-empty text")
    elif t == "wifi":
        if not obj["ssid"]:
            raise ProtocolError("wifi requires a non-empty ssid")
    elif t == "snap":
        if "port" in obj and not 1 <= obj["port"] <= 65535:
            raise ProtocolError(f"snap port out of range: {obj['port']!r}")
        if not obj["host"]:
            raise ProtocolError("snap requires a non-empty host")
    return True


def validate_event(obj: Dict[str, Any]) -> bool:
    """Validate a device->host event. Raises ProtocolError on violation."""
    if not isinstance(obj, dict):
        raise ProtocolError("event must be a JSON object")
    t = obj.get("t")
    if t not in EVENTS:
        raise ProtocolError(f"unknown event {t!r}")
    _check_spec(obj, EVENTS[t], "event")

    if t == "ble" and obj["state"] not in BLE_STATES:
        raise ProtocolError(f"unknown ble state {obj['state']!r}")
    return True


# ---------------------------------------------------------------------------
# Wire encoding / decoding
# ---------------------------------------------------------------------------

def encode(obj: Dict[str, Any]) -> bytes:
    """Serialize a message to wire bytes: compact JSON + newline, UTF-8."""
    if not isinstance(obj, dict) or not isinstance(obj.get("t"), str):
        raise ProtocolError("message must be a dict with a string 't' field")
    return (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def decode_line(line: Union[bytes, bytearray, str]) -> Optional[Dict[str, Any]]:
    """Parse one received line.

    Returns the message dict, or None when the line is not a valid protocol
    message (undecodable bytes, bad JSON, not an object, or no string "t"
    field). Malformed lines are silently ignored per the protocol spec.
    """
    if isinstance(line, (bytes, bytearray)):
        try:
            line = bytes(line).decode("utf-8")
        except UnicodeDecodeError:
            return None
    if not isinstance(line, str):
        return None
    line = line.strip()
    if not line:
        return None
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return None
    if not isinstance(obj, dict):
        return None
    if not isinstance(obj.get("t"), str):
        return None
    return obj
