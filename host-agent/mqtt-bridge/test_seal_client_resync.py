#!/usr/bin/env python3
"""Unit tests for seal_client's REPLAY-reactive auto-resync.

Spec: docs/seal-replay-reactive-resync-spec-2026-11-09.md
Run:  python3 test_seal_client_resync.py   (exit 0 = all pass)

Everything runs against temp files: a temp counter store replaces
seal_client.CTR_PATH, and synthetic telemetry logs replace the real
one. No device contact, no real counter movement.
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import seal_client as sc

_TMP = tempfile.mkdtemp(prefix="seal-resync-test-")
sc.CTR_PATH = os.path.join(_TMP, "watcher_seal_ctr")

_passed = 0


def check(name, cond, detail=""):
    global _passed
    if not cond:
        print("FAIL: %s %s" % (name, detail))
        sys.exit(1)
    _passed += 1
    print("ok: %s" % name)


def set_ctr(v):
    sc.write_counter(v)


def make_log(text, age_s=0):
    p = os.path.join(_TMP, "telemetry.log")
    with open(p, "w") as f:
        f.write(text)
    if age_s:
        old = time.time() - age_s
        os.utime(p, (old, old))
    return p


def floor_line(v):
    return '{"t":"seal_status","seal_floor": %d}\n' % v


def replay_line(sent, live):
    # verbatim shape from the real telemetry.log (2026-11-09 22:32 UTC)
    return ("2026-11-09 22:32:44     [info/log] SEAL: REPLAY ctr %d "
            "<= %d from muse-01 - DROPPED\n" % (sent, live))


# 1. advertised floor only — existing behavior, target = floor itself
set_ctr(78000)
r = sc.auto_resync(make_log(floor_line(78199)))
check("advertised only", r == (78000, 78199, "advertised"), repr(r))
check("advertised only moves counter", sc.read_counter() == 78199)

# 2. REPLAY line only — replay binds, target = live + 128
set_ctr(78400)
r = sc.auto_resync(make_log(replay_line(78456, 78485)))
check("replay only", r == (78400, 78613, "replay"), repr(r))
check("replay only moves counter to live+128", sc.read_counter() == 78613)

# 3. both present, REPLAY higher — replay binds with margin
set_ctr(78000)
r = sc.auto_resync(make_log(floor_line(78199) + replay_line(78456, 78485)))
check("both, replay higher", r == (78000, 78613, "replay"), repr(r))

# 4. both present, advertised higher — advertised binds, NO margin
set_ctr(78000)
r = sc.auto_resync(make_log(replay_line(78456, 78485) + floor_line(79000)))
check("both, advertised higher", r == (78000, 79000, "advertised"), repr(r))
check("advertised-higher target is the floor itself",
      sc.read_counter() == 79000)

# 5. multiple REPLAY lines — the most recent (last in tail) wins
set_ctr(78000)
log = make_log(replay_line(78500, 78510) + replay_line(78456, 78485))
check("latest replay wins (scanner)",
      sc.latest_replay_highwater(log) == 78485)
r = sc.auto_resync(log)
check("latest replay wins (target)", r == (78000, 78613, "replay"), repr(r))
set_ctr(78000)
log = make_log(replay_line(78456, 78485) + replay_line(78500, 78510))
r = sc.auto_resync(log)
check("latest replay wins (increasing)", r == (78000, 78638, "replay"),
      repr(r))

# 6. stale REPLAY below current counter — forward-only no-op
set_ctr(79109)
r = sc.auto_resync(make_log(floor_line(78199) + replay_line(78456, 78485)))
check("stale replay no-op", r == (79109, 78613, "replay"), repr(r))
check("stale replay leaves counter", sc.read_counter() == 79109)

# 7. counter already ahead of advertised floor — no move
set_ctr(79109)
r = sc.auto_resync(make_log(floor_line(78199)))
check("ahead of advertised no-op", r == (79109, 78199, "advertised"),
      repr(r))
check("counter unchanged", sc.read_counter() == 79109)

# 8. malformed REPLAY lines are ignored
bad = ("SEAL: REPLAY ctr abc <= xyz\n"
       "SEAL: REPLAY ctr 123\n"
       "SEAL: REPLAY <= 78485\n"
       "REPLAY ctr 1 <= 2\n"
       "SEAL: REPLAY ctr 78456 < 78485\n")
log = make_log(bad)
check("malformed replay ignored (scanner)",
      sc.latest_replay_highwater(log) is None)
set_ctr(78000)
check("malformed replay ignored (resync)", sc.auto_resync(log) is None)
check("counter unchanged after malformed", sc.read_counter() == 78000)

# 9. empty log — no fresh estimate, no move
set_ctr(78000)
check("empty log -> None", sc.auto_resync(make_log("")) is None)
check("counter unchanged after empty", sc.read_counter() == 78000)

# 10. stale log file (mtime beyond freshness gate) — estimators give up
log = make_log(floor_line(78199) + replay_line(78456, 78485),
               age_s=sc.FLOOR_MAX_AGE_S + 60)
check("stale log floor -> None", sc.latest_telemetry_floor(log) is None)
check("stale log replay -> None", sc.latest_replay_highwater(log) is None)
set_ctr(78000)
check("stale log resync -> None", sc.auto_resync(log) is None)

# 11. seal_command stamps the resync source (spec point 5)
real_auto = sc.auto_resync
try:
    sc.auto_resync = lambda: (100, 200, "replay")
    set_ctr(200)
    out = sc.seal_command("test-id", {"t": "ping"})
    check("seal_command resync source",
          out.get("resync") == {"from": 100, "floor": 200,
                                "source": "replay"}, repr(out.get("resync")))
    sc.auto_resync = lambda: None
    out = sc.seal_command("test-id", {"t": "ping"})
    check("seal_command no resync field when no sync",
          "resync" not in out)
finally:
    sc.auto_resync = real_auto

print("\n%d checks passed" % _passed)
