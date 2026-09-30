# Feature backlog (John's ideas → phases)

## Phase 1 — Core HID
- [ ] BLE keyboard + mouse + media keys (composite HIDS)
- [ ] Screen doubles as touchpad (tap=click, two-finger=right-click)
- [ ] Wheel doubles as scroll wheel; wheel-click cycles modes
- [ ] LVGL mode UI on the 412×412 round display

## Phase 2 — Voice
- [ ] "Timer 3 minutes" (wake word → command → number chaining → PCF8563 timer → chime)
- [ ] Voice: "click", "click the <thing>", "move the mouse to <thing>",
  "scroll down", "type …", "press enter", "open terminal"
- [ ] Spoken confirmations via speaker

## Phase 3 — Eyes on screen
- [ ] Point at screen → find screen quad → dewarp
- [ ] "Click the <button>" via host vision grounding → BLE HID click
- [ ] Still preview + crosshair overlay on Watcher screen

## Phase 4 — Autonomous
- [ ] "Click when the download progress bar finishes" (polling trigger)
- [ ] State-change watchers: *"tell me when the button turns blue"*, *"click OK
      when it becomes clickable"* — eyes watch a region, host diffs frames,
      trigger fires on change → click / keystroke / voice announcement
- [ ] "Open a terminal and type <command> once <action> finishes" (macro sequences)
- [ ] Point-at-watcher + "timer 3 minutes" (Himax hand event + voice fusion)
- [ ] Gesture triggers (hand-enter-frame, motion) for custom actions

## Stretch
- [ ] USB HID via J2-header dongle cable (wired option, no board rework)
- [ ] Serial-path host daemon (zero-mod wired input injection)
- [ ] Custom Himax gesture models (pointing vector) via SenseCraft
- [ ] microSD macro library, OTA
- [ ] **S3-orchestrated Himax firmware switching** (John, 2026-09-29): S3 reflashes the Himax between TFLM (inference) and capture-only (high-res) firmware on demand during a single uptime. No USB cable, no manual flash. Needs: bootloader trigger via GPIO/command, UART transport path S3→Himax for XMODEM, both images in S3 flash partitions. Timing TBD — not scheduled.

## Explicitly out of scope (for now)
- Native USB HID through the stock USB-C port — hardware-blocked (bridge only).
- On-device S3 pixel-level computer vision — wrong topology (Himax/host own this).
- Free-form dictation STT on-device — MultiNet commands + host Whisper fallback instead.
