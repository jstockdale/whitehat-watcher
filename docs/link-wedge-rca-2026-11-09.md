# S3↔Himax SPI link wedge — RCA and hardening/self-recovery design

**Incident:** 2026-11-09, ~21:16:51–21:37:44 UTC (telemetry log
`host-agent/mqtt-bridge/telemetry.log`, line range ≈2529910–2582215).
**Firmware at the time:** S3 0.4.49.1, Himax 0.4.20.61.
**Status:** analysis only — no code changed, nothing built or flashed.
**Outcome of the incident:** the link never self-recovered in ~21 minutes;
the owner power-cycled the whole device.

---

## 0. Corrections to the incident framing (log-verified)

Two details in the original incident description do not survive contact
with the log, and they matter for the RCA:

1. **The wedge began *before* the `dbg png`, not during its bulk read.**
   The crop set (`0,0,2592,486`) returned `ok:true` at 21:16:51
   (device uptime 40466032 ms, log line 2529910). The first
   `spi_send_cmd: Himax not RX-armed` failure is 178 ms later
   (uptime 40466210, line 2529912). When `dbg png` arrived at 21:17:29
   it never reached SPIDUMP: `eyes_capture_png` failed at the mode
   select — `set_sensor opt 8 attempt 0/1/2 failed: ESP_ERR_TIMEOUT`,
   then `capture_png: mode 1296x972 select failed` (lines 2531546–
   2531596). The bulk path is therefore **not** implicated in this
   incident; the failing surface is the plain AT command path. (The
   identical crop+png sequence, including two bulk captures, had
   succeeded at 20:23–20:31 UTC the same day.)
2. **The flood ran ~21 minutes, not 7.** First failure 21:16:51, last
   logged failure 21:37:44 (line 2582211), ending only at the owner's
   power cycle. ≈24,300 flood lines (warn+error pairs) are in the log;
   the device's log path itself drops under load, so that is a floor,
   not an exact attempt count. Early device timestamps show one attempt
   every ~51 ms (uptime 40466210→40467188, 20 attempts).

**Correction (owner, 2026-11-09, applies project-wide):** the wheel
button's push-and-hold is PTT (push-to-talk). There is **no**
hold-to-power-off gesture on this device, despite older schematic
notes suggesting a 3 s hold wakes it. Any text elsewhere implying a
hold-to-power gesture exists today is wrong. §5 (addendum) designs
the deliberate power-off mechanism this correction makes necessary.

Device state before the wedge was unremarkable: uptime ≈11.2 h,
heartbeats normal, `errors=0`, heap/internal-free at their usual tight
but stable values (heartbeat at 21:16:54, line ~2529960s).

---

## 1. The wedge mechanism

### 1.1 The flag protocol (one wire, two meanings)

S3 and Himax share a single-wire handshake on the SPI MISO line
(S3 GPIO6 = Himax PB2), documented on the S3 side at
`firmware/components/sscma_client/src/sscma_client_io_spi.c:47-50` and
implemented on the Himax side in
`sscma-fork/.../himax/we2/ma_transport_spi.cpp`:

- **MISO LOW** = Himax RX armed. The Himax muxes PB2 to GPIO13 and
  drives it LOW, and does so *only* after its RX DMA is armed
  (`ma_transport_spi.cpp:504-530`, `_spi_arm_rx()`; the mux/drive is at
  lines 525-527).
- **MISO HIGH** = "not RX-armed". After staging a response the Himax
  muxes PB2 back to SPI_S_DO (`ma_transport_spi.cpp:792-800` and the
  sibling handlers); the DW SPI slave tristates DO while deselected,
  so the S3-side pull-up (enabled in eyes init) takes the line HIGH.
  HIGH therefore conflates three physically different states:
  (a) a response is staged and waiting to be clocked out,
  (b) the transport is parked (PB2 deliberately released), and
  (c) **the Himax is not driving the pin at all** — crashed, hung,
  unpowered, or its transport never re-armed.

The S3 cannot distinguish (a) from (c) by looking at the wire. Every
recovery-relevant design consequence in §2-§4 follows from that.

### 1.2 The S3 side of the deadlock

Every S3→Himax command — including the AVAILABLE poll (transport cmd
0x03, `sscma_client_io_spi.c:120`) — is hard-gated on MISO LOW:

- `spi_send_cmd_()` waits up to 50 ms for LOW and fails fast with
  `ESP_ERR_TIMEOUT` (`sscma_client_io_spi.c:813-824`). The fail-fast is
  deliberate (v0.4.18.38 comment: "If timeout, the Himax is down; fail
  fast").
- The AT write path has the same 50 ms gate
  (`sscma_client_io_spi.c:431-434`).
- The READ path gates on LOW before it may clock anything out
  (`spi_txn_read_exec`, `sscma_client_io_spi.c:573`), and the AVAILABLE
  exec does the same (`sscma_client_io_spi.c:649`).

The SSCMA process task polls AVAILABLE in a loop
(`firmware/components/sscma_client/src/sscma_client_ops.c:207-230`):
on `ESP_OK && rlen` it reads and parses; **on failure it simply loops**
(1 ms delay). There is no failure counter, no backoff, no escalation,
no notification. Observed cadence in the wedge: one 50 ms gate timeout
per ~51-60 ms — the process task spent ~11 h of uptime design and the
whole 21-minute wedge doing exactly this.

So the S3-side stuck state is precisely:

> MISO reads HIGH forever → every command path refuses to transmit →
> the one mechanism that could clear a "staged TX" state (clocking
> bytes out) is only reachable *after* a successful LOW gate → the
> poller retries the gate forever and tells nobody.

Two escape tools exist in the S3 code but are unreachable from the
AT path:

- `spi_drain_tx_()` (`sscma_client_io_spi.c:781-811`) clocks 64-byte
  chunks with no MOSI until MISO goes LOW — written for exactly state
  (a). Its only callers are inside the bulk executor
  (`sscma_client_io_spi.c:1344` and `:1419`). (It also *discards* what
  it clocks out, which is wrong for AT responses — see §3-S3.)
- `spi_send_cmd_no_gate_()` ( `:894ff`) can transmit without the LOW
  gate, and the unpark path uses it to try AVAILABLE "on faith" and
  then CMD_RESET as a resync (`:952-993`). Its only caller is unpark
  (SD-card flow); the wedged AT path never calls it. Note PARK itself
  is **refuted** as a factor in this incident: `sscma_client_spi_park`
  has no callers anywhere in the current S3 tree (grep over
  `firmware/components`), and no park activity appears in the log.

### 1.3 The Himax side of the deadlock

On the Himax, all re-arm intelligence lives in `ma::spi_poll()`,
called from the AT-server main loop
(`library/sscma_micro/sscma/server/at/ma_server_at.cpp:113`, in a
`while(true)` loop with a 20 µs sleep). Three guards (design comments
at `ma_transport_spi.cpp:131-155` and `:1430-1453`):

- **Guard 1 — drained path.** The TX DMA-completion callback sets
  `_spi_tx_done` (`ma_transport_spi.cpp:1074-1080`). spi_poll then
  re-arms RX when the driver drain-signals agree and ≥2 ms have
  elapsed, with a 25/30 ms timer as fallback ceiling
  (`ma_transport_spi.cpp:1455-1488`). Re-arming calls `_spi_arm_rx()`,
  which arms RX DMA and only then drives PB2 LOW.
- **Guard 2 — staged watchdog.** Command handlers set
  `_spi_tx_staged` when `spi_write_dma` succeeds (READ:
  `ma_transport_spi.cpp:796-800`; READ_CRC, AVAILABLE, TEST_PATTERN
  handlers likewise). If staged data is still unclocked after 300 ms,
  spi_poll tears the transport down (`spi_close`/`spi_open`) and
  re-arms (`ma_transport_spi.cpp:1490-1517`). This covers the
  "S3 abandoned the response" case of any size.
- **Guard 3 — arm-failure escalation.** If `_spi_arm_rx()`'s
  `spi_read_dma` fails, `_spi_rx_arm_failed` is set
  (`ma_transport_spi.cpp:511-521`); spi_poll retries the arm and after
  ~10 consecutive failures does the same teardown + re-arm
  (`ma_transport_spi.cpp:1319-1365`).

The F3 "parse-retire" block in `_spi_rx_callback`
(`ma_transport_spi.cpp:~703-718`) clears `_spi_tx_staged` and
`_spi_tx_done` when the *next* valid command parses — sound under the
strictly-sequential bus discipline, because a new command proves the
previous response was fully clocked out.

Given spi_poll running, every *covered* stuck state self-clears in
≤300 ms. A 21-minute wedge therefore means the state was **not
covered**, or **spi_poll was not running**. That dichotomy structures
the hypotheses.

### 1.4 Ranked hypotheses

**H1 — Himax transport bookkeeping hole: both re-arm flags false,
RX unarmed, PB2 released. (Most likely.)**

There is a concrete uncovered state in the code. `_spi_arm_rx()`
begins:

```cpp
static void _spi_arm_rx() {                     // ma_transport_spi.cpp:504
    if (!_spi_dev || !_spi_opened) {
        return;                                  // ← silent, sets nothing
    }
    ...
    int32_t rc = _spi_dev->spi_read_dma(...);
    if (rc != E_OK) { _spi_rx_arm_failed = true; ... }
```

If a Guard-2 or Guard-3 teardown's `spi_open` **fails** (both teardown
sites ignore its result beyond setting `_spi_opened` from the return
value: Guard 2 at `ma_transport_spi.cpp:1505-1515`, Guard 3 at
`:1345-1355`), then `_spi_opened == false`, and the `_spi_arm_rx()`
call that follows returns immediately *without* setting
`_spi_rx_arm_failed`. End state: `_spi_tx_staged == false`,
`_spi_tx_done == false`, `_spi_rx_arm_failed == false`,
`_spi_opened == false`, PB2 muxed to SPI_S_DO (tristate → HIGH).
**No guard can ever fire again** — Guard 1 needs `_spi_tx_done`,
Guard 2 needs `_spi_tx_staged`, Guard 3 needs `_spi_rx_arm_failed`,
and nothing retries `spi_open`. spi_poll spins happily at ~1 kHz
doing nothing. This exactly reproduces the observed signature:
response delivered, then permanent HIGH, no Himax reboot (no INIT@
broadcasts appear in the S3 log during the window — the S3 relink
detector at `eyes.c:112-128` never fired), cure by power cycle.

What would have triggered the teardown in the first place is *not*
established — a Guard-2 firing needs a response staged >300 ms, which
the CROP exchange (completed in ms) does not obviously provide. Two
candidate triggers: (i) an earlier staged-but-slow response in the
minutes before (the last pre-wedge Himax traffic was the 20:31
captures — 45 min earlier, too far for a 300 ms watchdog to be the
proximate trigger, but the teardown could also come from Guard 3 if
an arm had failed at some point and the counter reached ~10 only
later — the counter is cumulative across the whole uptime between
successes); (ii) a race in which Guard 1's re-arm for the AVAILABLE
count and the F3 retire interact with the READ staging such that a
flag is cleared after the READ handler set it. I could not construct
(ii) deterministically from the code — the F3 block runs at parse
time, before the handler — so (i)/the spi_open-failure end-state is
the load-bearing part of H1, and its *entry point* is the part that
remains unproven.

*Confirm:* on the bench, capture the Himax UART DIAG stream during a
recurrence or an induced wedge (see §4.3): Guard-2/3 activity is
visible in the REARM stats emits (`wd=` counts `_spi_wd_aborts`;
`ma_transport_spi.cpp:300-325`) and a teardown would appear there. If
the end-state is H1's, a Himax-side register/flag dump (or a debugger
attach) shows `_spi_opened == false` with all guards idle.
*Refute:* if a recurrence shows `_spi_opened == true` and RX simply
unarmed with flags false, the hole is a different flag path and H1's
specific mechanism is wrong (the class — uncovered flag state —
would stand).

**H2 — Himax AT/executor thread stalled: spi_poll not running.**

spi_poll, the AT dispatch, and all three guards share one thread
(`ma_server_at.cpp:100-119`). If that thread (or the executor thread
that ran the CROP service) blocked permanently — e.g. inside the
camera stack touched by `setCropWindow` — no guard runs regardless
of flags, with the same external signature as H1.

`setCropWindow` (`ma_camera_himax.cpp:400-497`) is the heaviest thing
the Himax did in that minute: from the cleared state at 13:16 it took
the full path — `deInit()`, persistent-buffer reuse (the 0.4.20.61
fix, `:445-487`), `drv_ov5647_set_raw_buffer`, then
`drv_ov5647_init_raw_fullres` (`:489`). Against H2: the CROP reply the
S3 received reports `code 0` with the new geometry echoed, so
setCropWindow had returned success before the reply was composed —
the heavy call itself did not hang. For H2: whatever it left behind
(a sensor pipeline state, a lock, a wedged capture thread that the
AT executor later joins) is not visible from the S3 side, and a hang
*after* the handler returned is still possible in code I did not
fully trace (the executor's post-response path and the camera
thread's reaction to the new crop are outside the files read).

*Confirm:* Himax UART during a recurrence — if the DIAG ring stops
draining and the UART console is dead while power is applied, the
AT thread is stalled (favours H2 over H1, where the loop — and its
periodic DIAG/REARM emits — would still run). A spi_poll gap counter
(§3-H4) settles it definitively.
*Refute:* DIAG/REARM lines continuing to print during a wedge.

**H3 — S3 misreads the flag (GPIO6 contention or mux change).**

Ranked low. The S3 reads `gpio_get_level(GPIO_NUM_6)` directly
(`sscma_client_io_spi.c:1004-1035`); nothing in the tree reconfigures
GPIO6 at runtime, and the DW slave never *drives* DO high while
deselected — HIGH requires the pin to be undriven (pull-up) or the
Himax holding it in GPIO13 mode high, which no code path does.
*Confirm/refute:* one bench measurement — probe PB2 during a wedge.
LOW at the pin while the S3 logs HIGH ⇒ S3-side; HIGH/floating ⇒
Himax-side. Cheap and decisive; worth doing once (§4.3).

**H4 — A transient spill/misalignment cascaded into the wedge.**

Ranked low as a *cause*. Spills are a designed-for case (CRC-validated
realignment scan, `ma_transport_spi.cpp:632-675`); a frame that fails
at every offset is dropped and RX is re-armed (`:695`). That path
recovers by construction and cannot produce a silent permanent HIGH.

**Ruled out by evidence:** PARK (no S3 caller exists — §1.2); a Himax
reboot loop (no INIT@/relink in the log); an S3-side task death (the
worker, process task, heartbeats and BLE all kept running — the flood
itself is proof of life); the bulk/SPIDUMP path (never entered —
§0.1).

### 1.5 What the wedge cost while it lasted

The process task's failure loop is also a log flood: ~24,300 lines in
21 minutes through a log path that shares the telemetry uplink, plus
a prio-15 SPI worker kept busy 50 ms at a time. The device stayed up
and responsive on every other axis (heartbeats, host summaries) —
the failure was contained, just unrecoverable and very loud.

---

## 2. Why self-recovery failed

### 2.1 The recovery machinery that exists

There is a complete peripheral-recovery primitive on the S3:

`eyes_recover_camera(bool cycle_rail)`
(`firmware/components/eyes/eyes.c:650-695`):

1. clears `link_up_`;
2. if `cycle_rail`: drops the Himax rail via the PCA9535 expander
   (`PIN_HIMAX_RAIL` = bitmask `1<<11` = P13, `eyes.c:62`) for 300 ms
   and restores it;
3. calls `sscma_client_reset()` (`sscma_client_ops.c:831-870`), which
   suspends the process task, clears the parser buffers, and pulses
   the Himax RST line via the expander (`PIN_SSCMA_RST` = `1<<7` =
   P07, `eyes.c:58`; wired into the client config at `eyes.c:545`)
   for 100 ms, then resumes the process task;
4. waits 1 s, re-runs the AT handshake (`eyes_rehandshake_`:
   set_model retries + sensor preset), sets `link_up_ = true`.

Total ~2-4 s. The Himax reboots; **the S3 never does.** This is
exactly the owner's rule — recover the link/peripheral, never reboot
the device for a wedged peripheral — already implemented. The
rail-cycle rung additionally covers a Himax too hung for RST to take.

### 2.2 The four triggers, and why each one missed

| Trigger | Code | Why it didn't fire |
|---|---|---|
| Invoke-failure streak (≥3 → `eyes_recover_camera(false)`) | `eyes.c:735-749`, streak defined `:156-159` | Counts **snapshot invoke failures only**. The device was idle (wake-word listening is S3-side I2S; no invokes run), so the streak sat at 0 for 11 h and through the wedge. |
| `capture_png` link-down gate | `eyes.c:1982-1990` | Fires only if `link_up_` is false. `link_up_` is set by `on_connect` and cleared **only by `eyes_recover_camera` itself** (`eyes.c:108-110, 618-624`). A wedged-but-not-rebooted Himax produces no `on_connect`, so the flag stayed true and capture_png skipped recovery, went to `eyes_set_sensor_opt` (3 attempts, `eyes.c:1089-1099`), and failed. |
| Himax-reboot relink worker | `eyes.c:112-128, 226-250, 344-371` | Detects a Himax reboot via its INIT@ boot broadcasts (10 s debounce). The Himax never rebooted — it wedged in place — so nothing fired. |
| `dbg eyes_reset` (manual, rail default on) | `link/link.c:~2253-2280` | Human-only and **serial-console only**: `via_mqtt` callers are rejected by design ("a remote caller must never do this unattended"). The bench drives the device over MQTT, so even the manual escape hatch was policy-blocked during the incident. |

And the layer that *saw* the failure 12,000 times — the process
task's AVAILABLE poll (`sscma_client_ops.c:230`) — has no failure
accounting at all (§1.2). The `link_mon` task the project talks about
is a red herring here: `link_monitor_task`
(`firmware/components/link/link.c:986ff`) monitors the **host UART**
(baud auto-revert, pending host commands). Nothing in the firmware
monitors the Himax link's health.

**Summary of §2:** recovery failed not because the machinery is
missing but because every trigger is keyed to a signal the wedge
doesn't produce (invoke failures, a reboot broadcast, a stale flag
read as healthy, a human on the serial console). The one signal the
wedge produces in unlimited quantity — consecutive transport-gate
failures — is counted nowhere.

### 2.3 Escalation ladder assessment (vs. the owner's rule)

Proposed ladder, cheapest first; every rung already exists as a
primitive except the wiring:

1. **Drain** (§3-S3): clock out whatever the Himax has staged.
   Fixes state (a) of §1.1. No Himax state change, no reboot of
   anything. *Rule: trivially compliant.*
2. **No-gate CMD_RESET**: the Himax's RESET handler clears both
   rings and re-arms RX (`ma_transport_spi.cpp:~1000-1010`). Works
   only if the Himax's RX DMA happens to be armed (park-class
   states); in the H1/H2 end-states it will be clocked into the
   void — harmless, bounded (~50 ms), worth one attempt before
   escalating. *Rule: compliant (link-level resync).*
3. **`eyes_recover_camera(false)`** — RST pulse + re-handshake.
   The Himax reboots; the S3 doesn't. *Rule: this is precisely what
   the rule prescribes* — the wedged peripheral is reset, the device
   is not. (John's watchdog rulings in MEMORY.md: a slow camera
   peripheral must not reset the S3; resetting the *peripheral* is
   the sanctioned move.)
4. **`eyes_recover_camera(true)`** — rail cycle (P13 off 300 ms) +
   RST + re-handshake, for a Himax that ignores RST. *Rule:
   compliant, with two guards:* rate-limit it (e.g. escalate to rail
   only after an RST rung has failed, and cap rail cycles per hour
   with a long backoff) so a permanently dead Himax isn't
   power-cycled in a tight loop, and surface every cycle in
   telemetry/heartbeat so it can't silently mask dying hardware.

There is no rung 5. An S3 reboot is never part of the ladder.

One state caveat for the ladder's design: after any Himax reset the
crop window, sensor preset (beyond the handshake's preset-8 restore
in `eyes_rehandshake_`), and bulk state are gone. Recovery code must
treat Himax-side session state as lost; callers re-issue. The
handshake already restores the sensor preset; crop is caller state
and the crop caller must be told the link reset happened (a link
generation counter is the clean mechanism — see §4).

---

## 3. Hardening candidates

### S3 side

**S1 — Count transport-gate failures; escalate on a streak.**
*Location:* `sscma_client_ops.c:230` (process-task poll) plus the
io-layer error returns; ladder execution in `eyes.c` next to
`eyes_recover_camera`.
*Sketch:* a shared counter incremented on every `spi_send_cmd_`
LOW-gate timeout (available/write/read), reset on any successful
transaction. At a threshold (~20 consecutive ≈ 1-2 s of wedge),
and only when no snapshot or bulk transfer is in flight (the
invariants `eyes_recover_camera` already requires — it is currently
the caller's job to guarantee them), run the §2.3 ladder:
drain → RESET → `eyes_recover_camera(false)` → (next threshold,
with backoff) `eyes_recover_camera(true)`. Log one line per rung,
not per failure.
*Risk:* a false trigger during a legitimate long Himax-busy window
would reset a healthy camera. Mitigations already in the design:
bulk holds `bulk_active` and the process task skips polling then
(`sscma_client_ops.c:222-228`); AT writes during bulk queue behind
the io lock rather than failing the gate. The threshold must also
exceed the Himax's worst legitimate re-arm latency (Guard-2's
300 ms teardown window ⇒ ≥1 s of continuous failure is already
pathological). Residual risk: moderate-low; this is the core fix.

**S2 — Make `link_up_` honest.**
*Location:* `eyes.c` (`link_up_` at `:110`, gate at `:1982`).
*Sketch:* the S1 counter also feeds `link_up_`: after the same
threshold, set `link_up_ = false` (recovery sets it true again).
This makes capture_png's *existing* recovery gate work as designed,
and gives every other consumer (`eyes_status`, the relink logic) a
truthful flag. Also feed the counter from `eyes_set_sensor_opt` and
`eyes_set_crop` failures (today they just return errors to callers
who log and move on).
*Risk:* low; the flag's semantics ("AT handshake completed and
Himax answering") are arguably *restored*, not changed. Care: the
relink debounce logic reads `link_up_` (`eyes.c:242`) — a
recovery-driven clear followed by the Himax's INIT@ burst must not
double-trigger the relink worker; the existing `relink_inflight_`
guard plus the fact that recovery itself re-handshakes covers this,
but it must be a named test case (§4.3).

**S3 — Drain before declaring the gate failed.**
*Location:* `spi_txn_available_exec` / `spi_send_cmd_`
(`sscma_client_io_spi.c:813ff`, `:619ff`); reuse `spi_drain_tx_`
(`:781`).
*Sketch:* on a LOW-gate timeout, before returning
`ESP_ERR_TIMEOUT`, make one bounded drain attempt (~200 ms, not the
bulk path's 2 s). Unlike the bulk version, do **not** discard: if
bytes were clocked out, hand them to the AT parser path (they are,
by protocol, a staged response). If the line falls to LOW, retry
the command once.
*Risk:* clocking against a Himax that is mid-arm could add spill
bytes to the RX stream — a case the Himax already tolerates via
its CRC realignment scan (§1.4-H4). Bounded, and only on a path
that is currently a guaranteed failure. Risk: low-moderate. This
rung fixes state (a) wedges in ~200 ms without touching the Himax.

**S4 — Backoff in the process-task poll + flood suppression.**
*Location:* `sscma_client_ops.c:213-230`; `spi_send_cmd_` logging
(`sscma_client_io_spi.c:820-823`).
*Sketch:* consecutive available() failures grow the poll delay
(1 ms → 10 → 50 → 250 ms cap, reset on success), and the io layer
logs the gate failure once per state transition plus a periodic
counter line instead of once per attempt. Add `himax_link`
(up/down, failure streak, recoveries today) to the heartbeat.
*Risk:* backoff delays pickup of a genuine event after recovery —
capped at 250 ms and reset on any success or on ladder activity;
acceptable. Pure win otherwise: the 21-minute flood wrote ~24k log
lines through the telemetry path for zero information after the
first one.

**S5 — Port the bulk path's escalation philosophy to AT writes.**
*Location:* `client_io_spi_write` (`sscma_client_io_spi.c:425ff`).
*Sketch:* the bulk executor already does drain + CMD_RESET +
restart, up to `BULK_MAX_ATTEMPTS` (`:1437`, `:1344-1440`). The AT
write path fails once and gives up. It doesn't need the full
machinery (S1's ladder covers recovery), but a write that fails
the gate should *feed the S1 counter* rather than failing silently
into a caller's one-line warning — listed separately because
`set_sensor`'s 3-attempt retry loop (`eyes.c:1089-1099`) currently
burns ~1 s rediscovering a dead link on every call.
*Risk:* low (accounting only).

### Himax side

**H-A — Close the unmonitored `_spi_opened == false` state.**
*Location:* `ma_transport_spi.cpp:504-507` (`_spi_arm_rx` early
return), Guard 2 (`:1505-1515`), Guard 3 (`:1345-1355`).
*Sketch:* (1) `_spi_arm_rx()` must never fail silently: if
`!_spi_opened`, set `_spi_rx_arm_failed = true` so Guard 3 engages.
(2) Guard 3's teardown must treat a failed `spi_open` as an
arm failure too — keep a persistent retry: while `_spi_opened` is
false, spi_poll retries open+arm on the Guard-3 counter cadence.
No reachable state may have all of {staged, done, arm_failed}
false with RX unarmed.
*Risk:* if the hardware fault is permanent, this retries forever —
which is the correct behaviour for a peripheral (bounded by the
existing ~10-count cadence, and the S3 ladder is the outer loop).
Risk: low. This is the direct fix for H1's end-state, whatever its
entry point was.

**H-B — Flag-independent stuck-unarmed watchdog in spi_poll.**
*Location:* `ma_transport_spi.cpp`, `spi_poll()` beside Guard 2.
*Sketch:* track "RX continuously unarmed" time directly (a
timestamp set whenever `_spi_arm_rx` succeeds, cleared-checked
each poll). If RX has been unarmed for >2 s while
`!_spi_bulk_armed && !_spi_tx_staged && !_spi_tx_done`, force the
Guard-2 teardown + open + arm. The 2 s bound is conservative
against the design's own worst legitimate unarmed window (300 ms,
Guard 2) with ~6× margin. Gate on bulk explicitly: during bulk the
feeder legitimately holds direction state for seconds.
*Risk:* tearing down a state the designers intended — mitigated by
the flag gates above (any *known* in-flight state exempts it; it
fires only in states no guard claims). Risk: low-moderate, and it
future-proofs the whole H1 class, not just today's entry point.

**H-C — Verify the arm took.**
*Location:* Guard 1 (`ma_transport_spi.cpp:1455-1488`) /
`_spi_arm_rx()` tail.
*Sketch:* after `_spi_arm_rx()` in the Guard-1 path, read back the
PB2 mux/GPIO13 state; if PB2 is not in GPIO13-LOW mode, set
`_spi_rx_arm_failed` so Guard 3 engages instead of the system
believing it re-armed. Turns silent arm failures into the covered
Guard-3 path.
*Risk:* negligible (readback only, thread context).

**H-D — spi_poll gap instrumentation (H2 discriminator).**
*Location:* head of `spi_poll()`.
*Sketch:* record the max inter-call gap of spi_poll (the REARM
probes already track first-poll latency for TX; generalise it) and
emit a DIAG line whenever a gap exceeds ~500 ms. In any future
wedge this one number separates H1 (gaps stay ~1 ms; loop alive,
state uncovered) from H2 (gap ≈ wedge duration; loop stalled).
*Risk:* negligible (a timestamp, a counter, a rare log line).

**H-E — Preserve the re-arm timing contract.**
Note for whoever touches Guard 1: the standing rule from AGENTS.md
("TX-done timeout must be *shorter* than the S3's minimum
inter-command time; transfer_time+1500 µs for small transfers") was
written for the timer-based re-arm (≤0.4.20.36). The current
signal-driven Guard 1 (0.4.20.59) honours its intent by
construction — typical re-arm ≈2 ms after DMA-done, worst case the
25/30 ms ceiling, both far inside the S3's 50 ms LOW gate. Any
change must preserve: *worst-case re-arm after a completed TX
≤ ~30 ms, and never gated on the master doing something first.*
If a future change reverts to a pure timer, the AGENTS.md formula
is the floor for small transfers, not a ceiling to exceed.

**H-F — (evidence-dependent) camera re-init observability.**
*Location:* `ma_camera_himax.cpp:400-497` (`setCropWindow`).
*Sketch:* only if H2 gains evidence: emit DIAG at deInit/init
boundaries and around `drv_ov5647_init_raw_fullres`, so a stall
inside the camera stack is attributable after the fact. No
behaviour change proposed on current evidence.
*Risk:* none (logging), but don't gold-plate: H-D ships first and
tells us whether this is needed.

---

## 4. Implementation plan

Ordered; each step independently testable. S3 steps land first
(they recover the link regardless of which Himax hypothesis is
true); Himax steps reduce incidence.

1. **S3 observability (S4):** flood suppression in
   `spi_send_cmd_`, poll backoff in the process task, `himax_link`
   fields in the heartbeat. Files: `sscma_client_io_spi.c`,
   `sscma_client_ops.c`, heartbeat builder in `link.c`.
2. **S3 link-health accounting (S1+S2+S5):** shared consecutive-
   failure counter fed by available/write/read/set_sensor/set_crop;
   drives `link_up_`. Files: `sscma_client_io_spi.c`,
   `sscma_client_ops.c`, `eyes.c`.
3. **S3 escalation ladder (§2.3):** drain (S3, with parser hand-off)
   → no-gate CMD_RESET → `eyes_recover_camera(false)` → backoff →
   `eyes_recover_camera(true)` (rate-limited). Executed from a
   worker context that can honour the snap/bulk in-flight
   invariants (defer while `snap_busy_` or `bulk_active`). Add a
   link generation counter, bumped per recovery, so AT callers
   (crop especially) can detect that Himax session state was lost
   and re-issue. Files: `eyes.c`, `sscma_client_io_spi.c`.
4. **Himax uncovered-state fixes (H-A, H-C):** no silent arm
   failure; failed `spi_open` is retried by Guard 3; Guard-1 arm
   readback. File: `ma_transport_spi.cpp`.
5. **Himax stuck-unarmed watchdog + poll-gap instrumentation
   (H-B, H-D).** File: `ma_transport_spi.cpp`.
6. **Bench verification** (§4.1-4.3), then the standing 20-frame
   soak plus a scripted crop/strip loop as a soak extension.

### 4.1 Bench test — recovery behaviour (new S3 firmware, stock Himax)

- Baseline: run the incident sequence (crop set → png → strip set
  → png → clear) 20×; expect zero wedges, zero recoveries.
- Kill the Himax rail from the serial console (`dbg eyes_reset`
  with `rail:1` is itself a recovery — instead, power the Himax
  down via the existing rail control path and leave it down):
  verify the ladder *doesn't* spin — RST rung fails, rail rung
  fails, backoff grows, heartbeat reports the link down, the S3
  stays up and responsive throughout (this is the owner's-rule
  acceptance test: peripheral dead ⇒ device alive, honest, quiet).
- Restore the rail: verify the ladder recovers the link on its own
  (INIT@ burst, re-handshake, `link_up_` true) with no S3 reboot
  (uptime continuity in heartbeats).

### 4.2 Bench test — fault injection (the wedge, on demand)

Two injections, because there are two hypotheses:

- **H1 injection (Himax debug build):** a one-shot test hook
  (compile-time flag or a DIAG/test AT extension, in the spirit of
  the existing CMD_TEST_PATTERN) that, on the next staged response,
  forces the H1 end-state: clear `_spi_tx_staged`/`_spi_tx_done`
  and skip the re-arm. Expected: on *current* S3 firmware, the
  exact incident signature (gate flood, no recovery) — that
  signature match is the RCA confirmation that H1's end-state is
  *sufficient* to produce the incident. On *new* S3 firmware: the
  ladder recovers (drain fails, RESET fails, RST rung succeeds)
  in <10 s, no device reboot.
- **H2 injection:** a hook that stalls the AT loop for 60 s after
  the next command (sleep in the loop thread). Expected: Himax
  guards can't fire (they're in the stalled loop); the S3 ladder
  still recovers the link via RST. This injection is why the S3
  ladder — not any Himax-side fix — is the load-bearing change.

### 4.3 Bench test — evidence capture for the natural recurrence

- In parallel with §4.1: script the incident sequence in a loop
  (≥200 iterations) against *current* firmware with the Himax UART
  DIAG stream captured (the REARM emits print `_spi_wd_aborts` and
  the guard counters; DIAG drains to the Himax UART,
  `ma_transport_spi.cpp:236-262`). A natural recurrence with UART
  capture discriminates H1 vs H2 per §1.4.
- If a wedge is caught live: before power-cycling anything, probe
  PB2 (H3's decisive measurement) and note whether the Himax UART
  is still emitting (H2 discriminator).

### 4.4 Acceptance criteria

- Injected wedge (either kind) ⇒ link restored without an S3
  reboot, bounded (<10 s to RST rung), exactly one recovery
  sequence in telemetry, heartbeat shows the event and the
  recovery count.
- Dead-Himax test ⇒ no recovery loop tighter than the backoff
  schedule, rail cycles within their rate cap, S3 fully responsive.
- 200-iteration crop/strip loop ⇒ zero unrecovered wedges; any
  ladder activity is visible and explained.
- No regression: spitest, bulk captures (PNG + snaptest), and the
  20-frame soak all pass; inter-command timing unchanged (Guard-1
  contract, §3-H-E).

---

## Appendix — key evidence index

| Fact | Source |
|---|---|
| Crop reply ok, uptime 40466032 | telemetry.log:2529910 |
| First RX-armed failure 178 ms later | telemetry.log:2529912 |
| png died at set_sensor, never reached bulk | telemetry.log:2531546-2531596 |
| Last flood line 21:37:44; ≈24,300 flood lines | telemetry.log:2582211; count over 2529912-2582215 |
| No INIT@/relink during the wedge | grep over the window (empty) |
| S3 LOW gate, 50 ms, fail-fast | `sscma_client_io_spi.c:813-824` |
| Process task loops on failure, no accounting | `sscma_client_ops.c:207-230` |
| Drain exists, bulk-only callers | `sscma_client_io_spi.c:781-811`, callers `:1344`, `:1419` |
| Recovery primitive (RST + optional rail) | `eyes.c:650-695`; RST `sscma_client_ops.c:831-870` |
| Rail = P13 (bitmask 1<<11), RST = P07 (1<<7) | `eyes.c:62`, `eyes.c:58`, config at `eyes.c:545` |
| `link_up_` cleared only by recovery itself | `eyes.c:108-110`, `:618-624`, `:1982-1990` |
| Invoke streak is snap-path only | `eyes.c:156-159`, `:735-749` |
| `dbg eyes_reset` is serial-only | `link.c:~2253-2280` |
| `link_mon` watches the host UART, not Himax | `link.c:986ff` |
| Himax guards all live in spi_poll (AT loop) | `ma_server_at.cpp:113`; `ma_transport_spi.cpp:1319-1517` |
| Silent arm failure when `!_spi_opened` | `ma_transport_spi.cpp:504-507` |
| Teardowns ignore `spi_open` failure downstream | `ma_transport_spi.cpp:1505-1515`, `:1345-1355` |
| PARK has no S3 caller (ruled out) | grep over `firmware/components` |

*Uncertainty statement:* the S3-side half of this RCA (deadlock
shape, missing triggers, flood) is established from code + log.
The Himax-side entry point (how the transport reached an uncovered
state, or whether its loop stalled) is **inference ranked by
evidence**, because the Himax's DIAG stream goes to its own UART
and was not captured during the incident. §4.2/§4.3 are designed to
convert that inference into measurement before — or at latest
while — the fixes land. The S3 ladder (§4 step 3) is deliberately
hypothesis-independent: it recovers the link for every Himax-side
cause on the table, including ones not yet imagined.

---

## 5. Addendum — a deliberate power-off mechanism (owner scope addition, 2026-11-09)

### 5.0 Why this is in this report, and the correction it rests on

Per the owner's correction (§0): **the wheel button's push-and-hold
is PTT. There is no hold-to-power-off gesture on this device**, and
the older schematic-derived note "3 s hold wakes it" does not
describe shipped behaviour. Two consequences:

1. The device currently has **no deliberate off at all**. The only
   way the incident in §1 ended was the owner cutting power
   externally. A wedged-peripheral world (§2) makes this worse: the
   escalation ladder deliberately never reboots the S3, so a local,
   deliberate, *firmware-mediated* off is the missing complement —
   the human's rung, below "pull the plug" and above nothing.
2. Any gesture design starts from a hard constraint: **the hold
   gesture is taken**. PTT claims the button at >500 ms
   (`firmware/components/ptt/ptt.c:1`, `PTT_HOLD_MS 500` at
   `ptt.c:59`), the input component suppresses its short-press
   action while PTT owns the button (`ptt.c:8-10`), and long holds
   are *normal operation* — a dictation take lasts exactly as long
   as the user holds. A naive "hold N seconds = off" would
   eventually fire mid-sentence on a long take. That collision
   shapes every option below.

### 5.1 The actuator, and what firmware can and cannot do today

- The whole-system rail is switched by the PCA9535 expander's
  SYSTEM output: in the firmware's linear bit numbering that is
  expander bit 10 (PCA9535 port-1 pin 2, "P12"), driven **high** in
  the boot power-up sequence (`firmware/components/eyes/eyes.c:461`,
  comment `/* SYSTEM */`, inside the BSP sequence at
  `eyes.c:452-471`). Per the schematic notes in MEMORY.md, this pin
  drives Q10's gate at the 3.3 V buck; low cuts the whole system
  rail — S3, Himax, display, everything.
- **No power-off path exists in firmware today.** Grep over
  `firmware/components` + `main` finds no code that drives bit 10
  (or the 0xDF00 rail mask) low after the boot sequence, and no
  `power_off`/`EXP_POWER` symbol at all. This is a greenfield
  feature, not a wiring fix — one `esp_io_expander_set_level(
  expander, 1u << 10, 0)` at the end of a careful shutdown sequence,
  but the sequence is the feature.
- Once the rail drops, firmware is dead by definition: nothing on
  the device can sense a button, run a timer, or change its mind.
  Two hardware questions firmware cannot answer, flagged for
  schematic/bench verification before any build: (a) does the
  expander's output state hold Q10 off as the rail collapses (the
  expander's own supply domain matters here), and (b) what exactly
  re-raises SYSTEM on USB plug-in — presumably the USB/charger
  power path, but that is asserted by the design brief, not
  verified from anything in this tree.

### 5.2 Pre-off drain: what must be persisted or finished first

A hard cut at the wrong moment is indistinguishable from the crash
reboots this project already fights. The shutdown sequence, in
order, all *before* touching bit 10:

1. **Stop admitting new work.** Reject new commands/takes with a
   `powering_off` reply state; stop the wake-word/PTT claim path so
   a button press during the countdown can't start a take that the
   cut will amputate.
2. **Finish or cleanly abort in-flight work — never cut mid-flight.**
   - A PTT take that is recording or shipping: finish shipping it
     if it completes within a short bound (~5 s); otherwise abort
     it *by the take protocol's own rules* (end the take, ship what
     the protocol defines as final, or discard it whole). Audio
     shipping has no flow control; a mid-take cut loses the take
     silently and can leave the host waiting on chunks that will
     never come. The host must receive an explicit take-abort or
     take-end, not silence.
   - A bulk capture (PNG/snaptest streaming): same rule — complete
     it or send the protocol's abort; do not strand the host's
     reassembler.
   - A flash in progress: **refuse/defer power-off outright.** This
     is the one hard veto (see 5.4).
3. **Force-persist the seal replay high-water.** Normal operation
   persists `cmd_hw` only every 32 accepted commands
   (`components/seal/seal_dev.c:19-21`, `SEAL_HW_PERSIST_EVERY`;
   persist path `:1068ff`, boot floor = last persisted + 32 at
   `:965`). A cut without a final persist leaves the boot floor up
   to 32 behind the true counter — precisely the crash-reboot
   undershoot that has already forced manual seal resyncs on this
   project. Graceful off must force-persist the current high-water
   through the seal component's NVS task and *wait for its commit*
   (`nvs_commit`, `seal_dev.c:393`) before proceeding. (The design
   brief names this `seal_nvs_drain()`; no function of that exact
   name exists in the tree today — it is a small new addition to
   `seal_dev.c`: force persist + drain the NVS task queue + confirm
   commit.)
4. **Flush the host link.** Send the final `{"t":"power"}` reply /
   "going off" telemetry event and wait for the transport to
   actually drain (UART TX idle on the tethered path; publish
   completion on the MQTT path) — the reply that never leaves the
   buffer is worse than none, because the host then can't
   distinguish "off" from "wedged" (§1's whole problem).
5. **Close files.** If/when the SD capture audit log lands (designed,
   not built), close the open capture file cleanly — the audit
   log's hard rule is stop-don't-corrupt, and a torn final file is
   corruption by another name. Today: no SD writer exists, so this
   step is a placeholder with a named owner (the SD feature).
6. **Say so on-device.** Display "Powering off — plug in USB to
   wake" and a distinct LED pattern (the LED vocabulary has no OFF
   state today; `led.h` states are status patterns, and OFF/dark is
   already IDLE's look per the 0.4.40 no-LED-for-listening ruling —
   so the *transient* shutdown screen + a short farewell pattern is
   the signal, not a persistent LED state, which is impossible
   anyway once the rail drops). Then a short settle delay (~250 ms)
   so the user sees it, then drive bit 10 low.

Ordering note: steps 2–4 have failure modes; if any drain step
times out, the design question is abort-off vs proceed-anyway.
Recommendation: **abort the power-off and report why**, except when
the trigger was the physical gesture held through a second explicit
confirm (5.3-B), where proceed-after-timeout is acceptable — the
human is standing there watching it, which is the consent that
covers the lost take.

### 5.3 Options

**A — Sealed remote power-off command.**
A new sealed command (e.g. `{"t":"power","cmd":"off"}`) on the
existing command path, running the §5.2 sequence.
- *Scope/authorization:* under the device's control model (sealed
  commands only; scope enforced at verify time), power-off belongs
  in the highest routine scope tier, alongside `dbg at` and flash —
  never in a least-privilege HID/cursor key's scope. For MQTT
  triggers, the agent-side rule also applies: this is a dangerous
  device command, so it takes the owner's explicit go per use, like
  flashing.
- *Accidental-trigger safety:* strong by construction — a signed
  command plus an explicit confirm step (two-message handshake:
  `off` → device replies `confirm_required` with a nonce → sender
  echoes the nonce → device executes). No physical fumble can
  produce it.
- *Limits:* needs a live command path. In the §1 wedge the S3 was
  alive and MQTT-reachable, so A would have worked — but in a
  host-link-down scenario A is unreachable exactly when a local off
  is most wanted. A alone is insufficient.
- *Cost:* smallest of all options (one command + the §5.2
  sequence). 

**B — Physical: extended hold → on-device confirmation UI → second
press to confirm. (Recommended physical path.)**
- Hold the wheel button. At 500 ms PTT claims it as today — if the
  user is dictating, nothing changes. If the hold continues to a
  much longer threshold (recommend **5 s**: 10× the PTT threshold,
  far beyond any accidental press, reachable only deliberately),
  and the device is in a state where off is permitted (5.4), PTT's
  take is **ended and discarded** (announced on the display:
  "Take discarded — keep holding to power off" happened at the
  threshold crossing; the discard is the price of reusing the hold,
  and it is recoverable harm — a lost take, not a lost device) and
  the display switches to a confirmation screen: "Release, then
  press again within 5 s to power off. Rotate or wait to cancel."
- The confirm is a **fresh short press** inside the window —
  a different motor act from the hold, so no single continuous
  fumble (a stuck button, an object resting on the wheel) can
  complete it: a stuck button never produces release-then-press.
  Timeout or any wheel rotation cancels and returns to idle.
- *Accidental-trigger safety:* two distinct gestures + a visible
  countdown + cancel paths + a 30 s lockout after any cancelled
  prompt (so PTT-heavy use never sees prompt spam). The residual
  accident is "user holds 5 s through the PTT threshold by intent
  to dictate a very long take, then reflex-presses" — mitigated by
  the take-discard announcement appearing *before* the prompt and
  the press needing to be fresh.
- *Cost:* moderate — input/ptt coordination (PTT must expose
  "abort take, reason: power-prompt"), a display prompt state, and
  the idle/busy gating. All in components that already exist
  (`input.c`, `ptt.c`, `display.c`).

**C — Physical: press pattern (e.g. triple-press).**
Three short presses inside ~1.5 s raise the same confirmation
screen as B.
- *Fatal friction:* the wheel's short press is not free — it fires
  the mode's short-press action on release (left click / Enter /
  play-pause per mode, `components/input/input.c:20`, `:135ff`),
  and it fires *immediately* by design (no disambiguation delay,
  the same immediacy the touch path documents at `input.c:71-72`).
  A triple-press therefore emits three real HID clicks at the host
  before the pattern is recognized — unacceptable side effects for
  a power gesture. Suppressing them requires delaying every
  short-press by the pattern window (~400 ms), taxing the primary
  interaction to serve a rare one. **Not recommended** as the
  primary; viable only if the pattern uses a channel with no host
  side effect (e.g. press+rotate detents — but push+rotate already
  means mode-cycle, `input.c:186`).

**D — Hold-while-idle-only semantics (no prompt).**
Hold ≥5 s powers off directly, but the hold only *counts* if the
device was idle at press start and no take is active.
- Simpler than B, but "idle at press start" doesn't save it: PTT
  *starts* from idle — every dictation begins in the exact state D
  arms in. A 5-s-or-longer take is a normal take. D still eats long
  takes, just without B's warning and second chance. Strictly
  dominated by B. **Not recommended** except as B's degenerate
  fallback if a display prompt proves impossible (it doesn't — the
  display exists and already hosts message overlays,
  `input.c:143`).

**E — Deep sleep instead of rail cut.**
`esp_deep_sleep` with a wake source instead of driving bit 10 low.
- *For:* firmware survives (state retention in RTC RAM), wake can
  be a device-side event rather than only USB plug-in, no
  expander-latch hardware questions (5.1).
- *Against:* it is not *off* — parasitic draw continues, the rail
  stays up, and a wedged peripheral stays powered (arguably
  re-creating §1's problem in miniature). Wake-source plumbing is
  awkward: the wheel button is on the I2C expander (pin 3), not an
  RTC-capable GPIO, so button wake needs the expander's interrupt
  line routed appropriately — unverified in this tree, and no
  deep-sleep code exists in the firmware today (grep: none). It
  also doesn't satisfy "deliberate power-off" as the owner framed
  it (the P12/Q10 mechanism was specified). Recorded as an option
  because it may suit a future *sleep* feature; it is not a
  substitute for off. **Not recommended for this requirement.**

### 5.4 Behaviour while busy (all triggers, same rules)

| Device state at trigger | Behaviour |
|---|---|
| Idle / wake-word listening | Proceed (§5.2 sequence). |
| PTT take recording | Remote (A): reply `busy`, offer defer — execute when the take ends + ships, or on an explicit second confirm that discards the take. Physical (B): the hold *is* the take; the 5 s crossing ends+discards it as specified. Never cut mid-take silently. |
| Take shipping / bulk capture streaming | Defer until the transfer completes or aborts by protocol (bounded ~5 s), then proceed. If it doesn't settle, abort-off and report (A) / proceed only after the second confirm (B). |
| Flash in progress (S3 or Himax) | **Hard refuse**, both triggers, no override short of pulling power externally. A cut mid-flash is the one unrecoverable outcome on the table. |
| Link wedged (Himax unresponsive) | Proceed — Himax state is already lost; skip drain steps 2 (Himax parts) and don't let a dead peripheral veto the owner's off. (This is §2's ladder run in reverse: the peripheral never gets a vote over the device.) |
| Seal NVS task backed up / persist failing | Proceed after timeout **with a warning logged + telemetered**: the consequence is bounded and known (next boot's replay floor undershoots; resync may be needed — the project has that runbook), whereas refusing off because housekeeping failed would trap the user. Log it loudly. |

### 5.5 Wake path after off

- **USB plug-in** is the wake path (per the design brief): plugging
  in restores the power path, SYSTEM re-raises, and the device
  cold-boots through the normal sequence (boot power-up at
  `eyes.c:452-471`, handshake, seal floor from the persisted
  `cmd_hw` — correct *because* §5.2 step 3 persisted it).
- There is **no button wake from hard-off** and the design must not
  pretend otherwise: with the rail cut, the button is a dead switch
  on an unpowered expander. The owner's correction retires the
  "3 s hold wakes it" note; documentation (README/diagram) should
  say "Off = off. Plug in USB to wake." If a future hardware
  revision routes a wake-capable line, that's a hardware change,
  not a firmware option.
- Boot-after-off is distinguishable from boot-after-crash: §5.2
  step 3/4 can leave a "clean shutdown" marker in NVS, letting the
  boot log (and the agent) tell a deliberate off from a crash —
  worth having for exactly the REPLAY/floor forensics this project
  keeps doing.

### 5.6 Recommendation

Build **A + B**, sharing one shutdown sequence (§5.2):

1. **A first** (sealed command): smallest surface, covers the
   common case (bench work ends → off from the console/host), and
   it forces the §5.2 drain sequence to exist and be tested —
   including the seal force-persist, which also pays down the
   crash-reboot resync debt every time it's used.
2. **B second** (hold-5 s → prompt → fresh press): the local,
   no-connectivity path, designed *around* PTT rather than against
   it — PTT keeps the hold; power only borrows it past a threshold
   no dictation can reach *unannounced*, with the take-discard
   announced before the prompt and a two-gesture confirm no stuck
   button can fake.
3. Explicitly **not** C (HID side effects), **not** D (dominated by
   B), **not** E (sleep ≠ off; may return later as its own
   feature).
4. Before building either: verify the two hardware unknowns in
   §5.1 (expander latch behaviour at rail collapse; what re-raises
   SYSTEM on USB plug-in) on the bench/schematic — a power-off
   that sometimes latches back on, or can't be woken, is worse
   than none. The first bench test of A is done with the current
   collector/telemetry watching: command → final reply received →
   device dark → USB plug-in → cold boot with the clean-shutdown
   marker and a seal floor that needs no resync.

*Decision left to the owner, per the brief: A+B as recommended, or
A alone if the physical gesture isn't wanted. The §5.2 drain
sequence is required under every option and should be reviewed as
the core of the design.*

---

## Addendum — 2026-11-09 22:39 UTC: wedge reproduced, trigger named

Appended 2026-11-09 ~23:05 UTC by the background research pass, from
the live telemetry log; the body of this RCA is unchanged.

The dim-screen retry reproduced the wedge, on the same command and
the same fuse as the 21:16 incident (§1):

- `dbg crop 0,0,2592,486` set returned `ok:true` at 22:39:18 UTC —
  Himax reply `{"name":"CROP","code":0,...}` at device uptime
  460903 ms (telemetry.log:2584866-67).
- First failure 179 ms later: `MISO-flag timeout waiting for
  RX-armed LOW` + `spi_send_cmd: Himax not RX-armed (cmd=0x03)` at
  uptime 461082 ms (telemetry.log:2584868-69). The 21:16 incident's
  onset was 178 ms after its crop set. Same ~51 ms flood cadence.
- `dbg png` then died at mode-select (ESP_ERR_TIMEOUT); the
  crop-clear also timed out. As of 23:04 UTC the flood was still
  running (~29.5k error lines since onset) and the device was still
  heartbeating on fw 0.4.49.1 with the link down — recovery remains
  an S3 reboot, which had not yet happened.

**Tally for 2026-11-09** (set_crop log lines, code 0 = reached a
live Himax): 20:23:47 clean, 20:28:15 clean, 21:16:51 WEDGED
(+178 ms), 22:33:52 clean, 22:39:18 WEDGED (+179 ms). Five sets,
two wedges, both onsets inside ~180 ms of the crop reply.

**What this changes:**

1. AT+CROP is the first concrete trigger ever caught for the latent
   hole this RCA describes — an intermittent race on the crop-set
   response path, not a deterministic failure (3 of 5 sets were
   clean, including one 6 minutes before the second wedge).
2. The regression doc's appendix counts are superseded (whole-log
   `AT+CROP` attempts and `not RX-armed` totals; see its addendum).
3. The §4 escalation ladder is unchanged and still the fix
   direction; the trigger evidence raises its priority — the wedge
   is now reproducible on demand at roughly a 2-in-5 rate per crop
   set, which also gives the ladder a practical acceptance test
   (scripted crop sets until a wedge onset, then ladder recovery
   with no S3 reboot).
4. The pending dim-screen strip capture is blocked on the same
   bounce: crop remains set on the Himax and clears on its reset.

---

## Addendum 2 — 2026-11-09 ~23:50 UTC: H1 vs H2 under the CROP-trigger evidence

Appended by the link-hardening spec pass
(`docs/link-hardening-spec-2026-11-09.md`). The 22:39 addendum above
records the second wedge's evidence; this addendum adds only the
analysis that evidence now supports. No body text is changed.

### A2.1 The wedging exchanges completed end-to-end — the state forms at the final re-arm

In both incidents the crop exchange that preceded the wedge was
*fully successful on the wire*, and that fact localises the stuck
state far more tightly than "after the crop set":

- The S3's AT+CROP write passed the LOW gate (RX was armed), the
  Himax executed the command, the reply was staged, the S3's
  AVAILABLE poll passed the gate and clocked out the count, and
  the READ passed the gate and clocked out the reply bytes — the
  `set_crop` log lines show the reply received with `code 0` and
  the new geometry echoed.
- Each of those clock-outs sets `_spi_tx_done` on the Himax and
  requires a Guard-1 re-arm before the next S3 command can pass
  the LOW gate. At least one complete Guard-1 re-arm cycle
  therefore succeeded *inside* each wedging exchange (after the
  AVAILABLE count clock-out). The re-arm that never took effect
  is the **last one of the exchange** — after the final clock-out
  of the CROP response.
- The first failed poll follows ≤179 ms later. The idle gaps
  before each incident (45 min in the first, ~3.5 min in the
  second) are thereby excluded as the formation window: the link
  was provably alive at the start of the wedging exchange and
  dead ≤179 ms after its end. The stuck state forms **during or
  immediately after the response clock-out**, on the re-arm path.

### A2.2 Both wedges followed the full path through setCropWindow

`setCropWindow` has two paths (verified in the tree,
`ma_camera_himax.cpp:400-497`):

- **Fast path** (`:425-435`): `_crop_w != 0` and the new window has
  the same byte count → `drv_ov5647_recrop_fullres` only; no
  deInit, no sensor re-init.
- **Full path** (`:438-488`): `deInit()`, the persistent-buffer
  step (the 0.4.20.61 fix, `:445-487`), `drv_ov5647_set_raw_buffer`,
  `drv_ov5647_init_raw_fullres`.

`clearCropWindow()` zeroes `_crop_w` (`:499-509`), so any set
after a clear — or after a Himax reboot — takes the full path.
The day's five code-0 sets, by path:

| Set (UTC) | Path | Outcome |
|---|---|---|
| 20:23:47 | full (first set ever) | clean |
| 20:28:15 | fast (same byte count, crop already set) | clean |
| 21:16:51 | full (cleared 20:30:28) | **WEDGED, +178 ms** |
| 22:33:52 | full (first set after Himax reboot) | clean |
| 22:39:18 | full (cleared ~22:35) | **WEDGED, +179 ms** |

Full path: 2 wedges in 4 executions. Fast path: 0 in 1. In this
sample the full path — the one running the heavy sensor re-init
immediately before the reply — looks **necessary but not
sufficient**: a race, needing a second, timing-dependent
ingredient. (n is small; treat the path correlation as a strong
lead, not a proof.)

### A2.3 What this does to H1 vs H2

- **H1 (transport bookkeeping hole) — weight increases.** The
  failure now sits exactly on the path H1 describes: the Guard-1
  re-arm after a completed TX, with Guard 3's teardown and its
  failed-`spi_open` end-state (`ma_transport_spi.cpp:1336-1355`,
  plus the unconditional `_spi_rx_arm_failed` clear at `:1343`)
  as the escalation route if that re-arm's `spi_read_dma` failed.
  The whole chain fits inside the 179 ms window: Guard 3 counts
  one failure per spi_poll retry, and its teardown + failed open
  + silent `_spi_arm_rx()` no-op (`:504-507`) can complete in
  milliseconds. H-C's variant (the arm call returns E_OK but the
  PB2 mux/drive does not take, leaving no flag set anywhere)
  sits on the same spot and is indistinguishable from H1's
  end-state from outside — H-A/H-C together close both.
- **H2 (AT/executor thread stall) — weight decreases, not
  closed.** The thread had *just* completed a full
  multi-transaction exchange, including the heavy `setCropWindow`
  call itself, which returned before the reply was composed
  (§1.4). An H2 stall must therefore begin in the ≤179 ms
  post-reply window, on code that runs after every AT reply —
  code that also ran cleanly after the 20:23, 20:28 and 22:33
  crop replies and after every other AT exchange of the day.
  No blocking call on that post-reply path has been identified.
  H2's remaining plausible locus is narrower than §1.4 stated:
  something the *full-path* sensor re-init starts or leaves
  behind that the AT thread touches only after the reply. Only
  the H-D poll-gap counter or a UART capture of a recurrence
  closes H2; both remain in the plan.
- **Net:** the S3-first ordering in §4 is unchanged (the ladder
  is hypothesis-independent), but the Himax work is no longer
  "reduce incidence" in the abstract — H-A/H-C address the
  specific re-arm step both incidents died on, and they are
  co-equal in evidentiary support with the ladder.

### A2.4 The acceptance soak is a targeted regression test, not a generic one

Because the trigger is now a specific command on a specific
path, the soak in the hardening spec is designed against it:
every cycle's crop set is preceded by a crop **clear**, forcing
the full path (`deInit` + `init_raw_fullres`) every time. This
matters because the two soak geometries have identical byte
counts (2592×486 = 1296×972 = 1,259,712), so *without* the
clear, alternate sets would silently take the fast path and the
soak would stop testing the trigger while appearing to run it.
At the observed full-path rate (2/4), a 20-cycle soak expects
multiple onsets on pre-hardening firmware; even at a true rate
of 10%, the chance of no onset in 20 full-path sets is ~12% —
a clean run is evidence, and an onset the ladder recovers
in-session is the fix demonstrating itself.
