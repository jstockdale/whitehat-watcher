# Digital Twin

The twin tracks where the cursor is and where UI elements are, across three
coordinate frames. It never touches MQTT or hardware: bench I/O is injected
by the caller. Offline-testable via `test_twin.py` (42 tests).

## Frames

| Frame | Units | Meaning |
|---|---|---|
| Screen (S) | display px | Where intent lives ("click submit"). Origin top-left. |
| HID (H) | HID units | Where commands live. Origin = cursor pos at twin birth. **Never depends on the camera.** |
| Camera (C) | camera px | Where noisy observations live. |

Mappings: **H<->C** is affine (Jacobian + one anchor point: the camera
position of HID (0,0), set by the first cursor observation). **S<->C** is a
3x3 homography from screen edges and/or the fiducial.

## Data flow

```
console fiducial telemetry ──► twin.note_viewport() ──► viewport epoch
Himax frame ──► find_display_rectangle() ──► twin.update_geometry() ──► H_sc
Himax frame ──► find_fiducials() ──► twin.update_geometry_fiducials() ──► H_sc
command sent ──► twin.command(dx, dy)            # Kalman predict
Himax frame ──► cursor detection ──► twin.observe_cursor()  # Kalman correct
screenshot ──► twin.add_element(name, screen_xy)  # element map
plan: twin.move_to_element(name) -> (dx, dy)      # HID delta from belief
```

`cursor_moves.move_to_target(..., twin=twin)` wires this into the closed
loop: every glide is predicted, every detection corrects, and the twin's
filtered belief (not the raw detection) seeds the next iteration.

## Failure modes (and what the twin does)

- **Camera bumped.** `update_geometry` compares the new homography against
  the old one; a jump > 12px mean corner shift re-anchors S<->C, marks
  element camera positions stale, and keeps H-odom valid. No recalibration.
- **Occluded / wrong edges.** `find_display_rectangle` rejects quads whose
  aspect disagrees with the known screen aspect, and `update_geometry`
  refuses updates with confidence < 0.4. A wrong homography is worse than
  a stale one.
- **False cursor blob.** `observe_cursor` gates detections at 3-sigma from
  prediction; rejects are counted, never fused.
- **Repeated disagreement.** 5 consecutive rejections -> cursor LOST: the
  twin refuses `move_to_element` until `reanchor()` with a trusted
  detection. It stops instead of compounding.
- **Page scrolled / resized.** `note_viewport` bumps the epoch; elements
  from older epochs read as stale (and new observations discard, not
  average, the displaced position).
- **Screen overfills the frame** (current bench). Edge detection honestly
  reports nothing; the fiducial is the primary anchor until the camera
  pulls back.

## Modules

- `screen_twin.py` -- ScreenTwin: Kalman filter, element map, geometry.
- `screen_edges.py` -- edge detection, DLT homography, aspect guard.
- `fiducial.py` -- calibration symbol spec + detector.
- `cursor_moves.py` -- closed-loop executor; pass `twin=` to fuse.
- `test_twin.py` -- 42 tests incl. simulated bias correction, LOST,
  viewport epoch, geometry jumps, save/load.

## Console contract (watcher-console.html)

- Four fiducials: `#twinFidTL/TR/BL/BR` (72px, fixed). Their side
  margins are measured off the active tab's first white panel (same
  left/right/bottom inset as the content cards, desktop only); TL/TR float just above that
  panel, BL/BR sit at the bottom corners. Uniqueness is the notch:
  white 14px cut in the core quadrant facing the symbol's own corner.
  Do not restyle without updating `fiducial.py`.
- The topbar ANCHORS pill toggles symbol visibility (like the JIGGLE pill).
  Mode is AUTO by default: symbols show on the Muse tab, hide elsewhere.
  Clicking sets a sticky explicit ON/OFF that survives tab switches.
  Default: visible on the Muse tab, hidden everywhere else; switching
  tabs resets to the default. Hidden symbols are excluded from telemetry
  (zero-size rect), so the twin simply sees fewer marks.
- Status telemetry reports
  `fiducials: {vw, vh, sx, sy, marks: {tl: {x, y, size}, ...}}`
  (centers, sizes, viewport, scroll) from `getBoundingClientRect()`.
- Four identified marks -> the twin solves the full S<->C homography
  from fiducials alone (no edges, no cursor moves). Two or three marks
  only check the current map; one mark seeds translation.
- Commands tab has a manual page-screenshot button (local PNG download).
  Screenshots inform the twin; the camera always verifies before action.
