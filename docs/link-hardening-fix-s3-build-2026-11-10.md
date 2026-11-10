# Link-hardening fix round — S3 fw 0.4.51 build report

Date: 2026-11-10. Spec: `docs/link-hardening-fix-spec-2026-11-10.md`
(approved 2026-11-10 00:33 PST; §18.5 owner amendment approved 01:17 PST).
Scope: BUILD ONLY — no flash, no device commands, no bench contact.
The Himax .63 half is a separate later task; the sscma-fork tree was
not touched.

## Artifact identity

| | |
|---|---|
| File | `firmware/release/watcher-v0.4.51-merged.bin` (via `idf.py merge-bin -o <abs path>`, per standing discipline) |
| Size | 8,149,228 bytes |
| sha256 | `c748a7bb0407e1f358ba3e00cef75a3c062f77394df4c96877de481c835f8ae2` |
| App bin | `build/watcher_input_poc.bin`, 0x24a660 bytes (62 % of the 0x600000 partition free) |
| ELF SHA (app descriptor) | `018182f3226ec5ecdddd839d6968fbbb8413277f0980feb297672943ae6f7cfd` (16-char fingerprint `018182f3226ec5ec`) — read from the app descriptor of both the app bin and the merged bin at factory offset 0x20000; identical |
| Rollback | `firmware/release/watcher-v0.4.50-merged.bin` untouched (mtime 2026-11-10 00:24, pre-build) |

Artifact verification (from the built files, not the sources):
ELF `strings` contains `0.4.51` (exactly 1 standalone match) and zero
occurrences of `0.4.50`. New-code format strings present in the ELF:
`himax link episode open: trigger=%s streak=%u window60=%u total=%u%s`,
`himax gate still failing, streak=%u window60=%u total=%u drains=%u discarded=%u`,
`eyes_reset: rail=%d origin=%s key=%s`.

Build: `idf.py build` exit 0 (twice; second run forced recompilation of
all five touched files to surface warnings).

## D1 — streak resets only at exec-level completions (io_spi.c)

- Removed the counterfeit reset inside `spi_send_cmd_` (comment left
  explaining why a bare gate pass is not a transaction).
- Removed the exported `sscma_client_gate_streak_reset()` wrapper
  (verified zero callers repo-wide) and its header declaration.
- Read exec: stash-serve branch no longer resets inline; a single
  `if (ret == ESP_OK) gate_note_success_();` in the `err:` epilogue
  covers wire and stash exits exactly once.
- Available exec: wire path resets after the successful 2-byte
  clock-out, before the `*len` decode. The available DRAIN branch does
  NOT reset (D2 declares the recovery; the next ordinary completion,
  one poll later, resets).
- Bulk exec: reset inside the `ret == ESP_OK && done == size` success
  block. Write-exec ESP_OK epilogue reset kept (pre-existing site 1).
  Resync exec untouched — never resets.

**Call-site census (post-build invariant): exactly 4** call sites of
`gate_note_success_(` besides the definition (io_spi.c:509):

| Site | Line | Location |
|---|---|---|
| 1 | 784 | write-exec ESP_OK epilogue |
| 2 | 889 | read-exec ESP_OK epilogue (covers wire + stash exits) |
| 3 | 1019 | available-exec wire path, after count clock-out |
| 4 | 1903 | bulk-exec full-transfer success block |

## D2 — drain validation, as corrected in spec §0.1/§3 (io_spi.c)

(The originally-tasked CRC-frame test was unimplementable — CRC frames
exist only S3→Himax; implemented the corrected form.)

- `spi_drain_capture_` now reports its exit cause (`drain_exit_t`,
  io_spi.c:574–578): LOW / TIMEOUT / FULL / XMIT.
- Available-exec drain branch (io_spi.c:~950–1010) declares a recovery
  only if: exit == LOW, the segment remaining after stripping the
  trailing single-value 0xFF/0x00 idle run is ≥ 2 bytes, and the count
  decoded raw from the segment's first 2 bytes is ≤ DRAIN_STASH_CAP
  (4096, no 0xFFFF special case). A declared recovery increments
  `rec_drain`, returns the STRIPPED segment via the stash, and does
  not reset the streak (D1). Anything else: capture discarded
  wholesale (stash emptied), `gate_drain_discards_` incremented when
  anything was captured, original timeout stands. No per-discard log.
- New counters/accessors beside the streak state (io_spi.c:460s, 530s):
  `gate_failures_total_`, `gate_drain_discards_`, slotted window
  (below); header declarations + docs in `sscma_client_io.h`.

## D3 — himax_link immediately after fw (link.c)

The WI-1 block moved from after `stack_hwm` to immediately after the
`fw` AddString (link.c:~897–928), with a comment recording why it must
not move back. Char-budget construction (cJSON serialization order =
insertion order): prefix through `"fw":"0.4.51",` = 49 chars (6-digit
uptime). A representative serialized object —
`{"state":"down","streak":25,"gen":3,"rec_drain":12,"rec_reset":1,"rec_rst":0,"rec_rail":0,"last_rung":"drain"}` —
closes at char 172; worst-case widths (state "recovering", last_rung
"external", 5-digit counters) close ≈ char 195. Either way the object
ends ≥ 95 chars inside the bridge's ~290-char truncation point —
versus ~char 370 (always truncated) at the old position.

## G1 — rate trigger, FSEO rungs, episode-age floor, dwell close (eyes.c + io)

- Io layer: `gate_note_failure_` (the single failure funnel) now also
  increments the monotonic total and a slotted 60 s window (6 slots ×
  10 s, epoch-tagged, stale-epoch zeroing, uint16 saturation);
  accessors `sscma_client_gate_failures_window()` / `_total()`.
- Supervisor (eyes.c:779–780 defines; predicate :808): episode opens
  when `streak >= 25 || window >= 60`. Rung conditions read
  (streak OR FSEO), FSEO = total − total-at-open; the 1000 ms floor
  runs from episode open (`ep_open_ms`) everywhere — the old
  streak-age form could never be met by a flap.
- Episode health predicate `link_episode_unhealthy_`: streak > 0, or
  the streak-0 dwell (LINK_HEALTH_DWELL_MS 2000) has not completed.
  `link_wait_bus_free_` was rewritten on it (maintains the healthy
  mirror per 500 ms slice; returns false only when the dwell completes
  or the 65 s bound expires busy) and the post-wait re-check uses the
  predicate — the old single-sample streak-0 bail is gone. Episodes
  close only on dwell completion.
- Open log line: `himax link episode open: trigger=streak|rate
  streak=%u window60=%u total=%u`. Heartbeat state rider:
  `eyes_himax_link_info` reports "down" for the whole episode via the
  `link_episode_active_` mirror, not the instantaneous streak.

## G2 — poll summary re-keyed (sscma_client_ops.c:~246–266)

Summary condition is now `(streak > 0 || window > 0) && total/100 >
bucket` on the MONOTONIC io total; the bucket is never reset on
success (the `poll_summary_bucket_ = 0;` in the success path was
removed). Line format exactly as specced:
`himax gate still failing, streak=%u window60=%u total=%u drains=%u discarded=%u`.

## G3 — eyes_reset rail exactly-{0,1} (link.c + send_command.py)

Firmware (link.c:2356–2382): rail = 1 iff the `rail` key is a cJSON
number with `valuedouble == 1.0`; absent key keeps origin defaults
(serial 1 / remote 0); any other type/value → 0. The handler logs
`eyes_reset: rail=%d origin=mqtt|serial key=absent|number|other`
before executing. The stale "Serial console only" sentence in the
handler comment was corrected (the v0.4.50 origin-policy paragraph
below it is unchanged and now accurate).

Host (send_command.py:102–115): dedicated eyes_reset branch ahead of
the generic dbg return — keys ⊆ {t,cmd,rail}; rail must be a strict
int in {0,1} with bool explicitly excluded. Unit check (importing the
module and calling `allowed()`): bare, rail 0, rail 1 → accepted;
rail true / 2 / "1" / extra key → rejected; neighbouring commands
(wakeword on=1, ping) unaffected — 10/10 expectations met after
correcting one bad test expectation of mine (`dbg`/`status` was never
in DBG_ALLOWED, before or after this change; verified against HEAD).

## G4 — crediting hygiene (eyes.c)

- Episode open resets `last_rung_try_ms = 0` and snapshots
  `ep_open_gen = link_gen_`; `ep_credit_gen` starts 0.
- Rung successes (2/3/4) snapshot `ep_credit_gen = link_gen_` AFTER
  the rung call returns, so a generation the rung itself produced is
  inside its snapshot and any later move voids it.
- Close booking: a rung keeps credit iff it is one of reset/rst/rail
  AND `link_gen_ == ep_credit_gen`; else if `link_gen_ != ep_open_gen`
  the close books **external** (`last_rung_ = "external"`, no rec_*
  counter incremented); else "none". The fast-relapse test is
  unchanged and deliberately not extended to external closes.

## Owner amendment (§18.5) — rung ≥3 minimum re-fire spacing (eyes.c:1040–1058)

The rungs-3/4 backoff gate now uses an effective minimum gap of
`max(fail-backoff, LINK_BACKOFF_1_MS /* 5 s */)` whenever
`last_rung_try_ms != 0`, so a procedurally-successful-but-unhealing
rung can no longer re-fire at procedure cadence (~2 s); the
exponential fail ladder (5 s → 15 s → 60 s) still dominates from the
second consecutive failure on. Latch pacing (rail-capped 10-minute
RST retries) is untouched and still dominates when active.

## Version

`FW_VERSION` "0.4.50" → "0.4.51" (main/main.c:35). Exactly one
standalone "0.4.51" string in the ELF; no "0.4.50" anywhere in it.

## Deviations from the spec

**None.** Every S3 item (D1, corrected D2, D3, G1, G2, G3 both halves,
G4) and the §18.5 owner amendment are implemented as specced. Two
implementation notes, neither a deviation:

1. The spec's D3 estimate said the object ends "≈194 chars"; my
   measured construction (above) brackets 172–195 depending on
   runtime counter widths and state strings — the spec's figure sits
   inside that range, and the invariant that matters (object fully
   inside the ~290-char budget) holds at every width.
2. `sscma_client_gate_streak_age_ms()` is now unreferenced by the
   supervisor (the floor is episode-age based, per spec) but remains
   exported for observability callers; it is a public (non-static)
   symbol, so this produces no warning and no behaviour change.

## Warnings (honest accounting)

Build exit 0. Warnings in the five touched files — all pre-existing
dead code outside every edited region; **no new warnings introduced**:

- io_spi.c: `himax_ready_gpio_init_`, `sscma_client_spi_park`,
  `sscma_client_spi_unpark` defined but not used (:56, :1298, :1308).
- ops.c: unused variable `res` (:557).
- eyes.c: `analyze_frame`, `stream_png_b64`, `png_sink_append`,
  `stream_frame_b64` defined but not used (:1820–:2501).
- link.c, main.c: none.
- Toolchain noise (pre-existing): esptool `--flash-*` deprecation
  notices; Nimble Kconfig `default 0` note.

Build log tail:

```
[100%] Linking CXX executable watcher_input_poc.elf
[100%] Built target watcher_input_poc.elf
[100%] Generating binary image from built executable
Creating ESP32-S3 image...
Merged 2 ELF sections.
Successfully created ESP32-S3 image.
Generated .../firmware/build/watcher_input_poc.bin
watcher_input_poc.bin binary size 0x24a660 bytes. Smallest app
partition is 0x600000 bytes. 0x3b59a0 bytes (62%) free.
Project build complete.
```

## Not done here (by design)

No flash, no device commands, no bench contact. Next steps remain as
sequenced in the fix spec's acceptance order, each gated on John's go:
0.4.51 flash → dead-Himax drain test → FH-2 UART capture → Himax .63
build → soak re-run → D3 heartbeat sighting proof.
