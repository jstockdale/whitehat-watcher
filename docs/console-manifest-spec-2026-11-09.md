# Console command gate + firmware manifest — SPEC (2026-11-09)

Status: **spec only — awaiting John's go.** No console file, firmware,
or host tool is modified by this document. Per John's console file
discipline, the console change (when approved) ships batched, as one
version, with every script block validated; per his new-command
pattern, the firmware side follows the documented touch-point list.

## 1. The defect, from the sources

Console 0.4.190 (the copy on disk, `watcher-console.html`):

- `COMMAND_REGISTRY` carries a **single** entry for all debug
  subcommands: `"dbg":{label:"dbg",defaultOn:false,validate:function(c){return typeof c.cmd==="string"}}`,
  plus one special case, `"dbg snaptest"`.
- `commandRegId()` maps **every** other `{"t":"dbg","cmd":…}` to the
  generic id `"dbg"`. The Setup tab's per-subcommand checkboxes write
  allowlist keys (e.g. `dbg heap`, `dbg version`) that `isAllowedCmd()`
  never checks — and there is **no checkbox for `"dbg"` itself**.
- `crop`, `png`, and `strip` were never registered when they shipped
  in firmware — the documented new-command pattern (registry, defaults,
  Setup checkbox, `_pageSide` classification, MQTT routing, handler,
  allowlist migration bump) was not followed. That is the miss this
  spec repairs first.


Observed consequence, 2026-11-09: `dbg crop` over MQTT was rejected on
John's localhost:8000 origin in the morning and forwarded in the
afternoon. The console-source finding explains the shape: the gate
reads a per-origin saved allowlist in localStorage, and the morning
origin had no legacy saved `"dbg":true` while the earlier origin
evidently did. A gate whose answer depends on which origin's saved
state you happen to load is the bug, whichever origin is "right."

The drift is wider than three commands. Firmware 0.4.49.1's dispatch
in `firmware/components/link/link.c` implements **30 dbg subcommands**:
version, seal_status, heap, png, stacks, rings, spitest, crop, strip,
mem, tasks, pwr, pins, trace, touch, wheel, pointer, reboot, i2c,
fill, bl, eyes, eyes_reset, nvs_stress, at, ai, wakeword, codec,
loopback, snaptest (counted from the dispatch on 2026-11-09; the
spitest branch even appears twice).
`send_command.py`'s `DBG_ALLOWED` mirror lists **20** — it omits
seal_status, stacks, pins, trace, reboot, fill, bl, eyes_reset,
nvs_stress, and ai. Some omissions are deliberate policy (reboot is a
locked class; nvs_stress is destructive), some are structural
(`send_command.py` rejects any dbg carrying `on`/`v`/`color` keys,
which is why fill and bl can't pass its generic path and wakeword
needed a hand-written special case) — but today intent and accident
are indistinguishable in the list. That is the maintenance failure
the manifest exists to end.

## 2. Step 1 — console 0.4.191 correctness fix (no firmware change)

Scope, and only scope:

1. One registry entry **per dbg subcommand** (`dbg crop`, `dbg png`,
   `dbg strip`, …), each with its own validator matching the
   firmware's argument shape, generalizing the existing
   `"dbg snaptest"` special case in `commandRegId()`.
2. Setup checkboxes that write the **same keys the gate checks**, one
   per subcommand, rendered to John's pill rules (fixed size to
   maximum text, uniform height).
3. `ALLOWLIST_VERSION` migration bump so saved allowlists re-derive
   cleanly on every origin instead of silently keeping a stale
   `"dbg":true`/absent state.
4. Defaults by class (see §3): read-only subcommands default off
   (current `"dbg"` posture) unless John says otherwise per class;
   locked-class subcommands (reboot, at, nvs_stress) get **no**
   enabling checkbox at all — they stay unreachable from the page's
   MQTT path, as today in effect.
5. All three script blocks validated with `node --check` before
   delivery; John gets the downloadable file to host, as usual.

Explicitly out of step 1: any firmware change, any change to
`send_command.py` (its mirror is fixed as part of step 2), and any
redesign of the allowlist UI beyond making the checkboxes honest.

## 3. Command classes (proposed, for John's ruling)

From the firmware's 30, classified by what they *do*:

- **Read-only:** version, seal_status, heap, stacks, mem, tasks, pwr,
  pins, i2c, eyes, codec, pointer (query form).
- **Hardware-affecting / diagnostic:** png, crop, strip, snaptest,
  rings, spitest, trace, touch, wheel, fill, bl, ai, wakeword,
  loopback, eyes_reset.
- **Locked class (never page/MQTT-enableable):** reboot, at,
  nvs_stress.

John's rulings needed: (a) the class assignments, especially
eyes_reset and loopback; (b) per-class defaults in the page.

## 4. Step 2 — firmware → console manifest (designed pair)

John's proposal (2026-11-09): the firmware hands the console a
manifest of the commands it supports, so one console page works
correctly against any Watcher firmware version.

Design points, building on the sketch already discussed with John:

1. **Generated, not hand-written.** The manifest is built from the
   firmware's own dispatch table — the same table `link.c` switches
   on — so a new subcommand appears in the manifest in the same
   commit that implements it. The new-command pattern gains one line:
   "the manifest is generated; if your command isn't in the dispatch
   table, it isn't anywhere."
2. **Contents per command:** id (`dbg crop`), class (§3), argument
   shape (names, types, ranges), and the firmware version that
   introduced it. Plus the manifest's own schema version and the
   FW_VERSION it was generated from.
3. **Delivery:** offered at connect, alongside the existing
   version/seal_status exchange. Open question for John: a dedicated
   sealed `dbg manifest` query, or a field in the `ready`/status
   payload? The sealed query keeps the seal posture uniform (reads
   are sealed too, per his 2026-10-01 ruling); the status field is
   simpler. Spec recommends the sealed query.
4. **Descriptive, never permissive.** The manifest can make a command
   *visible* in Setup; only John's saved allowlist makes it
   *allowed*. Page-side hard locks (locked class above, plus the
   existing `dbg at` write/reboot/unbond rules) are page policy that
   no manifest can talk the page out of.
5. **Fallback.** Firmware too old to answer the manifest query →
   the page uses its built-in registry (post-0.4.191, so the fallback
   is the corrected one). One console keeps working across every
   firmware version — the property John asked for.
6. **Third mirror dies.** `send_command.py` consumes the same
   manifest (fetched once, cached with its FW_VERSION) instead of
   its hand-kept `DBG_ALLOWED`; its structural key rejection is
   replaced by per-command validators generated from the argument
   shapes, with the locked class still refused host-side.

## 5. Sequencing and gates

1. John rules on §3 classes/defaults and the §4.3 delivery question.
2. Step 1 (0.4.191) built as **one batched console version**,
   validated, delivered for John's hosting — his approval gate
   applies before the file changes, per standing console discipline.
3. Step 2 specced to implementation depth (dispatch-table generation
   mechanics on the S3 side) in a follow-on document, then built as
   a firmware + console pair with its own version numbers and its
   own flash/hosting approvals. Nothing in step 1 depends on step 2
   landing.

What this spec does **not** do: authorize a console edit, a firmware
build, or a flash. It exists so John's go — when he gives it — is a
one-line decision with the tradeoffs already on the table.
