# Himax link-hardening build — fw 0.4.20.62 (WI-4 + WI-5)

**Status: BUILT AND VERIFIED. NOT flashed** (Himax flashes are the owner's run).
Built 2026-11-09/10 from the 0.4.20.61 base (working tree confirmed identical to the
clean-copy pin `9e34913`, incl. the .61 `setCropWindow` buffer-reuse fix in
`ma_camera_himax.cpp`, before any edit). All changes are in ONE file:
`sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/library/sscma_micro_porting/porting/himax/we2/ma_transport_spi.cpp`
(307 diff lines vs the pinned .61 source), plus the version string (see §4).
Spec: `docs/link-hardening-spec-2026-11-09.md` §§5–7. H-F was out of scope and is not present.

## ⚠️ Deviations from the spec — read first

1. **`spi_poll()` head check relaxed (required by H-A(2), not spelled out in the spec).**
   The poll opened with `if (!_spi_opened) return;` — which makes the Guard-3 block
   *unreachable* while the transport is closed, so the spec's "next Guard-3 block entry
   retries open+arm" could never happen. The head now returns only when
   `!_spi_opened && !_spi_rx_arm_failed`. The rest of the poll body is inert in that
   state (Guard 1 needs `_spi_tx_done`, Guard 2 needs `_spi_tx_staged`, H-B requires
   `_spi_opened`, the bulk feeder requires `_spi_bulk_armed` — all false after a failed
   teardown), so Guard 3 is the only block that acts. Without this change H-A(2) is dead code.
2. **H-B's predicate gains an armed-state term the spec's literal flag list lacks.**
   The spec lists the predicate as `opened && !staged && !done && !arm_failed &&
   !bulk_armed` — but that conjunction is also true in *normal idle* (RX DMA armed,
   nothing in flight), so a literal implementation would tear the link down every ~2 s,
   contradicting the spec's own healthy-path test (§6.3: zero STUCK_UNARMED lines).
   The missing signal is "is an RX DMA actually outstanding", which no existing flag
   records. Added a tracker, `_spi_rx_armed` (set on successful `spi_read_dma` in both
   arm paths; cleared at RX-callback entry, in the teardown cycle, and at deInit), and
   the predicate is the spec's list **plus `!_spi_rx_armed`**. Same intent, same 2 s bound,
   same gates — with the armed/idle state correctly excluded. Parked operation is covered
   by construction: a parked arm is a real arm, so `_spi_rx_armed` is true while parked.
3. **H-A(1) applied to `_spi_arm_rx_parked()` too.** It had the identical silent
   early-return on `!_spi_opened`. The spec's §5.2 invariant ("no reachable state with
   all claim flags false, RX unarmed, transport closed") must hold for both arm entry
   points, so both got the fix.
4. **H-C: implemented** (the spec's conditional branch). A readback API does exist —
   `hx_drv_scu_get_all_pinmux_cfg()` exposes `pin_pb2`, and `hx_drv_gpio_get_in_value()`
   reads the pad — so H-C was built as specified, with abstain-on-API-error semantics
   (only a *positive* mismatch fails the arm; a readback error changes nothing, mirroring
   the re-arm probes' dead-probe abstention). **Residual boundary, noted not fixed:**
   the spec scopes H-C to Guard 1's re-arm branch, so the one arm in `SPI::init` is not
   readback-verified; a hypothetical boot-time mux failure would still present as a dead
   flag line until the first Guard-1 cycle or an S3-side ladder reset.
5. **Guard 2 teardown semantics widened slightly by the shared helper.** Pre-.62 Guard 2
   only closed/opened when the transport was open; via `_spi_transport_cycle()` it now
   also attempts an open when already closed. Strictly recovery-positive, and with
   H-A(1) any failure is reported into Guard 3 instead of vanishing.

## 1. WI-4 (H-A + H-C) — what changed

- **H-A(1)** — `_spi_arm_rx()` and `_spi_arm_rx_parked()`: the early return is split.
  `!_spi_dev` stays silent (never initialized; nothing to escalate). `!_spi_opened`
  with a live device now sets `_spi_rx_arm_failed = true` before returning — a failed
  arm is never silent again.
- **H-A(2)** — Guard-3 block in `spi_poll()`:
  - Block head: when `!_spi_opened && _spi_dev`, one `spi_open` attempt (`_spi_try_open()`)
    precedes the arm retry; if the transport is still closed, the flag is re-set and the
    arm is skipped (the entry is counted by the existing escalation counter).
  - Teardown: on cycle success the flag is cleared and the park-aware arm runs, exactly
    as before. On cycle **failure the flag now STAYS set** and the arm is skipped — the
    pre-.62 code cleared it unconditionally at this point, which is precisely how the
    incident's unrecoverable end-state was manufactured.
  - Plus deviation 1 (head-check relaxation) so any of this is reachable while closed.
- **H-C** — `_spi_verify_arm_took()` (new helper beside `spi_poll()`), called from
  Guard 1's re-arm branch immediately after `_spi_arm_rx()`, only when the arm did not
  already report a DMA failure. Reads back the PB2 mux (`pin_pb2 == SCU_PB2_PINMUX_GPIO13`)
  and the GPIO13 pad level (must be LOW); a positive mismatch sets `_spi_rx_arm_failed`,
  handing the state to Guard 3. Post-arm placement = zero added latency on the
  S3-visible re-arm (spec invariant 2). The REARM/DIAG emit formats are untouched.

## 2. WI-5 (H-B + H-D) — what changed

- **Shared teardown** — `_spi_transport_cycle()` (close if open, then one open attempt;
  returns open-state) and `_spi_try_open()` are now the single teardown/open paths,
  used by Guard 2, Guard 3, and H-B (the factoring spec §6.1 asks for). It clears
  `_spi_rx_armed` at close; flag policy stays with each caller.
- **H-B** — new block in `spi_poll()` after Guard 2, before the bulk feeder. Predicate:
  `_spi_opened && !_spi_rx_armed && !_spi_tx_staged && !_spi_tx_done &&
  !_spi_rx_arm_failed && !_spi_bulk_armed` held continuously > 2 s (static
  `_spi_unclaimed_since_us`, reset whenever the predicate is false) → bump
  `_spi_wd_aborts` (so REARM stats count it), emit `E DIAG STUCK_UNARMED teardown`
  via the deferred diag ring, teardown via the shared cycle, park-aware re-arm.
  Explicitly gated off during bulk (`_spi_bulk_armed` term). Side benefit: any RX
  callback path that returns without arming or staging is now also covered instead
  of persisting silently.
- **H-D** — at the very head of `spi_poll()`, before the early return: static
  last-call timestamp + running max gap (`_poll_last_call_us`, `_poll_max_gap_us`).
  A single gap > 500 ms emits one `E DIAG POLL_GAP <gap_ms>` via `diag_nb_emit`
  (drained later in the same poll). Gaps are only measured while the transport is
  supposed to be live (open, or closed with an arm failure pending), so an
  intentional deInit accrues no spurious gap. Discriminator behaviour preserved:
  H1 end-state → polls keep arriving, no POLL_GAP (loop alive, state uncovered);
  H2 stall → one POLL_GAP ≈ stall duration on resume. The REARM stats line format
  is NOT extended (host tooling parses it), per spec.

## 3. Test hooks (spec §5.6)

Implemented behind `WATCHER_LINK_TEST_HOOKS`, **default OFF**: transport cmd `0x0A`,
len selects — 1 = fail next `spi_open` once (consulted in `_spi_try_open`), 2 = park
the transport in the H-B unclaimed state, 3 = stall the AT loop 60 s after the next
real command is parsed (busy-wait at the head of `spi_poll`). **Compile-verified**:
a full `make cleanbuild` with the switch ON succeeded (build hash `9d02a80a…`), then
the switch was flipped back OFF and the release build produced from a second full
cleanbuild. The shipped binary is proven hooks-free by hash identity (§5).

## 4. Version bump mechanics

The version's authoritative source is the `WATCHER_FW_ID` string in
`library/sscma_micro/sscma/server/at/callback/watcher_build_hash.h` — it is the only
place the string exists in the tree, and `gen_build_hash.sh` *preserves* it from that
header while regenerating the hash (there is no other version input). The bump was
therefore a one-line edit of that string (`…0.4.20.61-spi` → `…0.4.20.62-spi`) before
building; `make cleanbuild` then regenerated the header with the fresh hash over the
new sources. The header was not hand-edited beyond that string.

## 5. Verification (read back from the artifacts, per project discipline)

| Check | Result |
|---|---|
| ELF FW ID (strings) | `watcher-himax-0.4.20.62-spi` ✓ |
| ELF build hash (strings) | `fc0bac1be88797e12bf0c5f6cfa2f96fc57b25b9c9d0ce1b0c9978dca44d0648` |
| Fingerprint (first 16) | **`fc0bac1be88797e1`** |
| Fresh vs .61 (`bfad1e39ecc5fb43…`) | differs ✓ (match would = stale build) |
| Fresh vs hooks-ON check build (`9d02a80a2caf…`) | differs ✓ |
| Hash recomputed from shipped sources (`cat ma_transport_spi.cpp/.h + ma_camera_himax.cpp \| sha256sum`) | `fc0bac1b…` — **identical to the ELF's embedded hash** → the ELF was built from the current hooks-OFF sources ✓ |
| Hooks switch in shipped source | `#define WATCHER_LINK_TEST_HOOKS 0` ✓ |
| `output.img` embeds FW ID + hash | both present (strings) ✓ |
| Warnings in the touched file | 0 (168 pre-existing warnings elsewhere in the tree, unchanged count) |
| Re-arm timing contract (invariant 2 / H-E) | untouched: no change adds latency to the arm path — H-A(1) is a flag write on a failure path, H-C is post-arm, H-B/H-D are poll-head/block additions off the re-arm path |

⚠️ **Ignore the makefile's own "Verify fingerprint" line** — it prints `0001020304050607`,
which is a lookup-table constant the naive `strings | grep` grabs first, NOT the build
fingerprint. Verify against **`fc0bac1be88797e1`** (boot DIAG line / strings on the ELF
or .img as above).

Build log tail (release build, exit 0; full log: `/tmp/himax62-release.log` on this VM):

```
Memory region         Used Size  Region Size  %age Used
 CM55M_S_APP_ROM:      248984 B       256 KB     94.98%
CM55M_S_APP_DATA:      232852 B       256 KB     88.83%
    CM55M_S_SRAM:       1428 KB      1428 KB    100.00%
   text    data     bss     dec     hex filename
 444600    4540  1494964  1944104  1daa28  obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf
=== Build complete ===
```

## 6. Flash inputs for the owner (Himax flash is your run — nothing was flashed)

The Himax flash flow consumes the packaged image, produced by the documented pipeline
(ELF → copy to `input_case1_secboot/` → `./we2_local_image_gen project_case1_blp_wlcsp.json`):

- **Image to flash:**
  `sscma-fork/sscma-example-we2-main/we2_image_gen_local/output_case1_sec_wlcsp/output.img`
  — 622,592 bytes (same size as .61's image)
  — sha256: `10f4ae0d706dc3cb9e35d409db2d8ec7964fdd4e039cc09df621c12ddcf3a130`
  (the .61 image it replaced hashed `f1e5a0c3…`, matching the recorded .61 value —
  same file, same flow.)
- **Source ELF** (for reference/symbols):
  `sscma-fork/sscma-example-we2-main/EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf`
  — sha256: `02e67878e6df851b3cfc13740ba7c8b4661006c95d254b856eda43b47fcad10d`
- **Expected identity after flashing:** boot DIAG / FW ID reports
  `watcher-himax-0.4.20.62-spi`, fingerprint **`fc0bac1be88797e1`**.
- **Rollback:** reflash the .61 image (hash `f1e5a0c3…`); the two sides are
  wire-compatible in any combination (spec invariant 9), so .62 runs against
  S3 0.4.49.1 as well as 0.4.50.
