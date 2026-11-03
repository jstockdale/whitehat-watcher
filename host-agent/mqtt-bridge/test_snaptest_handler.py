#!/usr/bin/env python3
"""Offline battery for the tiered snaptest_handler.

Feeds chunk-aligned emissions sliced from real captures through
handle_snaptest_message and checks the grading outcomes.
Usage: python3 test_snaptest_handler.py [handler_path]
"""
import base64
import importlib.machinery
import importlib.util
import json
import os
import struct
import sys
import tempfile
import zlib

BRIDGE = os.path.expanduser(
    "~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge")
HANDLER = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BRIDGE, "snaptest_handler.py.new")
SNAPS = os.path.join(BRIDGE, "snapshots")

loader = importlib.machinery.SourceFileLoader("snap_h", HANDLER)
spec = importlib.util.spec_from_loader("snap_h", loader)
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

tmp = tempfile.mkdtemp(prefix="snaptest-battery-")
h.SNAPSHOT_DIR = os.path.join(tmp, "snapshots")
h.CHUNK_DIR = os.path.join(tmp, "chunks")
h.BENCH_FLAG = os.path.join(tmp, "BENCH_ACTIVE")
open(h.BENCH_FLAG, "w").close()

RUN5 = open(os.path.join(SNAPS, "snap-20261102-234712-0.png"), "rb").read()
RUN4 = open(os.path.join(SNAPS, "snap-20261102-234325-0.png"), "rb").read()
ALL5 = ["snap-20261102-232828-0.png", "snap-20261102-233605-0.png",
        "snap-20261102-233946-0.png", "snap-20261102-234325-0.png",
        "snap-20261102-234712-0.png"]

ROW = 1296 * 3 + 1
fails = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (f"  {detail}" if detail else ""))
    if not cond:
        fails.append(name)


def emissions(png: bytes):
    """Split PNG bytes into device-style emissions: sig, then one per PNG chunk."""
    out = [png[:8]]
    pos = 8
    while pos < len(png):
        ln = struct.unpack(">I", png[pos:pos + 4])[0]
        out.append(png[pos:pos + 12 + ln])
        pos += 12 + ln
    return out


def idat_bytes(png: bytes) -> bytes:
    pos = 8
    buf = bytearray()
    while pos < len(png):
        ln = struct.unpack(">I", png[pos:pos + 4])[0]
        if png[pos + 4:pos + 8] == b"IDAT":
            buf += png[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    return bytes(buf)


def rows_for_idat_prefix(png: bytes, n_idats: int) -> int:
    idat = idat_bytes(png)[:n_idats * 8192]
    d = zlib.decompressobj()
    out = bytearray()
    try:
        for i in range(0, len(idat), 8192):
            out += d.decompress(idat[i:i + 8192])
    except zlib.error:
        pass
    return len(out) // ROW


def strict_png_ok(path: str) -> bool:
    data = open(path, "rb").read()
    if not h._png_walk_ok(data):
        return False
    try:
        return len(zlib.decompress(h._extract_idat(data))) == 972 * ROW
    except Exception:
        return False


def top_rows(path: str, n: int) -> bytes:
    data = open(path, "rb").read()
    return zlib.decompress(h._extract_idat(data))[:n * ROW]


def orig_rows(png: bytes, n: int) -> bytes:
    return zlib.decompress(idat_bytes(png))[:n * ROW]


def send(msg: dict):
    return h.handle_snaptest_message(json.dumps(msg))


def feed(png, mid, drop_seqs=(), truncate_seqs=(), swap=None, upto=None,
         reply="bytes", verdict=None):
    """Feed emissions of png under mid. Returns list of saved paths."""
    ems = emissions(png)
    saved = []
    last = len(ems) if upto is None else upto
    for seq in range(last):
        if seq in drop_seqs:
            continue
        raw = ems[seq]
        if swap and seq in swap:
            raw = swap[seq]
        b64 = base64.b64encode(raw).decode()
        blen = len(b64)
        if seq in truncate_seqs:
            b64 = b64[:-342]
        r = send({"t": "snaptest_chunk", "id": 0, "_mid": mid, "seq": seq,
                  "total": -1, "blen": blen, "b64": b64})
        if r:
            saved.append(r)
    if reply == "bytes":
        r = send({"t": "snaptest", "id": 0, "_mid": mid, "bytes": len(png)})
        if r:
            saved.append(r)
    elif reply == "err":
        r = send({"t": "snaptest", "id": 0, "_mid": mid, "err": "PNG encode failed"})
        if r:
            saved.append(r)
    if verdict:
        r = send(verdict)
        if r:
            saved.append(r)
    return saved


def sidecar_of(path):
    with open(os.path.splitext(path)[0] + ".json") as f:
        return json.load(f)


# 1. Replay all five verification captures -> byte-identical completes.
for i, name in enumerate(ALL5):
    png = open(os.path.join(SNAPS, name), "rb").read()
    saved = feed(png, f"replay{i}")
    ok = len(saved) == 1 and open(saved[0], "rb").read() == png \
        and "-partial" not in saved[0]
    check(f"replay {name}", ok, f"saved={saved}")

# 2. Splice: A partial (86 emissions), then B full -> A graded partial, B completes.
exp_rows_a = rows_for_idat_prefix(RUN5, 84)
saved_a = feed(RUN5, "spliceA", upto=86, reply=None)
saved_b = feed(RUN4, "spliceB")
partials = [p for p in saved_a + saved_b if "-partial" in p]
completes = [p for p in saved_b if "-partial" not in p]
check("splice: A graded partial on B start",
      len(partials) == 1 and len(saved_a) == 0,
      f"saved_a={saved_a} partials={partials}")
if partials:
    sc = sidecar_of(partials[0])
    check("splice: A rows as expected",
          abs(sc["rows_decoded"] - exp_rows_a) <= 2,
          f"rows={sc['rows_decoded']} expect~{exp_rows_a}")
    check("splice: A first gap is 86", sc["first_gap_seq"] == 86,
          f"gap={sc['first_gap_seq']}")
check("splice: B completes byte-identical",
      len(completes) == 1 and open(completes[0], "rb").read() == RUN4,
      f"saved_b={saved_b}")

# 3. Drops at IDAT k -> partial with the prefix rows.
for k in (8, 42, 84, 160):
    drop_seq = k + 2  # sig=0, IHDR=1, IDAT k at seq k+2
    saved = feed(RUN5, f"drop{k}", drop_seqs={drop_seq})
    exp = rows_for_idat_prefix(RUN5, k)
    ok = len(saved) == 1 and "-partial" in saved[0]
    detail = f"saved={saved}"
    if ok:
        sc = sidecar_of(saved[0])
        ok = abs(sc["rows_decoded"] - exp) <= 2 \
            and drop_seq in sc["missing_seqs"] \
            and strict_png_ok(saved[0]) \
            and top_rows(saved[0], sc["rows_decoded"]) == orig_rows(RUN5, sc["rows_decoded"])
        detail = f"rows={sc['rows_decoded']} expect~{exp}"
    check(f"drop IDAT {k}", ok, detail)

# 4. Truncated chunk (seq-167 pattern): b64 cut, blen intact -> corrupt boundary.
saved = feed(RUN5, "trunc", truncate_seqs={102})
exp = rows_for_idat_prefix(RUN5, 100)
ok = len(saved) == 1 and "-partial" in saved[0]
detail = f"saved={saved}"
if ok:
    sc = sidecar_of(saved[0])
    ok = "102" in sc["corrupt"] and abs(sc["rows_decoded"] - exp) <= 2
    detail = f"rows={sc['rows_decoded']} expect~{exp} corrupt={list(sc['corrupt'])}"
check("truncated chunk 102", ok, detail)

# 5. Abort after 42 IDATs + err reply -> immediate partial.
saved = feed(RUN5, "abort", upto=44, reply="err")
exp = rows_for_idat_prefix(RUN5, 42)
ok = len(saved) == 1 and "-partial" in saved[0]
detail = f"saved={saved}"
if ok:
    sc = sidecar_of(saved[0])
    ok = abs(sc["rows_decoded"] - exp) <= 2 and sc["device_err"] == "PNG encode failed"
    detail = f"rows={sc['rows_decoded']} expect~{exp}"
check("abort at 42 IDATs", ok, detail)

# 6. Missing seq 0 only -> still completes (signature is a constant).
saved = feed(RUN5, "nosig", drop_seqs={0})
ok = len(saved) == 1 and open(saved[0], "rb").read() == RUN5
check("missing seq 0 completes", ok, f"saved={saved}")

# 7. Tiny prefix (1 IDAT) -> partial under the any-rows policy.
saved = feed(RUN5, "tiny", upto=3, reply="err")
exp = rows_for_idat_prefix(RUN5, 1)
ok = len(saved) == 1 and "-partial" in saved[0]
detail = f"saved={saved}"
if ok:
    sc = sidecar_of(saved[0])
    ok = sc["rows_decoded"] == exp and exp >= 1
    detail = f"rows={sc['rows_decoded']} expect={exp}"
check("tiny prefix partial", ok, detail)

# 8. Wrong-data chunk mid-stream (same length, valid CRC) -> partial, not complete.
ems5 = emissions(RUN5)
ems4 = emissions(RUN4)
saved = feed(RUN5, "swap", swap={52: ems4[52]})
exp = rows_for_idat_prefix(RUN5, 50)
ok = len(saved) == 1 and "-partial" in saved[0]
detail = f"saved={saved}"
if ok:
    sc = sidecar_of(saved[0])
    ok = abs(sc["rows_decoded"] - exp) <= 3 \
        and top_rows(saved[0], sc["rows_decoded"]) == orig_rows(RUN5, sc["rows_decoded"])
    detail = f"rows={sc['rows_decoded']} expect~{exp}"
check("wrong-data chunk at seq 52", ok, detail)

# 9. Missing only the IEND chunk + page verdict -> partial with ALL rows.
last_seq = len(ems5) - 1
saved = feed(RUN5, "noiend", drop_seqs={last_seq}, reply=None,
             verdict={"t": "snaptest_image", "id": 0, "chunks": 170,
                      "bytes": len(RUN5), "msg": "image reassembled"})
ok = len(saved) == 1 and "-partial" in saved[0]
detail = f"saved={saved}"
if ok:
    sc = sidecar_of(saved[0])
    ok = sc["rows_decoded"] == 972 and strict_png_ok(saved[0]) \
        and open(saved[0], "rb").read() != RUN5  # re-encoded, not the original
    detail = f"rows={sc['rows_decoded']}"
check("missing IEND only", ok, detail)

print(f"\n{'ALL PASS' if not fails else 'FAILURES: ' + ', '.join(fails)}")
sys.exit(1 if fails else 0)
