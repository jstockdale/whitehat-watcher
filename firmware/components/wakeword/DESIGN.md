# Watcher Wake-Word Design (Feature 5)

**Status:** IMPLEMENTED — v0.3.4
**Date:** 2026-09-24
**Decisions by John (2026-09-24):**
- Default: **OFF**, opt-in via `{"t":"dbg","cmd":"wakeword","on":1}`
- Phrases to test: **"Hey Muse"** and **"Hey Watcher"** (both)
- Action on wake: **start PTT** → send audio to host → **show Muse's response on screen**
- Target release: **v0.3.4** (v0.3.3 ships without ESP-SR)

## Interim Model (v0.3.4)

ESP-SR 2.5.5 does **not** ship "Hey Muse" or "Hey Watcher" models. Custom
phrases require Espressif's custom training process (weeks, per community
reports). v0.3.4 ships with **"Hi, ESP" (`wn9_hiesp`)** as the working
interim integration model — the full ESP-SR pipeline (AFE, I2S time-share,
auto-PTT, message display) is real and tested, but the wake *phrase* is the
interim one until Espressif trains the custom models. The phrase selection
API (`wakeword_phrase`: 0=both, 1=hey_muse, 2=hey_watcher) is implemented
and persisted to NVS now, so when the custom models arrive they drop into
the 2nd WakeNet slot without an API change. Selecting hey_muse/hey_watcher
in v0.3.4 falls back to the interim model with a clear log line.

**Do not claim "Hey Muse"/"Hey Watcher" work in v0.3.4.** The release
notes must state the interim model explicitly.

## Goal

Hands-free voice activation: user says the wake-word, Watcher starts listening
for a command (triggers PTT flow). Complements the wheel-hold PTT for when the
user's hands are busy.

## Wake-Word Choice

**John's decision (2026-09-24): test both "Hey Muse" and "Hey Watcher".**

ESP-SR supports multiple wake-word models loaded simultaneously. Plan:
- Load both `wn9_hey_muse` and `wn9_hey_watcher` models (custom trained)
- Or start with one, A/B test, keep the better performer
- Selection via NVS: `wakeword_phrase` (0=both, 1=hey_muse, 2=hey_watcher)

"Hey Muse" ties to the assistant; "Hey Watcher" ties to the device.
Testing both lets the data decide.

## Architecture

### I2S Sharing (Critical Design Decision)

**Problem:** Both wake-word (continuous) and PTT (on-demand) need the I2S mic
(ES7243 on I2S0, 16kHz 16-bit mono).

**Chosen: Option B — Time-share with clean handoff**

- **Idle:** ESP-SR AFE owns I2S0, continuously listening for wake-word
- **Wheel held:** PTT suspends AFE, takes over I2S0 for recording
- **PTT done:** PTT releases I2S0, AFE resumes listening

**Rationale:**
- Option A (AFE owns I2S always, PTT uses AFE stream) is more seamless but
  requires refactoring PTT's I2S code to use AFE fetch API. Higher risk.
- Option B has a brief gap during handoff (~100ms), but PTT is user-initiated
  (wheel hold), so the user isn't speaking during the handoff.
- Wake-word is deaf during PTT recording — acceptable, user is already
  giving a voice command.

### Component Structure

New `wakeword` component:
```
wakeword/
├── CMakeLists.txt        (REQUIRES esp-sr, ptt, led, link)
├── include/wakeword.h    (public API)
└── wakeword.c            (AFE setup, detection task, I2S handoff)
```

**Public API:**
```c
esp_err_t wakeword_init(void);      // Init AFE, start listening
esp_err_t wakeword_start(void);     // Resume listening (after PTT)
esp_err_t wakeword_stop(void);      // Suspend (before PTT takes I2S)
bool wakeword_enabled(void);
void wakeword_set_enabled(bool en); // User setting: on/off
void wakeword_set_callback(wakeword_cb_t cb); // On detection
```

**Detection callback (John's decision 2026-09-24):**
```c
typedef void (*wakeword_cb_t)(void);
// Action: start PTT recording automatically (as if wheel was held).
// After PTT completes: audio goes to host, Muse processes it,
// host sends response text back, device displays it on screen.
```

### Wake → PTT → Response Flow

1. Wake-word detected → purple LED breathing (AI thinking)
2. PTT auto-starts (no wheel hold needed) → red LED (recording)
3. User speaks command → wheel-click or timeout ends recording
4. Audio encoded → sent to host via `watcher/audio`
5. Purple LED resumes (host processing)
6. **Host sends response text** via new `{"t":"msg","text":"..."}` command
7. Device displays response on screen (new MESSAGE mode UI)
8. Purple clears → dark idle

### Response Display (New Requirement)

John: "show your response on the screen."

New display capability needed for v0.3.4:
- `display_show_message(const char *text)` — full-screen scrollable text view
- Auto-wraps long responses, wheel scrolls through text
- Wheel-click or timeout dismisses → back to previous mode
- Font: 22pt for readability on 412×412 round display
- This is the first "output" UI (everything so far is input/status)

### PTT Integration

In `ptt.c`, before taking I2S:
```c
wakeword_stop();  // Suspend AFE, release I2S
// ... existing PTT I2S setup ...
```

After PTT done:
```c
// ... existing PTT cleanup ...
wakeword_start(); // Resume AFE listening
```

### LED Feedback

On wake-word detection:
- Purple breathing (AI_THINKING) — "I heard you, listening now"
- Then PTT flow takes over (red recording, etc.)

This reuses the existing LED language: purple = AI working.

### User Setting

Per John's voice design (2026-09-23): "voice input should be both wake-word
and push-to-talk, as a user-controllable setting."

- Setting stored in NVS: `wakeword_enabled` (bool, default: false)
- Control via debug command: `{"t":"dbg","cmd":"wakeword","on":1}`
- When disabled: PTT works as before (wheel-hold only), no AFE overhead

## ESP-SR Integration Details

### Component
```
dependencies:
  espressif/esp-sr: "^2.0.0"
```

### AFE Configuration
- Single mic (ES7243) — no beamforming needed
- 16kHz, 16-bit (matches PTT)
- Wake-word model: `wn9_hilexin` (default "Hi Lexin") OR custom
- AFE handles: NS (noise suppression), AGC (gain control), VAD

### Memory
- ESP-SR models: ~800KB-1.2MB (PSRAM)
- AFE buffers: ~200KB (PSRAM)
- Total: ~1.5MB PSRAM — fits in 8MB (currently ~8.0MB free)

### CPU
- AFE + wake-word: ~15-20% of S3 @ 240MHz (single core)
- Runs on Core 1 (App core); LVGL/BLE on Core 0 — minimal interference

## Hardware Validation Plan

1. **Build with esp-sr**: Verify it compiles and fits in flash/PSRAM
2. **AFE init**: Confirm mic input reaches the AFE (check VAD triggers)
3. **Wake-word detection**: Test with chosen phrase, measure false-reject rate
4. **I2S handoff**: Verify PTT still works (wheel-hold) with wake-word enabled
5. **False-trigger test**: Play background audio, ensure no spurious wake
6. **Power**: Measure current draw with AFE running (battery impact)

## Resolved Questions (John, 2026-09-24)

1. **Wake-word phrase?** → Test both "Hey Muse" and "Hey Watcher"
2. **Default on or off?** → OFF, opt-in
3. **Action on wake?** → Start PTT → send to Muse → show response on screen
4. **Which version?** → v0.3.4 (full wake-word build)

## v0.3.4 Scope

- [ ] ESP-SR component integration (AFE, dual wake-word models)
- [ ] I2S handoff: wake-word ↔ PTT
- [ ] Auto-PTT on wake detection
- [ ] `{"t":"msg","text":"..."}` host→device response command
- [ ] `display_show_message()` — scrollable response UI
- [ ] NVS: `wakeword_phrase` selection (both/hey_muse/hey_watcher)
- [ ] Hardware validation: detection rate, false triggers, handoff, power

## Estimated Effort

- ESP-SR integration: 4-6 hours (component, AFE config, build)
- I2S handoff: 2-3 hours (PTT refactor, testing)
- Response display UI: 2-3 hours (message view, scrolling)
- Host protocol (`t:msg`): 1-2 hours (link.c, console support)
- Tuning: 2-4 hours (thresholds, false-trigger)
- **Total:** 3-4 days with hardware access
