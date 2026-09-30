# Bench Plan: Pipeline Improvements Test Session
**Date:** 2026-09-25 (morning after build session)
**Goal:** Validate the five new pipeline modules on live hardware, graduating from human-eyeballed control to autonomous servo control.

---

## Standing rules (unchanged)

- **Bench is John's.** Explicit "go" before every cursor batch. Nothing moves without his word.
- **Announce every command** before sending (cursor_ops already prints them).
- **No clicks** until Phase 3, and only with explicit approval per click.
- **Abort available:** `mouse_abort` with `target_mid` kills a matching glide.
- **Camera may have moved overnight.** Phase 0 re-verifies geometry before trusting any cached positions.

---

## Phase 0 — Bench checkout (~5 min, no cursor moves)

1. Verify MQTT bridge alive: check `watcher/telemetry` flowing, console version current.
2. Verify Watcher alive: `{"t":"dbg","cmd":"version"}` → expect fw 0.3.11.
3. Take a snapshot, compare against last night's `/tmp/final2.jpg`:
   - Is the Claude pane in the same place? (ui_detect pane finder gives coords — compare to `(0,0,397,479)`.)
   - Is the camera framing the same? If the pane moved >20px, **stop and re-anchor** — all cached positions are stale.
4. Confirm jiggler state (restore whatever John had).

**Go/no-go:** Bridge + Watcher + camera geometry all nominal. If camera moved, do a quick visual re-anchor (find pane, find button) before proceeding.

---

## Phase 1 — Detection validation (~10 min, minimal moves)

**1a. UI detection (zero moves):**
- Run `ui_detect.find_orange_button()` + `find_text_field()` + `find_claude_pane()` on a fresh snapshot.
- Success: button found with conf > 0.7 within ~15px of where John sees it; text field rect sane; pane matches Phase 0.
- This validates the color model against *today's* lighting (morning vs last night's lighting may differ).

**1b. Wiggle detection (two small moves):**
- `cursor_detect.detect_wiggle` with a ±20 HID nudge.
- Success: returns a position (not None), and the detected displacement direction matches the commanded nudge direction.
- Run 3x in different screen areas (dark pane, bright console, near button) to check robustness across backgrounds.
- If blob fallback is needed anywhere, note it — wiggle is primary.

**Go/no-go:** Both detectors work on live frames in current lighting. If lighting broke the orange model, we retune thresholds (not a failure — that's what Phase 1 is for).

---

## Phase 2 — Servo dry run (~15 min, moves but no clicks)

**2a. Servo to a safe target:**
- Pick an empty area of the Claude pane (no buttons, no text field) — e.g. (200, 200).
- Run `servo_to(target, avoid_rects=[text_field])` with John's approval.
- Watch: does it converge within 8 iters? What's the final error? How many corrections?
- Success: converges to <8px within 5 iterations.

**2b. Servo to the submit button (HOVER ONLY, no click):**
- Target = detected button position from Phase 1a.
- Avoid rect = detected text field.
- Success: converges onto the button, cursor visibly on orange in the verification snap. **Do not click.**

**2c. Recovery drill (optional, John's call):**
- Deliberately drive the cursor into the text field (I-beam invisible), then let the servo's `_recover()` bring it back.
- Validates John's recovery rule in code.

**Go/no-go:** Servo converges reliably. If it oscillates or diverges, we tune gain (0.7 → 0.5) or check the Jacobian against Phase 4.

---

## Phase 3 — Full task rehearsal (~15 min, one approved click)

- Same task as last night: type a short message in Claude's box, servo to submit, **John approves the click**, verify.
- The difference: no human eyeballing. `ui_detect` finds the button, `servo` drives there, twin tracks belief throughout.
- Compare wall-clock time and iteration count vs last night's manual run (~11 moves, ~10 min of nudging).
- Success: message sent, fewer human interventions than last night (target: zero manual corrections).

---

## Phase 4 — Fresh calibration (only if needed, ~20 min)

**Trigger:** Phase 2 shows systematic overshoot/undershoot (servo consistently corrects the same direction), suggesting the Jacobian is stale.

- Run John's closed-box protocol via `calibrate.py`: 200-unit and 500-unit boxes, 5 photos each, no interleaved moves.
- RANSAC fit, compare against validated 2026-09-24 matrix.
- If better (lower RMS, more inliers), adopt; if not, keep the validated one.
- **Do not run preemptively** — only if the data says we need it.

---

## Abort / safety procedures

| Situation | Action |
|---|---|
| Cursor runs away / wrong direction | `mouse_abort` with the glide's `target_mid`; then `twin_ops.reanchor()` on next detection |
| Cursor invisible and recovery fails (6 nudges) | Stop, ask John to visually locate it |
| Detector returns garbage (button in wrong place) | Fall back to John's eyeball + manual coords; log the failure for threshold tuning |
| MQTT bridge drops | Stop all moves (they queue on the Watcher — abort on reconnect) |
| Anything unexpected | **Stop and ask John.** The bench is his. |

---

## What we're measuring

For each phase, log to the run report:
- Detection success rate + confidence scores
- Servo iterations to convergence + final error (px)
- Number of human interventions (target: decreasing across phases)
- Wall-clock time per task
- Any detector failures with the offending image saved for tuning

---

## Deliverable

Updated run report (via `task_report.py`) comparing manual (last night) vs servo-driven (today) performance: moves, time, accuracy, interventions. This is the "did the pipeline actually get better" receipt.

---

## Proposed order when John's ready

1. ☕ Finish coffee (no rush)
2. Phase 0 checkout — I run, John watches
3. Phase 1a (zero moves) — safe to run anytime
4. **John says go** → Phase 1b wiggle tests
5. **John says go** → Phase 2 servo dry runs
6. **John says go + approves click** → Phase 3 full rehearsal
7. Phase 4 only if the data demands it
8. Write the comparison report
