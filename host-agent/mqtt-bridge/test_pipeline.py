#!/usr/bin/env python3
"""
Integration test for the eyes-on-screen pipeline modules.

Tests that all modules import, interlock correctly, and produce sane
outputs on real bench images. NO hardware commands are sent.

Modules:
  cursor_ops   - HID planning, waypoint routing, recovery
  cursor_detect - wiggle + blob cursor detection
  ui_detect    - orange button + text field detection
  calibrate    - RANSAC Jacobian fitting
  twin_ops     - digital twin wiring
  servo        - closed-loop servo (planning math only)
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

PASS = []
FAIL = []


def check(name, fn):
    try:
        fn()
        PASS.append(name)
        print(f"  PASS {name}")
    except Exception as e:
        FAIL.append((name, str(e)))
        print(f"  FAIL {name}: {e}")


def t_imports():
    import cursor_ops, cursor_detect, ui_detect, calibrate, twin_ops, servo
    assert hasattr(cursor_detect, "detect_wiggle")
    assert hasattr(cursor_detect, "detect_bright_blob")
    assert hasattr(ui_detect, "find_orange_button")
    assert hasattr(ui_detect, "find_text_field")
    assert hasattr(calibrate, "calibration_report")
    assert hasattr(servo, "Servo")


def t_planner_avoids_textfield():
    import cursor_ops
    # Submit button inside text-field rect must route via 2 legs
    legs = cursor_ops.plan_around((345, 380), (379, 455),
                                  [(100, 440, 380, 475)])
    assert len(legs) == 2, f"expected 2 legs, got {len(legs)}"
    # Target above rect goes direct
    legs2 = cursor_ops.plan_around((345, 380), (379, 421),
                                   [(100, 440, 380, 475)])
    assert len(legs2) == 1, f"expected 1 leg, got {len(legs2)}"


def t_calibration_recovers_validated():
    import calibrate
    data = [((200,0),(41,-1)), ((0,200),(0,35)),
            ((500,0),(103,-4)), ((0,500),(2,99))]
    rep = calibrate.calibration_report(data)
    J = rep["J"]
    assert abs(J[0][0] - 0.205862) < 1e-4, f"J[0][0]={J[0][0]}"
    assert rep["n_outliers"] == 0


def t_calibration_rejects_outlier():
    import calibrate
    data = [((200,0),(41,-1)), ((0,200),(0,35)),
            ((500,0),(103,-4)), ((0,500),(2,99)),
            ((300,0),(200,150))]  # bogus
    rep = calibrate.calibration_report(data)
    assert 4 in rep["outlier_indices"], "RANSAC must flag bogus point"


def t_twin_predict_observe():
    import tempfile, os
    from twin_ops import TwinOps
    with tempfile.TemporaryDirectory() as td:
        ops = TwinOps(state_path=os.path.join(td, "t.json"))
        ops.twin.command(100, 0, note="test")
        assert ops.believed_hid() == (100.0, 0.0)
        v = ops.observe((20, 1), confidence=0.9)
        assert v.get("accepted", v) is True or v is True


def t_detector_chain():
    """ui_detect finds button -> plan_around routes -> servo plans."""
    import cursor_ops, ui_detect
    imgs = sorted(Path("/tmp").glob("reply_p*.jpg"))
    if not imgs:
        print("    (no /tmp/reply_p*.jpg images, skipping)")
        return
    img = str(imgs[-1])
    bx, by, bconf, bradius = ui_detect.find_orange_button(img)
    assert bx is not None, "button not found"
    assert bconf > 0.3, f"button confidence too low: {bconf}"
    tf = ui_detect.find_text_field(img)
    # find_text_field returns ((x0,y0,x1,y1), confidence)
    rect, tconf = tf[0], tf[1]
    if rect is not None:
        legs = cursor_ops.plan_around((300, 350), (bx, by), [rect])
        # Just verify it doesn't crash; leg count depends on geometry
        assert len(legs) >= 1


if __name__ == "__main__":
    print("Pipeline integration test (no hardware):")
    check("imports", t_imports)
    check("planner avoids text field", t_planner_avoids_textfield)
    check("calibration recovers validated J", t_calibration_recovers_validated)
    check("calibration rejects outlier", t_calibration_rejects_outlier)
    check("twin predict/observe", t_twin_predict_observe)
    check("detector -> planner chain", t_detector_chain)
    print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        sys.exit(1)
    print("ALL INTEGRATION TESTS PASSED")
