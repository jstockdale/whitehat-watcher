#!/usr/bin/env python3
"""Fast eyes-on-screen pipeline for the Watcher (host side).

What was slow before, and what this does instead:

  BEFORE (tonight's manual loop):
    - every photo cost a fixed ~30 s sleep, then manual inspection
    - calibration was a 4-corner, 4-photo square (~4+ min) that produced a
      garbage homography when extrapolated (4 clustered points cannot
      constrain projective terms -- lesson learned 2026-09-23)
    - cursor localization used template matching, which breaks whenever
      the cursor changes shape (arrow vs I-beam vs hand pointer)

  NOW:
    - event-driven capture: subscribe first, send snaptest, return the
      moment the last chunk lands (typically 5-10 s, no fixed sleeps)
    - NO separate calibration phase. Visual servoing: every homing move
      doubles as a calibration measurement, refining the local
      px-per-HID-unit scale each iteration. This is what actually worked
      tonight (the 300-dy move became a long-baseline calibration).
    - cursor found by micro-jitter differencing: shape-agnostic, works
      for arrow / I-beam / hand cursors alike
    - button found by blue-channel mask (ms, no template)
    - motion watchdog: periodic photos, global frame differencing; a
      large global shift means the device moved -> flag for re-home
    - local 2x2 Jacobian with cross terms (LocalMap): the projection is
      non-orthonormal, so a single scalar scale is wrong; every verified
      move refines the Jacobian cell for its screen region
    - move verification wired in: every servo move is checked against its
      expected displacement; a <50% result invalidates the cell and forces
      a fresh probe measurement instead of limping on with a bad map
    - exposure-normalized differencing + glare masking: global brightness
      shifts no longer poison diffs; saturated glare blobs are excluded
    - visibility policy: bright (white-on-white) regions switch to
      diff-only localization; glare regions are not servable
    - regression auto-collection: every failed case is saved with its
      frame into cursor-model/regression/ for the next training round

SAFETY: mouse_move is never sent unless the caller passes
allow_moves=True. The operator only sets that after John explicitly
says "go" for that run. The watchdog detects movement; it does NOT
re-home on its own.

Usage:
    python3 eyes_fast.py photo                 # one capture, print latency
    python3 eyes_fast.py button                 # capture + locate blue button
    python3 eyes_fast.py cursor --allow-moves   # jitter-pair cursor locate
    python3 eyes_fast.py home --allow-moves     # servo cursor onto button
    python3 eyes_fast.py watch                  # motion watchdog (no moves)

Credentials: recovered transiently from the running subscriber.py process
environ (same procedure as the flash flow); never printed or stored.
"""
import base64
import io
import json
import os
import shutil
import ssl
import subprocess
import sys
import time
import uuid

import paho.mqtt.client as mqtt

from topic_acl import publish_checked
from PIL import Image
import numpy as np

HOST = os.environ.get("MQTT_HOST", "mqtt.offx1.com")
PORT = int(os.environ.get("MQTT_PORT", "8883"))
USER = os.environ.get("MQTT_USER", "muse")
TOPIC = os.environ.get("MQTT_TOPIC", "watcher")
OUT_DIR = os.path.expanduser(
    "~/workspace/projects/watcher-input-assistant/himax-captures")

CAPTURE_TIMEOUT = 30      # s; event-driven, exits early on completion
MOVE_SETTLE = 1.2         # s between a move ack and the next capture
JITTER_PX = 40             # HID units for the cursor-reveal micro move
SERVO_TOL_PX = 4.0        # stop when cursor within this of target
SERVO_MAX_ITERS = 6
SERVO_MAX_STEP = 400      # HID units; never send a bigger single move
WATCH_INTERVAL = 45       # s between watchdog frames
WATCH_MOVE_THRESH = 0.06  # fraction of pixels changed => device moved

# ---- detection-pipeline hardening (2026-09-23, full set) ----------------
EXPOSURE_SHIFT_THRESH = 12.0  # gray levels; median shift above => reject diff
GLARE_SAT = 245               # pixel value treated as saturated
GLARE_MIN_PX = 200            # min saturated-blob size to count as glare
ERROR_BUDGET_PX = 150.0       # cumulative servo correction limit -> recalibrate
JACOBIAN_GRID = 4             # LocalMap is 4x4 cells over the frame
VERIFY_TOL_RATIO = 0.5        # move must cover >=50% of expected distance
OBSERVE_SETTLE_S = 2.0        # quiet time before snaptest after a settled move
REGRESSION_DIR = os.path.expanduser(
    "~/workspace/projects/watcher-input-assistant/cursor-model/regression")

# Cursor classes the v4 model will report; hotspot = the click point as a
# fraction of the detection bbox. Arrow tip is top-left, I-beam centers,
# pointing-hand fingertip is top-center. Diff-based localization returns a
# blob centroid approximating the arrow tip; per-class offsets apply once
# the model reports classes (see hotspot_for).
CURSOR_CLASSES = ("arrow", "ibeam", "hand")
CURSOR_HOTSPOT = {
    "arrow": (0.15, 0.10),   # tip
    "ibeam": (0.50, 0.50),   # center of the beam
    "hand": (0.45, 0.15),    # index fingertip
}


def recover_env():
    pids = subprocess.run(["pgrep", "-f", r"subscriber\.py"],
                          capture_output=True, text=True).stdout.split()
    for pid in pids:
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = f.read()
        except (PermissionError, FileNotFoundError):
            continue
        if b"MQTT_PASS=" in env:
            out = {}
            for entry in env.split(b"\0"):
                if b"=" not in entry:
                    continue
                k, v = entry.split(b"=", 1)
                key = k.decode("utf-8", "replace")
                if key.startswith("MQTT_"):
                    out[key] = v.decode("utf-8", "replace")
            # Guard: a wrapper shell's environ may contain MQTT_PASS= as a
            # substring of the command line without proper \0-separated
            # KEY=value entries. Only accept a pid that yields a usable dict.
            if "MQTT_PASS" in out:
                return out
            continue
    raise SystemExit("no subscriber with MQTT creds found")


class EyesLink:
    """One persistent MQTT client for commands + chunk capture."""

    def __init__(self, allow_moves=False):
        env = recover_env()
        self.host = env.get("MQTT_HOST", HOST)
        self.port = int(env.get("MQTT_PORT", PORT))
        self.user = env.get("MQTT_USER", USER)
        self.passwd = env["MQTT_PASS"]
        self.allow_moves = allow_moves
        self._runs = {}
        self._done_id = None
        self._seen_before = set()
        self._cmd_replies = []
        # Move journal (John's rule): every ACKED mouse_move is recorded so
        # a bad move can be backed out by sending its exact inverse.
        self.journal = []
        os.makedirs(OUT_DIR, exist_ok=True)

        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"muse-eyes-{os.getpid()}-{int(time.time())}")
        self.client.username_pw_set(self.user, self.passwd)
        self.client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                            tls_version=ssl.PROTOCOL_TLS_CLIENT)
        self.client.on_message = self._on_message
        self.client.connect(self.host, self.port, keepalive=30)
        self.client.subscribe(f"{TOPIC}/telemetry")
        self.client.loop_start()
        time.sleep(1.0)

    def _on_message(self, _c, _u, msg):
        if msg.topic != f"{TOPIC}/telemetry":
            return
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        for e in payload.get("events", []):
            try:
                j = json.loads(e.get("raw", ""))
            except Exception:
                continue
            t = j.get("t")
            if t == "snaptest_chunk":
                rid = j.get("id", 0)
                if rid in self._seen_before:
                    continue
                run = self._runs.setdefault(
                    rid, {"total": j.get("total"), "parts": {},
                          "last": False})
                run["total"] = j.get("total", run["total"])
                seq = j.get("seq")
                if seq not in run["parts"]:
                    run["parts"][seq] = j.get("b64", "")
                if j.get("last"):
                    run["last"] = True
                total = run["total"] or 0
                if (run["last"] and total
                        and len(run["parts"]) >= total
                        and self._done_id is None):
                    self._done_id = rid
            elif t == "snaptest":
                self._cmd_replies.append(j)
            elif t == "crop_zoom":
                # on-device crop-zoom reply (firmware support pending)
                self._cmd_replies.append(j)
            elif t == "ok" and isinstance(j.get("cmd"), str):
                # device ack, e.g. {"t":"ok","cmd":"mouse_move"}
                self._cmd_replies.append(j)

    def close(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass

    # ---- primitives -------------------------------------------------
    def _send_cmd(self, cmd):
        """Publish a console command in the {"id","cmd"} envelope the page
        expects. Raw (unenveloped) commands are silently ignored -- this
        was the bug that made the first capture attempt time out."""
        envelope = json.dumps({"id": uuid.uuid4().hex[:8], "cmd": cmd})
        publish_checked(self.client, f"{TOPIC}/command", envelope, qos=0)

    def capture(self, timeout=CAPTURE_TIMEOUT, settle_s=3.0):
        """Send snaptest, return (path, n_chunks, seconds) on completion.

        settle_s: seconds to wait BEFORE sending the snaptest request.
        The device needs quiet time after HID moves before it can reliably
        handle a snapshot (found 2026-09-23: 3s waits fixed intermittent
        timeouts). Default 3.0s; use 0 for back-to-back captures.
        """
        if settle_s > 0:
            time.sleep(settle_s)
        self._runs.clear()
        self._done_id = None
        # ignore runs that were already streaming before we asked
        self._seen_before = set(self._runs.keys())
        t0 = time.time()
        self._send_cmd({"t": "dbg", "cmd": "snaptest", "send": 1})
        deadline = t0 + timeout
        while time.time() < deadline and self._done_id is None:
            time.sleep(0.25)
        dt = time.time() - t0
        if self._done_id is None:
            partial = {rid: (len(r["parts"]), r["total"])
                       for rid, r in self._runs.items()}
            raise TimeoutError(f"no complete snapshot in {timeout}s "
                               f"(partial={partial})")
        rid = self._done_id
        run = self._runs[rid]
        total = run["total"]
        jpeg = base64.b64decode(
            "".join(run["parts"][i] for i in range(total)))
        stamp = time.strftime("%H%M%S")
        path = os.path.join(OUT_DIR, f"eyes-{stamp}-{rid}.jpg")
        with open(path, "wb") as f:
            f.write(jpeg)
        assert jpeg[:2] == b"\xff\xd8", "not a JPEG (SOI missing)"
        return path, total, dt

    def mouse_move(self, dx, dy, journal=True):
        """Gated: raises unless this session was explicitly authorized."""
        if not self.allow_moves:
            raise PermissionError(
                "mouse_move blocked: no per-use permission for this run "
                "(John must say go first)")
        dx = int(max(-SERVO_MAX_STEP, min(SERVO_MAX_STEP, dx)))
        dy = int(max(-SERVO_MAX_STEP, min(SERVO_MAX_STEP, dy)))
        self._cmd_replies.clear()
        self._send_cmd({"t": "mouse_move", "dx": dx, "dy": dy})
        # wait briefly for the device ack
        deadline = time.time() + 8
        while time.time() < deadline and not self._cmd_replies:
            time.sleep(0.25)
        ok = any(r.get("cmd") == "mouse_move" for r in self._cmd_replies)
        if ok and journal:
            # journal only moves the device confirmed, so back_out() can
            # invert exactly what happened (John's rule)
            self.journal.append((dx, dy))
        time.sleep(MOVE_SETTLE)
        return dx, dy, ok

    def mouse_dblclick(self, button="left"):
        """Gated: sends the device-native double-click (two clicks with
        on-device 120ms inter-click timing). Requires firmware with
        hid_ble_mouse_dblclick (v0.2.16+)."""
        if not self.allow_moves:
            raise PermissionError(
                "mouse_dblclick blocked: no per-use permission for this run "
                "(John must say go first)")
        self._cmd_replies.clear()
        self._send_cmd({"t": "mouse_dblclick", "button": button})
        deadline = time.time() + 8
        while time.time() < deadline and not self._cmd_replies:
            time.sleep(0.25)
        ok = any(r.get("cmd") == "mouse_dblclick" for r in self._cmd_replies)
        time.sleep(MOVE_SETTLE)
        return ok

    def back_out(self, n=1):
        """Undo the last n journaled moves by sending their exact inverse.

        John's recovery rule: if a move runs the cursor out of view,
        subtract the bad vector to bring it back. Only inverts moves the
        device actually acked. Returns the list of inverse moves sent.

        The inverse is sent with journal=False so a successful undo removes
        the original move without leaving the inverse as a new pending move.
        """
        if not self.allow_moves:
            raise PermissionError("back_out moves the mouse: needs per-use "
                                  "permission like any other mouse_move")
        undone = []
        for _ in range(min(n, len(self.journal))):
            dx, dy = self.journal.pop()
            inv = (-dx, -dy)
            rdx, rdy, ok = self.mouse_move(*inv, journal=False)
            undone.append((rdx, rdy, ok))
            if not ok:
                break
        return undone


# ---- vision ----------------------------------------------------------
def load_gray(path):
    return np.array(Image.open(path).convert("L")).astype(np.float32)


# ---- differencing primitives (exposure-normalized) --------------------
def signed_norm_diff(new, old):
    """Signed per-pixel difference with global exposure matched out.

    new/old: float32 grayscale arrays. Returns new - old after shifting
    new so its median matches old's. A global brightness change (camera
    auto-exposure, room light) then contributes ~0 instead of poisoning
    the diff. The cursor is tiny, so the median is unaffected by it.
    """
    new = np.asarray(new, dtype=np.float32)
    old = np.asarray(old, dtype=np.float32)
    shift = float(np.median(old)) - float(np.median(new))
    return (new + shift) - old


def exposure_shifted(a, b, thresh=EXPOSURE_SHIFT_THRESH):
    """True if global brightness moved too far for a diff to be trusted."""
    a = np.asarray(a, dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    return abs(float(np.median(a)) - float(np.median(b))) > thresh


def detect_glare(path_or_gray, sat=GLARE_SAT, min_px=GLARE_MIN_PX):
    """Boolean mask of specular glare: large saturated-white blobs.

    Glare doesn't move with the cursor or the UI, so it is excluded from
    differencing and centroiding. Accepts a path or a gray array.
    """
    from scipy import ndimage
    img = (load_gray(path_or_gray) if isinstance(path_or_gray, str)
           else np.asarray(path_or_gray, dtype=np.float32))
    sat_mask = img > sat
    lab, n = ndimage.label(sat_mask)
    if n == 0:
        return np.zeros(img.shape, dtype=bool)
    sizes = ndimage.sum(sat_mask, lab, range(1, n + 1))
    glare = np.zeros(img.shape, dtype=bool)
    for k, sz in enumerate(sizes, start=1):
        if sz >= min_px:
            glare |= (lab == k)
    return glare


def is_glare_region(path, x, y, r=15, frac=0.5):
    """True if most of the region around (x,y) is saturated glare."""
    g = detect_glare(path)
    h, w = g.shape
    x0, x1 = max(0, int(x - r)), min(w, int(x + r))
    y0, y1 = max(0, int(y - r)), min(h, int(y + r))
    if x0 >= x1 or y0 >= y1:
        return False
    return bool(g[y0:y1, x0:x1].mean() > frac)


def detect_camera_shift(ref_gray, cur_gray, thresh=WATCH_MOVE_THRESH):
    """True if the whole frame changed -> the camera moved, not the UI.

    Exposure-normalized, so a room-light change doesn't false-positive.
    When True, every cached coordinate and Jacobian cell is stale.
    """
    ref_gray = np.asarray(ref_gray, dtype=np.float32)
    cur_gray = np.asarray(cur_gray, dtype=np.float32)
    if ref_gray.shape != cur_gray.shape:
        return True
    d = np.abs(signed_norm_diff(cur_gray, ref_gray))
    return float((d > 30).mean()) >= thresh


def classify_visibility(path, x, y, r=15):
    """'ok' | 'bright' | 'glare' for the region around (x,y).

    bright = white-on-white risk: the cursor may be invisible; never
             trust appearance here, use motion-diff only.
    glare  = saturated specular blob: avoid the region entirely.
    """
    if is_glare_region(path, x, y, r):
        return "glare"
    if is_bright_region(path, x, y, r):
        return "bright"
    return "ok"


def localization_policy(path, x, y):
    """Which localization method the pipeline may trust at (x,y).

    Returns 'visual_or_diff', 'diff_only', or 'relocate'. This is a
    decision, not a flag: 'bright' regions switch the strategy to
    deliberate motion-diff (move to create a signature), and 'glare'
    regions are not servable at all.
    """
    v = classify_visibility(path, x, y)
    if v == "ok":
        return "visual_or_diff"
    if v == "bright":
        return "diff_only"
    return "relocate"


def hotspot_for(cls_name, bbox):
    """Click point for a model detection: (x, y, w, h) + class -> (x, y).

    The v4 cursor model reports one of CURSOR_CLASSES; the hotspot is the
    actual click point, not the bbox center (arrow tip != bbox center).
    Unknown classes fall back to center.
    """
    x, y, w, h = bbox
    fx, fy = CURSOR_HOTSPOT.get(cls_name, (0.5, 0.5))
    return float(x + fx * w), float(y + fy * h)


def find_button(path):
    """Blue-circle-with-white-center button -> (x, y) centroid or None.

    Pure color mask: fast (ms) and template-free. Returns None when the
    button is not in frame instead of guessing.
    """
    img = np.array(Image.open(path).convert("RGB")).astype(np.float32)
    b, r, g = img[:, :, 2], img[:, :, 0], img[:, :, 1]
    mask = (b > 140) & ((b - r) > 45) & ((b - g) > 45)
    ys, xs = np.nonzero(mask)
    if len(xs) < 80:          # too few blue pixels to be the button
        return None
    # keep the largest connected-ish blob: use distance from median
    mx, my = np.median(xs), np.median(ys)
    d = np.hypot(xs - mx, ys - my)
    keep = d < (np.median(d) * 3 + 12)
    if keep.sum() < 80:
        return None
    return float(xs[keep].mean()), float(ys[keep].mean())


def locate_cursor(link, jitter=JITTER_PX):
    """Reveal the cursor by micro-jitter differencing.

    Shape-agnostic: works for arrow, I-beam, and HAND cursors because it
    detects *motion*, not appearance.

    CURSOR STATES (learned 2026-09-23):
    - Arrow: default over non-interactive areas
    - I-beam: over text/editable areas
    - POINTING HAND: over clickable UI (buttons, links) -- this is what
      fooled visual detection tonight; the hand looks nothing like an arrow
      at camera resolution. Diff-based detection handles all three.

    Restores the cursor to its exact starting point afterwards (net-zero).

    Exposure-normalized and glare-masked: a global brightness shift
    between the two frames (or a saturated glare blob) no longer creates
    phantom detections. Returns (None, p1) on exposure rejection so the
    caller can retry -- a rejected diff is not evidence of absence.
    """
    p0, _, _ = link.capture()
    a = load_gray(p0)
    link.mouse_move(jitter, 0)
    p1, _, _ = link.capture(settle_s=OBSERVE_SETTLE_S)
    b = load_gray(p1)
    link.mouse_move(-jitter, 0)   # restore
    if exposure_shifted(a, b):
        log_regression_case("locate_exposure", p1, None, None,
                            "exposure shift across jitter reveal")
        return None, p1
    d = signed_norm_diff(b, a)
    d[detect_glare(p1)] = 0  # glare never moves; exclude it
    diff = np.abs(d)
    # ignore global illumination flicker: threshold relative to frame noise
    thr = max(28.0, np.median(diff) + 4 * diff.std())
    ys, xs = np.nonzero(diff > thr)
    if len(xs) == 0:
        return None, p1
    # cursor is small: drop large changed regions (UI animation)
    mx, my = np.median(xs), np.median(ys)
    d = np.hypot(xs - mx, ys - my)
    keep = d < 40
    if keep.sum() < 3:
        return None, p1
    return (float(xs[keep].mean()), float(ys[keep].mean())), p1


def frame_change(a_path, b_path, normalize=True):
    """Fraction of pixels changed between two frames (0..1).

    normalize=True (default) matches out global exposure shifts first, so
    a room-light change doesn't read as device movement.
    """
    a = load_gray(a_path)
    b = load_gray(b_path)
    if a.shape != b.shape:
        return 1.0
    d = (np.abs(signed_norm_diff(b, a)) if normalize
         else np.abs(a - b))
    return float((d > 30).mean())


def find_lowest_blue_bubble(path, min_size=800):
    """Target selector for the chat-bubble task: the bottom-most large
    blue region (the latest chat bubble). Returns (x, y) centroid."""
    from scipy import ndimage
    img = np.array(Image.open(path).convert("RGB")).astype(np.float32)
    b, r, g = img[:, :, 2], img[:, :, 0], img[:, :, 1]
    mask = (b > 140) & ((b - r) > 45) & ((b - g) > 45)
    lab, n = ndimage.label(mask)
    if n == 0:
        return None
    sizes = ndimage.sum(mask, lab, range(1, n + 1))
    best = None
    for k, s in enumerate(sizes, start=1):
        if s < min_size:
            continue
        ys, xs = np.where(lab == k + 1)
        cy = float(ys.mean())
        if best is None or cy > best[1]:
            best = ((float(xs.mean()), cy), cy, int(s))
    return best[0] if best else None


def find_rail_icons(path):
    """Detect grey circular icons in the left rail.

    The Watcher Console / Muse UI left rail has a vertical stack of small
    grey circles containing icons. This finds them via:
    - Left third of image (the rail region)
    - Grey color (R≈G≈B, mid-brightness, low saturation)
    - Compact blob shape (roughly circular)

    Returns list of (x, y, size) sorted top-to-bottom, or [] if none found.
    Use this to re-detect the chat bubble icon instead of stale coordinates.
    """
    from scipy import ndimage
    img = np.array(Image.open(path).convert("RGB")).astype(np.float32)
    h, w = img.shape[:2]
    # Left rail: left third of image
    rail = img[:, :w//3]
    r, g, b = rail[:, :, 0], rail[:, :, 1], rail[:, :, 2]
    # Grey: channels close together, mid brightness (not black, not white)
    # and low saturation
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = mx - mn
    bright = (r + g + b) / 3
    grey = (sat < 30) & (bright > 60) & (bright < 180)
    # Clean up and find blobs
    lab, n = ndimage.label(grey)
    if n == 0:
        return []
    sizes = ndimage.sum(grey, lab, range(1, n + 1))
    icons = []
    for k, s in enumerate(sizes, start=1):
        if s < 50 or s > 2000:  # icon-sized blobs only
            continue
        ys, xs = np.where(lab == k)
        # Check roughly circular: bounding box aspect ratio near 1
        h_bb = ys.max() - ys.min() + 1
        w_bb = xs.max() - xs.min() + 1
        if max(h_bb, w_bb) / max(1, min(h_bb, w_bb)) > 1.5:
            continue
        # Must be reasonably compact (not a thin line)
        if h_bb < 8 or w_bb < 8:
            continue
        icons.append((float(xs.mean()), float(ys.mean()), int(s)))
    # Sort top-to-bottom
    icons.sort(key=lambda t: t[1])
    return icons


def find_chat_icon(path, feed_icon_y=None):
    """Find the chat-bubble icon in the left rail.

    Per John (2026-09-23): the chat button is two icons ABOVE the feed icon
    in the left rail. If feed_icon_y is given (y-coordinate of the feed icon),
    returns the icon two positions above it. Otherwise returns the topmost
    icon (heuristic -- verify visually).

    Returns (x, y) or None.
    """
    icons = find_rail_icons(path)
    if not icons:
        return None
    if feed_icon_y is not None:
        # Find the icon closest to feed_icon_y, then go two up
        idx = min(range(len(icons)), key=lambda i: abs(icons[i][1] - feed_icon_y))
        chat_idx = idx - 2
        if 0 <= chat_idx < len(icons):
            return icons[chat_idx][0], icons[chat_idx][1]
        return None
    # Fallback: topmost icon (verify!)
    return icons[0][0], icons[0][1]


# ---- general-purpose UI element finder (arbitrary icons/places) --------
# Goal (per John 2026-09-23): find ARBITRARY icons or places on screen,
# not just hardcoded ones. Three approaches:

def find_template(screen_path, template_path, threshold=0.7):
    """Find a template image within a screenshot.

    Uses sum-of-squared-differences (SSD) which is robust for finding an
    icon cropped from a similar screenshot. More reliable than NCC for
    our camera images which have varying local contrast.

    Args:
        screen_path: path to the screenshot (e.g. from link.capture())
        template_path: path to the template image (cropped icon)
        threshold: minimum match quality (0..1). Higher = stricter.
            Quality = 1 / (1 + normalized_ssd). 0.7 is a good default.

    Returns (x, y) center of best match, or None if below threshold.
    """
    from scipy import signal
    screen = np.array(Image.open(screen_path).convert("L")).astype(np.float32)
    tmpl = np.array(Image.open(template_path).convert("L")).astype(np.float32)
    th, tw = tmpl.shape
    sh, sw = screen.shape
    if th > sh or tw > sw:
        return None
    # SSD via: (a-b)^2 = a^2 + b^2 - 2ab
    # a^2 term: local sum of squares of screen
    # Use uniform filter for local sums
    from scipy import ndimage
    # Local sum of screen^2 (valid region only)
    s2 = screen ** 2
    # Box filter sum
    local_s2 = ndimage.uniform_filter(s2, size=(th, tw), mode="constant",
                                     cval=0) * th * tw
    vh, vw = sh - th + 1, sw - tw + 1
    local_s2 = local_s2[th//2:th//2+vh, tw//2:tw//2+vw]
    # Cross-correlation term
    corr = signal.correlate(screen, tmpl, mode="valid", method="fft")
    # SSD = local_s2 + sum(tmpl^2) - 2*corr
    t2 = (tmpl ** 2).sum()
    ssd = local_s2 + t2 - 2 * corr
    ssd = np.maximum(ssd, 0)
    # Normalize by template energy for scale invariance
    # Quality metric: 1 / (1 + ssd / (t2 + eps))
    quality = 1.0 / (1.0 + ssd / (t2 + 1e-9))
    y, x = np.unravel_index(np.argmax(quality), quality.shape)
    peak_q = quality[y, x]
    if peak_q < threshold:
        return None
    return float(x + tw / 2), float(y + th / 2)


def save_template(screen_path, x, y, w, h, out_path):
    """Crop a template from a screenshot for use with find_template().

    Args:
        screen_path: screenshot to crop from
        x, y: top-left corner of crop region (camera pixels)
        w, h: width/height of crop region
        out_path: where to save the template

    Returns out_path.
    """
    img = Image.open(screen_path)
    # Clamp to image bounds
    iw, ih = img.size
    x = max(0, int(x))
    y = max(0, int(y))
    w = min(int(w), iw - x)
    h = min(int(h), ih - y)
    crop = img.crop((x, y, x + w, y + h))
    crop.save(out_path)
    return out_path


def find_color_blob(path, color="blue", min_size=80):
    """Find the largest blob of a named color. Generalized from find_button().

    Args:
        path: screenshot path
        color: one of "blue", "red", "green", "yellow", "grey"
        min_size: minimum pixel count

    Returns (x, y) centroid of largest matching blob, or None.
    """
    img = np.array(Image.open(path).convert("RGB")).astype(np.float32)
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    if color == "blue":
        mask = (b > 140) & ((b - r) > 45) & ((b - g) > 45)
    elif color == "red":
        mask = (r > 140) & ((r - b) > 45) & ((r - g) > 45)
    elif color == "green":
        mask = (g > 140) & ((g - r) > 45) & ((g - b) > 45)
    elif color == "yellow":
        mask = (r > 140) & (g > 140) & (b < 100)
    elif color == "grey":
        mx = np.maximum(np.maximum(r, g), b)
        mn = np.minimum(np.minimum(r, g), b)
        sat = mx - mn
        bright = (r + g + b) / 3
        mask = (sat < 30) & (bright > 60) & (bright < 180)
    else:
        raise ValueError(f"Unknown color: {color}")
    return _compact_centroid(mask, max_diam=200)


def describe_screen(path):
    """Quick summary of what's visible: rail icons, colored blobs.

    Returns dict with detected elements. Useful for deciding what to
    look for next, or for John to verify the camera view.
    """
    result = {
        "rail_icons": len(find_rail_icons(path)),
        "blue_blob": find_color_blob(path, "blue"),
        "red_blob": find_color_blob(path, "red"),
        "green_blob": find_color_blob(path, "green"),
    }
    return result




def _compact_centroid(mask, max_diam=60):
    """Centroid of the largest compact cluster in a boolean mask."""
    from scipy import ndimage
    lab, n = ndimage.label(mask)
    if n == 0:
        return None
    sizes = ndimage.sum(mask, lab, range(1, n + 1))
    best = None
    for k, sz in enumerate(sizes, start=1):
        ys, xs = np.where(lab == k)
        w = xs.max() - xs.min() + 1
        h = ys.max() - ys.min() + 1
        if max(w, h) > max_diam:
            continue
        if best is None or sz > best[0]:
            best = (sz, float(xs.mean()), float(ys.mean()))
    return None if best is None else (best[1], best[2])


def locate_cursor_verified(link, jitter=JITTER_PX):
    """Jitter reveal with verification: the cursor must DISAPPEAR from its
    old spot AND REAPPEAR displaced by the jitter. Rejects UI animation
    false positives (which only disappear). Returns (pos, p1) or (None, p1).
    Net-zero: cursor ends where it started.
    """
    p0, _, _ = link.capture()
    a = load_gray(p0)
    link.mouse_move(jitter, 0)
    p1, _, _ = link.capture(settle_s=OBSERVE_SETTLE_S)
    b = load_gray(p1)
    link.mouse_move(-jitter, 0)  # restore
    if exposure_shifted(a, b):
        return None, p1
    d = signed_norm_diff(b, a)
    gone = _compact_centroid(d < -25)   # where it was
    here = _compact_centroid(d > 25)    # where it went
    if gone is None or here is None:
        return None, p1
    dx, dy = here[0] - gone[0], here[1] - gone[1]
    dist = (dx ** 2 + dy ** 2) ** 0.5
    # sane: moved roughly along +x, between 2px and jitter*2.5 px
    if not (2.0 <= dist <= jitter * 2.5 and dx > -dist * 0.5):
        return None, p1
    return gone, p1  # restored position == where it was


def locate_cursor_strict(link, jitter=100):
    """Strict cursor reveal: back-and-forth jitter with direction and scale
    validation. Rejects the false positives that fooled locate_cursor_verified
    on 2026-09-23 (e.g. a noise blob at (37,54) that "validated" but was not
    the cursor).

    Uses a larger jitter (100u) for better SNR, then requires:
    1. Old position matches between D1-disappeared and D2-appeared (tol 15px)
    2. New position matches between D1-appeared and D2-disappeared (tol 15px)
    3. Displacement is RIGHTWARD (dx > 5px) and mostly horizontal (|dy| < 12px)
    4. Displacement magnitude is sane for the jitter (5..30px for 100u)

    Returns (x, y, scale_x, frame_path) or (None, None, None, frame_path).
    Net-zero.
    """
    from collections import Counter

    def _blobs(mask, min_n=10, max_n=300, grid=20):
        ys, xs = np.nonzero(mask)
        if len(xs) < min_n:
            return []
        gx, gy = xs // grid, ys // grid
        c = Counter(zip(gx.tolist(), gy.tolist()))
        out = []
        for (cx, cy), n in c.most_common(8):
            if min_n <= n <= max_n:
                sel = (gx == cx) & (gy == cy)
                out.append((float(xs[sel].mean()),
                            float(ys[sel].mean()), n))
        return out

    def _match(b1, b2, tol=15):
        for x1, y1, _n1 in b1:
            for x2, y2, _n2 in b2:
                if abs(x1 - x2) < tol and abs(y1 - y2) < tol:
                    return (x1, y1), (x2, y2)
        return None

    pA, _, _ = link.capture()
    A = load_gray(pA)
    link.mouse_move(jitter, 0)
    pB, _, _ = link.capture(settle_s=OBSERVE_SETTLE_S)
    B = load_gray(pB)
    link.mouse_move(-jitter, 0)
    pC, _, _ = link.capture(settle_s=OBSERVE_SETTLE_S)
    C = load_gray(pC)

    if exposure_shifted(A, B) or exposure_shifted(B, C):
        return None, None, None, pC
    D1 = signed_norm_diff(B, A)
    D2 = signed_norm_diff(C, B)
    m_old = _match(_blobs(D1 < -30), _blobs(D2 > 30))
    m_new = _match(_blobs(D1 > 30), _blobs(D2 < -30))
    if not (m_old and m_new):
        return None, None, None, pC
    ox = (m_old[0][0] + m_old[1][0]) / 2
    oy = (m_old[0][1] + m_old[1][1]) / 2
    nx = (m_new[0][0] + m_new[1][0]) / 2
    ny = (m_new[0][1] + m_new[1][1]) / 2
    dx, dy = nx - ox, ny - oy
    # Strict: rightward, mostly horizontal, sane magnitude for this jitter
    lo, hi = jitter * 0.05, jitter * 0.30
    if not (dx > 5 and abs(dy) < 12 and lo <= dx <= hi):
        return None, None, None, pC
    # Tighten: require scale consistency -- the measured px/HID must be
    # within [0.05, 2.0] and the displacement must exceed 4px (noise floor).
    # (Added 2026-09-23 after a false positive at (122,309).)
    sx = dx / jitter
    if not (0.05 <= sx <= 2.0 and dx >= 4):
        return None, None, None, pC
    return ox, oy, sx, pC


def calibrate_xy(link, jitter=80):
    """Measure px-per-HID scale on BOTH axes with a cross jitter.

    Does: +x, -x (back), +y, -y (back) -- net-zero. Captures before/after
    each move and finds the cursor displacement via the same blob-matching
    as locate_cursor_strict.

    Returns (sx, sy) or (None, None) if either axis fails validation.
    The projection is non-orthonormal: NEVER use sx for y moves.
    """
    from collections import Counter

    def _blobs(mask, min_n=10, max_n=300, grid=20):
        ys, xs = np.nonzero(mask)
        if len(xs) < min_n:
            return []
        gx, gy = xs // grid, ys // grid
        c = Counter(zip(gx.tolist(), gy.tolist()))
        out = []
        for (cx, cy), n in c.most_common(8):
            if min_n <= n <= max_n:
                sel = (gx == cx) & (gy == cy)
                out.append((float(xs[sel].mean()),
                            float(ys[sel].mean()), n))
        return out

    def _match(b1, b2, tol=15):
        for x1, y1, _n1 in b1:
            for x2, y2, _n2 in b2:
                if abs(x1 - x2) < tol and abs(y1 - y2) < tol:
                    return (x1, y1), (x2, y2)
        return None

    def _measure(dx_hid, dy_hid):
        """Move (dx_hid, dy_hid), return measured (dx_px, dy_px) or None."""
        pA, _, _ = link.capture()
        A = load_gray(pA)
        link.mouse_move(dx_hid, dy_hid)
        pB, _, _ = link.capture()
        B = load_gray(pB)
        link.mouse_move(-dx_hid, -dy_hid)  # return to start
        pC, _, _ = link.capture()
        C = load_gray(pC)
        # Cursor was at old pos in A, new pos in B, back at old in C
        D1 = B - A
        D2 = C - B
        m_old = _match(_blobs(D1 < -30), _blobs(D2 > 30))
        m_new = _match(_blobs(D1 > 30), _blobs(D2 < -30))
        if not (m_old and m_new):
            return None
        ox = (m_old[0][0] + m_old[1][0]) / 2
        oy = (m_old[0][1] + m_old[1][1]) / 2
        nx = (m_new[0][0] + m_new[1][0]) / 2
        ny = (m_new[0][1] + m_new[1][1]) / 2
        return nx - ox, ny - oy

    # X axis
    r = _measure(jitter, 0)
    if r is None:
        return None, None
    dx_px, dy_px = r
    # X move must be primarily horizontal and rightward
    if not (dx_px > 5 and abs(dy_px) < 12):
        return None, None
    sx = dx_px / jitter
    if not (0.05 <= sx <= 2.0):
        return None, None

    # Y axis (note: +dy HID = down in screen pixels per John's convention)
    # The camera may be rotated relative to the screen, so we don't assume
    # the sign of dy_px -- we measure it and record the direction.
    r = _measure(0, jitter)
    if r is None:
        return sx, None
    dx_px, dy_px = r
    # Y move must be primarily vertical (direction agnostic -- camera may
    # be rotated). We record the sign for callers to interpret.
    if not (abs(dy_px) > 5 and abs(dx_px) < 12):
        return sx, None
    sy = abs(dy_px) / jitter
    if not (0.05 <= sy <= 2.0):
        return sx, None
    # Return sy as positive magnitude; caller must check sign of dy_px
    # to know if +HID_y maps to +camera_y or -camera_y.
    # We stash the direction in a function attribute for convenience.
    calibrate_xy.y_sign = 1 if dy_px > 0 else -1

    return sx, sy


# ---- session learnings (2026-09-23, baked in) -------------------------
# 1. CURSOR STATES: The cursor is NOT always an arrow. Over clickable UI
#    (e.g. Spotify buttons) it becomes a POINTING HAND. It can also be an
#    I-beam over text. Visual detection MUST handle all three states.
#    Diff-based detection (motion, not appearance) is the primary method
#    because it is shape-agnostic. (John: "the cursor is a pointing hand")
#
# 2. SCALE: Measured 2026-09-23: ~6.0 HID units per camera pixel (0.167
#    px/unit), NOT the 2.35-3.0 estimated earlier. The "halfway" incident:
#    a (+96,-3) move covered only ~16px instead of 32px because the scale
#    was wrong. ALWAYS verify moves; NEVER trust a stale scale.
#
# 3. MOVE VERIFICATION: After every homing move, diff before/after to
#    measure actual displacement. If actual < 50% of expected, the scale
#    is wrong -- recalibrate immediately, do not continue with bad scale.
#
# 4. CAMERA SHIFT: The camera can physically shift mid-session (seen
#    2026-09-23: frame showed more of the room, all positions invalid).
#    The watchdog detects this via global frame differencing. When
#    detected, ALL cached positions are stale -- re-locate from scratch.
#
# 5. WHITE-ON-WHITE: When the cursor is over a white/light button, it is
#    invisible (white-on-white). Use diff-based verification, not visual.
#    If the target region mean brightness > 180, visual confirmation is
#    unreliable.
#
# 6. KEEP-ALIVE: The daemon died silently on 2026-09-23 and the screen
#    slept. Check health before precise work: is the process running?
#    Are its moves being ACKed? See check_keepalive_health() below.
#
# 7. EXPOSURE: Global brightness shifts (auto-exposure, room light) poison
#    naive diffs. All differencing is now exposure-normalized (median
#    matched out); a diff across a large exposure shift is rejected
#    outright, not trusted. See signed_norm_diff/exposure_shifted.
#
# 8. GLARE: Saturated white blobs that don't move are specular glare --
#    detect and mask them out of every analysis pass (detect_glare).
#    A target inside glare is not servable: the policy raises instead
#    of servoing blind. Move the camera or pick another target.
#
# 9. LOCAL JACOBIAN: The projection is non-orthonormal (John). The map is
#    a spatially-varying 2x2 WITH CROSS TERMS (LocalMap), one cell per
#    screen region, refined by every verified move. Never a global scalar,
#    never axis-separable. hid_for() inverts J to compute moves.
#
# 10. ERROR BUDGET: Cumulative servo corrections are budgeted at 150px
#     (ERROR_BUDGET_PX). Past it, raise RecalibrationNeeded instead of
#     limping on with a corrupt map.
#
# 11. REGRESSION: Every failed case (verify miss, lost cursor, exposure
#     rejection, camera shift, glare target) is saved with its frame to
#     cursor-model/regression/manifest.jsonl -- misses become training
#     data for the next model round. See log_regression_case.
#
# 12. MOVE/OBSERVE DISCIPLINE: mouse_move waits for the device ack plus
#     MOVE_SETTLE; snapshots happen only after quiet time. move_then_
#     observe() is the one sanctioned sequence -- never observe mid-motion.

DEFAULT_PX_PER_UNIT = 0.167  # measured 2026-09-23 (6.0 HID/px), was 0.30

# ---- coordination: keep-alive vs active work ---------------------------
# A simple file lock prevents the background keep-alive from moving the
# mouse while a calibration or precise measurement is in progress.
# (John's "lol" -- 2026-09-23 -- taught us not to interleave moves.)
import os as _os
_LOCK_PATH = "/tmp/watcher_bench.lock"

def bench_lock():
    """Acquire the bench lock. Returns True if acquired, False if busy."""
    try:
        fd = _os.open(_LOCK_PATH, _os.O_CREAT | _os.O_EXCL | _os.O_WRONLY)
        _os.write(fd, str(_os.getpid()).encode())
        _os.close(fd)
        return True
    except FileExistsError:
        return False

def bench_unlock():
    """Release the bench lock."""
    try:
        _os.unlink(_LOCK_PATH)
    except FileNotFoundError:
        pass

def bench_is_locked():
    """Check if the bench is currently locked."""
    return _os.path.exists(_LOCK_PATH)


# ---- regression auto-collection --------------------------------------
def log_regression_case(kind, frame_path, expected_xy, actual_xy, note=""):
    """Save a failed/interesting case for the next training round.

    Copies the frame into cursor-model/regression/frames/ and appends a
    row to manifest.jsonl: {ts, kind, frame, expected, actual, note}.
    Called on every verification failure, lost cursor, exposure
    rejection, and camera shift -- tonight's misses are tomorrow's
    training data.
    """
    frames_dir = os.path.join(REGRESSION_DIR, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    dest = os.path.join(frames_dir, f"{ts}-{kind}.jpg")
    try:
        shutil.copy(frame_path, dest)
    except OSError:
        dest = frame_path  # fall back to the original path
    def _xy(v):
        return None if v is None else [round(float(v[0]), 1),
                                       round(float(v[1]), 1)]
    row = {
        "ts": ts,
        "kind": kind,
        "frame": dest,
        "expected": _xy(expected_xy),
        "actual": _xy(actual_xy),
        "note": note,
    }
    with open(os.path.join(REGRESSION_DIR, "manifest.jsonl"), "a") as f:
        f.write(json.dumps(row) + "\n")
    return dest


# ---- move/settle discipline -------------------------------------------
def move_then_observe(link, dx, dy):
    """THE sanctioned move->observe sequence. Never observe mid-motion.

    mouse_move waits for the device ack + MOVE_SETTLE; then the device
    gets OBSERVE_SETTLE_S of quiet before the snaptest request (the
    pre-wait that fixed intermittent snapshot timeouts on 2026-09-23).
    Returns (path, n_chunks, seconds) like link.capture().
    """
    link.mouse_move(dx, dy)
    return link.capture(settle_s=OBSERVE_SETTLE_S)


# ---- coarse-to-fine + on-device crop-zoom ------------------------------
def request_crop_zoom(link, x, y, w, h, timeout=10):
    """Ask the Watcher for a crop-zoomed snapshot of region (x,y,w,h).

    John 2026-09-23: the Himax chip can crop-zoom when differencing can't
    resolve detail. FIRMWARE SUPPORT PENDING: if the device doesn't answer
    (or answers without image data), falls back to a host-side PIL crop of
    a full frame. Returns (PIL.Image, source) with source 'device'|'host'.
    """
    link._cmd_replies.clear()
    link._send_cmd({"t": "crop_zoom", "x": int(x), "y": int(y),
                    "w": int(w), "h": int(h)})
    deadline = time.time() + timeout
    reply = None
    while time.time() < deadline:
        for r in link._cmd_replies:
            if r.get("cmd") == "crop_zoom":
                reply = r
                break
        if reply:
            break
        time.sleep(0.25)
    if reply and reply.get("b64"):
        img = Image.open(io.BytesIO(base64.b64decode(reply["b64"])))
        return img.convert("RGB"), "device"
    # fallback: host-side crop of a full frame
    path, _, _ = link.capture()
    full = Image.open(path).convert("RGB")
    fw, fh = full.size
    cw, ch = int(w), int(h)
    x0 = max(0, min(fw - cw, int(x - cw / 2)))
    y0 = max(0, min(fh - ch, int(y - ch / 2)))
    return full.crop((x0, y0, x0 + cw, y0 + ch)), "host"


def coarse_to_fine_locate(link, region=None, zoom=3, target_fn=None):
    """Wide frame -> candidate region -> tight crop -> precise center.

    region: optional (x, y, w, h) camera-px to start from (else full frame).
    target_fn: callable(PIL.Image, (ox, oy)) -> (x, y) in full-frame coords
      or None, e.g. a wrapper running find_button on the crop. Without it,
      returns the crop center for the caller to refine.
    Uses request_crop_zoom (on-device once firmware supports it, else host).
    """
    path, _, _ = link.capture()
    full = Image.open(path).convert("RGB")
    fw, fh = full.size
    if region is None:
        region = (fw / 2, fh / 2, fw, fh)
    cx, cy, w, h = region
    tw, th = max(40, w / zoom), max(40, h / zoom)
    crop, src = request_crop_zoom(link, cx, cy, tw, th)
    print(f"  coarse-to-fine: crop {tw:.0f}x{th:.0f} @ ({cx:.0f},{cy:.0f}) "
          f"(src={src})", flush=True)
    if target_fn is None:
        return (cx, cy)
    ox, oy = max(0, int(cx - tw / 2)), max(0, int(cy - th / 2))
    return target_fn(crop, (ox, oy))  # full-frame coords or None


# ---- probe measurement (net-zero Jacobian probes) ---------------------
def _measure_move(link, dx_hid, dy_hid):
    """Net-zero probe move; returns (measured (dx_px, dy_px), None) or
    (None, reason) on failure.

    Capture A -> move -> capture B -> move back -> capture C, then match
    the disappeared/appeared blobs across D1=B-A and D2=C-B with
    exposure-normalized, glare-masked diffs. Used for fresh Jacobian
    measurements; the cursor ends where it started.
    """
    from collections import Counter

    def _blobs(mask, min_n=10, max_n=300, grid=20):
        ys, xs = np.nonzero(mask)
        if len(xs) < min_n:
            return []
        gx, gy = xs // grid, ys // grid
        c = Counter(zip(gx.tolist(), gy.tolist()))
        out = []
        for (ccx, ccy), n in c.most_common(8):
            if min_n <= n <= max_n:
                sel = (gx == ccx) & (gy == ccy)
                out.append((float(xs[sel].mean()),
                            float(ys[sel].mean()), n))
        return out

    def _match(b1, b2, tol=15):
        for x1, y1, _n1 in b1:
            for x2, y2, _n2 in b2:
                if abs(x1 - x2) < tol and abs(y1 - y2) < tol:
                    return (x1, y1), (x2, y2)
        return None

    pA, _, _ = link.capture()
    A = load_gray(pA)
    link.mouse_move(dx_hid, dy_hid)
    pB, _, _ = link.capture(settle_s=OBSERVE_SETTLE_S)
    B = load_gray(pB)
    link.mouse_move(-dx_hid, -dy_hid)
    pC, _, _ = link.capture(settle_s=OBSERVE_SETTLE_S)
    C = load_gray(pC)
    if exposure_shifted(A, B) or exposure_shifted(B, C):
        log_regression_case("measure_exposure", pB, None, None,
                            "exposure shift during Jacobian probe")
        return None, "exposure"
    glare = detect_glare(pB) | detect_glare(pC)
    D1 = signed_norm_diff(B, A)
    D2 = signed_norm_diff(C, B)
    D1[glare] = 0
    D2[glare] = 0
    m_old = _match(_blobs(D1 < -30), _blobs(D2 > 30))
    m_new = _match(_blobs(D1 > 30), _blobs(D2 < -30))
    if not (m_old and m_new):
        log_regression_case("measure_nomatch", pB, None, None,
                            f"probe ({dx_hid},{dy_hid}) found no cursor pair")
        return None, "nomatch"
    ox = (m_old[0][0] + m_old[1][0]) / 2
    oy = (m_old[0][1] + m_old[1][1]) / 2
    nx = (m_new[0][0] + m_new[1][0]) / 2
    ny = (m_new[0][1] + m_new[1][1]) / 2
    return (nx - ox, ny - oy), None


# ---- local 2x2 Jacobian (John's non-orthonormal rule) -------------------
class CameraShifted(RuntimeError):
    """The camera moved mid-session: all cached geometry is invalid."""


class RecalibrationNeeded(RuntimeError):
    """Error budget exhausted or Jacobian unmeasurable: recalibrate."""


class LocalMap:
    """Spatially-varying local 2x2 Jacobian: camera_px = J @ hid_units.

    John's rule (2026-09-23): the projection is non-orthonormal, so the
    mapping MUST be a local 2x2 with cross-axis terms, measured per screen
    region -- never a single global scalar. Each grid cell holds its own J,
    refined by every verified move that passes through it (exponential
    moving average). Cells with no measurements fall back to the default J
    (measured 2026-09-23: ~6.0 HID/px near frame center).
    """

    def __init__(self, frame_wh=(416, 416), grid=JACOBIAN_GRID, lr=0.5):
        self.w, self.h = frame_wh
        self.grid = grid
        self.lr = lr
        s = DEFAULT_PX_PER_UNIT
        self.default = np.array([[s, 0.0], [0.0, s]])
        self.cells = {}

    def _key(self, x, y):
        gx = min(self.grid - 1, max(0, int(x / self.w * self.grid)))
        gy = min(self.grid - 1, max(0, int(y / self.h * self.grid)))
        return (gx, gy)

    def J_at(self, x, y):
        return self.cells.get(self._key(x, y), self.default)

    def invalidate(self, x, y):
        """Drop the cell at (x,y) back to the default (stale measurement)."""
        self.cells.pop(self._key(x, y), None)

    def invalidate_all(self):
        self.cells.clear()

    def hid_for(self, x, y, dx_px, dy_px):
        """HID units needed at (x,y) to move (dx_px, dy_px) in the camera."""
        J = self.J_at(x, y)
        if abs(np.linalg.det(J)) < 1e-4:
            d = np.diag(J)
            d = np.where(np.abs(d) < 0.05, DEFAULT_PX_PER_UNIT, d)
            return float(dx_px / d[0]), float(dy_px / d[1])
        hid = np.linalg.solve(J, np.array([dx_px, dy_px], float))
        return float(hid[0]), float(hid[1])

    def observe(self, x, y, hid_dx, hid_dy, px_dx, px_dy):
        """Fold one verified move into the local cell (EMA update).

        Only call with moves that passed verify_move -- garbage in would
        corrupt the cell. Degenerate updates are rejected, old J kept.
        """
        J = self.J_at(x, y).astype(float)
        hid = np.array([hid_dx, hid_dy], float)
        px = np.array([px_dx, px_dy], float)
        n = float(hid @ hid)
        if n < 1e-9:
            return J
        resid = px - J @ hid
        J_new = J + np.outer(resid, hid) / n
        J_new = np.clip(J_new, -2.0, 2.0)
        if not (0.0025 <= abs(np.linalg.det(J_new)) <= 4.0):
            return J  # reject degenerate update
        J_blend = (1 - self.lr) * J + self.lr * J_new
        self.cells[self._key(x, y)] = J_blend
        return J_blend


# ---- visual servoing: homing that calibrates itself ------------------
class Servo:
    """Iterative homing with a self-refining LOCAL 2x2 Jacobian.

    No separate calibration phase: every verified move refines the
    Jacobian cell for its screen region (see LocalMap). Every move is
    verified against its expected displacement (the 2026-09-23 "halfway"
    lesson); a <50% result invalidates the cell and forces a fresh probe
    measurement instead of continuing with a bad map. A cumulative error
    budget forces full recalibration rather than limping on. A global
    frame change raises CameraShifted -- the camera moved, all cached
    geometry is dead.
    """

    def __init__(self, link, px_per_unit=DEFAULT_PX_PER_UNIT):
        self.link = link
        self.map = LocalMap()
        # legacy attrs, kept for callers that read servo.sx/sy
        s = float(px_per_unit)
        self.s = self.sx = self.sy = s
        self.error_budget = 0.0
        self.moves_verified = 0
        self.moves_failed = 0
        self._ref_gray = None

    def _check_camera(self, gray, frame_path):
        if self._ref_gray is None:
            self._ref_gray = gray
            return
        if detect_camera_shift(self._ref_gray, gray):
            log_regression_case("camera_shift", frame_path, None, None,
                                "global frame change vs session reference")
            raise CameraShifted(
                "camera moved mid-session: all cached geometry invalid; "
                "re-locate from scratch")

    def _measure_jacobian(self, cx, cy, jitter=60):
        """Fresh local Jacobian at (cx,cy) via net-zero cross probes.

        Returns the measured J, stores it in the cell, and resets the
        error budget. Raises RecalibrationNeeded when a probe fails or
        the measured J is degenerate. Net-zero: cursor ends where it
        started.
        """
        rx, reason = _measure_move(self.link, jitter, 0)
        if rx is None:
            raise RecalibrationNeeded(f"x probe failed ({reason})")
        ry, reason = _measure_move(self.link, 0, jitter)
        if ry is None:
            raise RecalibrationNeeded(f"y probe failed ({reason})")
        # columns of J: the x-probe response is column 0, y-probe column 1
        J = np.array([[rx[0] / jitter, ry[0] / jitter],
                      [rx[1] / jitter, ry[1] / jitter]])
        J = np.clip(J, -2.0, 2.0)
        det = np.linalg.det(J)
        if not (0.0025 <= abs(det) <= 4.0):
            raise RecalibrationNeeded(
                f"degenerate probe J (det={det:.4f})")
        self.map.cells[self.map._key(cx, cy)] = J
        self.error_budget = 0.0
        print(f"  fresh J at ({cx:.0f},{cy:.0f}): "
              f"[{J[0,0]:.3f} {J[0,1]:.3f}; {J[1,0]:.3f} {J[1,1]:.3f}]",
              flush=True)
        return J

    def home_onto(self, target_xy, max_iters=SERVO_MAX_ITERS,
                  tol=SERVO_TOL_PX, cursor_xy=None):
        """Drive the cursor onto target_xy. Returns (landed, iters).

        Raises CameraShifted / RecalibrationNeeded instead of limping on
        with bad geometry -- the operator decides what to do next.
        """
        if cursor_xy is None:
            cursor_xy, p0 = locate_cursor(self.link)
            if cursor_xy is None:
                raise RuntimeError("cursor not found by jitter reveal")
            self._ref_gray = load_gray(p0)
        cx, cy = cursor_xy
        for it in range(1, max_iters + 1):
            tx, ty = target_xy()
            dx_px, dy_px = tx - cx, ty - cy
            dist = (dx_px ** 2 + dy_px ** 2) ** 0.5
            J = self.map.J_at(cx, cy)
            print(f"  iter {it}: cursor=({cx:.1f},{cy:.1f}) "
                  f"target=({tx:.1f},{ty:.1f}) dist={dist:.1f}px "
                  f"J=[{J[0,0]:.3f} {J[0,1]:.3f}; "
                  f"{J[1,0]:.3f} {J[1,1]:.3f}] "
                  f"budget={self.error_budget:.0f}/{ERROR_BUDGET_PX:.0f}",
                  flush=True)
            if dist <= tol:
                return True, it
            mx, my = self.map.hid_for(cx, cy, dx_px, dy_px)
            mx = int(round(mx))
            my = int(round(my))
            mx = max(-SERVO_MAX_STEP, min(SERVO_MAX_STEP, mx))
            my = max(-SERVO_MAX_STEP, min(SERVO_MAX_STEP, my))
            if mx == 0 and my == 0:
                return dist <= tol * 2, it
            p_before, _, _ = self.link.capture()
            A = load_gray(p_before)
            self._check_camera(A, p_before)
            # visibility policy is a decision: glare => not servable,
            # bright => diff-only (which is what we do anyway -- note it)
            pol = localization_policy(p_before, tx, ty)
            if pol == "relocate":
                log_regression_case("target_glare", p_before,
                                    (tx, ty), (cx, cy),
                                    "target inside glare region")
                raise RuntimeError(
                    "target is inside a glare region: move the camera or "
                    "pick another target; not servoing blind")
            if pol == "diff_only":
                print("  target region is bright (white-on-white risk): "
                      "diff-only localization", flush=True)
            exp = J @ np.array([mx, my], float)
            self.link.mouse_move(mx, my)
            p_after, _, _ = self.link.capture(settle_s=OBSERVE_SETTLE_S)
            B = load_gray(p_after)
            self._check_camera(B, p_after)
            if exposure_shifted(A, B):
                log_regression_case("servo_exposure", p_after,
                                    (tx, ty), (cx, cy),
                                    "exposure shift across servo move")
                print("  exposure shifted across move; retrying iter",
                      flush=True)
                continue
            ok, adx, ady, ratio, new_xy = verify_move_arrays(
                A, B, float(exp[0]), float(exp[1]))
            if not ok:
                self.moves_failed += 1
                self.map.invalidate(cx, cy)
                log_regression_case(
                    "servo_verify_failed", p_after, (tx, ty),
                    (cx + adx, cy + ady) if new_xy is not None else (cx, cy),
                    f"ratio={ratio:.2f} "
                    f"expected=({exp[0]:.1f},{exp[1]:.1f})")
                print(f"  move verified at {ratio:.0%} of expected; "
                      f"invalidating cell and re-probing...", flush=True)
                nc, _ = locate_cursor(self.link)
                if nc is None:
                    raise RuntimeError("lost cursor during servo")
                cx, cy = nc
                # fresh measurement (resets budget); raises if it fails
                self._measure_jacobian(cx, cy)
                continue
            # verified: fold the measurement into the local map
            self.moves_verified += 1
            self.map.observe(cx, cy, mx, my, adx, ady)
            self.sx, self.sy = float(J[0, 0]), float(J[1, 1])  # legacy sync
            self.error_budget += ((adx - exp[0]) ** 2
                                  + (ady - exp[1]) ** 2) ** 0.5
            if self.error_budget > ERROR_BUDGET_PX:
                raise RecalibrationNeeded(
                    f"error budget exhausted ({self.error_budget:.0f}px > "
                    f"{ERROR_BUDGET_PX:.0f}px): recalibrate from scratch")
            if new_xy is not None:
                cx, cy = float(new_xy[0]), float(new_xy[1])
                continue
            # differencing couldn't re-localize: full re-reveal
            pos, _ = locate_cursor(self.link)
            if pos is None:
                raise RuntimeError("lost cursor during servo")
            cx, cy = pos
        return False, max_iters


# ---- move verification (the "halfway" lesson) ------------------------
def verify_move_arrays(A, B, expected_dx_px, expected_dy_px,
                       tol_ratio=VERIFY_TOL_RATIO):
    """Core of verify_move on float32 gray arrays.

    Exposure-normalized: a global brightness shift between the frames
    rejects the verification outright (returns ok=False) instead of
    producing a nonsense ratio.

    Returns (ok, actual_dx, actual_dy, ratio, new_xy). new_xy is the
    measured cursor position in B (centroid of the appeared blob) or
    None when the blobs can't be separated -- then displacement falls
    back to the extent-along-direction estimate.
    """
    A = np.asarray(A, dtype=np.float32)
    B = np.asarray(B, dtype=np.float32)
    if exposure_shifted(A, B):
        return False, 0.0, 0.0, 0.0, None
    d = signed_norm_diff(B, A)
    thr = max(28.0, np.median(np.abs(d)) + 4 * np.abs(d).std())
    pos = _compact_centroid(d > thr)    # where it appeared
    neg = _compact_centroid(d < -thr)   # where it was
    exp_len = (expected_dx_px ** 2 + expected_dy_px ** 2) ** 0.5
    if exp_len < 1.0:
        return True, 0.0, 0.0, 1.0, pos  # no move expected, nothing to verify
    if pos is None or neg is None:
        # blobs not separable (e.g. white-on-white): fall back to the
        # extent of the combined changed region along the move direction
        ys, xs = np.nonzero(np.abs(d) > thr)
        if len(xs) < 3:
            return False, 0.0, 0.0, 0.0, None
        mx, my = np.median(xs), np.median(ys)
        ux, uy = expected_dx_px / exp_len, expected_dy_px / exp_len
        proj = (xs - mx) * ux + (ys - my) * uy
        actual_len = max(0.0, (proj.max() - proj.min()) - 10.0)
        ratio = actual_len / exp_len
        return (ratio >= tol_ratio, ux * actual_len, uy * actual_len,
                ratio, None)
    adx, ady = pos[0] - neg[0], pos[1] - neg[1]
    actual_len = (adx ** 2 + ady ** 2) ** 0.5
    # must point roughly along the expected direction, not backwards
    if adx * expected_dx_px + ady * expected_dy_px < 0:
        return False, adx, ady, 0.0, pos
    ratio = actual_len / exp_len
    return ratio >= tol_ratio, adx, ady, ratio, pos


def verify_move(p_before, p_after, expected_dx_px, expected_dy_px,
                tol_ratio=VERIFY_TOL_RATIO):
    """Check if a move actually covered the expected distance.

    Returns (ok, actual_dx, actual_dy, ratio). If ratio < tol_ratio,
    the scale is wrong -- do NOT continue with the stale scale.

    The 2026-09-23 incident: a (+96,-3) HID move was expected to cover
    (+32,-1)px but only covered ~(+16,0)px (ratio 0.5). The scale was
    6.0 HID/px, not 3.0. This function catches that.
    """
    A = load_gray(p_before)
    B = load_gray(p_after)
    ok, adx, ady, ratio, _ = verify_move_arrays(
        A, B, expected_dx_px, expected_dy_px, tol_ratio)
    return ok, adx, ady, ratio


def is_bright_region(path, x, y, r=15, thresh=180):
    """Check if the region around (x,y) is bright (white-on-white risk).

    Returns True if mean brightness > thresh. When True, visual cursor
    confirmation is unreliable -- use diff-based methods instead.
    (Learned 2026-09-23: cursor invisible on white play/pause button.)
    """
    img = np.array(Image.open(path).convert("L")).astype(np.float32)
    h, w = img.shape
    x0, x1 = max(0, int(x-r)), min(w, int(x+r))
    y0, y1 = max(0, int(y-r)), min(h, int(y+r))
    if x0 >= x1 or y0 >= y1:
        return False
    return float(img[y0:y1, x0:x1].mean()) > thresh


def check_keepalive_health():
    """Check if the keep-alive daemon is running and healthy.

    Returns (running, pid_or_none, msg). A healthy keep-alive:
    - Has a process running (pgrep keepalive.py)
    - Has a recent heartbeat (touched every cycle)

    The 2026-09-23 incident: keepalive died silently, screen slept.
    Call this before precise work; if not healthy, restart it.
    """
    import subprocess
    try:
        out = subprocess.run(["pgrep", "-f", "keepalive\\.py"],
                             capture_output=True, text=True, timeout=5)
        pids = out.stdout.strip().split()
        if not pids:
            return False, None, "keepalive.py not running"
        # Check heartbeat file (touched every cycle by keepalive.py)
        hb_path = "/tmp/watcher-keepalive.hb"
        if os.path.exists(hb_path):
            age = time.time() - os.path.getmtime(hb_path)
            if age > 180:  # 3 min without heartbeat = stuck
                return False, int(pids[0]), f"heartbeat stale ({age:.0f}s ago)"
            return True, int(pids[0]), f"healthy (heartbeat {age:.0f}s ago)"
        return True, int(pids[0]), "running (no heartbeat file yet)"
    except Exception as e:
        return False, None, f"health check failed: {e}"


# ---- motion watchdog ---------------------------------------------------
def watch(link, interval=WATCH_INTERVAL, thresh=WATCH_MOVE_THRESH):
    """Photo loop that flags device movement. Never moves the cursor.

    A large *global* frame change between consecutive snapshots means the
    camera moved (screen content changes are usually local). Prints MOVED
    and returns True when detected; the operator then asks John for a
    re-home -- moves still need his per-use permission.
    """
    print(f"watchdog: photo every {interval}s, move threshold {thresh}",
          flush=True)
    prev, _, dt = link.capture()
    print(f"  reference frame {prev} ({dt:.1f}s)", flush=True)
    while True:
        time.sleep(interval)
        try:
            cur, _, dt = link.capture()
        except TimeoutError as e:
            print(f"  capture timeout: {e}", flush=True)
            continue
        chg = frame_change(prev, cur)
        print(f"  {cur}: change={chg:.3f} ({dt:.1f}s)", flush=True)
        if chg >= thresh:
            print("MOVED: device appears to have shifted. "
                  "Ask John for a re-home; not moving on my own.",
                  flush=True)
            return True
        prev = cur


# ---- CLI ---------------------------------------------------------------
def main():
    if len(sys.argv) < 2:
        print(__doc__.split("Usage:")[1].split("Credentials:")[0])
        sys.exit(2)
    mode = sys.argv[1]
    allow = "--allow-moves" in sys.argv
    link = EyesLink(allow_moves=allow)
    try:
        if mode == "photo":
            path, n, dt = link.capture()
            print(f"OK {path}: {n} chunks in {dt:.1f}s")
        elif mode == "button":
            path, n, dt = link.capture()
            b = find_button(path)
            print(f"OK {path}: {n} chunks in {dt:.1f}s")
            print("button:", ("none in frame" if b is None
                              else f"({b[0]:.1f},{b[1]:.1f})"))
        elif mode == "cursor":
            pos, path = locate_cursor(link)
            print("cursor:", "not found" if pos is None
                  else f"({pos[0]:.1f},{pos[1]:.1f})")
        elif mode == "home":
            path, _, _ = link.capture()
            b = find_button(path)
            if b is None:
                print("button not in frame; ask John to bring it into view")
                sys.exit(1)
            print(f"button at ({b[0]:.1f},{b[1]:.1f}); servoing...",
                  flush=True)
            servo = Servo(link)
            landed, iters = servo.home_onto(lambda: b)
            print("LANDED" if landed else "NOT LANDED", f"in {iters} iters")
        elif mode == "bubble":
            # Chat-bubble task: fresh frame (camera may have moved), target
            # the bottom-most blue chat bubble, servo the cursor onto it.
            path, n, dt = link.capture()
            tgt = find_lowest_blue_bubble(path)
            print(f"OK {path}: {n} chunks in {dt:.1f}s", flush=True)
            if tgt is None:
                print("no chat bubble found in frame")
                sys.exit(1)
            print(f"target bubble at ({tgt[0]:.1f},{tgt[1]:.1f}); servoing...",
                  flush=True)
            servo = Servo(link)
            landed, iters = servo.home_onto(lambda: tgt)
            print("LANDED" if landed else "NOT LANDED", f"in {iters} iters")
        elif mode == "watch":
            watch(link)
        else:
            print(f"unknown mode {mode}")
            sys.exit(2)
    finally:
        link.close()


if __name__ == "__main__":
    main()
