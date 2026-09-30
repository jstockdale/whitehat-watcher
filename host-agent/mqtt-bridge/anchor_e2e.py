#!/usr/bin/env python3
"""
End-to-end anchor pipeline: detect in camera frame, pair with page
telemetry, refine the twin's S<->C homography.

Usage:
    python3 anchor_e2e.py <snapshot.jpg> [--telemetry <json>]

The telemetry JSON comes from report_fiducials.py:
    {"vw":..,"vh":..,"sx":..,"sy":..,"marks":{"tl":{"x":..,"y":..},...}}
"""
import json
import sys
sys.path.insert(0, "/home/hatch/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge")

from fiducial import find_fiducials_v2
from anchor_pipeline import geometric_assign as assign_corners_geometric
from screen_twin import ScreenTwin


def main():
    snap = sys.argv[1]
    telem_path = None
    if "--telemetry" in sys.argv:
        telem_path = sys.argv[sys.argv.index("--telemetry") + 1]
    if not telem_path:
        print("need --telemetry <json from report_fiducials.py>")
        sys.exit(2)

    telem = json.load(open(telem_path))
    marks_t = telem.get("marks", {})
    vw, vh = telem.get("vw"), telem.get("vh")
    print(f"telemetry: viewport {vw}x{vh}, marks: {sorted(marks_t.keys())}")

    # detect (returns tuple: (deduplicated_list, debug_dict))
    raw = find_fiducials_v2(snap)
    dets = raw[0] if isinstance(raw, (list, tuple)) else raw
    print(f"detector: {len(dets)} candidates")
    points = [d["center"] for d in dets]
    scores = {d["center"]: d["score"] for d in dets}
    assigned = assign_corners_geometric(points)
    print("assigned:")
    for cid, xy in assigned.items():
        if cid.startswith("_"):
            continue
        print(f"  {cid}: cam=({xy[0]:.1f},{xy[1]:.1f}) "
              f"score={scores.get(xy, 0):.3f}")

    # pair with telemetry
    marks = []
    for cid, xy in assigned.items():
        if cid.startswith("_"):
            continue
        tm = marks_t.get(cid)
        if not tm:
            print(f"  {cid}: no telemetry mark, skipping")
            continue
        marks.append({"corner_id": cid,
                      "screen_xy": (tm["x"], tm["y"]),
                      "cam_xy": xy,
                      "confidence": scores.get(xy, 0.5)})
    print(f"paired: {len(marks)} marks")

    # refine twin
    twin = ScreenTwin(J=[[0.205862, 0.003448], [-0.007586, 0.194828]])
    res = twin.update_geometry_fiducials(marks, (vw, vh))
    print("twin update:", json.dumps(res, indent=1))

    # report absolute position of image center as a sanity check
    cx, cy = 320.0, 240.0
    if twin.H_sc is not None:
        s = twin.cam_to_screen((cx, cy))
        print(f"camera center (320,240) -> screen ({s[0]:.1f},{s[1]:.1f})")
    print(f"geometry confidence: {twin.geometry_confidence:.3f}")


if __name__ == "__main__":
    main()
