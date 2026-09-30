# Watcher S3 Firmware — SD Card Support Specification

**Status:** SPEC ONLY — not implemented. For review before green-lighting.
**Target:** ESP32-S3 firmware (ESP-IDF), Watcher input assistant
**Date:** 2026-09-29
**Author:** Muse (spec), for John's review

### John's rulings (2026-09-29)
1. **Rotation:** No auto-delete. 32GB card is plenty. Keep files, refuse to write when full, tell the user (log + MQTT event + `dbg sd` status).
2. **Encryption:** Unencrypted v1 is fine. Physical access to SD = physical access to bench. Encryption is future work.
3. **Version:** Full feature targets **0.5.0**. 0.4.x stays the known-good path for point releases. Git branches: `release/0.4.x`, `feature/sd-card-0.5.0`.
4. **Audio format:** Compressed. Opus recommended (high quality + low bandwidth, see §4.3).
5. **Log verbosity:** **Global loglevel device setting** — USB serial, SD card log, and MQTT log streams all emit the same lines at the same verbosity. Not per-output.

---

## 1. Requirements (from John)

1. **Camera captures** → SD card
2. **Audio captures** (PTT recordings, wake-word recordings) → SD card
3. **Per-boot log** → SD card (audit trail: what happened each boot, errors, events)

---

## 2. Hardware — verified from schematic

**Source:** `~/workspace/.tmp-sch/watcher_sch.pdf` / `watcher_sch.txt`

### 2.1 SD slot wiring (J4, microSD)

| SD pin | Net | ESP32 connection | Notes |
|--------|-----|------------------|-------|
| DAT2 | SWITCH / SD_DET | PCA9535 expander P04 | Card detect (via IO expander) |
| CD/DAT3 | SPI_SD_CS | ESP_GPIO46 (via Q19 transistor) | Chip select |
| CMD | — | ESP_GPIO5 / SPI2_MOSI | Shared with Himax SPI |
| CLK | — | ESP_GPIO4 / SPI2_SCK | Shared with Himax SPI |
| DAT0 | — | ESP_GPIO6 / SPI2_MISO | Shared with Himax SPI |
| DAT1 | — | NC (ESD only) | Not connected |
| VDD | — | Via Q12, gate = EXP_SD_EN | SD power rail |
| VSS | GND | — | — |

### 2.2 Key findings

- **SPI mode only.** DAT1 is unconnected, so 4-bit SDMMC mode is impossible. The card must use **SPI mode** (1-bit).
- **SPI2 bus is shared with the Himax.** GPIO4 (SCK), GPIO5 (MOSI), GPIO6 (MISO) are the same lines used for the Himax SSCMA SPI slave. The SD card (CS=GPIO46) and Himax (CS=GPIO21) have separate chip selects.
  - **Implication:** SD and Himax cannot be used simultaneously. All SD transactions must be mutex-protected against Himax SPI traffic. The existing `PIN_SD_CS` is already parked HIGH in `eyes.c:247` with the comment "SPI2 bus shared with the SD card."
- **Card detect exists** via expander P04 (`SD_DET`). Polarity needs verification on hardware (likely active-low when card inserted, via the SWITCH pin on the slot).
- **SD power rail** (`EXP_SD_EN`) gates VDD via Q12. The rail is currently powered on at boot with the other rails. A power-cycle sequence (off → 100ms → on) may be needed for reliable card re-initialization after errors.

### 2.3 Speed/size limits

- SPI mode on ESP32-S3: practical max ~20 MHz (SD SPI mode spec allows up to 25 MHz, but signal integrity on shared bus suggests 20 MHz max, 10 MHz for init).
- Throughput estimate: ~1–2 MB/s real-world for sequential writes in SPI mode.
- Card size: SDHC/SDXC supported by ESP-IDF `sdspi` driver. No hardware limit below 2TB (FAT32/exFAT).
- **No hardware limitation blocks this work.**

---

## 3. ESP-IDF Integration

### 3.1 Components

ESP-IDF provides (verified present in `~/esp-idf/components/`):
- `sdmmc` — includes `sdspi_host` (SPI-mode SD driver)
- `fatfs` — FAT filesystem (FAT32/exFAT via FatFs)

### 3.2 Configuration

```c
// Host: use SDSPI_HOST_DEFAULT (SPI2_HOST)
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
host.slot = SPI2_HOST;  // shares bus with Himax — must coordinate
host.max_freq_khz = 20000;  // 20 MHz; use SDMMC_FREQ_PROBING (400 kHz) for init

// Slot: SPI mode, GPIO46 CS
sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
slot_config.gpio_cs = 46;
slot_config.host_id = SPI2_HOST;

// Mount
esp_vfs_fat_sdspi_mount_config_t mount_config = {
    .format_if_mount_failed = false,  // NEVER auto-format (see §8)
    .max_files = 5,
    .allocation_unit_size = 16 * 1024,
};
esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
```

### 3.3 Kconfig

- `CONFIG_FATFS_*` — defaults are fine; consider `CONFIG_FATFS_LONG_FILENAMES=y`
- No special SDMMC Kconfig needed for SPI mode

### 3.4 SPI bus sharing — critical design point

The SD card shares SPI2 with the Himax. **All SD access must hold a mutex that excludes Himax SPI traffic.**

Existing code:
- `eyes.c` initializes SPI2 and parks SD CS high (`eyes.c:241-247`)
- The `sscma_client` uses SPI2 for Himax transactions

Proposed:
- Create a `spi2_bus_mutex` (FreeRTOS mutex) in a shared component
- Himax SPI transactions (`sscma_client_io_spi.c`) acquire it
- SD card driver acquires it for each transaction batch
- **Never hold the mutex across a blocking FATFS operation that might wait on Himax** (deadlock risk). Design SD writes as: acquire → transfer → release, with no Himax calls in between.

### 3.5 Card detect

- Poll expander P04 (`SD_DET`) via `esp_io_expander_get_level()` 
- Debounce: 3 consecutive reads, 50ms apart
- On insertion: attempt mount. On removal: unmount cleanly, mark unavailable.
- Poll interval: 2 seconds (low overhead via existing expander read path)

### 3.6 RAM impact

From `firmware/RAM_AUDIT_0.4.6.md`:
- Internal RAM baseline (0.4.4): **~11KB free**, largest block 112 bytes — critically tight
- FATFS needs: ~10KB for FAT cache + ~512B sector buffer + VFS overhead ≈ **12-15KB**
- **This does NOT fit in current internal RAM.**

Mitigations (in order of preference):
1. **PSRAM task stack migration** (already approved by John, pending): frees ~168KB internal. This alone solves it.
2. Allocate FATFS buffers with `MALLOC_CAP_SPIRAM`: FatFs supports custom memory via `ff_memalloc()`. ESP-IDF's `esp_vfs_fat` uses internal by default but can be overridden.
3. Reduce `max_files` to 3 and `allocation_unit_size` to 8KB to minimize buffers.

**Dependency:** The PSRAM stack migration (or equivalent internal RAM relief) should land before or with SD support. Alternatively, force all FATFS allocations to PSRAM via `ff_memalloc` override.

---

## 4. File Organization

### 4.1 Directory structure

```
/sdcard/
  watcher/
    captures/
      cam_20260929_220000_001.jpg
      cam_20260929_220005_002.raw8    # 1296x972 RAW8 from Himax v0.4.4+
      cam_20260929_220005_002.raw10   # 1296x972 packed RAW10
    audio/
      ptt_20260929_220010.wav
      wake_20260929_220015.wav
    logs/
      boot_20260929_220000.log
      boot_20260929_221500.log
```

### 4.2 Timestamps — the RTC problem

The S3 has **no RTC battery**. Time sources in priority order:
1. **SNTP** (if Wi-Fi connected): wall-clock time, accurate
2. **Uptime** (always available): `esp_timer_get_time()` — monotonic since boot

**Naming convention:**
- If SNTP synced: `cam_20260929_220000_001.jpg` (YYYYMMDD_HHMMSS)
- If not synced: `cam_boot003_000123_001.jpg` (boot count + uptime seconds)
- Boot count: persisted in NVS (`sd_boot_ctr`), incremented each boot

This ensures unique filenames even without wall-clock time.

### 4.3 File formats

| Data | Format | Rationale |
|------|--------|-----------|
| Camera (current) | JPEG (`.jpg`) | Himax delivers JPEG; already compressed |
| Camera (1296x972) | RAW8 (`.raw8`) or RAW10 (`.raw10`) | Native sensor output; S3/host does demosaic |
| PTT audio | Opus (`.opus`) | 16kHz mono voice: ~24-32 kbps Opus ≈ transparent. 30s ≈ 90-120KB vs 960KB WAV. Verify `esp-opus` component builds for S3 during implementation; fallback to WAV if blocked. |
| Wake-word audio | Opus (`.opus`) | Same as PTT |

**Why Opus:** Best quality-per-bit for voice. Handles 16kHz mono natively. Low encode overhead on S3 (fixed-point). If the ESP-IDF Opus component proves problematic, fallback is WAV (simple, large) — not ADPCM-on-card (saves little vs complexity).

**Audio format details** (verified from source):
- PTT: 16kHz, 16-bit, mono I2S (`ptt.c:45,233-234`). Currently stored as ADPCM in PSRAM (`ptt.c:154,345`).
- Wake-word: 16kHz (`wakeword.c:36`).
- WAV header: 44 bytes, PCM. For ADPCM recordings, either decode to PCM before writing or store as WAV-ADPCM (simpler: decode to PCM; ADPCM→PCM is trivial).

### 4.4 Rotation / retention

- **No auto-delete.** 32GB card is plenty for now (John, 2026-09-29).
- When the card fills up: stop writing, log a clear error (`"SD card full"`), surface via `dbg sd` status and MQTT event. **Tell the user.**
- Rationale: auto-deletion risks destroying evidence John wants for audit.

---

## 5. Camera Capture Saving

### 5.1 Current flow (verified from `eyes.c`)

1. `eyes_snap()` triggers Himax capture via `AT+INVOKE` or snapshot command
2. JPEG received over SPI into `snap_buf_` (512KB PSRAM buffer, `eyes.c:176`)
3. Currently: POSTed via HTTP (`eyes.c:644`) or base64-encoded for MQTT

### 5.2 Proposed SD save flow

```
eyes_snap() → JPEG in snap_buf_ → [NEW] sd_save_capture(snap_buf_, snap_len_)
  → /sdcard/watcher/captures/cam_<timestamp>_<seq>.jpg
  → return esp_err_t
```

- **Synchronous** for v1: the snapshot flow already blocks waiting for the Himax. Adding a 100-500ms SD write (for ~100KB JPEG at 1-2MB/s) is acceptable.
- **Missing/slow card:** if SD unavailable, skip silently (log at INFO). Don't fail the snapshot.
- **1296x972 RAW:** 1.26MB (RAW8) takes ~1 second at 1MB/s. Still acceptable for still capture. Queue if needed in v2.

### 5.3 Performance

| Operation | Size | Time @ 1MB/s | Notes |
|-----------|------|--------------|-------|
| 640x480 JPEG | ~50-100KB | 50-100ms | Current snapshots |
| 1296x972 RAW8 | 1.26MB | ~1.3s | Future (Himax v0.4.4+) |
| 1296x972 RAW10 | 1.57MB | ~1.6s | Future |

SPI bus is idle during SD writes (Himax not streaming), so no contention.

---

## 6. Audio Capture Saving

### 6.1 Current flow (verified from `ptt.c`)

1. PTT recording: I2S → ADPCM → `adpcm_buf_` in PSRAM (up to 30s, `ptt.c:59,154`)
2. Currently: transmitted via MQTT or played back

### 6.2 Proposed SD save flow

```
ptt_stop() → ADPCM in adpcm_buf_ → decode ADPCM→PCM → WAV header + PCM
  → /sdcard/watcher/audio/ptt_<timestamp>.wav
```

- **WAV header:** 44 bytes, standard PCM format chunk
- **ADPCM decode:** IMA-ADPCM decode is ~20 lines; well-understood
- **Size:** 30s × 16kHz × 2 bytes = 960KB max per recording
- **Wake-word:** same flow, prefix `wake_` instead of `ptt_`

### 6.3 Queuing

Audio saves happen at end-of-recording (not real-time). Synchronous write is fine — the user already waited for the recording to finish.

---

## 7. Per-Boot Log (Audit Trail)

### 7.1 What to log

```
[BOOT] fw=0.4.12 build_date=2026-09-29 reset_reason=ESP_RST_POWERON
[BOOT] boot_count=42 sntp=no (uptime-based timestamps)
[HIMAX] init: sensor=OK model=OK algorithm=OK trigger=OK
[HIMAX] sensor_opt=4 (640x480) spi_link=UP
[WIFI] connected ssid=... ip=... sntp_synced=yes
[SEAL] mode=bridged dev_verify=true floor=7659
[SD] mounted=yes card_size=29.8GB free=29.1GB
[ERROR] eyes: set_sensor opt 3 attempt 0 failed: ESP_FAIL  (if any)
[EVENT] user pressed PTT at uptime=123.4s
[EVENT] snapshot saved: /sdcard/watcher/captures/cam_....jpg
[SHUTDOWN] uptime=3600s reason=user_poweroff
```

### 7.2 When to write

- **Buffered in PSRAM** during boot (ring buffer, ~8KB)
- **Flushed to SD** at end of boot sequence (after `app_main` completes init)
- **Appended** during runtime for significant events (errors, captures, mode changes)
- **Flush on:** clean shutdown, every 60 seconds, and when buffer reaches 75% full

### 7.3 Format

**Human-readable text** (not JSON) for v1. Rationale:
- John will read these directly
- `grep`-friendly
- JSON Lines can be added later if machine parsing is needed

Each line: `[CATEGORY] key=value key=value ...`

### 7.4 Timestamp handling

- Boot log filename uses best-available time (SNTP > boot_count+uptime)
- Log lines use uptime (`[+123.456s]`) until SNTP syncs, then wall-clock
- If SNTP syncs mid-boot, note the transition: `[TIME] sntp synced, switching to wall-clock`

### 7.5 Global loglevel (John's ruling 2026-09-29)

**Loglevel is a global device setting, not per-output.** USB serial console, SD card log, and MQTT log streams all emit the same log lines at the same verbosity.

- Runtime setting via `esp_log_level_set("*", level)`, persisted in NVS (`log_level`)
- Settable via console/MQTT: `{"t":"dbg","cmd":"loglevel","level":"debug"}`
- Levels: `error` < `warn` < `info` < `debug` < `verbose` (ESP-IDF standard)
- Default: `info`
- The SD card boot log captures exactly what the USB console would show at the current loglevel — enabling headless-run debugging by pulling the card.

---

## 8. Error Handling & Edge Cases

| Case | Behavior |
|------|----------|
| **No SD card** | Boot normally. Log `"SD: no card"` to internal log. All save operations become no-ops (return `ESP_ERR_NOT_FOUND`, callers ignore). |
| **Card full** | Stop writing. Log `"SD: card full"` as error. Set a flag visible in `dbg` status. Never auto-delete. |
| **Card removed during write** | Detect via `SD_DET` (expander P04) or write error. Abort current write, unmount, mark unavailable. Resume on reinsertion. Partial file may exist — acceptable (FATFS recovers on next mount). |
| **Corrupted filesystem** | Mount fails → log error, do NOT auto-format. Expose via `dbg sd` status. John can manually reformat or we add an explicit `{"t":"dbg","cmd":"sd_format"}` command requiring confirmation. **Never auto-format.** |
| **Power loss during write** | FATFS is not journaled. Mitigations: (a) short writes, (b) `f_sync()` after each file close, (c) write to temp file then rename (atomic on FAT). Use (c) for captures: write `cam_....tmp` → rename to `cam_....jpg`. |
| **SPI bus contention** | All SD operations hold `spi2_bus_mutex`. If Himax holds it (e.g., during active streaming), SD write waits with timeout (5s), then skips. |

---

## 9. Privacy & Security

- **Encryption: out of scope for v1.** Note as future work. The SD card will contain unencrypted images and audio.
- **Physical access = data access.** Document this clearly for John.
- **Seal audit:** SD card writes SHOULD be noted in the boot/event log (e.g., `[EVENT] snapshot saved: ...`). This provides the audit trail John wants without requiring cryptographic sealing of the files themselves.
- **Future:** encrypt files with the device key before writing (AES-256-GCM, key from NVS). Separate spec.

---

## 10. Implementation Plan

### Phase 1: SD driver + mount (foundation)
- [ ] Create `sdcard` component (`firmware/components/sdcard/`)
- [ ] SPI2 bus mutex (shared with `sscma_client`)
- [ ] `sdspi` init, card detect via expander P04
- [ ] FATFS mount at `/sdcard`, `ff_memalloc` → PSRAM override
- [ ] `{"t":"dbg","cmd":"sd"}` status command (mounted, size, free)
- [ ] Handle insertion/removal gracefully
- **Depends on:** PSRAM stack migration (for internal RAM) OR `ff_memalloc` PSRAM override
- **Test:** mount/unmount, card detect, `dbg sd` output

### Phase 2: Boot log
- [ ] PSRAM ring buffer for boot events
- [ ] Flush to `/sdcard/watcher/logs/boot_<ts>.log` at end of init
- [ ] Runtime event appending + periodic flush
- [ ] NVS boot counter
- **Depends on:** Phase 1
- **Test:** verify log file created on each boot, contains expected entries

### Phase 3: Camera capture saving
- [ ] `sd_save_capture()` called from snapshot completion
- [ ] Temp-file + rename for atomicity
- [ ] Handle 1296x972 RAW8/RAW10 (large file support)
- **Depends on:** Phase 1
- **Test:** snapshot → verify file on card, verify JPEG integrity

### Phase 4: Audio capture saving
- [ ] ADPCM → PCM decoder
- [ ] WAV header writer
- [ ] Hook into `ptt_stop()` and wake-word capture completion
- **Depends on:** Phase 1
- **Test:** record PTT → verify WAV plays correctly

### Target version
- **0.5.0** (minor version bump — this is a significant new feature)
- Phase 1 could land in 0.4.13 if John wants incremental progress

### Testing checklist
- [ ] Boot without SD card → normal boot, no errors
- [ ] Insert card while running → auto-mount, `dbg sd` shows info
- [ ] Remove card during write → clean recovery, no crash
- [ ] Fill card → clear "card full" error, no data loss on existing files
- [ ] Power cycle during write → filesystem recovers on next mount
- [ ] 100 consecutive snapshots → all files present and valid
- [ ] Verify no SPI bus contention with Himax (concurrent snapshot + SD write)

---

## 11. Open Questions for John

All resolved 2026-09-29 — see "John's rulings" at the top of this spec.

---

## References

- Schematic: `~/workspace/.tmp-sch/watcher_sch.pdf` (J4 SD slot, §2.1)
- SD CS already parked: `firmware/components/eyes/eyes.c:241-247`
- SPI pin defines: `firmware/components/eyes/eyes.c:50-55`
- RAM audit: `firmware/RAM_AUDIT_0.4.6.md`
- PTT audio: `firmware/components/ptt/ptt.c` (16kHz/16-bit/ADPCM)
- Wake-word audio: `firmware/components/wakeword/wakeword.c` (16kHz)
- Snapshot flow: `firmware/components/eyes/eyes.c:473-710`
- Reset reason logging: `firmware/main/main.c:275`
