#!/usr/bin/env python3
"""
Move execution for the Watcher eyes-on-screen cursor pipeline.

Two pieces:
- check_move_sane: pre-move sanity checklist. Warns instead of trusting.
- move_to_target: iterative closed-loop move. Keeps correcting until the
  cursor is within tolerance of the target or it runs out of iterations.

Bench I/O is injected as callables (send_glide_fn, snapshot_fn) so this
module never touches MQTT itself and stays testable offline:

    history = move_to_target(
        cal, current=(122, 224), target=(235.5, 246.5),
        send_glide_fn=lambda dx, dy, ms: None,   # real one sends MQTT
        snapshot_fn=lambda: take_snapshot(...),  # real one captures
    )
"""
from typing import Callable, Dict, List, Optional, Tuple

from cursor_calibration import CursorCalibrator
from cursor_vision import localize_cursor, IMG_W, IMG_H
from screen_twin import ScreenTwin

# HID moves bigger than this are almost certainly a bug, not a plan.
MAX_SANE_HID = 2000


def check_move_sane(cursor: Tuple[float, float],
                    target: Tuple[float, float],
                    hid_dx: int, hid_dy: int,
                    img_w: int = IMG_W, img_h: int = IMG_H,
                    cal: Optional[CursorCalibrator] = None) -> List[str]:
    """
    Pre-move sanity checklist. Returns a list of warning strings (empty =
    everything looks fine). The caller decides whether to proceed.

    Warns when:
    - the target is outside the image bounds
    - the HID delta is unreasonably large (> MAX_SANE_HID on either axis)
    - the predicted landing (via cal) is outside the image
    - the move is a no-op (target == cursor)
    """
    warnings = []
    cx, cy = cursor
    tx, ty = target

    if not (0 <= tx < img_w and 0 <= ty < img_h):
        warnings.append(
            f"target ({tx:.0f},{ty:.0f}) is outside the {img_w}x{img_h} image")

    if abs(hid_dx) > MAX_SANE_HID or abs(hid_dy) > MAX_SANE_HID:
        warnings.append(
            f"HID delta ({hid_dx},{hid_dy}) exceeds sane max {MAX_SANE_HID} "
            f"-- likely a bad target or calibration")

    if hid_dx == 0 and hid_dy == 0:
        warnings.append("HID delta is (0,0): move is a no-op")

    dist = ((tx - cx) ** 2 + (ty - cy) ** 2) ** 0.5
    if dist < 2:
        warnings.append(
            f"cursor already within {dist:.1f}px of target; move is pointless")

    if cal is not None and cal.J is not None:
        px, py = cal.predict_landing((cx, cy), hid_dx, hid_dy)
        if not (0 <= px < img_w and 0 <= py < img_h):
            warnings.append(
                f"predicted landing ({px:.0f},{py:.0f}) is outside the "
                f"{img_w}x{img_h} image -- cursor may leave the frame")

    return warnings


def move_to_target(cal: CursorCalibrator,
                   current: Tuple[float, float],
                   target: Tuple[float, float],
                   tolerance_px: float = 8.0,
                   max_iters: int = 3,
                   send_glide_fn: Optional[Callable[[int, int, int], None]] = None,
                   snapshot_fn: Optional[Callable[[], str]] = None,
                   before_img: Optional[str] = None,
                   dur_ms: int = 1000,
                   min_confidence: float = 0.5,
                   twin: Optional[ScreenTwin] = None) -> List[Dict]:
    """
    Iteratively move the cursor onto target.

    Each iteration: compute HID via the inverse Jacobian, sanity-check,
    send the glide, snapshot, re-localize the cursor, measure the error.
    Stops when error <= tolerance_px or max_iters is reached.

    send_glide_fn(dx, dy, dur_ms): performs the move (injected bench I/O).
    snapshot_fn() -> image_path: captures and returns the new frame path.
    before_img: the frame `current` was localized from (lets the first
        iteration localize instead of flying blind).
    twin: optional ScreenTwin. When given, every commanded move is fed
        to twin.command() (predict) and every detection to
        twin.observe_cursor() (correct); the twin's filtered belief --
        not the raw detection -- seeds the next iteration, so weak or
        occluded detections don't derail the loop. A twin sanity-check
        mismatch is appended to the iteration's warnings.

    Returns the full history: one dict per iteration with
    {iter, hid, predicted, landed, error_px, confidence, warnings}.
    The last entry's error_px tells the caller whether we converged.
    """
    if send_glide_fn is None or snapshot_fn is None:
        raise ValueError(
            "send_glide_fn and snapshot_fn are required (bench I/O is "
            "injected so this stays testable offline)")

    history: List[Dict] = []
    pos = (float(current[0]), float(current[1]))
    last_img: Optional[str] = before_img

    if twin is not None and twin.hid_origin_cam is None:
        # anchor the twin's HID origin on the known current position
        twin.observe_cursor(pos, confidence=0.9)

    for i in range(max_iters):
        hid = cal.compute_move_to_target(pos, target)
        warnings = check_move_sane(pos, target, hid[0], hid[1], cal=cal)
        predicted = cal.predict_landing(pos, hid[0], hid[1])

        entry: Dict = {"iter": i + 1, "hid": hid, "predicted": predicted,
                       "from": pos, "warnings": warnings}

        if twin is not None:
            twin.command(hid[0], hid[1], note=f"move_to_target iter {i + 1}")
        send_glide_fn(hid[0], hid[1], dur_ms)
        new_img = snapshot_fn()

        if last_img is not None:
            landed, conf, detail = localize_cursor(
                last_img, new_img, prior_pos=pos)
        else:
            landed, conf, detail = None, 0.0, {"note": "no before image"}

        entry.update({"landed": landed, "confidence": conf,
                      "detail": detail, "after_img": new_img})

        if landed is not None:
            err = ((landed[0] - target[0]) ** 2 +
                   (landed[1] - target[1]) ** 2) ** 0.5
            entry["error_px"] = round(err, 1)
            if twin is not None:
                obs = twin.observe_cursor(landed, confidence=conf)
                entry["twin_observation"] = obs
                sc = twin.sanity_check(landed)
                if not sc["ok"]:
                    entry["warnings"] = warnings + [sc["message"]]
            if conf >= min_confidence:
                pos = (float(landed[0]), float(landed[1]))
            else:
                entry["warnings"] = entry.get("warnings", warnings) + [
                    f"localization confidence {conf:.2f} < {min_confidence}: "
                    f"keeping predicted position, consider re-hunting"]
                pos = predicted
            if twin is not None and twin.believed_cam is not None:
                # filtered belief carries through weak detections
                pos = twin.believed_cam
        else:
            entry["error_px"] = None
            pos = predicted

        history.append(entry)
        last_img = new_img

        if entry["error_px"] is not None and entry["error_px"] <= tolerance_px:
            break

    return history
