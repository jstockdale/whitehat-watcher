# Device UI design pass — Watcher 412×412 round display

Date: 2026-09-23. Status: **proposal**, needs John's reactions before implementation.
Covers the on-device LVGL UI only (the web-based Watcher Console is a separate surface).

## 1. Constraints (non-negotiable)

- **Round 412×412 panel.** Corners don't exist. All content lives in the inscribed
  area: keep interactive content within ~±150px of center horizontally at the
  vertical extremes, full 412 width only near the vertical middle. The current
  firmware already centers a vertical column — keep that pattern.
- **RAM is tight.** LVGL runs on 2× 10-row internal DMA buffers (8,240 bytes each).
  Consequences: prefer partial redraws (LVGL does this natively), keep the object
  tree small (< ~40 live objects per screen), avoid full-screen animations, no
  blur/shadow-heavy styles, one font family with 3–4 sizes max.
- **Inputs:** capacitive touch (also the BLE touchpad — see modes), rotary wheel
  with click, and the side buttons. No keyboard on device.
- **Glanceable, one-thumb.** The Watcher is wrist/handheld-adjacent. Every primary
  action reachable in ≤ 2 gestures. Text ≥ 16px, touch targets ≥ 44px.
- **Best-effort init.** Display init can fail and the device must still work
  headless — so the UI layer must tolerate `display_ready() == false` and every
  screen update must be a no-op until ready (the current `display_set_*` pattern).

## 2. Navigation model: modes, not apps

The device is a *mode ring* — but with 7 modes, pure click-to-cycle gets tedious,
so **wheel-click opens a mode switcher** instead of cycling blindly:

```
wheel-click → mode list overlay → wheel-rotate highlights → wheel-click/tap selects
```

- The switcher is a vertical list reusing the MENU list component: icon + name
  per mode, current mode highlighted. Auto-dismisses after 4 s with no change.
- The switcher works from *everywhere*, including POINTER mode (it's the escape
  hatch from the touchpad). You can never get stuck.
- Modes (switcher order = frequency order):

```
POINTER → VOICE → EYES → TIMER → MEDIA → KEYS → MENU
```

- **Wheel-rotate** scrolls/adjusts within the current mode (menu lists, volume,
  timer digits, scroll events in pointer mode).
- **Touch** behaves per-mode (see §3). In UI modes touch drives LVGL; in POINTER
  mode touch is captured for BLE HID and LVGL ignores it.
- A **mode pill** (small rounded label, top-center, e.g. `◉ POINTER`) is persistent
  across all screens so you always know where you are.

## 3. Screens

### 3.1 Boot screen (transient, ~1.5 s)
- Black → centered "Watcher" wordmark (montserrat 32), firmware version small
  below (`v0.1.5`), thin progress arc while subsystems init.
- Skipped fast if already initialized (reboot into last mode).

### 3.2 POINTER mode (default) — the touchpad
- Screen is mostly empty: dim crosshair or dot grid at 20% opacity (so you can see
  finger position), mode pill on top, tiny hint at bottom: `wheel-click: mode`.
- All touch → BLE HID mouse (relative moves, tap = left-click,
  two-finger tap = right-click). LVGL input device **disabled** in this mode.
- Wheel-rotate → BLE scroll wheel events.
- Status icons (BLE dot, battery) stay visible but dim.

### 3.3 VOICE mode
- Idle: the **face** (§9) at rest, soft blink every ~4 s. Two ways to talk to it,
  both live, user-controllable (MENU → Voice):
  - **Wake word** — *"Watcher…"* → `listening` face, ring pulse. Toggleable
    (off for quiet rooms / battery).
  - **Push-to-talk** — tap the face (or hold the side button) → `listening`.
    Always available as long as Voice mode's master switch is on.
- On command recognized: show the parsed command text large
  (`Timer · 3 minutes`), then a confirmation state (`✓ started` / spoken chime).
- On failure: `confused` face + `didn't catch that` for 1.5 s, back to idle.
  Never a dead end.
- Master kill-switch: MENU → Voice → `off` disables both paths (face shows
  `sleepy` in VOICE mode to make the state obvious).

### 3.4 EYES mode (Phase 3) — the exciting one

- Still preview from the Himax (via SSCMA/SPI) scaled to fit the round screen,
  with a crosshair overlay at the host's current gaze/click target.
- Below preview: one-line status from the host link
  (`looking for: "Download"` / `clicking…` / `done ✓`).
- This is a *monitor*, not a viewfinder — no touch interaction except the mode
  switcher. Preview updates at whatever the SSCMA link sustains; the crosshair
  is drawn by LVGL on top (cheap overlay, not a re-encode).

**Interaction loop** (the thing we're really building toward):

1. Point the Watcher at the computer screen. Host detects the screen quad in
   the still, dewarps, and confirms lock — Watcher shows a corner-bracket
   overlay on the preview (`screen locked`).
2. Command it by voice or console: *"click the blue Submit button"*,
   *"scroll down twice"*, *"type hello"*.
3. Host grounds the target in the dewarped frame → crosshair animates to it on
   the Watcher preview → BLE HID click fires → toast `clicked ✓`.
4. Polling triggers (Phase 4): *"click OK when the download finishes"* — the
   Watcher watches, the host decides, the click fires itself.
5. **State-change watchers** — the highest-value EYES primitive: *"tell me when
   the progress bar finishes"*, *"click it when the button turns blue"*. The
   Watcher holds a still of the region; the host diffs new stills against it
   (cheap pixel-diff on the Himax or host, no VLM needed per frame) and fires
   the trigger on change → click, keystroke, macro, or a spoken *"done"*.
   This is dramatically cheaper than re-grounding every frame and it's robust
   to the resolution limits — a progress bar filling or a button changing
   color is visible even in a soft 640px still.

**Resolution reality check** (OV5647 5 MP, 120° FOV, fixed focus 3 m, SSCMA
stills ≤ 640 px to the host):

- Full-frame downscale to 640 px at desk distance: a 24" monitor covers only
  ~170 px of frame width. Fine for *finding* the screen; hopeless for detail.
- The move that makes it work: **ROI crop on the Himax** — capture a 640×480
  window of the *full 5 MP sensor* centered on the detected screen quad instead
  of downscaling the whole frame. Near 1:1 between screen pixels and sensor
  pixels for a 1080p monitor at arm's length.
- Top risk remains the **3 m fixed focus** — desk distances will be soft. Small
  sensors have deep depth of field, so it may be acceptable; close-up legibility
  is the first hardware test (before any host ML work).
- 120° FOV = heavy fisheye; the host dewarp must use a fisheye model, and edge
  sharpness will lag the center. Keep targets away from frame edges.
- Cadence is stills at ~0.5–2 s, not video — so the interaction model is
  **command-based, not continuous**. No real-time cursor chasing; "click the X"
  works, "drag this window" doesn't (yet).
- Honest envelope: **large targets (buttons, icons, window chrome) = plausible
  with ROI crop + dewarp + VLM grounding; dense text / small controls = not
  reliable.** Design EYES interactions around big targets and let the host say
  `too small to hit reliably` when it can't.

### 3.6 TIMER mode — standalone timer / stopwatch

The 412 round screen is a *great* timer face. Two tabs, tap the header to switch:

- **Timer:** giant digits (montserrat 48+ if flash allows, else 32), wheel-rotate
  sets minutes (5-min detents feel right; fine-adjust by tap-hold?), tap
  start/pause, long-press... no — dedicated `reset` pill below. On expiry:
  chime via speaker + `happy` face + toast. Voice *"timer 3 minutes"* jumps here
  with the countdown already running. Backed by the PCF8563 RTC so it survives
  deep sleep.
- **Stopwatch:** tap start/stop, wheel-click… (mode switcher!) — so wheel-rotate
  = lap, tap = start/stop, small `reset` pill. Lap list shows last 3, tiny.
- Runs in background: leaving the mode doesn't stop it; the mode pill shows a
  tiny `⏱` badge while a timer is active, and expiry pulls you back with
  chime + face.

### 3.7 MEDIA mode — BLE media remote

- Big play/pause toggle (96px target, thumb-friendly), prev/next track flanks,
  mute pill. **Wheel-rotate = volume** (the most natural mapping on the device),
  with a volume arc around the edge of the screen.
- Sends BLE HID consumer-control keys; works with whatever's paired — no
  on-device pairing UI needed beyond MENU → BLE.
- Now-playing line (small, scrolling?) — only if we get AVRCP metadata over BLE;
  nice-to-have, not v1.

### 3.8 KEYS mode — BLE macro pad

- 3×3 grid of large keys (each ~110px, fits the round screen with the center
  key dead-center). Tap a key → sends its keystroke/macro over BLE HID.
- Default layout v1: arrows + `Enter`/`Esc`/`Tab`/`Del` + one `Ctrl+C`?
  (Tune with John.) Everything remappable from the web console — macros stored
  on microSD later, compiled-in defaults now.
- Deliberately **not** a full on-screen keyboard: 412px round is too cramped
  for QWERTY; the macro pad covers the real use (shortcuts, arrows, navigation).
  Revisit if someone begs.

### 3.9 MENU mode — settings list
- Vertical list, wheel-rotate moves selection, tap or wheel-click… wait, no:
  wheel-click cycles modes. So **tap selects**, wheel-rotate moves, and a
  dedicated `‹ back` row (or side-button) goes up. Consistent rule, stated once
  in a first-run hint.
- Rows (v1):
  - `BLE` — status + `pair` action row (puts device discoverable, shows 6-digit code)
  - `Wi-Fi` — status; `set up` shows `use console → Setup tab` hint (no on-device keyboard, ever)
  - `Voice` — master on/off, wake-word on/off, push-to-talk on/off (three toggles)
  - `Backlight` — slider (tap +/- steppers, 44px targets)
  - `Display sleep` — 15s / 1m / 5m / never
  - `Device info` — fw version, uptime, heap summary (the `mem` numbers, humanized)
  - `Reboot`
- Header shows `MENU` + mode pill; footer hint `tap: select`.

### 3.10 Toast overlay (all modes)
- Small rounded banner sliding from top-center: `BLE connected`, `Timer done`,
  `Wi-Fi lost`, `flash complete`. Auto-dismiss 2.5 s, queue depth 3, never blocks
  input. This replaces most "status screen" needs — which is why the old static
  status screen goes away.

### 3.11 What's deliberately gone
- The old static status screen (title + BLE/WiFi lines). Its information moves to:
  mode pill + status dots (always visible) and MENU → Device info (details).
- No on-device text entry. Wi-Fi credentials etc. live in the web console.

## 4. Visual language

### 4.1 Color system — Okabe–Ito, dark-theme adapted

You were right to ask: there is canonical research. **Okabe & Ito (2008)** derived
an 8-color palette distinguishable under protanopia, deuteranopia, *and*
tritanopia (~8% of men, ~0.5% of women have some CVD); **Wong (2011, Nature
Methods)** popularized it for design. The core rules we adopt:

1. **Never encode meaning by hue alone.** Every state pairs color with shape,
   icon, text, or position (WCAG 1.4.1).
2. **No red↔green pairing, ever.** The classic "red bad / green good" is invisible
   to most colorblind users.
3. **Check luminance, not just hue.** Colors that differ in hue but match in
   brightness merge under CVD and in grayscale.

The palette (canonical hex; luminance-tuned variants for near-black noted):

| Token | Hex | Role |
|---|---|---|
| `ok_orange` | `#E69F00` | Attention / waiting / warning (replaces the old amber) |
| `ok_sky` | `#56B4E9` | Info, secondary, links |
| `ok_green` | `#009E73` | Success / confirmed / "good" (bluish green, not pure green) |
| `ok_vermilion` | `#D55E00` | Error / destructive (not pure red) |
| `ok_purple` | `#CC79A7` | Accent / special emphasis |
| `ok_blue` | `#0072B2` | Primary brand (deep; use lifted variant on dark bg) |
| `ok_yellow` | `#F0E442` | Highlight — low contrast on its own, use as fill w/ dark text |
| `ink` | `#f2f2f2` | Primary text (off-white, not pure `#fff` — less halation) |
| `ink_dim` | `#a8a8a8` | Secondary text |
| `bg` | `#0b0b0e` | Near-black background (not pure `#000` — avoids halation, keeps glow readable) |

Safest pairs anywhere in the UI: **blue+orange**, **blue+yellow**. Every semantic
state gets a redundant channel: ✓/!/✕ glyphs, distinct shapes, and text labels —
so the UI survives full grayscale.

Dark-theme note: `#0072B2` is too dark for strokes on `#0b0b0e`; wherever deep
blue is needed as a *line* color, use `ok_sky` instead and reserve `#0072B2`
for fills behind light text.

### 4.2 Type & shape (unchanged)

- **Type:** montserrat only. 32 hero, 22 rows/primary, 16 hints/secondary. Never
  smaller than 16.
- **Shape language:** pills and rounded rects (radius 20+), 2px ring outlines for
  selection instead of filled highlight (cheaper redraws, visible on round crop).
- **Motion budget:** only opacity and arc-rotation micro-animations; nothing that
  forces full-screen redraws at 60 fps. The 10-row DMA buffers punish that.

## 5. Power & attention behavior

- **Display sleep:** backlight to 0 after timeout (default 1 min), LVGL tick
  continues. Any wheel/touch/button wakes instantly to the previous mode.
- **Deep attention rule:** the screen never demands attention except for toasts.
  Voice confirmations are toasts + chime, not modal dialogs.

## 6. LVGL implementation notes

- One `lv_disp`, screens as `lv_obj` containers swapped with `lv_scr_load_anim`
  (use `LV_SCR_LOAD_ANIM_FADE_IN`, 150 ms — cheap).
- Mode manager owns: current mode enum, `ui_mode_set()`, input routing
  (enable/disable the LVGL touch indev in POINTER mode), and the persistent
  top bar (mode pill + status dots as a layer above screens via `lv_layer_top()`).
- Toast system: single label+panel on `lv_layer_top()`, timer-driven.
- Fonts: keep montserrat 16/22/32 built in; add 48 for hero/command text only if
  flash budget allows (check `.rodata` after build).
- Every public `ui_*` setter follows the existing pattern: no-op unless
  `display_ready()`, `lvgl_port_lock(0)` non-blocking, skip update if unchanged
  (avoid redundant redraws → saves the DMA buffers for real work).
- String policy: all user-visible strings in one `ui_strings.h` — we will want
  them reviewable in one place, and it keeps `display.c` from bloating.

## 7. Open questions for John

1. **Palette adoption** — Okabe–Ito system in §4.1: good, or tweak any token?
   (Face blue intentionally sits near `ok_sky`, lifted for dark-bg pop.)
2. **Wheel-click = cycle modes**: good, or should wheel-click be "select" in MENU
   and a *long*-click cycles modes? (Current proposal keeps one rule everywhere.)
3. **POINTER default**: should the device boot into POINTER (touchpad) or into a
   home/status screen? Proposal says POINTER — the device is an input assistant
   first.
4. **Battery**: is there a fuel gauge on the Watcher to show, or skip the icon?
5. ~~**Voice**: push-to-talk via tap, wake-word, or both?~~ — decided: **both**,
   user-controllable (2026-09-23).
6. Any **fifth mode** I'm missing? (Candidates: TIMER standalone? MEDIA remote?
   Keep at 4 for v1?)

## 9. The little guy — face design

The unit on its stand reads as a small creature, so we lean in: the Watcher gets
a **face**, a cute stylized robot rendered in electric blue on black with glowing
white outlines. It appears in "companion moments" — never where it would fight
for space with real work (no face in POINTER mode; the touchpad needs the room).

### 9.1 Construction

- Two eyes: glowing ring-arcs (270° arcs), ~46px diameter, spaced ~110px apart,
  centered slightly above middle. Mouth: a short arc below, ~60px wide.
- Face color: **electric blue** — `ok_sky` `#56B4E9` lifted one step toward cyan
  for dark-bg pop (tune on hardware; start `#7FD4FF`-ish). Glow: 3 concentric
  arcs per eye at opacities 80/35/15 — fakes bloom with zero shader cost.
- A thin white outer ring (2px, 25% opacity) traces the *screen edge* during
  face moments, like a halo — sells "glowing white outlines" cheaply.

### 9.2 Expression vocabulary (shape carries meaning, not color — CVD-safe by construction)

| State | Eyes | Mouth | Motion | Appears |
|---|---|---|---|---|
| `wake` | scale from slits → ovals | small `o` → smile | 600 ms open animation | boot, wake from sleep |
| `idle` | soft ovals | tiny smile | blink every ~4 s (120 ms) | VOICE mode idle |
| `listening` | widen (taller ovals) | small `o` | slow ring pulse | wake-word detected |
| `thinking` | glance up-right | flat line | rotating arc above (spinner) | host/voice processing |
| `happy` | `^ ^` arcs | big smile arc | quick double-blink | command success, timer done |
| `sleepy` | closed curves (`‿ ‿`) | tiny line | `z` glyph drifts up, fades | display sleep transition |
| `confused` | one oval, one small | tilted line | head-tilt = whole face rotates 8° | error / didn't understand |
| `party` | `^ ^` + sparkle ticks | open smile | 3 fast blinks | first BLE pair, OTA done |

### 9.3 Where the face lives

- **Boot screen:** face wakes up (`wake` expression) over the wordmark — the
  device introducing itself.
- **VOICE idle:** the face *is* the screen. The mic glyph from §3.3 is replaced;
  `listening`/`thinking`/`happy`/`confused` map directly onto voice states.
- **Sleep/wake:** `sleepy` plays going down, `wake` coming up. Makes the power
  behavior feel intentional instead of the screen just dying.
- **Toasts:** success toasts (`BLE connected`, `Timer done`) get a mini
  `happy` face (64px) beside the text. Error toasts get mini `confused`.
- **EYES mode:** the face steps aside — crosshair takes over. A tiny face could
  peek in the corner showing `thinking` while the host reasons; decide on hardware.

### 9.4 LVGL implementation

- Face = one container (~220×220, centered) holding 2 eye-arcs + mouth-arc +
  glow arcs (~9 objects total — trivial for the object budget).
- Blink: timer callback animates eye arc end-angles (or container y-scale);
  120 ms, partial redraw only.
- Glow arcs are static (no per-frame cost); only eye/mouth angles animate.
- Expression API: `ui_face_set(UI_FACE_HAPPY)` — one call, the face module owns
  all geometry. Strings policy applies: no text baked into the face module.
- The face does NOT need the name yet — but it will want one. (John's call.)

### 9.5 Open face questions

- Face color: electric cyan-blue as proposed, or match `ok_sky` exactly?
- Should the face have a name / should the wake word be its name?
- Mini-face in toasts: yes, or too cute for errors?

## 8. Suggested build order (firmware)

1. Mode manager + mode switcher + input routing (POINTER touch → HID already exists;
   just gate LVGL indev).
2. Toast system (needed by everything).
3. TIMER (big digits, RTC, chime — high delight per effort).
4. MEDIA (consumer keys + volume arc — mechanical).
5. KEYS macro pad (grid + web-console remapping).
6. MENU list + rows (mechanical).
7. VOICE screen states incl. the face (needs ESP-SR events first — Phase 2 anyway).
8. EYES preview + crosshair + screen-lock overlay (Phase 3, needs SSCMA frame path
   and the ROI-crop + focus legibility test first).
9. Boot screen + display sleep + face wake/sleep (polish, last).
