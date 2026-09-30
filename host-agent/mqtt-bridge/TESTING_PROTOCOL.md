# Watcher Bridge — Testing Protocol

**Read this before sending any device command.** Last updated 2026-09-27
(policy: unsealed path kept for unkeyed devices, per John).

## 1. The policy (John 2026-09-27)

**Seal is the default and best practice for ALL keyed devices.**
Commands are WMS2-sealed (Ed25519, commander `muse-01`, topic-bound).

**Unsealed is supported for UNKEYED devices** — fresh/unprovisioned
hardware you need to reach BEFORE the seal is configured (bootstrap,
provisioning). The unsealed legacy path is an intentional feature,
not a bug.

In strict mode the seal gate drops every unsealed message on
`watcher/command` before it reaches a keyed device:

```
SEAL: UNSEALED msg on 'watcher/command' - DROPPED
```

An unsealed send to a keyed device in strict mode silently does
nothing. That is the security working as designed — not a bug to work
around. Against an unkeyed device (grace mode / no keys provisioned)
the same unsealed command is accepted and processed.

## 2. Canonical sender

**Always use `send_command.py`.** It is the single canonical sender:

```bash
MQTT_PASS='...' python3 send_command.py '{"t":"ping"}'
```

- Sealing is **ON by default** (WMS2/Ed25519, commander `muse-01`,
  topic-bound to `watcher/command`).
- `--seal` states the default explicitly. Use it in scripts so the
  intent is visible in logs.
- `--unkeyed` sends the legacy unsigned `{"id","cmd"}" JSON. For
  unkeyed/unprovisioned devices (bootstrap/provisioning), or to
  deliberately test strict-mode rejection (see §4). Prefer `--unkeyed`
  over `--no-seal` — the flag states the reason in the command line.
- `--no-seal` is a legacy alias for `--unkeyed` (same behavior).
- The script enforces the authorized-command allowlist (mirrors the
  console): `ping`, `dbg <version|mem|heap|tasks|pwr|i2c|touch|wheel|
  pointer|spitest>`, `logs n=1..100`, and the HID class
  (`hid_status|mouse_move|mouse_glide|mouse_wheel|scroll|mouse_click|
  key|type|consumer`). HID commands additionally require John's
  explicit per-use permission in chat — the allowlist makes them
  *possible*, his word makes them *allowed*.
- It prints `[SEALED]` or `[UNSEALED]` on the "sent" line so the seal
  state is greppable in logs.

### Deprecated: `send_cmd.py`

`send_cmd.py` used to publish **unsealed** commands. On 2026-09-27 a
test agent used it by mistake and 8 unsealed commands were correctly
dropped by the seal gate. `send_cmd.py` is now a thin wrapper that
execs `send_command.py` (sealed by default) and prints a deprecation
warning to stderr. **New code must call `send_command.py` directly.**

Behavior changes vs the old `send_cmd.py`:
- Commands are now sealed (they reach the device in strict mode).
- The authorized-command allowlist is enforced (the old script sent
  anything, including non-allowlisted `dbg` subcommands).
- The envelope is the canonical `{"sealed": <base64>}` form, not the
  legacy `{"cmd","id"}` JSON.

## 3. `--unkeyed` / `--no-seal`: when (and when not) to use it

Use `--unkeyed` for **unkeyed devices** — fresh hardware where you
need to talk to the device BEFORE the seal is configured (bootstrap,
provisioning, first contact). This is a legitimate, supported path.

```bash
# Bootstrap: reach an unprovisioned device before seal setup.
MQTT_PASS='...' python3 send_command.py --unkeyed '{"t":"ping"}'
```

Use `--unkeyed` (or `--no-seal`) **also** to deliberately verify that
strict mode rejects unsealed traffic on a keyed device. That is a
real test — see §4b — but the command will NOT reach the device.

```bash
# Seal-rejection test on a KEYED device: expect DROP, not a pong.
MQTT_PASS='...' python3 send_command.py --unkeyed '{"t":"ping"}'
```

Never use `--unkeyed` for routine control of a keyed device. If a
command "doesn't work" against a keyed device, check the seal state
first (the `[SEALED]`/`[UNSEALED]` tag on the sent line) before
debugging anything else — an `[UNSEALED]` send to a keyed device in
strict mode is dropped by design.

## 4. Test sweep protocol

Every full test sweep MUST exercise the seal gate to prove it is
working. Which sub-tests apply depends on the device under test:

### 4a. Sealed path — must succeed (keyed devices)

```bash
MQTT_PASS='...' python3 send_command.py --seal '{"t":"ping"}'
```

Expect: `sent id=... [SEALED ctr=N]`, then a pong in the reply window.
In `telemetry.log`, the forward appears as:

```
→ sealed_fwd watcher/command (... b64 chars) · ping
```

### 4b. Unsealed path — must be dropped (keyed device, strict mode)

```bash
MQTT_PASS='...' python3 send_command.py --unkeyed '{"t":"ping"}'
```

Expect: the unsealed-send WARNING on stderr, `sent id=...
[UNSEALED]`, and **no pong**. In `telemetry.log`:

```
✕ SEAL: UNSEALED msg on 'watcher/command' - DROPPED
{"t":"seal_event","seal":"rejected","rc":"UNSEALED",...}
```

If the unsealed command gets a reply on a keyed device, strict mode
is NOT enforced — stop and investigate before continuing.

### 4c. Unsealed path — must be accepted (unkeyed device)

When the device under test has NO seal keys provisioned (fresh
hardware, bootstrap), the same unsealed send MUST succeed — the
device is in grace/unkeyed mode and the unsealed path is the only
way to reach it:

```bash
MQTT_PASS='...' python3 send_command.py --unkeyed '{"t":"ping"}'
```

Expect: `sent id=... [UNSEALED]`, then a pong. No `DROPPED` in the
log. This is correct — seal the device (provision keys) before
treating it as keyed.

### 4d. What to grep in telemetry.log

| Pattern | Meaning |
|---|---|
| `→ sealed_fwd watcher/command` | sealed command forwarded |
| `[SEALED ctr=` | our send was sealed (from send_command.py stdout, if captured) |
| `SEAL: UNSEALED msg on 'watcher/command' - DROPPED` | seal gate dropped an unsealed message (expected for §4b, alarming otherwise) |
| `"seal":"rejected","rc":"UNSEALED"` | structured seal_event for the drop |
| `SEAL: BAD TAG` / `SEAL: REPLAY` | tampered or replayed envelope — investigate immediately |

## 5. Other publishers of `watcher/command` (audit 2026-09-27)

| Script | Sealed? | Notes |
|---|---|---|
| `send_command.py` | ✅ default | Canonical sender. |
| `send_cmd.py` | ✅ default (wrapper) | Deprecated; execs `send_command.py`. |
| `report_fiducials.py` | ✅ default, `--no-seal` opt-out | Same pattern as `send_command.py`. |
| `run_calibration.py` | ✅ default, `--no-seal` opt-out | Sealed 2026-09-27 (glide + snaptest publishes). Requires John's physical bench presence. |
| `send_flash.py` / `transfer-flash.py` | ✅ | Firmware flow uses the sealed forward path. |
| `subscriber.py` | ✅ (verifier) | The bridge itself; seal-checks inbound `watcher/command`. |
| `keepalive.py` | ⚠️ unsealed (intentional) | Daemon publishes unsealed `keepalive`/`jiggle` device commands to `watcher/command`. **Intentional bootstrap feature, not a bug** (John 2026-09-27): the daemon must reach devices before seal keys are provisioned. On keyed devices in strict mode the seal gate drops these; the primary keepalive path is `watcher/keepalive_bridge`, which the page handles directly without a device command. |
| `gen_seal_vectors.py`, `gen_tel_vectors.py`, `apply_seal_patch.py` | n/a | Offline test-vector / patch tools; never publish live. |
| `audit_topics.py` | n/a | Static AST auditor; never publishes live. |

## 6. Credentials

- Broker password: env `MQTT_PASS`, or `~/.config/watcher-mqtt/mqtt_pass`
  (0600). Never print it, log it, or paste it into chat.
- Signing key: `~/.ssh/watcher_seal_muse01` (0600). `seal_client`
  loads it; if missing, sealing fails closed — run `seal_keygen.py`
  once or use `--no-seal` for grace-mode only.
- If the bridge subscriber is running, its `/proc/<pid>/environ` is
  the transient recovery path (see `run-subscriber.sh`). Never persist
  the recovered value anywhere new.

## 7. Checklist for future test agents

1. [ ] I am using `send_command.py`, not `send_cmd.py`.
2. [ ] The sent line says `[SEALED]` — unless this step targets an
       unkeyed device or is the §4b rejection test (then `[UNSEALED]`
       is expected, and I used `--unkeyed` so the intent is explicit).
3. [ ] John gave explicit per-use permission for any HID command (mouse/key/type/click/scroll).
4. [ ] The console "Allow Muse to run commands" toggle is on (else: no echo).
5. [ ] Sweep includes the seal gate: §4a (sealed succeeds on keyed
       device) AND §4b (unsealed dropped on keyed device in strict
       mode). If testing an unkeyed device, §4c (unsealed accepted)
       instead of §4b.
6. [ ] I grepped `telemetry.log` for `DROPPED`/`rejected` and every hit is explained.
7. [ ] I did not print, log, or store any credential.
8. [ ] I know whether the device under test is keyed or unkeyed, and
       chose sealed/unsealed accordingly (seal = best practice for
       keyed; unsealed = supported for unkeyed bootstrap).
