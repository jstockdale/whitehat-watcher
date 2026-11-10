# Watcher System Runbook

**Last updated:** 2026-11-10
**Purpose:** Reference for build systems, command systems, MQTT, and bench capabilities. Created per John's request — "this isn't the first time you have forgotten we have bench capabilities."

---

## 1. Build Systems

### S3 (ESP32-S3) Firmware
- **Source:** `~/workspace/projects/watcher-input-assistant/firmware/`
- **Toolchain:** ESP-IDF v5.3 — `source ~/esp-idf/export.sh` first (not on default PATH)
- **Build:** `idf.py build` (from `firmware/` directory)
- **Merge:** `idf.py merge-bin -o <absolute-path>` (MUST use absolute path; reads `build/flash_args`)
  - Factory partition at 0x20000 (not 0x10000 — v0.1.7 shipped non-booting image because of this)
- **Version:** `firmware/main/main.c` → `#define FW_VERSION "x.y.z"`
- **Current:** 0.4.51
- **Deliver:** Via MQTT `transfer-flash.py` (John clicks "Yes, flash it" in page)

### Himax (HX6538 WE2) Firmware
- **Source:** `~/workspace/projects/watcher-input-assistant/sscma-fork/sscma-example-we2-main/`
- **Toolchain:** ARM GNU 13.2 — `export PATH=~/workspace/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin:$PATH`
- **Build:** `cd EPII_CM55M_APP_S && make cleanbuild` (regenerates the build fingerprint + full clean — required for every build; makefile is in `EPII_CM55M_APP_S/`, NOT top level!)
- **Version:** `EPII_CM55M_APP_S/library/sscma_micro/sscma/server/at/callback/watcher_build_hash.h` → `#define WATCHER_FW_ID`
- **Package:** 
  1. `cp EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf we2_image_gen_local/input_case1_secboot/`
  2. `cd we2_image_gen_local && ./we2_local_image_gen project_case1_blp_wlcsp.json`
  3. Output: `output_case1_sec_wlcsp/output.img`
  4. Verify: `strings output.img | grep "watcher-himax-"`
- **Current:** 0.4.20.62
- **Deliver:** Via `curl` + `sha256sum` (NEVER attachment — corrupts binary). John flashes via XMODEM.
- **Critical:** Always `make cleanbuild` (partial rebuilds ship stale objects — the tree's header dependency tracking is broken); always copy FRESH ELF before image gen (silently packages stale ELF otherwise)

### Version Rules (John)
- Every distinct binary gets a unique version. Never reuse a version number.
- Format: S3 `0.4.x`, Himax `0.4.20.x`

---

## 2. Command Systems

### S3 Commands (via MQTT → Page → USB → S3)
Send via: `python3 send_command.py '{"t":"dbg","cmd":"..."}'` (from `host-agent/mqtt-bridge/`)
- Must acquire bench lock first: `from bench_lock import acquire_bench`
- Must resync seal counter: read `seal_floor` from telemetry.log, call `resync_counter(floor)`
- Commands: `spitest`, `png`, `at` (with `"at":"AT+..."`), `version`, `mem`, `heap`, etc.
- `dbg at` → S3 → **SPI** → Himax AT server (3s timeout in `eyes_at()`)
- `dbg png` → S3 captures via `AT+SPIDUMP` (15s timeout) then SPI bulk download

### Himax AT Commands

**Two paths — THIS IS CRITICAL:**

| Path | Route | Timeout | Use for |
|------|-------|---------|---------|
| **Direct UART** | Page `himax_at` → USB → Himax UART1 (PB6/PB7) | 5s, response-aware | Camera commands (RAWDUMP, SPIDUMP) |
| **S3 SPI bridge** | MQTT `dbg at` → S3 → SPI → Himax | 3s fixed (`eyes_at`) | Quick commands (ID?, SENSOR?) |

- **Direct UART** (`himax_at` in watcher-console.html): Page handles directly, bypasses S3. Polls 100ms, stops on complete JSON, 5s timeout. **Use for camera.**
- **S3 SPI bridge** (`dbg at`): S3 forwards via `sscma_client_request()` over SPI. 3s timeout. **Fails for slow commands.**

**Himax AT Server Architecture:**
- ONE AT server (`ATServer::threadEntry()`), MULTIPLE transports
- Loops through all transports (UART, SPI), calls `transport->receiveIf()`
- Same `execute()` for all — command processor is identical
- Response goes back via the same transport the command arrived on
- Commands: `RAWDUMP`, `RAWREAD`, `SPIDUMP`, `SENSOR`, `ID?`, etc.

### Bench Capabilities (Don't Forget!)
- `himax_at` — page-side, direct USB UART to Himax (in watcher-console.html, NOT S3 firmware)
- `himax_at_batch` — multiple AT commands in one MQTT message
- `dbg spitest` — SPI link test (16 patterns, no camera)
- `dbg png` — S3 captures via AT+SPIDUMP then SPI bulk download (respects John's "pull the frame" rule? NO — it captures! See below)
- Direct UART via John's tablet — always works, bypasses all S3 issues

### John's "Pull the Frame" Rule
- "Pull the frame" = download EXISTING framebuffer, NEVER capture-then-download
- `dbg png` VIOLATES this — it does `AT+SPIDUMP` (capture) then bulk download
- For pure download, use `AT+RAWREAD` after a `RAWDUMP` was already done

### SPI AT Bug (Fixed 2026-10-06, v0.4.18.36)
- **Symptom:** `dbg at` with `RAWDUMP`/`SPIDUMP` timed out, but UART (`himax_at`) worked
- **Root cause:** `client_io_spi_write()` (AT commands over SPI) did NOT wait for MISO-flag LOW (Himax RX-armed). The bulk path (`spi_send_cmd_`) had the wait, but AT path didn't.
- **Fix:** Added `spi_miso_wait_level_(0, 50, "RX-armed LOW (AT cmd)")` to `client_io_spi_write()`
- **Lesson:** Two SPI paths (AT vs bulk) — both need MISO-flag waits!

---

## 3. MQTT Systems

### Bridge
- **Subscriber:** `subscriber.py` (must be running!) — bridges MQTT ↔ page ↔ USB
- **Health check:** `python3 bridge_health.py` (MUST run before ANY bench work — John's rule #1)
- **Broker:** `mqtt.offx1.com:8883` (TLS), user `muse`
- **Credential:** In subscriber's `/proc/<pid>/environ` as `MQTT_PASS` (NEVER log/print/write it)
- **Find PID:** `pgrep -f "subscriber.py"` (NOT `ps aux`; NOT `pkill -f` without anchoring)

### Bench Lock
- **Why:** Multiple processes → interleaved MQTT → lost progress
- **Use:** `from bench_lock import acquire_bench, release_bench`
- **Check:** `python3 bench_lock.py` (exit 1 = busy)
- **Clear stuck:** `python3 bench_lock.py --clear` (only if holder hung but not dead)

### Seal (Command Authentication)
- All discrete commands must be sealed (Ed25519)
- Read `seal_floor` from telemetry.log BEFORE sending: `grep -a "seal_floor" telemetry.log | tail -1`
- Resync: `python3 -c "from seal_client import resync_counter; resync_counter(<floor>)"`
- `send_command.py` seals by default

### Topics
- `watcher/command` — S3 commands (sealed)
- `watcher/telemetry` — device telemetry
- `watcher/status`, `watcher/heartbeat`, `watcher/console_heartbeat`

---

## 4. SPI Link (Phase 2: MISO-flag)

### Hardware
- S3 GPIO6 (MISO) ↔ Himax PB2 (MISO/DO)
- S3 GPIO4 (SCK) ↔ Himax PB4
- S3 GPIO5 (MOSI) ↔ Himax PB3
- S3 GPIO21 (CS) ↔ Himax PB5

### MISO-flag Design (John's decision, 2026-10-05)
- **Himax PB2** swaps between `GPIO13` (driven LOW) and `SPI_S_DO` (released)
- **LOW** (driven) = RX armed, safe to send command
- **HIGH** (released, S3 pull-up) = TX ready, safe to clock
- **S3:** `gpio_pullup_en(GPIO_NUM_6)` + `gpio_get_level()` polls (ns latency)
- **Race-free:** Mux release IS the signal (atomic)

### Why Not PB8?
- PB8 → D2 diode → PCA9535 P06 (NOT P07! P07 is reset line)
- D2 blocks LOW, no pull-down → node floats, latches HIGH forever
- We polled the WRONG PIN (P07=reset, always HIGH) for weeks!
- Independent review proved PB8 path electrically broken

### Current Status
- **Spitest:** 159/160 across two 80-test batches (MISO-flag + parser scan-realignment validated)
- **Bulk/PNG:** Working — full 1296×972 frames in ~4 s, byte-exact; 1:1 crop windows live since Himax 0.4.20.61

---

## 5. Key Files

| What | Where |
|------|-------|
| S3 SPI driver | `firmware/components/sscma_client/src/sscma_client_io_spi.c` |
| S3 version | `firmware/main/main.c` |
| Himax SPI driver | `sscma-fork/.../porting/himax/we2/ma_transport_spi.cpp` |
| Himax version | `sscma-fork/.../sscma/server/at/callback/watcher_build_hash.h` |
| S3 AT bridge | `firmware/components/eyes/eyes.c` → `eyes_at()` (3s timeout!) |
| S3 command handler | `firmware/components/link/link.c` → `handle_command()` |
| Console (himax_at) | `~/workspace/your_files/watcher-console/watcher-console.html` |
| Telemetry log | `host-agent/mqtt-bridge/telemetry.log` (use `grep -a`) |
| Findings doc | `docs/spi-debug-summary-2026-10-05.md` |
| Review doc | `docs/spi-phase2-review-2026-10-06.md` |
| Independent review | `~/workspace/user/files/spi-phase2-independent-review-2026-10-05__1.md` |

---

## 6. Common Pitfalls

1. **Forgetting `himax_at` exists** — It's page-side, not S3 firmware. Use for camera commands.
2. **Wrong Himax build dir** — Makefile is in `EPII_CM55M_APP_S/`, not top level.
3. **Stale ELF** — Always copy FRESH ELF to `input_case1_secboot/` before image gen.
4. **Wrong pin** — P06 is sync, P07 is reset. Double-check schematics.
5. **`dbg at` vs `himax_at`** — `dbg at` = SPI (3s timeout, fails for camera). `himax_at` = UART direct (works).
6. **Bridge down** — Always run `bridge_health.py` first. Dead subscriber eats commands silently.
7. **No bench lock** — Always acquire before sending commands.
8. **Stale seal floor** — Always read fresh `seal_floor` before sending.
