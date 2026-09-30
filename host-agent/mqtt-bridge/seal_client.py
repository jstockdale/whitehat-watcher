#!/usr/bin/env python3
"""seal_client.py — host-side sealing for Watcher MQTT commands.

Loads the commander Ed25519 key (muse-01), maintains the persistent
monotonic counter, and seals command bodies into WMS2 envelopes
(kind=2, topic-bound) for publication as {"sealed": "<base64>"}.

D4 zero-budget replay: after a verifier reboot the verifier's
high-water floor may exceed our persisted counter; sealing from our
own counter would eat loud REPLAY drops walking past it. Resync with
the reported floor first:

    python3 seal_client.py --resync <floor>

which sets the persisted counter to max(own, floor) under an
anti-oracle cap (refuses floor > own + 2^20). The floor source is
the verifier's status: Phase 2 firmware reports seal_floor;
Phase 1 page reports seal_hw in watcher/status.

Key material lives in ~/.ssh/ (0600). The counter lives in
~/.ssh/watcher_seal_ctr and is fsync'd on every increment.
"""
import base64
import json
import os
import stat
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mqtt_seal as mqs
from mqtt_seal import OK
from nacl.signing import SigningKey

SSH = os.path.expanduser("~/.ssh")
SEED_PATH = os.path.join(SSH, "watcher_seal_muse01")
CTR_PATH = os.path.join(SSH, "watcher_seal_ctr")

COMMANDER_ID = "muse-01"


def load_signing_key():
    with open(SEED_PATH) as f:
        seed_hex = f.read().strip()
    return SigningKey(bytes.fromhex(seed_hex))


def read_counter():
    if os.path.exists(CTR_PATH):
        with open(CTR_PATH) as f:
            try:
                return int(f.read().strip())
            except ValueError:
                return 0
    return 0


def write_counter(ctr):
    tmp = CTR_PATH + ".tmp"
    with open(tmp, "w") as f:
        f.write(str(ctr) + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
    os.replace(tmp, CTR_PATH)


def next_counter():
    """Atomically read-increment-persist the command counter."""
    ctr = read_counter() + 1
    write_counter(ctr)
    return ctr


def resync_counter(floor):
    """D4 zero-budget resync: next = max(own_persisted, floor).

    Anti-oracle: a floor more than 2^20 above our own counter is
    refused (a hostile status could otherwise burn counter space).
    Returns the new persisted counter.
    """
    own = read_counter()
    floor = int(floor)
    if floor < 0:
        raise ValueError("negative floor")
    if floor > own + (1 << 20):
        raise ValueError("floor %d exceeds own %d + 2^20 — refusing"
                         % (floor, own))
    new = max(own, floor)
    write_counter(new)
    return new


def seal_command(cmd_id, cmd, topic="watcher/command"):
    """Seal {"id": cmd_id, "cmd": cmd} -> {"sealed": base64_envelope}."""
    body = json.dumps({"id": cmd_id, "cmd": cmd},
                      separators=(",", ":")).encode()
    ctr = next_counter()
    ts_ms = int(time.time() * 1000)
    env = mqs.seal_control(topic, COMMANDER_ID, ctr, ts_ms, body,
                           load_signing_key())
    return {"sealed": base64.b64encode(env).decode("ascii"),
            "ctr": ctr, "ts_ms": ts_ms}


if __name__ == "__main__":
    # --resync <floor>: D4 commander resync after a verifier reboot.
    if len(sys.argv) == 3 and sys.argv[1] == "--resync":
        try:
            new = resync_counter(int(sys.argv[2]))
        except ValueError as e:
            print("resync refused: %s" % e, file=sys.stderr)
            sys.exit(1)
        print("counter resynced to %d" % new)
        sys.exit(0)
    print("usage: seal_client.py --resync <floor>", file=sys.stderr)
    sys.exit(2)
