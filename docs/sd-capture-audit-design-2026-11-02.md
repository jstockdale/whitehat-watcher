# SD Capture Audit Log — Design (2026-11-02)

John's feature: **every capture the Watcher makes — PNG images and audio —
is also written to the device's local SD card**, forming an on-device audit
log of what the device captured. The console Privacy Log represents
"everything the watcher captured this session"; this SD log is the durable,
device-side source of truth behind it.

## John's decisions (2026-11-02)

1. **Card:** 32 GB microSD is installed in the device.
2. **Full card:** STOP auditing. Log it, surface it in telemetry/heartbeat,
   and tell the user to free up space or install a bigger card.
   Never ring-overwrite (an audit log that silently eats its oldest
   entries is not an audit log).
3. **Time:** Bring up SNTP on the S3 over device Wi-Fi. When wall-clock
   time is unavailable, entries fall back to sequence numbers (+ uptime).
4. **Sequence:** A monotonically increasing sequence number names every
   capture. The current sequence value is stored in a **config file on
   the card**; with no config present, start at `000000`.
5. **Audio scope:** Record **both** PTT captures **and** wake-word trigger
   audio.

## Layout on card

```
/sdcard/audit/
    config.json          {"next_seq": 123, "schema": 1}
    index.jsonl          append-only, one JSON object per capture
    img-000123.png
    aud-000124.adpcm     (or the native capture format; see Open Q1)
```

Index entry (one line per capture):

```json
{"seq":123,"type":"image","trigger":"snaptest","bytes":1548180,
 "sha256":"<hex>","complete":true,"uptime_ms":475610,
 "ts":1730736000,"time_src":"sntp"}
```

- `trigger`: `snaptest` | `dbg_png` | `ptt` | `wakeword`
- `complete:false` marks a partial audit copy (writer queue overflow or
  card error mid-write). The file is kept and flagged, never silently
  truncated without a record.
- `time_src`: `sntp` | `none` (entry uses seq + uptime only).
- `sha256` is computed incrementally by the writer task as bytes land.

## Write path (tee, never block the live path)

- The PNG sink callback and the audio source each see their bytes exactly
  once. They `memcpy` into a PSRAM queue (bounded, e.g. 512 KB) and return
  immediately. A dedicated **audit writer task** drains the queue to FAT.
- If the queue fills (slow card), the audit copy is dropped for that
  capture and the index records it as `complete:false` with a
  `drop_reason`. **Captures never fail or slow down because of the SD
  card** — same rule as the Himax: a peripheral must not break the main
  function.
- Card absent / mount failure at boot: auditing disabled for the boot,
  heartbeat reports `sd_present:false`, everything else works normally.

## Sequence integrity

- FAT is not transactional, so `config.json` is written via
  write-temp-then-rename (`config.tmp` → `config.json`).
- The sequence is committed to config **after** the capture file closes.
- Self-healing at boot: scan `/audit/`; effective next sequence =
  `max(config.next_seq, highest seq on disk + 1)`. A crash between file
  close and config commit can therefore never reuse a number.
- Rollover at 999999: stop auditing and surface it (same user-facing
  path as a full card). At ~1.5 MB/PNG, a 32 GB card fills (~21k images)
  long before 6 digits run out; wake-word clips are small but frequent,
  so the guard still exists.

## Card full

- Writer task checks free space before each capture (and `fwrite`
  results during it). On ENOSPC: stop auditing, set a sticky
  `audit_full` flag, log it, heartbeat reports
  `audit_state:"full"` so the console can show the user:
  "SD audit stopped — card full. Free space or install a larger card."
- Auditing resumes only on reboot with space available (deliberate:
  no silent mid-session resume after partial cleanup).

## SNTP (new)

- Start SNTP when device Wi-Fi reaches got-IP (native mode). In bridged
  mode (no device Wi-Fi) there is no time source — entries use
  `time_src:"none"`. This is expected, not an error.
- Side effect to note: the MQTT seal currently runs `ts_window=0`
  because the S3 had no clock. With SNTP, a timestamp window becomes
  *possible* later — separate decision, not part of this feature.

## Telemetry / console

- Heartbeat gains: `sd_present`, `sd_free_mb`, `audit_seq`,
  `audit_state` (`ok` | `full` | `nocard` | `off`).
- Each completed audit write emits a telemetry notice so the console
  Privacy Log can show SD-saved captures for the session as they happen.
- Future (not v1): a sealed `dbg` read of the index so the console can
  render the device's full audit history, not just this session's.

## Capacity sanity (32 GB)

- PNG ≈ 1.3–1.6 MB → roughly 20,000 images to fill the card.
- Wake-word/PTT audio (ADPCM, seconds per clip) → negligible by
  comparison. Card-full is a "someday" event, but the handling is
  specified now, per John's ruling.

## Open questions

1. **Audio file format:** RESOLVED (John, 2026-11-02) — compressed and
   universally playable: **AAC (.m4a) at 64 kbps** via Espressif's
   encoder as first choice; **Shine MP3** (tiny fixed-point, open
   source) as fallback; WAV only as a stopgap if both encoders become
   a yak-shave (a 5 s clip is ~160 KB, negligible on 32 GB). Raw ADPCM
   rejected: poor playability for a human-reviewed audit artifact.
2. **Scope check — `dbg png` / `strip` / `crop` variants:** audit every
   image-producing path, or snaptest-family only? Recommendation: every
   path that produces an image (the tee sits at the PNG sink, so this is
   the natural outcome anyway).
3. **Retrieval:** v1 is card-out-of-device reading only. In-device read
   (sealed dbg fetch) is a follow-up.

## Sequencing

Lands as its own feature build (0.4.21+) AFTER the Step 0 split-timer
measurement on 0.4.20, so the optimization baselines stay clean. The tee
adds a memcpy per chunk to the sink path; its cost will be visible in
`t_cb_us` when it ships, which is exactly what the timers are for.
