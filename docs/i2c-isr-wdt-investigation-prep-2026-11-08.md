# I2C-ISR interrupt-WDT panic — investigation prep (2026-11-08)

Purpose: the legacy-I2C interrupt-watchdog panic has now occurred twice.
Per John's ruling, a watchdog firing is the correct behavior — the bug is
whatever wedges the bus/ISR, not the watchdog. This prep collects the
evidence in one place so the investigation starts from facts, not recall.
It authorizes nothing: no device commands, no firmware change.

## The two sightings

1. **2026-11-07 10:14:21 UTC, fw 0.4.40**, uptime ~1164 s, device
   idle-listening. Guru Meditation: interrupt watchdog timeout on CPU0.
   Backtrace symbolized against the matching 0.4.40 ELF (SHA prefix
   93211813a, confirmed identical to the local build ELF): legacy I2C
   driver ISR (`i2c_isr_handler_default`) with `i2c_hw_fsm_reset` /
   `i2c_hw_disable` on the stack — the shared I2C bus had wedged and the
   driver's ISR recovery spun past the IWDT. Device rebooted and recovered
   cleanly on its own (back at 750000 baud, wake word listening, mic reads
   flowing). Logged at the time as watch-for-recurrence; no fix attempted.
2. **2026-11-08, fw 0.4.43**, during the recovery boot immediately after
   the (separately root-caused) take-end probe crash. Reported in the
   bench chat as the same I2C interrupt-watchdog panic recurring. Full
   panic block and symbolization for this second sighting have NOT yet
   been pulled into this file — that is step 1 below.

## What shares the bus

The wedged bus is I2C0, shared by: ES7243E codec control, the PCA9535
expander (Himax rails/reset, CODEC_PA), and touch. Since 0.4.43 the ES8311
is also created/opened at boot and never closed, which changes boot-time
and steady-state bus traffic versus both sighting baselines (sighting 1
predates the always-open ES8311; sighting 2 is the first boot with it).

## What is NOT yet established

- Whether sighting 2's backtrace matches sighting 1's ISR/FSM-reset shape
  (assumed from the bench report, not yet symbolized in this record).
- Which bus participant was being addressed when each wedge occurred.
- Whether the wedge is a device holding SDA/SCL, a missed interrupt, or
  the legacy driver's FSM-reset path spinning on a recoverable state.
- Whether sighting 2 is causally tied to the crash-recovery boot (dirty
  bus state carried across the panic reset) or an independent occurrence
  that merely happened then. The two readings imply different hunts:
  recovery-boot-only suggests a bus-recovery-at-boot fix; independent
  recurrence suggests a runtime wedge source.

## Gated investigation sequence (each step on John's go)

1. **Pull and symbolize sighting 2's full panic block** from telemetry,
   against the 0.4.43 ELF. Confirm/deny the ISR + FSM-reset shape before
   any theory work (bottom-up: shape first, cause second).
2. **Bracket both sightings in telemetry:** what I2C-touching activity
   (expander writes, codec config, touch reads, boot rail sequencing)
   immediately precedes each wedge.
3. **Only then** form the discriminator: an experiment that separates
   "bus left dirty across reset" from "runtime wedge" — e.g. whether a
   clean power-cycle boot ever shows it vs only panic-recovery boots.
4. Fix candidates are deliberately NOT listed here; per John's debugging
   directive, root cause first, informed fix second — no whack-a-mole.

## Watch posture until then

- Recurrence count: 2. Both recoveries were clean and automatic.
- Any further sighting: capture timestamp, firmware version, boot vs
  runtime, and the preceding ~30 s of telemetry, and append it here.
- This does not block the 0.4.43.1 verification sequence; it rides along
  as a watch item there.

## Resolution (2026-11-09) — root cause established, fixed in 0.4.48.1

Three more sightings after this prep was written: 2026-11-08 23:47:02
UTC (0.4.43 window), 2026-11-09 04:47:58 (0.4.47) and 2026-11-09
08:13:36 (0.4.48) — total **5**. John supplied the 0.4.48 panic as a
console screenshot and ordered the fix.

**Shape (sighting 5, symbolized; ELF SHA 3b745cd3 verified against
the 0.4.48 build):** device idle-listening ~548 s, wake-word reads
clean (err=0). Core 0 backtrace: `input_task` → `wheel_button_down`
(wheel.c:156) → `esp_io_expander_get_level` → vendor `read_input_reg`
→ `i2c_master_write_read_device` → `i2c_master_cmd_begin` (timeout
branch, i2c.c:1627) → `i2c_hw_fsm_reset` (i2c.c:708) →
`i2c_hw_disable`, with `i2c_isr_handler_default` spinning in ISR
context until the interrupt WDT fired. Same shape as sighting 1.

**Root cause chain:**

1. **Trigger:** a rare transaction timeout on I2C0 (30 ms vendor
   timeout) — a marginal-bus event; which slave glitches is not
   established and, with the layers below fixed, no longer matters.
2. **Amplifier (ours):** the vendor PCA95xx read cache never
   engages. The expander is created via the non-ex constructor:
   `int_gpio = -1`, `update_interval_us` left 0 — and the driver's
   cache gate (`int_gpio == -1 || need_update || expired`) treats
   no-INT-pin as "always read the bus". `wheel_button_down()` is
   polled every 10 ms by input_task → **~100 real I2C0
   transactions/second (~8.6M/day)** for a button that changes a
   few times a day. At that exposure, a 1-in-10⁵ bus event fires
   daily.
3. **Fatal step (ESP-IDF legacy driver):** the timed-out
   `cmd_begin` runs `i2c_hw_fsm_reset()`; occasionally the recovery
   spins in the ISR past the interrupt WDT → panic + clean reboot.
   The watchdog behaved correctly throughout (John's standing
   ruling); it stays enabled and untouched.

**Fix (fw 0.4.48.1, our components only — vendor/ is never
modified):**

- `wheel.c`: `wheel_button_down()` samples the bus at most every
  50 ms (the 500 ms PTT claim threshold and ~30 ms debounce lose
  nothing) and, after a failed read, backs off 250 ms serving the
  last known state — a bus episode now meets a quiet bus instead of
  a 100 Hz hammer while the driver resets it. Episode start and
  recovery are logged (`wheel` tag) so the cure is measurable in
  telemetry. Bus transactions for the button drop ~5× immediately,
  and ~all episode-retry amplification goes away.
- `eyes.c`: I2C0 clock 400 kHz → 100 kHz. The bus carries only the
  expander and codec control; standard mode quadruples timing
  margins against the trigger class at no functional cost.

**Verification posture:** success looks like (a) no further IWDT
panics across days of listening and (b) occasional logged
"expander read failed (episode N)" + "recovered" pairs instead —
episodes absorbed quietly are the fix working, not new failures.
If panics recur WITH episodes logged, the next suspect layer is the
sscma sync-pin get_level path; if they recur WITHOUT episodes, the
timeout source is another I2C0 caller and the hunt reopens with
that discriminator already in place.
