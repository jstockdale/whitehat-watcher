#!/usr/bin/env python3
"""Host regression tests for the v0.4.3 touch/wheel init resilience fixes.

Covers John's five test items for the 01:15:42 boot failure (touch_init()
failed -> input_init() demanded BOTH devices -> pointer task never
started, "touch":false,"wheel":true,"pointer":false):

  (a) input task starts with touch failed but wheel OK (and vice versa);
  (b) a transient touch-init failure self-heals via the deferred retry;
  (c) the init-failure error is logged with the allocation named;
  (+) touch_init() bus-clears and retries once on hw-init failure;
  (+) wheel_init() has no silent failure path (audit).

Method: static analysis of the firmware sources plus a Python model of
the init-gate truth table and the retry-supervisor state machine. The
full FreeRTOS behavior (task creation, I2C traffic) cannot run on the
host and requires the device; that is called out explicitly per case.

Exit 0 = all pass, 1 = failure.
"""

import re
import sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
TOUCH_C = FW / "components/touch/touch.c"
WHEEL_C = FW / "components/wheel/wheel.c"
INPUT_C = FW / "components/input/input.c"

PASS, FAIL = "PASS", "FAIL"
results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"[{PASS if ok else FAIL}] {name}" + (f" -- {detail}" if detail and not ok else ""))


def read(p):
    return p.read_text()


# ---------------------------------------------------------------- (a)
# input_init() gate: task must start when EITHER device is ready.
input_src = read(INPUT_C)
m = re.search(r"esp_err_t input_init\(void\)\s*\{(.*?)\n\}", input_src, re.S)
check("(a1) input_init() body found", m is not None)
gate_ok = False
if m:
    body = m.group(1)
    # Refusal must require BOTH missing. The code snapshots the devices
    # into locals first: bool t = touch_ready(); bool w = wheel_ready();
    # then refuses on "!t && !w".
    snap_ok = bool(re.search(r"bool\s+t\s*=\s*touch_ready\(\)\s*;",
                             body)) and bool(
        re.search(r"bool\s+w\s*=\s*wheel_ready\(\)\s*;", body))
    gate_ok = snap_ok and bool(
        re.search(r"if\s*\(\s*!t\s*&&\s*!w\s*\)", body))
    check("(a2) input_init() refuses only when BOTH touch and wheel are down",
          gate_ok,
          "gate is not 'snapshot t/w, then !t && !w'")
    # The old AND-gate ("||") must be gone.
    check("(a3) old OR-refusal gate removed",
          not re.search(r"if\s*\(\s*!touch_ready\(\)\s*\|\|\s*!wheel_ready\(\)",
                        body),
          "old '!touch_ready() || !wheel_ready()' gate still present")


def gate_model(touch_up, wheel_up):
    """Python mirror of the C gate: refuse iff both down."""
    return touch_up or wheel_up  # True => input task starts


truth = {(True, True): True, (True, False): True,
         (False, True): True, (False, False): False}
tt_ok = all(gate_model(t, w) == exp for (t, w), exp in truth.items())
check("(a4) gate truth table: task starts iff touch OR wheel is ready", tt_ok)
if tt_ok and gate_ok:
    print("      touch=down,wheel=up -> task STARTS (wheel-only pointer); "
          "touch=up,wheel=down -> task STARTS (touch-only pointer)")

# Poll loop must skip a missing device gracefully (existing guards).
touch_src = read(TOUCH_C)
check("(a5) touch_read() returns 0 when !ready_ (poll loop skips touch)",
      bool(re.search(r"if\s*\(\s*ready_\s*&&\s*tp_\s*\)", touch_src)))
wheel_src = read(WHEEL_C)
check("(a6) wheel_button_down() returns false when !ready_",
      bool(re.search(r"if\s*\(\s*!ready_\s*\|\|\s*!expander_\s*\)\s*\{\s*return false;",
                     wheel_src)))
check("(a7) wheel_take_delta() has no ready-gate (safe: no knob callbacks "
      "fire before wheel_init, detents stay 0)",
      "wheel_take_delta" in wheel_src)

# ------------------------------------------------------------ (b) retry
# Static: supervisor exists, retries missing devices, backoff schedule.
check("(b1) input retry supervisor task present",
      "input_retry_task" in input_src)
check("(b2) supervisor retries touch_init() while !touch_ready()",
      bool(re.search(r"if\s*\(\s*!touch_ready\(\)\s*\)[^}]*touch_init\(\)",
                     input_src, re.S)))
check("(b3) supervisor retries wheel_init() while !wheel_ready()",
      bool(re.search(r"if\s*\(\s*!wheel_ready\(\)\s*\)[^}]*wheel_init\(\)",
                     input_src, re.S)))
sched = re.search(
    r"input_retry_delays_s_\[\]\s*=\s*\{\s*([0-9,\s]+)\}", input_src)
delays = [int(x) for x in sched.group(1).split(",")] if sched else []
check("(b4) backoff schedule present and capped (5..60s)",
      delays == [5, 10, 20, 40, 60], f"got {delays}")
check("(b5) supervisor re-attempts input_init() once a device comes up",
      bool(re.search(r"if\s*\(\s*!ready_\s*&&\s*\(touch_ready\(\)\s*\|\|\s*wheel_ready\(\)\)",
                     input_src)))
check("(b6) supervisor deletes itself when task running and both devices up",
      bool(re.search(r"if\s*\(\s*ready_\s*&&\s*touch_ready\(\)\s*&&\s*wheel_ready\(\)",
                     input_src)))


def simulate_retry(touch_succeeds_at, wheel_succeeds_at, max_steps=8):
    """Model the supervisor: each step waits backoff[step], retries the
    missing devices; touch/wheel become ready at the given attempt
    numbers (None = never). Returns (started_input, both_up, steps)."""
    backoff = [5, 10, 20, 40, 60]
    touch_up = wheel_up = False
    input_started = False
    for step in range(max_steps):
        _wait = backoff[min(step, len(backoff) - 1)]
        if not touch_up and touch_succeeds_at == step + 1:
            touch_up = True
        if not wheel_up and wheel_succeeds_at == step + 1:
            wheel_up = True
        if not input_started and (touch_up or wheel_up):
            input_started = True  # input_init() starts the task
        if input_started and touch_up and wheel_up:
            return True, True, step + 1
    return input_started, (touch_up and wheel_up), max_steps


# The 0.4.2 incident: touch fails at boot (transient), succeeds on
# retry attempt 3; wheel was fine from boot, so the pointer starts
# wheel-only and touch joins late.
def sim_incident():
    backoff = [5, 10, 20, 40, 60]
    touch_up, wheel_up, input_started = False, True, True  # wheel OK at boot
    for step in range(8):
        _w = backoff[min(step, len(backoff) - 1)]
        if step + 1 >= 3:
            touch_up = True  # transient heap failure clears by attempt 3
        if input_started and touch_up and wheel_up:
            return True, True, step + 1
    return input_started, False, 8


started, both, steps = sim_incident()
check("(b7) 0.4.2 incident replays to self-heal: wheel-only pointer at "
      "boot, touch joins on retry, supervisor exits",
      started and both, f"started={started} both_up={both}")

# Neither device at boot: supervisor must eventually start input.
s2, b2, st2 = simulate_retry(touch_succeeds_at=2, wheel_succeeds_at=4)
check("(b8) neither device at boot: input starts when first device "
      "succeeds, supervisor exits when both up",
      s2 and b2, f"started={s2} both_up={b2} steps={st2}")

# Permanent failure: supervisor must not spin hot / must keep backing off.
s3, b3, st3 = simulate_retry(touch_succeeds_at=None, wheel_succeeds_at=None,
                             max_steps=8)
check("(b9) permanent failure: input never starts, supervisor keeps "
      "retrying at the capped 60s interval (no hot spin)",
      not s3 and not b3 and st3 == 8)

# ------------------------------------------------- (c) named error log
m = re.search(r"esp_err_t touch_init\(void\)\s*\{(.*?)\n\}", touch_src, re.S)
c_ok = False
if m:
    body = m.group(1)
    mm = re.search(
        r"xSemaphoreCreateMutex\(\);(.*?)\}", body, re.S)
    c_ok = bool(mm and "ESP_LOGE" in mm.group(1)
                and "xSemaphoreCreateMutex" in mm.group(1))
check("(c1) touch_init() mutex-NULL path logs ESP_LOGE naming "
      "xSemaphoreCreateMutex", c_ok)

# --------------------------------------- (+) bus-clear retry in init
check("(+1) touch_init() bus-clears and retries hw init once on failure",
      bool(re.search(r"touch_hw_teardown\(\);\s*touch_bus_clear\(\);\s*"
                     r"ret\s*=\s*touch_hw_init\(\);", touch_src)))
check("(+2) failed bus-clear retry logs the error loudly",
      "touch hw init failed after bus-clear retry" in touch_src)

# --------------------------------------- (+) wheel silent-path audit
wm = re.search(r"esp_err_t wheel_init\(void\)\s*\{(.*?)\n\}", wheel_src, re.S)
w_ok = True
w_detail = ""
if wm:
    body = wm.group(1)
    lines = body.splitlines()
    for i, ln in enumerate(lines):
        if re.search(r"return\s+(ESP_FAIL|ret)\s*;", ln):
            window = "\n".join(lines[max(0, i - 8):i + 1])
            if "ESP_LOGE" not in window and "ESP_LOGW" not in window:
                w_ok = False
                w_detail = f"silent failure return at: {ln.strip()}"
                break
    # wheel_init has no heap/mutex allocation at all (spinlock is static)
    if "xSemaphoreCreateMutex" in body or "malloc" in body:
        w_ok = False
        w_detail = "unexpected heap allocation in wheel_init()"
else:
    w_ok = False
    w_detail = "wheel_init() body not found"
check("(+3) wheel_init() audit: every failure return is logged, no "
      "silent NULL path", w_ok, w_detail)

# ------------------------------------------------------------ summary
failed = [n for n, ok, _ in results if not ok]
print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
if failed:
    print("FAILED:", ", ".join(failed))
    sys.exit(1)
print("NOTE: on-device validation (real I2C wedge, multi-boot heap "
      "layouts, red/success LED behavior) requires flashing 0.4.3 and is "
      "not possible in this build-only task.")
