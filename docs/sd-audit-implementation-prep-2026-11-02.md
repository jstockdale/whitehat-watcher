# SD Audit Log — Implementation Prep (2026-11-02, background prep, NOT applied)

Prepared while the bench was busy with the bridge-reassembler verification.
Purpose: reconcile the two existing SD designs against the firmware as it
actually stands on 0.4.22, so the SD build can start from verified facts
instead of re-discovering them. No code was written or changed.

Source designs:
- `docs/sd-card-spec.md` (2026-09-29, John's rulings at its top)
- `docs/sd-capture-audit-design-2026-11-02.md` (2026-11-02, John's rulings)

## Production context: the queue ahead of SD is now clear

- 0.4.22 chunk-loss verification: 5/5 passed 2026-11-02 (RCA closed for
  the bridged path).
- Tiered bridge reassembler (John's "save a partial for any rows at all"
  ruling): implemented, 17/17 offline checks, installed 2026-11-03
  00:01 UTC, and **production-verified**: live snaptest `_mid 76cfdbc1`
  started 16:02:15 PT, device reported 1,379,453 bytes / 172 chunks,
  new handler saved `snap-20261102-160535-0.png` at 16:05:35 PT at
  exactly 1,379,453 bytes (COMPLETE path; partial path is offline-tested
  only, by design).
- Step 0 split-timer baseline is done (pure deflate CPU ≈ 5.4 s of a
  ~159 s wall), which was the Nov 2 design's stated gate for SD work.

## Where the two designs conflict — John's Nov 2 rulings win, two items need a fresh call

| Topic | Sep 29 spec | Nov 2 design | Prep verdict |
|---|---|---|---|
| Target version | **0.5.0** (John's ruling #3: "0.4.x stays the known-good path") | "its own feature build (0.4.21+)" | **CONFLICT — needs John's call.** 0.4.21/0.4.22 were spent on chunk fixes; SD is either 0.4.23 (breaks the Sep ruling) or 0.5.0 (honors it). Prep assumes 0.5.0 until John says otherwise. |
| Audio format | Opus (John's ruling #4) | AAC .m4a 64 kbps, Shine MP3 fallback, WAV stopgap (John, Nov 2) | Nov 2 supersedes — it was an explicit, later, format-specific call. |
| Scope | Captures **plus per-boot log** to SD (§7), global loglevel (§7.5) | Captures only; boot log not mentioned | **OPEN — needs John's call.** Boot log was a Sep requirement, not a Sep ruling that Nov 2 addressed. Don't silently drop it; either include Phase 2 from the Sep plan or get an explicit defer. |
| Layout / naming | `/sdcard/watcher/{captures,audio,logs}`, timestamp names w/ boot-count fallback | `/sdcard/audit/`, `img-000123.png` seq names, `index.jsonl`, `config.json` on card | Nov 2 supersedes for captures (seq + index is the audit-grade design). Sep layout only survives if the boot log is included. |
| Card full | Stop, log, surface, never auto-delete (ruling #1) | Stop auditing, sticky `audit_state:"full"`, never overwrite | **Agree** — same ruling, restated. |
| Encryption | Unencrypted v1 fine (ruling #2) | Silent (index carries SHA-256) | Sep ruling stands; Nov SHA-256 index is compatible. |

## Hardware facts the Nov 2 design does not mention (from Sep spec, schematic-verified)

These are the constraints most likely to bite during implementation:

1. **SPI mode only.** DAT1 is unconnected — 4-bit SDMMC is impossible.
   Use `sdspi` on SPI2_HOST, ≤20 MHz (10 MHz probing/init).
2. **The SD slot shares SPI2 with the Himax** (SCK GPIO4, MOSI GPIO5,
   MISO GPIO6; SD CS = GPIO46, Himax CS = GPIO21). SD and Himax cannot
   transact simultaneously. Current firmware parks SD CS high at
   `firmware/components/eyes/eyes.c:479-485` (`PIN_SD_CS 46`,
   eyes.c:58) with the comment "SPI2 bus shared with the SD card."
3. **Card detect** is expander PCA9535 P04 (`SD_DET`, polarity to verify
   on hardware); **SD power rail** is `EXP_SD_EN` via Q12, already on at
   boot; a rail power-cycle may be needed to re-init a wedged card.
4. **Throughput** in SPI mode ≈ 1–2 MB/s sequential write. A 1.4 MB PNG
   is ~0.7–1.4 s of card time. Against the *bridged* chunk arrival rate
   (~8.2 KB per ~0.95 s ≈ 8.6 KB/s) the Nov design's 512 KB PSRAM queue
   is ample. Against a future native-MQTT arrival rate it is not the
   queue that saves you — it's that the writer pauses during Himax bulk
   transfers (next point).

## Bus-contention recommendation (the one real design addition)

The Nov design's writer task drains the queue to FAT independently.
On this hardware that drain uses the same SPI2 bus the Himax bulk
transfer uses. ESP-IDF's bus driver arbitrates *transactions* between
devices, and `sscma_client_io_spi.c` has its own per-device lock — but
the Himax bulk path is timing-sensitive (poll/handshake around staged
transfers), so interleaving SD transactions mid-bulk is an unforced
risk. Meticulous v1: **the audit writer only runs when no Himax
capture/bulk transfer is in progress** — gate it on the same
busy state the eyes component already tracks, drain between captures.
Captures are minutes apart on the bench; the queue absorbs the wait,
and the fail-open rule (mark `complete:false`, never block a capture)
already covers the pathological case. Revisit only if native mode makes
captures back-to-back.

## RAM: the Sep spec's blocker has (mostly) lifted — measured, not assumed

- Sep spec §3.6: internal RAM ~11 KB free (0.4.4) vs FATFS needing
  ~12–15 KB internal → "does NOT fit", gated on PSRAM task migration.
- **Now:** PSRAM task stacks have landed (`task_psram` component;
  snaptest runs a 16 KB PSRAM stack). Live heartbeat on 0.4.22,
  2026-11-02 16:03–16:05 PT: `internal_free: 45474`,
  `internal_largest: 21504`, `psram_free: 3820620`.
- Verdict: FATFS *probably* fits in internal RAM now, but the margin is
  one large allocation away from fragmentation trouble. Keep the Sep
  spec's `ff_memalloc` → PSRAM override as the plan; it costs nothing
  and removes the question. Set `max_files` small (3) per Sep §3.6.

## Code survey (verified 2026-11-02 against the live tree)

- **No SD code exists**: no `sdcard` component; components are display,
  eyes, hid_ble, input, led, link, mode, net, pki, ptt, seal,
  sscma_client, task_psram, timer, touch, wakeword, wheel.
- **No SNTP anywhere** in firmware. Hook point exists: `net.c:48`
  already handles `IP_EVENT_STA_GOT_IP` — SNTP start belongs there.
- **PNG tee point**: `png_streaming_sink_cb` (eyes.c:1721) and the
  snaptest chunk path `on_snaptest_chunk` (main.c:233). The Nov design's
  "tee at the PNG sink" is exactly eyes.c:1721; note the dbg-png mirror
  sink at eyes.c:1677 must tee too (Nov open Q2 recommendation: audit
  every image path).
- **Audio source**: PTT is 16 kHz/16-bit/mono, stored as IMA-ADPCM in
  PSRAM (`ptt.c:154,346`). An AAC/MP3 audit file therefore needs a
  decode-ADPCM→PCM step before the encoder — the Sep spec §6.2 flow
  (decode → encode) is the right shape; only the encoder changed.
- **No AAC encoder is currently in the tree**: `managed_components/`
  has esp-sr, esp_codec_dev, esp_new_jpeg, etc. — no esp-aac and no
  Shine. Budget real time for adding/vendoring one; WAV stopgap
  (John-approved fallback) de-risks the schedule: ship the audit path
  with WAV, swap the encoder behind the writer when it builds clean.
- **Heartbeat** already carries `internal_free`/`internal_largest`
  (link.c) — adding `sd_present`/`sd_free_mb`/`audit_seq`/`audit_state`
  per the Nov design follows an existing pattern.

## Suggested build sequence when John green-lights it

1. `sdcard` component only: sdspi mount on SPI2, card-detect poll,
   `ff_memalloc`→PSRAM, `dbg sd` status. No capture hooks. Verify:
   mount with the 32 GB card in, `dbg sd` reports size/free, bench
   snaptest still passes 5/5-style (bus contention smoke test).
2. PNG tee + writer (gated on Himax-idle) + seq config/index per Nov
   design. Verify against the reassembler: SD bytes == device bytes
   == bridge-saved bytes on a live snaptest.
3. SNTP on got-IP + `time_src` in index.
4. Audio: WAV stopgap first (PTT + wake-word), then AAC/Shine swap.
5. Boot log: only if John confirms the Sep scope still stands.

Decisions needed from John (one line each): **(a)** version — 0.5.0 per
his Sep ruling, or 0.4.23? **(b)** boot log — in v1, deferred, or dropped?
