# Watcher Firmware v0.4.9 — Bench Test Report

**Test date:** 2026-09-27
**Firmware:** 0.4.9 (`watcher input PoC 0.4.9 up`)
**Console:** 0.4.96 (Muse stream visibility + SEAL popover)
**Status:** **10/10 PASS** — stable, sealed, validated on hardware

## Why this test

Firmware 0.4.9 fixed the v0.4.8 `xTaskGetHandle` boot panic (20-char
`sscma_client` task names renamed to fit FreeRTOS's 15-char limit) and
flashed cleanly earlier today. The device has been stable since —
heartbeats every 60s, zero resets, zero panics. This report is the
first full bench validation of 0.4.9 on hardware: seal, camera, HID,
memory, watchdog, BLE, and version, run end-to-end through John's
console in bridged mode.

## Test matrix

| # | System | Status | Evidence |
|---|--------|--------|----------|
| 1 | Seal: sealed ping → pong | PASS | `{"t":"pong","_mid":"bfa851cf"}` |
| 2 | Seal: unsealed ping → dropped | PASS | `SEAL: UNSEALED msg on 'watcher/command' - DROPPED` + `seal_event` rejected `UNSEALED` |
| 3 | Camera | PASS | 3 snaps: 25,956 B (336 ms), 19,706 B (330 ms), 19,747 B (320 ms); SOI+EOI markers present, no "rx buffer is full" |
| 4 | Mouse | PASS | Moves executed, pattern drawn, safe clicks — visible on John's console page |
| 5 | Text | PASS | Typed into safe field, no password fields touched, nothing submitted |
| 6 | Scroll | PASS | Up/down scroll verified |
| 7 | Seal status | PASS | `seal_floor:519`, `seal_dev_verify:true`, `mqtt_mode:bridged`, `seal_armed:false` |
| 8 | Memory | PASS | `internal_free:25566` rock-solid across the session; `heap_free:7484286` stable, no leaks after HID activity |
| 9 | Watchdog | PASS | Zero resets/panics; uptime 8,414 s and climbing at last heartbeat |
| 10 | BLE + version | PASS | MTU 256 negotiated, `hid_ble` active; `fw:"0.4.9"` on every telemetry frame |

## Changes

### 1. Unsealed-command incident → unified sender (security fix)

**What happened:** a test agent used `send_cmd.py` (which published
**unsealed** JSON to `watcher/command`) instead of `send_command.py`
(which seals). 8 unsealed commands were sent. The strict-mode seal
gate correctly **dropped all 8** — the device never saw them — but the
tooling hole had to close.

**Fix (`host-agent/mqtt-bridge/`):**
- `send_command.py` is now the single canonical sender: argparse with
  mutually exclusive `--seal` / `--no-seal` (sealed is default);
  `--no-seal` prints a loud stderr warning and is documented as
  rejection-testing-only. Sent lines tag `[SEALED]` / `[UNSEALED]`.
- `send_cmd.py` rewritten as a thin wrapper that execs
  `send_command.py` (sealed by default) with a deprecation warning.
- `run_calibration.py`: both publish sites now seal via
  `seal_client.seal_command()` (with `--no-seal` escape hatch).
- `TESTING_PROTOCOL.md` (new): the one rule — all control commands
  sealed, always — plus the sweep protocol (§4a sealed ping must
  succeed, §4b unsealed ping must be dropped), expected telemetry
  patterns, and a 7-item checklist for future test agents.
- **Audit:** `report_fiducials.py`, `send_flash.py`/`transfer-flash.py`,
  `subscriber.py` already sealed or verifier-side. `keepalive.py`
  retains an unsealed legacy device-command path — documented as a
  known exception; decision pending (seal it or remove it).

**Impact:** today's sweep proved both sides — test 1 (sealed) accepted,
test 2 (un-sealed) rejected with `seal_event`. Strict mode works as
designed.

### 2. Console: Muse command stream (0.4.94 → 0.4.95)

In bridged mode all Muse commands go through the opaque `sealed_fwd`
path, which never fed the Muse tab's history — the entire stream was
invisible. **0.4.94** added `_sealPeekCmd()` (full inner cmd from a
WMS2 kind-2 envelope, no verification) and logs
`MUSE → <cmd> (bridged|native|page)` with the device reply as
expandable context. **0.4.95** fixed specificity: `_museCmdLabel()`
shows `dbg seal_status`, not just `dbg` — equal detail to local
commands. Tests: layout 151/151, seal 36/36, fail-closed E2E pass.

### 3. Console: SEAL STRICT popover (0.4.96)

Clicking the SEAL STRICT status line now opens a popover (not a modal)
with live seal state: mode, floor, armed, dev_verify, recent
accept/reject events, and key fingerprints. Dismisses on
click-outside. No pill/height changes.

## Telemetry evidence

```
sealed ping      → {"t":"pong","_mid":"bfa851cf"}
unsealed ping    → ✕ SEAL: UNSEALED msg on 'watcher/command' - DROPPED
                    {"t":"seal_event","seal":"rejected","rc":"UNSEALED",...}
camera           → {"t":"snaptest","id":0,"bytes":25956,"ms":336,"soi":true,"eoi":true}
                   {"t":"snaptest","id":0,"bytes":19706,"ms":330,"soi":true,"eoi":true}
                   {"t":"snaptest","id":0,"bytes":19747,"ms":320,"soi":true,"eoi":true}
heartbeat        → {"t":"heartbeat","uptime_s":8414,"fw":"0.4.9",
                    "heap_free":7484286,"internal_free":25566,...}
seal status      → {"t":"seal_status","mqtt_mode":"bridged","seal_floor":519,
                    "seal_armed":false,"seal_dev_verify":true,...}
```

Notable: `internal_free:25566` is healthy — a world away from the
1.5 KiB pressure that wedged the camera in 0.4.1. Uptime 8,414 s with
zero resets since the 0.4.9 flash.

## Known issues

- `{"t":"dbg","cmd":"nvs_stress"}` is serial-only by design (spawns
  PSRAM-stack test tasks unsafe over MQTT) — **not yet run on
  hardware**. Its ALL PASS is the last outstanding validation of the
  0.4.7/0.4.8 NVS broker fix. John to run via page serial console.
- `keepalive.py` unsealed legacy path: documented exception, decision
  pending (seal or remove).
- PTT: long recordings starve the task watchdog (completes anyway);
  fix pending (yield/feed WDT during capture).

## Next steps

1. John runs `nvs_stress` over serial → confirm ALL PASS.
2. Decide `keepalive.py` legacy path: seal or remove.
3. With (1) green, 0.4.9 is fully validated, not merely stable.
