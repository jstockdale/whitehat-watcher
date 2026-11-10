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

AUTO-RESYNC (2026-11-09): seal_command() now does this itself. Before
incrementing, it reads the freshest seal_floor from the bridge's
telemetry.log — a value that arrived over the MAC-authenticated
telemetry channel, so it carries the same trust as the manual ritual
it replaces — and resyncs forward when the device is ahead. Floors
older than FLOOR_MAX_AGE_S are ignored (bridge down = no fresh word
from the device = fall back to our own counter, exactly the old
behavior). A resync failure never blocks a send: it warns and seals
from the persisted counter. The manual --resync stays for forensics,
and --auto-resync runs the same path once, verbosely.

REPLAY-REACTIVE RESYNC (2026-11-09, spec:
docs/seal-replay-reactive-resync-spec-2026-11-09.md, margin ruled by
John): the advertised floor is the device's persisted/boot floor,
but its LIVE high-water can run ahead of it — the console page seals
the same muse-01 key from its own counter store, and a crash-reboot
moves the persisted floor by the NVS stride. When that happens the
device says so in plain text: `SEAL: REPLAY ctr <sent> <= <live>`,
naming the live high-water. auto_resync() therefore takes the
binding floor as max(advertised floor, latest REPLAY high-water),
and when the REPLAY candidate binds it resyncs to live +
REPLAY_MARGIN (the page may seal again between our scan and our
send). A dropped command is still lost — the device gives no retry
signal — but the next send heals itself: one drop per desync event
instead of silent drops until a human reads the log. resync_counter
stays forward-only, so a stale REPLAY line can never drag the
counter down.

Key material lives in ~/.ssh/ (0600). The counter lives in
~/.ssh/watcher_seal_ctr and is fsync'd on every increment.
"""
import base64
import json
import os
import re
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


TELEMETRY_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "telemetry.log")
FLOOR_MAX_AGE_S = 120        # log mtime fresher than this = bridge alive
_FLOOR_RE = re.compile(r'"seal_floor"\s*:\s*(\d+)')
_REPLAY_RE = re.compile(r"SEAL:\s*REPLAY\s+ctr\s+(\d+)\s*<=\s*(\d+)")
REPLAY_MARGIN = 128          # headroom over a REPLAY-named high-water


def _telemetry_tail(log_path=TELEMETRY_LOG, max_age_s=FLOOR_MAX_AGE_S):
    """Freshness-gated tail read shared by the floor estimators.

    Returns the decoded tail text, or None when the log is missing
    or stale. Freshness is gated on the log file's mtime (filesystem
    truth, timezone-free): a bridge that has stopped writing has no
    fresh word from the device. The log's line timestamps are
    deliberately NOT compared against the local clock — the
    subscriber stamps in UTC while this host's wall clock is local,
    and any fixed assumption about either breaks silently.
    """
    try:
        st = os.stat(log_path)
    except OSError:
        return None
    if time.time() - st.st_mtime > max_age_s:
        return None
    try:
        with open(log_path, "rb") as f:
            f.seek(max(0, st.st_size - (16 << 20)))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return None


def latest_telemetry_floor(log_path=TELEMETRY_LOG,
                           max_age_s=FLOOR_MAX_AGE_S):
    """Best estimate of the device's seal floor from the telemetry log.

    The value originates from the device's MAC-authenticated
    telemetry (seal_status replies and the boot-time ready line),
    relayed verbatim by the bridge subscriber into the log.

    Aggregation is MAX over a generous log tail, not last-line-wins,
    for three measured reasons (2026-11-09):
      * floor lines are sparse (~14 in 8 MB of log) — a narrow tail
        can hold none at all;
      * the page-side seal_status can report 0 before it loads its
        high-water, and a 0 must never mask the device's real floor;
      * floors are monotonic — every reported value is a lower bound
        on the current floor, so the max is the best estimator and
        can never overshoot the device's true floor.
    """
    tail = _telemetry_tail(log_path, max_age_s)
    if tail is None:
        return None
    floors = [int(m.group(1)) for m in _FLOOR_RE.finditer(tail)]
    return max(floors) if floors else None


def latest_replay_highwater(log_path=TELEMETRY_LOG,
                            max_age_s=FLOOR_MAX_AGE_S):
    """Live high-water named by the most recent SEAL REPLAY line.

    When a sealed command is dropped as a replay, the device log
    says so in plain text — `SEAL: REPLAY ctr <sent> <= <live>` —
    and <live> is the verifier's live high-water at that moment,
    which can run ahead of every telemetry-advertised floor (the
    page seals the same key from its own counter store; a
    crash-reboot moves the persisted floor by the NVS stride).
    The most recent line in the tail wins: the high-water is
    monotonic, so the freshest report is the tightest lower bound.
    Returns None when the tail holds no REPLAY line (or the log
    is missing/stale, via the shared freshness gate).
    """
    tail = _telemetry_tail(log_path, max_age_s)
    if tail is None:
        return None
    lives = [int(m.group(2)) for m in _REPLAY_RE.finditer(tail)]
    return lives[-1] if lives else None


def auto_resync(log_path=TELEMETRY_LOG):
    """Forward-only resync from the freshest telemetry floor.

    The binding floor is max(advertised floor, latest REPLAY
    high-water) over whichever estimators have fresh data. When
    the REPLAY candidate binds, the target is live + REPLAY_MARGIN
    (the page may seal again between this scan and our next send;
    a bare live + 1 would just eat the next drop too). When the
    advertised floor binds, the target is the floor itself, exactly
    the pre-REPLAY-reactive behavior.

    Returns (own_before, target, source) with source "advertised"
    or "replay"; target <= own_before when the counter was already
    ahead (no move made). Returns None when neither estimator has
    fresh data. Raises ValueError only from resync_counter's
    anti-oracle refusal — callers decide whether that is fatal.
    """
    advertised = latest_telemetry_floor(log_path)
    replay = latest_replay_highwater(log_path)
    if advertised is None and replay is None:
        return None
    if replay is not None and (advertised is None or replay > advertised):
        target, source = replay + REPLAY_MARGIN, "replay"
    else:
        target, source = advertised, "advertised"
    own = read_counter()
    if target <= own:
        return (own, target, source)
    resync_counter(target)
    return (own, target, source)


def seal_command(cmd_id, cmd, topic="watcher/command"):
    """Seal {"id": cmd_id, "cmd": cmd} -> {"sealed": base64_envelope}.

    Auto-resyncs the replay counter from the freshest telemetry
    floor first (see module docstring); a floor problem never blocks
    the send — it warns on stderr and seals from the persisted
    counter, which is the pre-auto-resync behavior.
    """
    synced = None
    try:
        r = auto_resync()
        if r is not None and r[1] > r[0]:
            synced = r
    except Exception as e:  # anti-oracle refusal or log weirdness
        print("seal auto-resync skipped: %s" % e, file=sys.stderr)
    body = json.dumps({"id": cmd_id, "cmd": cmd},
                      separators=(",", ":")).encode()
    ctr = next_counter()
    ts_ms = int(time.time() * 1000)
    env = mqs.seal_control(topic, COMMANDER_ID, ctr, ts_ms, body,
                           load_signing_key())
    out = {"sealed": base64.b64encode(env).decode("ascii"),
           "ctr": ctr, "ts_ms": ts_ms}
    if synced is not None:
        out["resync"] = {"from": synced[0], "floor": synced[1],
                         "source": synced[2]}
    return out


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
    # --auto-resync: run the seal_command() floor sync once, verbosely.
    if len(sys.argv) == 2 and sys.argv[1] == "--auto-resync":
        try:
            r = auto_resync()
        except ValueError as e:
            print("auto-resync refused: %s" % e, file=sys.stderr)
            sys.exit(1)
        if r is None:
            print("no fresh seal_floor in telemetry.log "
                  "(bridge down or device silent); counter unchanged "
                  "at %d" % read_counter())
        elif r[1] > r[0]:
            print("counter resynced %d -> %d (telemetry floor, "
                  "source=%s)" % (r[0], r[1], r[2]))
        else:
            print("counter %d already >= telemetry floor %d; no move"
                  % (read_counter(), r[1]))
        sys.exit(0)
    print("usage: seal_client.py --resync <floor> | --auto-resync",
          file=sys.stderr)
    sys.exit(2)
