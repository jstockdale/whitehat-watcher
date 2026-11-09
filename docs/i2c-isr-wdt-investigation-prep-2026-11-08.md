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
