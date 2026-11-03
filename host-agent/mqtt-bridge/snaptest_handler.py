#!/usr/bin/env python3
"""Auto-capture snaptest images when bench is active.

Tiered reassembly (2026-11-02, docs/bridge-reassembler-design-2026-11-02.md):
streams are keyed by (id, _mid) and GRADED when they end — on the IEND
chunk, on the device's terminal {"t":"snaptest"} reply, on the page's
snaptest_image verdict, or when a new stream starts. Only the verified
prefix is ever used: chunks contiguous from seq 0 that match their blen
and decode strictly. Outcomes:

  COMPLETE  bit-exact stream (length == device-reported bytes when
            known, PNG CRC walk clean, full decompress exact) — saved
            as snap-<ts>-<id>.png, exactly as before.
  PARTIAL   stream ended incomplete but the verified prefix yields at
            least one complete scanline — saved as
            snap-<ts>-<id>-partial.png (real rows, one red cut-line
            row, gray below) plus a .json sidecar with the accounting.
            Policy (John 2026-11-02): save partials for ANY rows; the
            floor question gets revisited with real partials in hand.
  FAILED    no recoverable rows (or no IHDR) — no image; the per-seq
            chunk files stay on disk and the log says why.

Only active when the BENCH_ACTIVE flag file exists — prevents filling
disk with images when we're not on the bench.

Usage: touch BENCH_ACTIVE to enable, rm BENCH_ACTIVE to disable.
"""

import base64
import binascii
import json
import os
import struct
import time
import zlib

BRIDGE_DIR = os.path.dirname(os.path.abspath(__file__))
BENCH_FLAG = os.path.join(BRIDGE_DIR, "BENCH_ACTIVE")
SNAPSHOT_DIR = os.path.join(BRIDGE_DIR, "snapshots")
# v0.4.18.86: Save raw chunks for debugging/reassembly (John 2026-10-08)
# The telemetry log truncates base64 to 160 chars, so we need the full
# chunks persisted separately.
CHUNK_DIR = os.path.join(BRIDGE_DIR, "chunks")

PNG_SIG = b"\x89PNG\r\n\x1a\n"
IEND_TRAILER = bytes([0, 0, 0, 0, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82])

# Streaming reassembly state: (img_id, mid) -> stream dict.
_pending = {}
# Terminal replies seen before any chunk state existed (defensive; in
# practice chunks always precede the reply). (img_id, mid) -> info dict.
_reply_stash = {}
# Legacy non-streaming (JPEG-era) path, unchanged: img_id -> dict.
_legacy = {}


def is_bench_active() -> bool:
    """Check if bench work is active."""
    return os.path.exists(BENCH_FLAG)


def handle_snaptest_message(raw: str) -> str | None:
    """Try to parse a raw message as snaptest data. Returns image path or None."""
    if not is_bench_active():
        return None
    try:
        msg = json.loads(raw)
    except Exception:
        return None
    if not isinstance(msg, dict):
        return None
    try:
        t = msg.get("t")
        if t == "snaptest_chunk":
            return _feed_chunk(msg)
        if t == "snaptest":
            return _note_reply(msg)
        if t == "snaptest_image":
            return _note_verdict(msg)
    except Exception:
        import traceback
        print("[snaptest] handler error:")
        traceback.print_exc()
    return None


# ------------------------------------------------------------------
# Stream state helpers
# ------------------------------------------------------------------

def _new_stream(img_id, mid, total, streaming):
    return {
        "id": img_id, "mid": mid, "total": total, "streaming": streaming,
        "chunks": {},          # seq -> decoded bytes (verified only)
        "corrupt": {},         # seq -> reason (blen mismatch / bad b64)
        "iend_seq": None,      # seq whose bytes end with the IEND trailer
        "expected_bytes": None,
        "device_err": None,
        "page_verdict": None,
    }


def _find_stream(img_id, mid):
    """Exact (id, mid) state, else the most recent ungraded same-id state."""
    key = (img_id, mid)
    if key in _pending:
        return key
    candidates = [k for k in _pending if k[0] == img_id]
    if candidates:
        return candidates[-1]
    return None


def _grade_stale_streams(except_key=None):
    """Grade every pending stream except except_key (new-stream trigger)."""
    path = None
    for key in list(_pending.keys()):
        if key != except_key:
            p = _grade_stream(key, "new-stream")
            if p:
                path = p
    return path


# ------------------------------------------------------------------
# Ingest
# ------------------------------------------------------------------

def _feed_chunk(msg: dict) -> str | None:
    """Feed a chunk into the reassembler. Returns path when a grade saves."""
    img_id = msg.get("id", 0)
    seq = msg.get("seq", 0)
    total = msg.get("total", 0)
    b64_data = msg.get("b64", "")
    mid = msg.get("_mid") or None
    streaming = (total is not None and total < 0)

    if not streaming:
        return _feed_legacy(msg)

    # v0.4.18.86 (John 2026-10-08): Persist raw chunks to disk. The
    # telemetry log truncates base64, so we save full chunks here for
    # reassembly/debug. Kept for every chunk, corrupt or not.
    try:
        os.makedirs(CHUNK_DIR, exist_ok=True)
        chunk_path = os.path.join(CHUNK_DIR, f"chunk-{img_id}-{seq}.b64")
        with open(chunk_path, "w") as f:
            f.write(b64_data)
    except Exception:
        pass

    key = (img_id, mid)
    result_path = None

    if seq == 0:
        # A seq-0 always opens a new generation: grade whatever is still
        # ungraded (other streams, and a previous generation under this
        # same key) before accepting it. This replaces the old
        # delete-on-reset: stale data is accounted, never merged and
        # never silently dropped.
        result_path = _grade_stale_streams(except_key=None) or result_path
        if key in _pending:
            p = _grade_stream(key, "new-stream")
            if p:
                result_path = p

    if key not in _pending:
        _pending[key] = _new_stream(img_id, mid, total, streaming)
        stash = _reply_stash.pop(key, None)
        if stash:
            _pending[key]["expected_bytes"] = stash.get("bytes")
            _pending[key]["device_err"] = stash.get("err")
    st = _pending[key]
    st["total"] = total

    # Verify the chunk before admitting it to the prefix. A failed chunk
    # marks the corruption boundary; it does not poison the stream.
    blen = msg.get("blen")
    if blen is not None:
        try:
            blen = int(blen)
        except Exception:
            blen = None
    if blen is not None and blen != len(b64_data):
        st["corrupt"][seq] = f"blen mismatch (got {len(b64_data)}, want {blen})"
        return result_path
    try:
        raw = base64.b64decode(b64_data, validate=True)
    except (binascii.Error, ValueError) as e:
        st["corrupt"][seq] = f"b64 invalid ({e})"
        return result_path

    st["chunks"][seq] = raw
    if raw.endswith(IEND_TRAILER):
        st["iend_seq"] = seq
        p = _grade_stream(key, "iend")
        if p:
            result_path = p
    return result_path


def _note_reply(msg: dict) -> str | None:
    """Device terminal reply {"t":"snaptest"}: the stream is over — grade it."""
    img_id = msg.get("id", 0)
    mid = msg.get("_mid") or None
    info = {"bytes": msg.get("bytes"), "err": msg.get("err")}
    key = _find_stream(img_id, mid)
    if key is None:
        _reply_stash[(img_id, mid)] = info
        return None
    st = _pending[key]
    if info["bytes"] is not None:
        st["expected_bytes"] = info["bytes"]
    if info["err"]:
        st["device_err"] = info["err"]
    return _grade_stream(key, "device-reply")


def _note_verdict(msg: dict) -> str | None:
    """Page verdict {"t":"snaptest_image"}: stream is over — grade what we hold."""
    img_id = msg.get("id", 0)
    key = _find_stream(img_id, None)
    if key is None:
        return None
    _pending[key]["page_verdict"] = {
        "chunks": msg.get("chunks"), "bytes": msg.get("bytes"),
        "msg": msg.get("msg"),
    }
    return _grade_stream(key, "page-verdict")


# ------------------------------------------------------------------
# Grading
# ------------------------------------------------------------------

def _png_walk_ok(data: bytes) -> bool:
    """Walk PNG chunks verifying CRCs; require IHDR first, IEND last, no trailing bytes."""
    if not data.startswith(PNG_SIG):
        return False
    off = 8
    first = True
    while off + 8 <= len(data):
        (length,) = struct.unpack(">I", data[off:off + 4])
        ctype = data[off + 4:off + 8]
        end = off + 8 + length + 4
        if end > len(data):
            return False
        (crc_stored,) = struct.unpack(">I", data[off + 8 + length:end])
        if binascii.crc32(data[off + 4:off + 8 + length]) & 0xFFFFFFFF != crc_stored:
            return False
        if first and ctype != b"IHDR":
            return False
        first = False
        off = end
        if ctype == b"IEND":
            return off == len(data)
    return False


def _extract_idat(data: bytes) -> bytes:
    """Concatenated IDAT payloads from a (possibly truncated) PNG byte prefix."""
    out = bytearray()
    off = 8
    while off + 8 <= len(data):
        (length,) = struct.unpack(">I", data[off:off + 4])
        ctype = data[off + 4:off + 8]
        avail = len(data) - (off + 8)
        take = min(length, max(avail, 0))
        if ctype == b"IDAT":
            out += data[off + 8:off + 8 + take]
        if take < length:
            break  # truncated final chunk — nothing beyond it exists
        off += 8 + length + 4
    return bytes(out)


def _png_chunk(typ: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + typ + payload
            + struct.pack(">I", binascii.crc32(typ + payload) & 0xFFFFFFFF))


def _unique_path(base: str) -> str:
    path = base
    n = 2
    while os.path.exists(path):
        root, ext = os.path.splitext(base)
        path = f"{root}-{n}{ext}"
        n += 1
    return path


def _grade_stream(key, trigger: str) -> str | None:
    """Grade a finished stream: COMPLETE / PARTIAL / FAILED. Clears state."""
    st = _pending.pop(key, None)
    if st is None:
        return None
    img_id = st["id"]
    chunks = st["chunks"]
    corrupt = st["corrupt"]
    os.makedirs(SNAPSHOT_DIR, exist_ok=True)

    max_seen = max(list(chunks.keys()) + list(corrupt.keys()), default=-1)
    missing = [s for s in range(max_seen + 1)
               if s not in chunks and s not in corrupt]

    # Verified prefix: contiguous from seq 0. Seq 0 is the 8-byte PNG
    # signature — a format constant, so a stream missing ONLY seq 0 is
    # reassembled with the constant, flagged in the accounting.
    parts = []
    synthesized_sig = False
    first_gap = None
    s = 0
    if 0 in corrupt:
        first_gap = 0
    elif 0 in chunks:
        parts.append(chunks[0])
        s = 1
    elif 1 in chunks:
        parts.append(PNG_SIG)
        synthesized_sig = True
        s = 1
    else:
        first_gap = 0
    if first_gap is None:
        while True:
            if st["iend_seq"] is not None and s > st["iend_seq"]:
                break
            if s in corrupt or s not in chunks:
                first_gap = s
                break
            parts.append(chunks[s])
            s += 1
    prefix = b"".join(parts)
    prefix_through_iend = (
        st["iend_seq"] is not None and first_gap is None)

    def accounting(rows_decoded=None, rows_total=None):
        return {
            "complete": False,
            "id": img_id,
            "mid": st["mid"],
            "trigger": trigger,
            "rows_decoded": rows_decoded,
            "rows_total": rows_total,
            "first_gap_seq": first_gap,
            "missing_seqs": missing,
            "corrupt": {str(k): v for k, v in sorted(corrupt.items())},
            "expected_bytes": st["expected_bytes"],
            "got_bytes": len(prefix),
            "iend_seen": st["iend_seq"] is not None,
            "synthesized_sig": synthesized_sig,
            "device_err": st["device_err"],
            "page_verdict": st["page_verdict"],
        }

    # ---- COMPLETE -------------------------------------------------
    if prefix_through_iend:
        expected_ok = (st["expected_bytes"] is None
                       or len(prefix) == st["expected_bytes"])
        decompress_ok = False
        if expected_ok and _png_walk_ok(prefix):
            try:
                w, h = struct.unpack(">II", prefix[16:24])
                raw = zlib.decompress(_extract_idat(prefix))
                decompress_ok = (len(raw) == h * (w * 3 + 1))
            except Exception:
                decompress_ok = False
        if expected_ok and decompress_ok:
            timestamp = time.strftime("%Y%m%d-%H%M%S")
            path = _unique_path(os.path.join(
                SNAPSHOT_DIR, f"snap-{timestamp}-{img_id}.png"))
            with open(path, "wb") as f:
                f.write(prefix)
            return path
        # Fell through: contiguous but not bit-clean (e.g. a wrong-data
        # chunk). Try to recover the honest prefix as a partial.

    # ---- PARTIAL --------------------------------------------------
    rows = b""
    n_rows = 0
    width = height = None
    if len(prefix) >= 33 and prefix.startswith(PNG_SIG):
        try:
            width, height = struct.unpack(">II", prefix[16:24])
            bit_depth, color_type = prefix[24], prefix[25]
        except Exception:
            width = height = None
    if width and height and bit_depth == 8 and color_type == 2:
        row_bytes = width * 3 + 1
        # Feed the IDAT stream in slices: decompress() raises on corrupt
        # data and the output of THAT call is lost, so slicing keeps
        # everything produced before the error point. All accumulated
        # output is genuine sequential deflate output.
        d = zlib.decompressobj()
        buf = bytearray()
        idat = _extract_idat(prefix)
        try:
            for i in range(0, len(idat), 8192):
                buf += d.decompress(idat[i:i + 8192])
        except zlib.error:
            pass
        out = bytes(buf)
        complete_rows = len(out) // row_bytes
        # Filter-byte brake: never display a row whose filter byte is
        # not a valid PNG filter (0-4) — paranoia against wrong-data
        # chunks emitting plausible garbage.
        n_rows = 0
        for i in range(complete_rows):
            if out[i * row_bytes] > 4:
                break
            n_rows += 1
        rows = out[:n_rows * row_bytes]

    if n_rows >= 1:
        row_bytes = width * 3 + 1
        raw = bytearray(rows)
        if n_rows < height:
            raw += bytes([0]) + bytes([255, 0, 0]) * width       # red cut line
            gray = bytes([0]) + bytes([128, 128, 128]) * width
            raw += gray * (height - n_rows - 1)
        ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
        png = (PNG_SIG + _png_chunk(b"IHDR", ihdr)
               + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 6))
               + _png_chunk(b"IEND", b""))
        timestamp = time.strftime("%Y%m%d-%H%M%S")
        path = _unique_path(os.path.join(
            SNAPSHOT_DIR, f"snap-{timestamp}-{img_id}-partial.png"))
        with open(path, "wb") as f:
            f.write(png)
        sidecar = accounting(rows_decoded=n_rows, rows_total=height)
        with open(os.path.splitext(path)[0] + ".json", "w") as f:
            json.dump(sidecar, f, indent=1)
        print(f"[snaptest] PARTIAL image saved: {path} "
              f"(rows {n_rows}/{height}, first gap seq {first_gap}, "
              f"missing {len(missing)}, corrupt {len(corrupt)}, "
              f"trigger {trigger})")
        return path

    # ---- FAILED ---------------------------------------------------
    reason = ("no IHDR" if not (width and height)
              else "no recoverable rows")
    print(f"[snaptest] stream FAILED ({reason}); no image saved "
          f"(id={img_id} mid={st['mid']} trigger={trigger} "
          f"chunks={len(chunks)} missing={len(missing)} "
          f"corrupt={len(corrupt)} device_err={st['device_err']})")
    return None


# ------------------------------------------------------------------
# Legacy non-streaming path (JPEG-era), unchanged behavior
# ------------------------------------------------------------------

def _feed_legacy(msg: dict) -> str | None:
    img_id = msg.get("id", 0)
    seq = msg.get("seq", 0)
    total = msg.get("total", 0)
    b64_data = msg.get("b64", "")
    is_last = msg.get("last", False)

    try:
        os.makedirs(CHUNK_DIR, exist_ok=True)
        chunk_path = os.path.join(CHUNK_DIR, f"chunk-{img_id}-{seq}.b64")
        with open(chunk_path, "w") as f:
            f.write(b64_data)
    except Exception:
        pass

    if img_id not in _legacy:
        _legacy[img_id] = {"chunks": {}, "total": total}
    _legacy[img_id]["chunks"][seq] = b64_data
    _legacy[img_id]["total"] = total
    pending = _legacy[img_id]

    if len(pending["chunks"]) >= total or is_last:
        try:
            os.makedirs(SNAPSHOT_DIR, exist_ok=True)
            parts = []
            for i in range(total):
                if i in pending["chunks"]:
                    parts.append(pending["chunks"][i])
                else:
                    return None
            b64_combined = "".join(parts)
            img_data = base64.b64decode(b64_combined)
            if not img_data.startswith(b"\xff\xd8"):
                return None
            eoi = img_data.rfind(b"\xff\xd9")
            if eoi > 0:
                img_data = img_data[:eoi + 2]
            timestamp = time.strftime("%Y%m%d-%H%M%S")
            path = os.path.join(SNAPSHOT_DIR, f"snap-{timestamp}-{img_id}.jpg")
            with open(path, "wb") as f:
                f.write(img_data)
            del _legacy[img_id]
            return path
        except Exception:
            return None
    return None
