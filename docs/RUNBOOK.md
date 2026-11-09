# Watcher System Operations Manual (RUNBOOK)

**Last updated:** 2026-10-06
**Purpose:** The definitive operations manual for the Watcher bench — build systems, command paths, MQTT infrastructure, SPI link, and every bench capability. Written because the assistant kept forgetting bench capabilities (e.g. `himax_at` is page-side in `watcher-console.html`, not S3 firmware). Read this before doing bench work.

---

## Table of Contents

1. [Pre-flight checklist (do this first, every time)](#1-pre-flight-checklist-do-this-first-every-time)
2. [Build systems](#2-build-systems)
3. [Firmware delivery](#3-firmware-delivery)
4. [Command systems (all paths)](#4-command-systems-all-paths)
5. [MQTT systems](#5-mqtt-systems)
6. [SPI link (S3 ↔ Himax)](#6-spi-link-s3--himax)
7. [Himax AT server architecture](#7-himax-at-server-architecture)
8. [Bench capabilities checklist](#8-bench-capabilities-checklist)
9. [Version history](#9-version-history)
10. [Common pitfalls](#10-common-pitfalls)
11. [Key files index](#11-key-files-index)
12. [John's standing rules](#12-johns-standing-rules)

---

## 1. Pre-flight checklist (do this first, every time)

Bench rule #1 (John, 2026-09-30): **verify connectivity FIRST** — before ANY bench work (reading telemetry, sending commands, debugging), confirm you can reach the bench. A dead subscriber silently eats test traffic.

```bash
# 1. Bridge health — exit 0 = subscriber alive + telemetry fresh
python3 ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/bridge_health.py

# 2. Bench lock — acquire before any bench operation
cd ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
python3 -c "from bench_lock import acquire_bench; print('LOCK:', acquire_bench('<your-task-name>') is not None)"
# If acquire_bench() returns None, the bench is busy — do NOT proceed. Report who's holding it.
# Check status: python3 bench_lock.py  (exit 1 = busy)
# Force-clear stuck lock (holder hung but not dead): python3 bench_lock.py --clear

# 3. Seal floor — AUTOMATED since 2026-11-09: seal_command() resyncs
#    forward from the freshest telemetry floor before every seal
#    (max over the log tail; the anti-oracle cap still applies, and a
#    floor problem warns instead of blocking the send). This step is
#    now just a sanity check; the manual resync below it is the
#    fallback if the bridge has been down:
python3 seal_client.py --auto-resync
# Manual fallback: FLOOR=<floor from a fresh seal_status/ready line>
# python3 -c "from seal_client import resync_counter; print('resynced:', resync_counter($FLOOR))"
# (Do NOT take the floor from `grep | tail -1`: the page-side
# seal_status can report 0 before loading its high-water.)

# 4. Confirm versions on bench
grep -a "fw=0.4" telemetry.log | tail -1          # S3 version
grep -a "DIAG watcher-himax" telemetry.log | tail -1  # Himax version
```

**Announce every device command immediately before sending it** (John's standing rule). Bench grants ("the bench is yours") are per-session, commissioned and narrated, never standing license.

---

## 2. Build systems

### 2.1 S3 (ESP32-S3) firmware

| Item | Value |
|------|-------|
| Source | `~/workspace/projects/watcher-input-assistant/firmware/` |
| Toolchain | ESP-IDF v5.3 — **not on default PATH** |
| Version file | `firmware/main/main.c` → `#define FW_VERSION "x.y.z"` |
| Current | `0.4.18.35` |

```bash
# Environment setup (REQUIRED in each new shell)
source ~/esp-idf/export.sh

# Build (from firmware/ directory)
cd ~/workspace/projects/watcher-input-assistant/firmware
idf.py build

# Merge into single flashable image — output path MUST be absolute
# (relative resolves against the build dir; idf.py reads build/flash_args)
idf.py merge-bin -o ~/workspace/your_files/himax-flash-kit/v0.4.18.35/watcher_s3_0.4.18.35_merged.bin

# Verify
sha256sum ~/workspace/your_files/himax-flash-kit/v0.4.18.35/watcher_s3_0.4.18.35_merged.bin
```

**Critical lessons:**
- **Never hand-write `esptool.py merge_bin`** with the default 0x10000 app offset. `partitions.csv` auto-aligns app partitions to 64KB, so factory sits at **0x20000** (v0.1.7 shipped a non-booting image because of this — bootloader: "image at 0x20000 has invalid magic byte"). Always use `idf.py merge-bin`.
- In ESP-IDF, definitions that must reach component sources have to be set **BEFORE `project()`** in the top-level CMakeLists (component targets are created during `project()`; anything added after never reaches them).
- CMake function-style macros (`-DFOO(x)=...`) can't be passed through the build system reliably. Put logic in a real header `configure_file()`'d into `${CMAKE_BINARY_DIR}/config` and pass only plain `NAME=value` definitions.

### 2.2 Himax (HX6538 WE2) firmware

| Item | Value |
|------|-------|
| Source | `~/workspace/projects/watcher-input-assistant/sscma-fork/sscma-example-we2-main/` |
| Toolchain | ARM GNU 13.2 — **not on default PATH** |
| Version file | `EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/callback/watcher_build_hash.h` → `#define WATCHER_FW_ID "watcher-himax-x.y.z-spi"` |
| Current | `0.4.20.39` |

```bash
# Environment setup (REQUIRED in each new shell)
export PATH=~/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin:$PATH

# Build — the makefile is in EPII_CM55M_APP_S/, NOT the top level!
cd ~/workspace/projects/watcher-input-assistant/sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S
touch <changed-source>.cpp   # make doesn't always detect changes — touch first!
make -j$(nproc)

# Package secure image — CRITICAL: copy the FRESH ELF first
# (the generator silently packages a stale ELF otherwise)
ELF=EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf
cp $ELF we2_image_gen_local/input_case1_secboot/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf
cd we2_image_gen_local
./we2_local_image_gen project_case1_blp_wlcsp.json
# Output: output_case1_sec_wlcsp/output.img  (expect "IMAGE GEN DONE")

# Verify the version string is the NEW one (catches stale builds)
strings output_case1_sec_wlcsp/output.img | grep "watcher-himax-"
# MUST show watcher-himax-0.4.20.39-spi (or whatever you just built)

# Stage for delivery
mkdir -p ~/workspace/your_files/himax-flash-kit/v0.4.20.39
cp output_case1_sec_wlcsp/output.img ~/workspace/your_files/himax-flash-kit/v0.4.20.39/watcher_sscma_v0.4.20.39_spi.img
sha256sum ~/workspace/your_files/himax-flash-kit/v0.4.20.39/watcher_sscma_v0.4.20.39_spi.img
```

**Critical lessons:**
- **Stale builds are the #1 Himax pitfall.** `0.4.20.31` was delivered with 100ms-timeout code despite source saying 2500µs — `make` didn't recompile. Always `touch` the changed `.cpp` before `make`. Always verify with `strings` on the ELF *and* the packaged image.
- **The 16-char boot fingerprint is stale/non-unique** (e.g. `bdb53f70a364ed94` appears across versions). The `watcher-himax-x.y.z-spi` string is the reliable version indicator. Check the DIAG line's version string, ignore the fingerprint.
- Clean rebuild: `make clean && make -j$(nproc)` (takes ~9 minutes — full TensorFlow Lite rebuild).

### 2.3 Version rules (John, standing)

- **Every distinct binary gets a unique version.** Never mint two different binaries with the same version — "a version number is a promise that one exact artifact is running."
- Hotfix = patch bump (e.g. `0.4.18` → `0.4.18.1`). Bad builds keep their numbers so history stays trackable ("name it 0.4.1 so we can track the bad build").
- Format: S3 `0.4.18.x`, Himax `0.4.20.x`.

---

## 3. Firmware delivery

### 3.1 S3 delivery (MQTT → page → John flashes)

John performs all S3 flashes (his click in the page). Never flash autonomously.

```bash
cd ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
# Recover credential transiently (see §5 for full procedure)
export MQTT_PASS=$(tr '\0' '\n' < /proc/$(pgrep -f 'subscriber.py' | head -1)/environ | grep '^MQTT_PASS=' | cut -d= -f2-)

# Transfer (handles fw-missing resends while running; page shows Yes/No prompt)
timeout 120 python3 transfer-flash.py ~/workspace/your_files/himax-flash-kit/v0.4.18.35/watcher_s3_0.4.18.35_merged.bin --confirm
# Expect: "page: transfer complete, hash OK — waiting on John's Yes/No in the page"
```

Then John clicks **"Yes, flash it"** in the console page. Verify in `telemetry.log`:
- `fw-flashed <transfer-id>`
- Boot lines, then `host_summary` with `"fw":"0.4.18.35"`

**Known issue (unfixed):** Once `transfer-flash.py` exits, the page's resend requests go unanswered. If a transfer stalls (e.g. stuck at 461/474), re-run the transfer fresh. The proper fix (persistent chunk caching in `subscriber.py`) is still open — John has asked about this; answer remains "not fixed."

### 3.2 Himax delivery (curl + XMODEM, John flashes)

**Always deliver via `curl` with the canonical filename plus `sha256sum` verification** (John, 2026-10-05 standing rule). Never the default attachment flow — it corrupts Himax binaries.

Exact format (John 2026-10-04, copy verbatim):

```bash
curl -L -o <canonical-filename> \
  "<url>"
sha256sum <canonical-filename>
# Firmware build fingerprint (matches the DIAG line at boot):
# <watcher-himax-x.y.z-spi>
# File hash (verifies the download — match against the sha256sum output above):
# <full sha256>
```

Upload first: `/opt/hatch/bin/remote-storage upload-file --path <img>` → returns `url` and `expires_at` (links expire; anyone with the link can access).

John flashes via XMODEM over the Himax UART (his standing rule: he performs Himax flashes). Verify post-flash: `grep -a "DIAG watcher-himax" telemetry.log | tail -1` must show the new version string.

---

## 4. Command systems (all paths)

### 4.1 The three AT paths — THIS IS CRITICAL

There are **three** distinct ways to send AT commands to the Himax. Confusing them has wasted bench time.

| Path | Command | Route | Timeout | Use for |
|------|---------|-------|---------|---------|
| **A. Page direct UART** | `himax_at` (console) | Page → USB → Himax UART1 (PB6/PB7) | 5s, response-aware (polls 100ms, stops on complete JSON) | **Camera commands** (RAWDUMP, SPIDUMP, SENSOR) |
| **B. S3 SPI bridge** | `dbg at` (MQTT) | MQTT → S3 → SPI → Himax | 3s fixed (`eyes_at()` in `eyes.c`) | Quick commands only (ID?, SENSOR?) |
| **C. John's tablet** | (serial terminal) | Tablet USB → Himax UART | None (manual) | Ground truth debugging |

**Key facts:**
- `himax_at` is **page-side only** (in `watcher-console.html`, NOT S3 firmware). The page sends AT directly over its WebSerial Himax port, bypassing the S3 entirely. There is also `himax_at_batch` (multiple AT commands in one MQTT message).
- `dbg himax_at` **does not exist** in S3 firmware — don't try it (the S3 will refuse: "command is not on the authorized list").
- `dbg at` goes through the S3's `eyes_at()` → `sscma_client_request()` over **SPI** with a hardcoded 3s timeout. It works for fast commands but is the wrong tool for camera capture.
- The Himax has **ONE AT server** with **MULTIPLE transports** (see §7). The command processor is identical; only the transport differs.

**Rule of thumb:** Camera commands → `himax_at` (page) or John's tablet. Quick queries → `dbg at` is fine.

### 4.2 S3 commands via MQTT (the standard path)

```
You → MQTT (sealed) → console page → USB serial → S3 → reply → page → MQTT → telemetry.log
```

Send via the canonical sender (from `host-agent/mqtt-bridge/`):

```bash
export MQTT_PASS=$(tr '\0' '\n' < /proc/$(pgrep -f 'subscriber.py' | head -1)/environ | grep '^MQTT_PASS=' | cut -d= -f2-)
python3 send_command.py '{"t":"dbg","cmd":"spitest"}' --timeout 30
python3 send_command.py '{"t":"dbg","cmd":"at","at":"AT+ID?"}' --timeout 10
python3 send_command.py '{"t":"ping"}' --timeout 10
```

- `send_command.py` seals by default (WMS2/Ed25519, commander `muse-01`, topic-bound to `watcher/command`). `--no-seal`/`--unkeyed` for unkeyed devices only.
- **Announce each device command immediately before sending** (John's standing rule).
- The script enforces an authorized-command allowlist mirroring the console: `ping`; `dbg` subcommands (`version|mem|heap|tasks|pwr|i2c|touch|wheel|pointer|spitest|png|at|...`); `logs`; HID class (needs John's per-use permission).
- Replies come back through `watcher/telemetry`; check with `grep -a` on `telemetry.log`.

**`dbg` subcommands of note:**
- `spitest` — SPI link test: 16 transfers (4 patterns × 4 lengths), CRC-verified. **Baseline: 16/16 when the link works.** No camera involved.
- `png` — S3 captures via `AT+SPIDUMP` (15s timeout) then SPI bulk download. **Note:** this *captures*, violating John's "pull the frame" rule (see below).
- `at` — raw AT passthrough to Himax **over SPI** (3s timeout). Dangerous verbs (RESET/OTA/ERASE/…) blocklisted in `link.c`.
- `version`, `mem`, `heap`, `tasks` — diagnostics.

### 4.3 John's "pull the frame" rule (literal interpretation)

John, 2026-09-30 (standing): **"pull the frame" means download the existing framebuffer, never capture-then-download.** Read his commands specifically and exactly as worded; never expand beyond them.

- `dbg png` **violates** this — `eyes_raw_pull()` does `AT+SPIDUMP` (fresh capture) then bulk download.
- For a pure download of an already-captured frame: `himax_at` → `AT+RAWDUMP` (capture once), then `AT+RAWREAD=<off>,<len>` chunks — or the S3 bulk path after a capture exists.

### 4.4 Direct UART via John's tablet (ground truth)

John's Galaxy Tab S9 Ultra (his Linux port) has USB serial to both chips. When he reports observations from direct UART ("the camera works fine over UART"), treat as ground truth and verify from there. The direct UART path bypasses every S3/MQTT/page issue — it's the reference for "is the Himax itself healthy."

---

## 5. MQTT systems

### 5.1 Architecture

```
                    ┌─────────────┐
  You ──MQTT───────▶│   Broker    │──MQTT──▶ console page ──USB──▶ S3 ──SPI/UART──▶ Himax
  (sealed)          │mqtt.offx1.com│         (WebSerial)            ▲
                    │  :8883 TLS  │                                │ telemetry
  ◀──MQTT───────────│  user muse  │◀──MQTT── page ──USB────────────┘
  (telemetry.log)   └─────────────┘
```

- **Bridged mode** is current: bench commands route Muse → broker → page → USB → device. Native MQTT on the S3 is the end state (not yet).
- The page is never trusted for sealed commands long-term (Phase 2+ device re-verification), but Phase 1 trust caveat stands.

### 5.2 Bridge (`subscriber.py`)

- **Long-running daemon** on this machine. Subscribes to the broker, writes everything to `telemetry.log`. **John wants it kept running at all times** (2026-09-30 standing rule).
- Holds the broker credential **only in its process environment** — never stored anywhere on disk.
- **Recover transiently** (never print/log/write it):
  ```bash
  SUB=$(pgrep -f 'subscriber.py' | head -1)
  export MQTT_PASS=$(tr '\0' '\n' < /proc/$SUB/environ | grep '^MQTT_PASS=' | cut -d= -f2-)
  ```
  - Find PID with `pgrep -f`, **never** `ps aux` (command lines can expose environments).
  - Never `pkill -f <pattern>` with an unanchored pattern — it matches your own shell's command line too (killed my own exec once). Anchor it: `pgrep -f "^python3 subscriber"` or resolve PID first.
  - If the subscriber isn't running, **ask John** (don't restart blindly without recovering the credential first).
- **Health check:** `python3 bridge_health.py` — exit 0 = subscriber alive + telemetry fresh. **Run before EVERY bench action.**
- Watchdog: `bridge-watchdog.sh` exists; the bridge has died and been caught before — report tally, say what's known/unknown.

### 5.3 Bench lock

Multiple processes hitting the bench simultaneously cause interleaved MQTT requests and lost progress (John, 2026-09-30).

```python
from bench_lock import acquire_bench, release_bench
if acquire_bench('my-task-name') is None:
    # bench busy — do NOT proceed, report holder
```
- `python3 bench_lock.py` → exit 1 = busy (shows holder).
- `python3 bench_lock.py --clear` → force-clear a stuck lock (only when holder is hung but not dead).
- Auto-clears if holder PID dies (stale lock detection).
- **Acquire before:** `send_command.py`, downloads, flash transfers.

### 5.4 Seal (WMS2 command authentication)

John, 2026-10-01 (standing): the S3 executes control commands **iff sealed** under the PKI multi-tier trust chain (Tier 0 offline root → Tier 1 provisioning cert → device-pinned keys). **Sealed-only covers read commands too** — discrete reads (`dbg`, AT reads) must carry a valid seal. The continuous telemetry stream stays on BLAKE2b-MAC + XChaCha20-Poly1305 (per-packet signatures would be wasteful).

**Before every send:** nothing manual — since 2026-11-09
`seal_client.seal_command()` auto-resyncs the counter forward from
the freshest `seal_floor` in the bridge's telemetry log (which
arrives over the MAC-authenticated telemetry channel) before
sealing; `send_command.py` reports `[floor-synced A->B]` when it
moved. The manual resync (`seal_client.py --resync <floor>`, or
`--auto-resync` to run the same path verbosely) remains for
forensics and bridge-down recovery. `send_command.py` seals by
default (`--no-seal` only for unkeyed/bootstrap).

Key material: commander `muse-01` keys at `~/.ssh/watcher_seal_*` (0600). Never request raw credentials; use the Secure Vault flow.

### 5.5 Topics (prefix `watcher`)

| Topic | Direction | Content |
|-------|-----------|---------|
| `watcher/command` | bridge → page | Sealed commands `{"id","cmd"}` |
| `watcher/telemetry` | page → bridge | Device events, logs, replies |
| `watcher/status` | page → bridge | `console: online`, page version, `cmd: true/false` |
| `watcher/heartbeat` | page → bridge | Device 1/min heartbeat |
| `watcher/console_heartbeat` | page → bridge | Page 1/min heartbeat |

- `grep -a` with `telemetry.log` (it's binary-ish; plain grep misses lines).
- ACL guard: `topic_acl.py` checks every publish against the local ACL mirror before sending (loud `ACL-DENY` on violation). `audit_topics.py` statically scans all publish sites.

### 5.6 Console page

- `watcher-console.html` — single self-contained file, served by John locally (his own webserver per his preference).
- Version shown in Setup tab and `watcher/status` (`page=0.4.140`). Check it's current before debugging page behavior.
- `himax_at` / `himax_at_batch` live here (page-side, direct USB UART to Himax).
- After console changes: export → `~/workspace/your_files/watcher-console/watcher-console.html` → John downloads + hard-refreshes.

---

## 6. SPI link (S3 ↔ Himax)

### 6.1 Hardware

| S3 | Himax | Signal |
|----|-------|--------|
| GPIO21 | PB5 | CS |
| GPIO4 | PB4 | SCK |
| GPIO5 | PB3 | MOSI |
| GPIO6 | PB2 | MISO |
| GPIO17/18 | PB6/PB7 | UART1 (AT commands, logs, flasher) |
| GPIO47/48 | (shared I²C) | PCA9535 expander @ 0x21 |

12 MHz, Mode 0, strictly half-duplex at the controller level (driver flips TMOD per direction).

### 6.2 Phase 2: MISO-flag handshake (current design, John's decision 2026-10-05)

The S3 needs to know when the Himax's TX response is staged before clocking. Timeout guessing ("whack-a-mole") is banned by John — the link must be event-driven.

**Design:** Himax PB2 swaps between two pinmux functions at runtime:
- **GPIO13, driven LOW** = RX armed — safe for S3 to send a command
- **SPI_S_DO, released** = TX ready — the DW slave tristates DO when deselected, so the S3's internal pull-up takes MISO HIGH in <1µs — safe to clock

**Why it's race-free:** "Response staged" and "pin connected to the SPI function" are **one atomic action** — the mux release. There is no window where the S3 can see ready before the pin is ready (the failure mode of every separate-flag design).

**Himax state machine** (`ma_transport_spi.cpp`, 0.4.20.39):
1. Init: arm RX DMA first; set GPIO13 LOW *before* muxing PB2 → GPIO13
2. Command RX callback: parse; stay GPIO-LOW during processing
3. Stage response: `spi_write_dma()` → on `E_OK`, mux PB2 → `SPI_S_DO` (the HIGH edge). On `E_NORES`/error: abort/reinit, stay LOW, count it
4. TX drained (`_spi_tx_callback`, fires only after master clocks everything out): arm RX DMA, *then* mux PB2 → GPIO13 LOW
5. `CMD_RESET` (0x06) = full resync primitive

**S3 side** (`sscma_client_io_spi.c`, 0.4.18.35):
- `gpio_pullup_en(GPIO_NUM_6)` at init (load-bearing — undriven MISO reads deterministic 0xFF)
- `wait(LOW, 50ms)` before sending command → `spi_transmit(cmd)` → `wait(HIGH, 50ms)` → clock read
- `gpio_get_level()` is ~ns; no I²C in the critical path (vs ~100µs/poll for the old expander approach)

**Status:** spitest **16/16** ✅ (2026-10-06). MISO-flag validated.

**Known limitation (0.4.18.35):** Not yet the review's strict "never blind-clock" implementation — `spi_sync_tx_ready_()` discards the wait-result boolean and returns void, so callers still clock after a TX-ready timeout. The wait-LOW gate is intentionally non-fatal for compatibility with Himax <0.4.20.38. Strict timeout failure/recovery is future work.

### 6.3 Why PB8 was abandoned

Phase 2 originally used Himax PB8 → D2 diode → PCA9535 P06 → expander INT → S3 GPIO2. The independent review (2026-10-05) proved it **electrically broken**:

1. **Wrong pin polled for weeks:** The sync is on **P06** (package pin 7). We polled **P07** (bit 7, package pin 8) = `EXP_Himax_Rst`, the expander *output* driving Himax reset, pulled HIGH always. We were reading our own reset line.
2. **Floating node:** D2's cathode has **no pull-down** anywhere; PCA9535 has no internal pulls; input leakage ≤1µA. PB8 HIGH drives ~3.0V; PB8 LOW reverse-biases D2 and the node **floats**, decaying over ms→forever. After the first response, the S3 reads latched HIGH forever. "PB8 LOW = busy" never reaches the S3. No firmware fixes this — needs a 22–33K pull-down (one resistor, kept as fallback).
3. **No FIFO-fill race:** The TX FIFO is 32 bytes (not 4090); DMA watermark fills it in µs. Whole-transfer constant fills (`a3…`, `f7…`) are the documented DW SSI slave **underflow** behavior (TXE: previous frame re-sent) — the slave was never armed, not "slow to fill."
4. **`_spi_tx_callback` fires after the master drains the FIFO** (gated on TXE + !BUSY + DMA-done) — moving PB8 there would be a circular wait. The review vetoed it.

Fallback order if MISO-flag ever hits an unknown: pull-down resistor at D2's cathode → PB7-as-GPIO (sacrifices the 921600-baud flasher UART).

### 6.4 History (condensed)

| Era | Approach | Result |
|-----|----------|--------|
| 0.4.18.22–28 | Fixed pre-clock delays (1–2ms), 2ms gaps | spitest 16/16, PNG FFs then 0x05s (timeout coupling) |
| 0.4.20.30–33 | Himax timeout tuning (100ms → 2500µs → 1500µs) | 0/16 → 16/16 (timeout must be < S3 inter-cmd time) |
| 0.4.18.31 | Poll PCA9535 P07 (wrong pin!) | 6/16, one perfect PNG chunk (first handshake real, then latched) |
| 0.4.18.32/33 | GPIO2 interrupt + semaphore | 3/16 (worse — INT fires on any input change, shared with touch) |
| 0.4.18.34 | Optimized polling (back-to-back I²C) | 6/16 |
| **0.4.18.35 + 0.4.20.39** | **MISO-flag** | **spitest 16/16** ✅ |

Full history: `docs/spi-debug-summary-2026-10-05.md`. Independent review: `~/workspace/user/files/spi-phase2-independent-review-2026-10-05__1.md`.

---

## 7. Himax AT server architecture

- **ONE AT server** (`ATServer::threadEntry()` in `ma_server_at.cpp`), **MULTIPLE transports** (UART1, SPI slave).
- The thread loops over `device->getTransports()`, calls `transport->receiveIf()` on each, and `execute(buf, transport)` on command receipt.
- **Same command processor for all transports.** The `Transport&` is threaded through so the response goes back via the incoming transport.
- Commands registered via `addService("RAWDUMP", ...)` etc.; handlers receive `(args, transport, encoder)`.
- **Implication:** If a command works over UART but fails over SPI, the bug is in the *transport path* (S3 SPI client, timeouts, MISO-flag interaction) — not the command implementation.

---

## 8. Bench capabilities checklist

Don't re-derive these — they exist:

**Device commands (via `send_command.py`, sealed, announced):**
- [ ] `{"t":"ping"}` — connectivity probe
- [ ] `{"t":"dbg","cmd":"version"}` / `mem` / `heap` / `tasks` — diagnostics
- [ ] `{"t":"dbg","cmd":"spitest"}` — SPI link test, 16 transfers, **expect 16/16**
- [ ] `{"t":"dbg","cmd":"png"}` — S3 `AT+SPIDUMP` capture + SPI bulk download (captures! see §4.3)
- [ ] `{"t":"dbg","cmd":"at","at":"AT+ID?"}` — Himax AT **over SPI** (3s timeout; quick commands only)
- [ ] `{"t":"dbg","cmd":"logs","n":50}` — device logs

**Page-side (watcher-console.html, no S3 involved):**
- [ ] `himax_at` — AT command **direct over USB UART** to Himax (5s response-aware; use for camera: RAWDUMP, SPIDUMP, SENSOR)
- [ ] `himax_at_batch` — multiple AT commands in one message
- [ ] Firmware transfer → John's "Yes, flash it" → S3 flash

**Himax AT commands (via `himax_at` or John's tablet UART):**
- [ ] `AT+ID?` — identity (fast, works over both paths)
- [ ] `AT+SENSOR?` — sensor list; `AT+SENSOR=0,0,8` / `AT+SENSOR=0,1,8` — sensor init
- [ ] `AT+RAWDUMP` — capture one frame via Sensor→INP→WDMA2, report `{width,height,bytes}`
- [ ] `AT+RAWREAD=<off>,<len>` — read chunk of raw capture buffer (base64)
- [ ] `AT+SPIDUMP` — capture + report `{len,sha256,width,height}` for SPI bulk pull

**Host tooling (`host-agent/mqtt-bridge/`):**
- [ ] `bridge_health.py` — connectivity check (run first!)
- [ ] `bench_lock.py` — bench mutex
- [ ] `send_command.py` — canonical sealed sender
- [ ] `transfer-flash.py` — S3 firmware transfer
- [ ] `send_flash.py` — flash John's cached .bin by SHA
- [ ] `seal_client.py` — `resync_counter()`
- [ ] `reassemble_png.py` — reassemble chunked PNG downloads
- [ ] `cursor_ops.py` — HID helpers (`hid_from_camera`, `move_to_target`, `click`, …)

**John's direct UART** — tablet serial to Himax (ground truth; bypasses everything).

---

## 9. Version history

### S3
| Version | Change | Test result |
|---------|--------|-------------|
| 0.4.18.30 | 2ms pre-clock + 2ms gaps restored | Baseline before GPIO work |
| 0.4.18.31 | Poll PCA9535 P07 (wrong pin) | spitest 6/16; PNG chunk 2 perfect (`ff_lead=0`) |
| 0.4.18.32 | GPIO2 ISR + semaphore | spitest 3/16 (interrupt worse than polling) |
| 0.4.18.33 | Check P07 before waiting | spitest 3/16 |
| 0.4.18.34 | Optimized polling (no interrupt) | spitest 6/16 |
| **0.4.18.35** | **MISO-flag + GPIO6 pull-up** | **spitest 16/16** ✅ |

### Himax
| Version | Change | SHA-256 |
|---------|--------|---------|
| 0.4.20.37 | PB8 TX-ready signaling (GPIO2) | `2b31a4d1…` |
| 0.4.20.38 | MISO-flag (superseded — flash didn't take, stale build suspected) | `7363eb40…` |
| **0.4.20.39** | **MISO-flag, clean rebuild** | `8ae7d341…` |

### Bench pairing
- **Current:** S3 `0.4.18.35` + Himax `0.4.20.39`

---

## 10. Common pitfalls

1. **Forgetting `himax_at` exists** — it's page-side (console HTML), not S3 firmware. For camera AT commands, use it — not `dbg at`.
2. **`dbg at` vs `himax_at`** — `dbg at` = S3→SPI→Himax (3s fixed timeout). `himax_at` = page→USB UART→Himax (5s response-aware). Different transports, different reliability for slow commands.
3. **Wrong Himax build directory** — the makefile is in `EPII_CM55M_APP_S/`, not the repo top level.
4. **Stale Himax ELF** — `touch` the changed `.cpp` before `make`; verify with `strings` on ELF *and* packaged image; always copy the **fresh** ELF into `input_case1_secboot/` before running the image generator.
5. **Stale boot fingerprint** — the 16-char hex on the DIAG line doesn't change between versions. Trust the `watcher-himax-x.y.z-spi` string.
6. **Bridge down** — `bridge_health.py` first, always. A dead subscriber silently eats commands.
7. **No bench lock** — acquire before `send_command.py`, downloads, transfers.
8. **Stale seal floor** — read fresh `seal_floor` from telemetry.log before every send; `send_command.py` will fail/replay-reject otherwise.
9. **`pkill -f` killing your own shell** — the pattern matches your own command line. Anchor it or resolve the PID with `pgrep` first.
10. **`grep` on telemetry.log** — use `grep -a` (the log has binary-ish content).
11. **Pull the frame ≠ capture** — John's rule: download existing framebuffer, never capture-then-download. `dbg png` captures.
12. **Upload before sharing the URL** — `/opt/hatch/bin/remote-storage upload-file` first, *then* give John the URL (learned 2026-10-06).
13. **Attachment corrupts Himax binaries** — always `curl` + `sha256sum` format (John's standing rule).

---

## 11. Key files index

| What | Path |
|------|------|
| S3 version | `~/workspace/projects/watcher-input-assistant/firmware/main/main.c` |
| S3 SPI driver | `~/workspace/projects/watcher-input-assistant/firmware/components/sscma_client/src/sscma_client_io_spi.c` |
| S3 eyes (AT bridge, `eyes_at`) | `~/workspace/projects/watcher-input-assistant/firmware/components/eyes/eyes.c` |
| S3 command handler | `~/workspace/projects/watcher-input-assistant/firmware/components/link/link.c` |
| Eyes SPI bus init (pull-up) | `~/workspace/projects/watcher-input-assistant/firmware/components/eyes/eyes.c` |
| Himax version | `~/workspace/projects/watcher-input-assistant/sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/callback/watcher_build_hash.h` |
| Himax SPI transport | `~/workspace/projects/watcher-input-assistant/sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp` |
| Himax AT server | `~/workspace/projects/watcher-input-assistant/sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/ma_server_at.cpp` |
| Console page | `~/workspace/your_files/watcher-console/watcher-console.html` |
| Bridge daemon | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/subscriber.py` |
| Telemetry log | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/telemetry.log` |
| Canonical sender | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/send_command.py` |
| S3 flash transfer | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/transfer-flash.py` |
| Bench lock | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/bench_lock.py` |
| Seal client | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/seal_client.py` |
| MQTT seal arch | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/MQTT_SEAL_ARCHITECTURE.md` |
| Flash flow | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/FLASH_FLOW.md` |
| Bridge runbook | `~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/RUNBOOK.md` |
| SPI findings | `~/workspace/projects/watcher-input-assistant/docs/spi-debug-summary-2026-10-05.md` |
| Phase 2 review | `~/workspace/projects/watcher-input-assistant/docs/spi-phase2-review-2026-10-06.md` |
| Independent review | `~/workspace/user/files/spi-phase2-independent-review-2026-10-05__1.md` |
| Schematic | `~/workspace/.tmp-sch/watcher_sch.pdf` |
| S3 flash kits | `~/workspace/your_files/himax-flash-kit/v0.4.18.x/` |
| Himax flash kits | `~/workspace/your_files/himax-flash-kit/v0.4.20.x/` |

---

## 12. John's standing rules

- **Be thorough and meticulous.** Verify against code and evidence; never guess.
- **No whack-a-mole:** don't guess timeouts for multiple rounds — instrument or experiment. Event-driven > polling > guessed delays.
- **Bench rule #1:** `bridge_health.py` before ANY bench work.
- **Bench lock** before any bench operation.
- **Fresh `seal_floor`** before every send; all discrete commands sealed.
- **Announce** every device command immediately before sending.
- **Ask before flashing** (S3: his click; Himax: his XMODEM). Unattended flashing stays OFF.
- **Unique version per binary.** Never reuse a version number.
- **Himax delivery:** `curl` + canonical filename + `sha256sum` (never attachments).
- **"Pull the frame"** = download existing framebuffer, never capture-then-download. Read instructions literally; ask on ambiguity.
- **His observations are ground truth** — verify from there, don't doubt first. Push back with evidence, not deference, if the evidence shows otherwise.
- **GitHub:** explicit permission before any write (push, repo create). Reads are fine.
- **The bench is his** — grants are per-session ("the bench is yours"), commissioned and narrated, never standing. Leave things as found; report anything left different.
- **His words are literal** — read every instruction twice for smuggled steps.

---

*This manual is a living document. Update it when the system changes — that's the point.*
