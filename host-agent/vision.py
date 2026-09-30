"""Phase 0 vision helpers (real OpenCV code).

Pipeline for "eyes on screen":
  1. The Watcher POSTs a JPEG still of the room to StillServer.
  2. find_screen_quad() locates the monitor as the largest quadrilateral
     contour in the still.
  3. warp_screen() produces a top-down view of the display.
  4. (Phase 3) a grounding step maps a natural-language target
     ("the Download button") to normalized (x, y) coordinates in the
     warped view, which watcherd click_at then executes.

Only steps 1-3 are implemented here. Grounding is a clearly marked stub.
"""

from __future__ import annotations

import os

import cv2
import numpy as np


# ---------------------------------------------------------------------------
# Screen detection + rectification (implemented)
# ---------------------------------------------------------------------------

def _order_points(pts: np.ndarray) -> np.ndarray:
    """Order 4 points as top-left, top-right, bottom-right, bottom-left."""
    rect = np.zeros((4, 2), dtype=np.float32)
    s = pts.sum(axis=1)
    rect[0] = pts[np.argmin(s)]  # top-left: smallest x+y
    rect[2] = pts[np.argmax(s)]  # bottom-right: largest x+y
    diff = np.diff(pts, axis=1).ravel()
    rect[1] = pts[np.argmin(diff)]  # top-right: smallest y-x
    rect[3] = pts[np.argmax(diff)]  # bottom-left: largest y-x
    return rect


def find_screen_quad(bgr: np.ndarray, min_area_ratio: float = 0.05) -> np.ndarray | None:
    """Find a monitor screen in a BGR image.

    Returns the 4 corner points (ordered TL, TR, BR, BL, float32) of the
    largest quadrilateral contour covering at least ``min_area_ratio`` of
    the image area, or None when no plausible screen is found.
    """
    if bgr is None or bgr.ndim != 3:
        raise ValueError("bgr must be a BGR image array")
    height, width = bgr.shape[:2]

    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (5, 5), 0)
    edges = cv2.Canny(blurred, 50, 150)
    edges = cv2.dilate(edges, None, iterations=1)  # close small gaps in bezels

    contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    min_area = min_area_ratio * width * height

    best: np.ndarray | None = None
    best_area = 0.0
    for contour in contours:
        area = cv2.contourArea(contour)
        if area < min_area or area <= best_area:
            continue
        peri = cv2.arcLength(contour, True)
        approx = cv2.approxPolyDP(contour, 0.02 * peri, True)
        if len(approx) == 4:
            best = approx.reshape(4, 2).astype(np.float32)
            best_area = area

    if best is None:
        return None
    return _order_points(best)


def warp_screen(bgr: np.ndarray, quad: np.ndarray, w: int = 1280, h: int = 800) -> np.ndarray:
    """Perspective-warp the screen quad to a top-down w x h view."""
    quad = np.asarray(quad, dtype=np.float32).reshape(4, 2)
    dst = np.array([[0, 0], [w - 1, 0], [w - 1, h - 1], [0, h - 1]], dtype=np.float32)
    matrix = cv2.getPerspectiveTransform(_order_points(quad), dst)
    return cv2.warpPerspective(bgr, matrix, (w, h))


def draw_quad(bgr: np.ndarray, quad: np.ndarray | None, color=(0, 255, 0)) -> np.ndarray:
    """Return a copy of the image with the detected quad outlined (debug)."""
    annotated = bgr.copy()
    if quad is not None:
        pts = np.asarray(quad, dtype=np.int32).reshape((-1, 1, 2))
        cv2.polylines(annotated, [pts], True, color, 3)
    return annotated


def save_debug(path: str, image: np.ndarray) -> str:
    """Write a debug image (annotated still, warped view). Returns the path."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    if not cv2.imwrite(path, image):
        raise IOError(f"cv2.imwrite failed for {path!r}")
    return path


# ---------------------------------------------------------------------------
# Target grounding (STUBS — Phase 3)
# ---------------------------------------------------------------------------

def ground_template(warped_bgr: np.ndarray, template_bgr: np.ndarray) -> tuple[float, float]:
    """STUB (Phase 3): locate a known UI element by template matching.

    Intended behaviour: match ``template_bgr`` inside the warped screen view
    and return the normalized (x, y) center of the best match in [0, 1].

    Not implemented in Phase 0.
    """
    raise NotImplementedError(
        "ground_template is a Phase 3 stub: implement with cv2.matchTemplate "
        "against the warped screen view and return normalized center coords."
    )


def find_target_vlm(warped_bgr: np.ndarray, description: str) -> tuple[float, float]:
    """Future Muse VLM hook (NOT implemented in Phase 0 — do not call).

    Intended contract for Phase 3+:
      Input:  a warped (top-down) screenshot of the screen plus a natural
              language target, e.g. description="the blue Download button".
      Output: normalized (x, y) coordinates in [0, 1] of the target's
              center within the warped view.

    The host agent will call this by sending the warped image + description
    to Muse (vision-language model) and parsing the returned coordinates;
    ``watcherd click_at --x X --y Y`` then executes the click. No API is
    called from this stub.
    """
    raise NotImplementedError(
        "find_target_vlm is a Phase 3 hook: wire it to a VLM call that takes "
        "(warped screenshot, natural-language description) and returns "
        "normalized (x, y) coordinates."
    )
