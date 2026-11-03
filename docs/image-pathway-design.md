# Watcher Image Pathway Design

**Version:** 2026-10-06  
**Status:** v0.4.18.37 implements PNG snaptest migration

## Overview

The Watcher camera pipeline captures frames on the Himax HX6538, transfers them to the ESP32-S3 via SPI, encodes to PNG on-device (S3 ROM miniz), and streams via MQTT.

## 1. PNG on Device (S3 ROM miniz)

**Status:** ✅ Implemented (`firmware/components/eyes/png_stream.c`)

### Pipeline
```
Himax (1296x972 RAW8 Bayer)
  → SPI bulk → S3 PSRAM (1,259,712 bytes)
  → Bilinear debayer (3-row window) → RGB
  → PNG row filter (SUB)
  → ROM miniz tdefl (streaming)
  → IDAT chunks → PNG file in PSRAM
```

### Technical Details
- **Encoder:** S3 ROM miniz (`tdefl_compress_buffer` at fixed ROM address)
  - MINIZ_NO_ZLIB_APIS set — only low-level `tdefl` API available
  - `mz_crc32` maps to hardware CRC32 (`esp_rom_crc`)
- **No hardware deflate** on S3 — it's the ROM's software miniz (bare-bones, but fast enough)
- **Output:** Standard PNG (IHDR, IDAT, IEND chunks written manually)
- **Buffer:** 2MB PSRAM for PNG output (`PNG_OUT_CAP`)
- **Typical size:** 300-600KB for 1296x972 natural image (2:1 to 4:1 compression)

### API
```c
// Encode raw Bayer frame to PNG
png_stream_cfg_t cfg = {
    .width = 1296, .height = 972,
    .cfa = PNG_CFA_BGGR,
    .filter = PNG_FILTER_SUB,
    .tdefl_probes = 0,
    .idat_chunk = 8192,
};
png_stream_encode(raw_buf, &cfg, png_sink_append, &ctx, &res);
```

## 2. Himax Hardware JPEG

**Status:** ❌ Not available — skipped

### Assessment
- Himax tree contains `3rdparty/JPEGENC/` — **software** JPEG encoder (BitBank library)
- HX6538 WE2 (Cortex-M55 + Ethos-U55 NPU) has **no dedicated hardware JPEG encoder**
- Software JPEG on M55 would be slow and consume NPU budget

### Decision (John, 2026-10-06)
Use S3 PNG path instead. PNG is lossless (meets "minimal quality loss" requirement) and we already have the encoder.

## 3. Snaptest → PNG Migration

**Status:** ✅ Implemented in v0.4.18.37

### Before (JPEG)
```
dbg snaptest
  → sscma_client_invoke() → Himax captures JPEG via SSCMA
  → JPEG in snap_buf_ (512KB)
  → analyze_frame() checks FFD8/FFD9
  → stream via snaptest_chunk_cb_
```

### After (PNG)
```
dbg snaptest
  → snap_grab_frame_png_()
    → AT+SPIDUMP → SPI bulk → raw frame (1.2MB)
    → PNG encode via ROM miniz → snap_buf_ (1MB)
  → analyze_frame() checks PNG signature (89504E47)
  → stream via snaptest_chunk_cb_ (unchanged)
```

### Changes
1. **`eyes.h`**: Updated `eyes_snaptest_result_t` comments (JPEG → PNG)
2. **`eyes.c`**: 
   - `analyze_frame()`: Check PNG signature (89 50 4E 47) instead of JPEG SOI (FFD8)
   - `analyze_frame()`: Check IEND chunk instead of JPEG EOI (FFD9)
   - New `snap_grab_frame_png_()`: SPIDUMP + bulk + PNG encode
   - `snaptest_task()`: Call `snap_grab_frame_png_()` instead of `snap_grab_frame_()`
   - `SNAP_BUF_SIZE`: Increased 512KB → 1MB (PNG larger than JPEG)
3. **HTTP snapshot path**: Unchanged (still uses JPEG via INVOKE)

## 4. MQTT Chunked PNG Transfer

**Status:** ✅ Implemented (existing `png_chunk` events)

### Protocol
```json
// Each chunk:
{
  "t": "png_chunk",
  "seq": 0,
  "total": 150,
  "last": false,
  "b64": "<base64-encoded 768-byte chunk>"
}

// Terminal reply after last chunk:
{
  "t": "dbg",
  "cmd": "png",
  "_mid": "<id>",
  "ok": true
}
```

### Chunking
- **Chunk size:** 768 bytes raw → ~1024 chars base64
- **1296x972 PNG (400KB):** ~150 chunks
- **640x480 PNG (100KB):** ~50 chunks

### Encryption
**Requirement (John, 2026-10-06):** All image data encrypted with telemetry key.

Current status: `png_chunk` events go via `link_send()` → MQTT. 
**TODO:** Verify `link_send()` applies WMS2 encryption (BLAKE2b-MAC + XChaCha20-Poly1305) to `png_chunk` events, or add it.

The telemetry stream already uses:
- BLAKE2b-MAC for authentication
- XChaCha20-Poly1305 (kind=1) for encryption
- Same telemetry key (`~/.config/watcher/tel_seed`)

Image chunks should use the same scheme on the `watcher/image` topic (or piggyback on the existing event encryption).

### Host Reassembly
```python
# Pseudocode for host-agent
chunks = {}
for msg in mqtt_stream:
    if msg['t'] == 'png_chunk':
        chunks[msg['seq']] = base64.b64decode(msg['b64'])
        if msg['last']:
            png_data = b''.join(chunks[i] for i in range(msg['total']))
            # Verify PNG signature
            assert png_data[:8] == b'\x89PNG\r\n\x1a\n'
            open('capture.png', 'wb').write(png_data)
```

## 5. SD Card Capture (Roadmap)

**Status:** 📋 Planned — not yet implemented  
**Requirement (John, 2026-10-06):** Capture EVERY snap and EVERY audio recording to the built-in SD card. Enabled by default. User can disable in Setup.

### Purpose (John, 2026-10-06)
1. **Convenience:** User can easily access captures without MQTT transfer
2. **Audit trail:** Complete local record of what the device has captured — what it saw (images) and heard (audio). Critical for trust/transparency in an AI assistant that observes the user's screen and environment.

### Use Cases
- **Snaps:** Every `dbg png`, `dbg snaptest`, and HTTP snapshot saves to SD
- **Audio:** Every audio recording saves to SD (when audio pipeline lands)
- High-volume capture (timelapse, dataset collection)
- Offline operation (no MQTT/WiFi)
- Debug (inspect frames without transfer overhead)

### Design (Proposed)
```
# Snaps (PNG)
dbg png → /sdcard/captures/snap_YYYYMMDD_HHMMSS.png

# Audio (when implemented)
audio record → /sdcard/audio/rec_YYYYMMDD_HHMMSS.wav (or .opus)
```

### Configuration
- **Default:** Enabled
- **Setup UI:** Checkbox "Save captures to SD card" (checked by default)
- **Device setting:** `sd_capture_enabled` (bool, persisted in NVS)
- When disabled: skip SD write, only do the MQTT/streaming path

### Requirements
- SD card mounted via SDMMC (check current S3 SD support)
- FAT32 filesystem
- Directories: `/sdcard/captures/` (images), `/sdcard/audio/` (recordings)
- Filename: `snap_YYYYMMDD_HHMMSS.png` or sequential if RTC not set
- Handle card full / write errors gracefully (log warning, continue without SD)
- Handle card missing (log once, continue without SD)

### Future Enhancements
- `dbg sd_list` — list captures on card
- `dbg sd_get <filename>` — transfer specific file via MQTT chunks
- `dbg sd_delete <filename>` — free space
- Auto-delete oldest when full (configurable)

## Version History

| Version | Changes |
|---------|---------|
| 0.4.18.36 | Fixed SPI AT MISO-flag bug (`client_io_spi_write`) |
| 0.4.18.37 | Migrated snaptest from JPEG to PNG; increased SNAP_BUF_SIZE to 1MB; refactored base64 chunking into shared helper |
| 0.4.18.38 | Made MISO-flag timeout fatal (was non-fatal for Himax <0.4.20.38 compat). If Himax not RX-armed, fail fast instead of sending into void during Himax reboot. |
| 0.4.18.39 | Fixed `spi_sync_tx_ready_()` to return error on MISO HIGH timeout instead of blind-clocking. Prevents S3/Himax protocol desync during bulk transfer. |
| 0.4.18.40 | Mode discovery now retries `AT+SENSORS?` up to 3 times on timeout. Fixes "0 modes known" after Himax slow boot. |
| 0.4.18.41 | Reverted the SENSORS? bypass (per John's feedback: the full table is useful for mode support negotiation). Keeps the v0.4.18.40 retry logic. |
| 0.4.18.42 | Fixed `client_io_spi_available()` to check MISO before sending CMD_AVAILABLE. If HIGH (Himax TX ready), skip the poll and return 4096 to trigger direct read. Fixes 30s AT response delay over SPI. |
| 0.4.18.43 | Changed the HIGH-case return from 4096 to 512 bytes. The 4096-byte over-read left the Himax TX state confused (stuck HIGH), blocking bulk transfer. Reading in 512-byte chunks lets the process task drain the TX buffer naturally until Himax drives LOW. |
| 0.4.18.44 | Changed HIGH-case return from 512 to 2048 bytes. The 512-byte chunks were too small — the AT+SENSORS? response (~1-2KB) was fragmented across multiple reads, causing timeouts. 2048 is enough for AT responses without over-reading. Also fixed the response parser to find the NEAREST `\r{` prefix before the `}\n` suffix (was finding the FIRST, causing invalid JSON when stale fragments preceded valid responses). |
| 0.4.18.45 | Reverted the `available()` MISO check (v0.4.18.42-44). The background task's 2048-byte read on HIGH interfered with foreground spitest/bulk transfers, causing data corruption (spitest 3/16) and bulk timeouts. Keeps the parser fix (nearest prefix) which correctly handles concatenated responses without interference. Added synchronous TX drain in `spi_send_cmd_()`: if Himax is HIGH when a command needs to be sent, drain the TX FIFO (64-byte chunks until LOW, 2s timeout) instead of failing. This handles pending AT responses before bulk transfers without background-task interference. |
| 0.4.18.46 | Fixed the drain to be called explicitly at the START of bulk transfers and spitest, NOT automatically in `spi_send_cmd_()`. The automatic drain was discarding BULK data mid-transfer (HIGH is expected between bulk commands). Now: `client_io_spi_read_bulk()` and `sscma_client_io_spi_test_pattern()` drain the TX FIFO (if HIGH) before starting, then proceed with the normal protocol where HIGH is expected and handled correctly. |
| 0.4.18.47 | Added MISO check to `available()`: if HIGH, return 256 (not 0, not 2048). The 256-byte chunk triggers a small synchronous read via the background task. Small enough to avoid Himax TX confusion and background/foreground interference. The parser accumulates fragments (with nearest-prefix fix) until complete JSON. Process task skips available() during bulk (bulk_active), spitest drains at start. This should fix the 30s AT delay without breaking bulk/spitest. |
| 0.4.18.48 | **Root cause fix from verification agent:** Removed the 2ms `wait_delay` before the 2-byte count clock in `client_io_spi_available()`. The Himax re-arms RX (LOW) 1.5ms after arming the count; the S3 was clocking at 2.1ms, reading 0x0000. Now clocks immediately like the bulk path (`spi_bulk_poll_available_`). Also fixed heap over-read: `strncmp(next_req->cmd, name->valuestring, sizeof(cmd))` → `strcmp()` (`ops.c:361`). The 32-byte strncmp read past the 9-byte heap string. Reverted the v0.4.18.47 MISO-check (return 256) — the timing fix addresses the root cause; the MISO check was based on the wrong theory. |
| 0.4.18.49 | Added `test_active` flag to prevent background AT polling during spitest. The v0.4.18.48 timing fix made `available()` work correctly, but the background task now interferes with spitest (3/16). The flag is set in `sscma_client_io_spi_test_pattern()` and checked in `client_io_spi_available()` (returns 0 immediately if set). |

## Open Questions

1. **Encryption:** Does `link_send()` encrypt `png_chunk` events with telemetry key? (Need to verify)
2. **Performance:** PNG encode time for 1296x972 on S3? (Measure — if >30s, consider lower resolution default)
3. **SD card:** Does current S3 firmware support SDMMC? (Check)
