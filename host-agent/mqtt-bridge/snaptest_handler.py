#!/usr/bin/env python3
"""Auto-capture snaptest images when bench is active.

Collects snaptest_chunk messages and reassembles them into JPEG files.
Only active when the BENCH_ACTIVE flag file exists — prevents filling
disk with images when we're not on the bench.

Usage: touch BENCH_ACTIVE to enable, rm BENCH_ACTIVE to disable.
"""

import base64
import json
import os
import time

BRIDGE_DIR = os.path.dirname(os.path.abspath(__file__))
BENCH_FLAG = os.path.join(BRIDGE_DIR, "BENCH_ACTIVE")
SNAPSHOT_DIR = os.path.join(BRIDGE_DIR, "snapshots")

# In-progress reassembly: id -> {chunks: dict, total: int}
_pending = {}


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
    
    t = msg.get("t")
    if t == "snaptest_chunk":
        return _feed_chunk(msg)
    elif t == "snaptest_image":
        # Image complete notification — chunks should already be assembled
        return None
    
    return None


def _feed_chunk(msg: dict) -> str | None:
    """Feed a chunk into the reassembler. Returns path when complete."""
    img_id = msg.get("id", 0)
    seq = msg.get("seq", 0)
    total = msg.get("total", 0)
    b64_data = msg.get("b64", "")
    is_last = msg.get("last", False)
    
    if img_id not in _pending:
        _pending[img_id] = {"chunks": {}, "total": total}
    
    _pending[img_id]["chunks"][seq] = b64_data
    _pending[img_id]["total"] = total
    
    # Check if complete
    pending = _pending[img_id]
    if len(pending["chunks"]) >= total or is_last:
        # Reassemble in order
        try:
            os.makedirs(SNAPSHOT_DIR, exist_ok=True)
            parts = []
            for i in range(total):
                if i in pending["chunks"]:
                    parts.append(pending["chunks"][i])
                else:
                    # Missing chunk
                    return None
            
            b64_combined = "".join(parts)
            img_data = base64.b64decode(b64_combined)
            
            # Verify JPEG
            if not img_data.startswith(b"\xff\xd8"):
                return None
            
            # Trim at EOI if needed
            eoi = img_data.rfind(b"\xff\xd9")
            if eoi > 0:
                img_data = img_data[:eoi+2]
            
            timestamp = time.strftime("%Y%m%d-%H%M%S")
            path = os.path.join(SNAPSHOT_DIR, f"snap-{timestamp}-{img_id}.jpg")
            with open(path, "wb") as f:
                f.write(img_data)
            
            del _pending[img_id]
            return path
            
        except Exception:
            return None
    
    return None
