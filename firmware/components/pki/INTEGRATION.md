# Watcher PKI — Firmware Integration Guide

Design brief (source of truth): `tools/pki/PKI_DESIGN_BRIEF.md`.
This component (`firmware/components/pki/`) is the device side of that
design: **crypto verification is real and complete; dispatcher wiring
and persistence are the follow-up firmware task.**

## 1. Files in this component

| File | Status |
|------|--------|
| `pki_verify.h` / `pki_verify.c` | **Real.** Full verification logic: canonical-byte builders, cJSON canonical-JSON serializer (byte-identical to Python `json.dumps(sort_keys=True, separators=(",",":"), ensure_ascii=True)`), mbedTLS ECDSA P-256 verify, Monocypher Ed25519 verify, expiry/window/nonce/scope/schema checks. |
| `tier2_keys.h` | **Generated, never checked in.** Baked by `firmware/tools/bake_tier2_keys.py`. The build refuses to configure without it (loud `FATAL_ERROR`). |
| `CMakeLists.txt` | Registers the component; fails configure LOUDLY if `tier2_keys.h` is missing. |
| `host_test/` | Host unit test (`run.sh`): compiles `pki_verify.c` with `-DPKI_HOST_TEST`, memcmps C canonical bytes against Python-generated vectors. All green as of 2026-09-27. |
| `INTEGRATION.md` | This file. |

## 2. Build hook — where it runs

**Manual pre-build step** (chosen over a CMake custom command so the
bake is explicit, auditable, and never runs silently with wrong inputs):

```bash
cd firmware/
TIER0_PUBKEY_HEX=<64 hex chars of John's offline Tier 0 Ed25519 pubkey> \
  tools/bake_tier2_keys.py \
    --tier0-pubkey-hex "$TIER0_PUBKEY_HEX" \
    --cert-file /path/to/tier1-cert.json     # or --tier1-pubkey-url <GET /tier1/pubkey>
idf.py build
```

What the script does, in order: fetch/parse the Tier 1 cert →
recompute canonical bytes per the brief →
**Ed25519-verify the Tier 0 signature (pure-Python RFC 8032, zero
third-party deps)** → check `issued_at <= now <= expires_at` →
emit `components/pki/tier2_keys.h` (`TIER0_PUBKEY[32]`,
`TIER1_P256_PUBKEY[65]` uncompressed, `TIER1_CERT_SERIAL`,
`TIER1_CERT_ISSUED_AT`, `TIER1_CERT_EXPIRES_AT`, all marked
`/* GENERATED — do not edit */`).

Any failure (bad signature, expired/not-yet-valid cert, malformed cert,
missing Tier 0 key) exits **2** with a `FATAL:` message on stderr —
it never bakes a bare unverified pubkey.

`--self-test` runs the whole pipeline with ephemeral in-memory keys
(no secrets touch disk) and bakes a header stamped
`TEST VECTORS ONLY — DO NOT SHIP`. Use it for crypto-logic smoke
tests and the host unit test only.

CI option (not wired yet): a CMake custom command in `pki/CMakeLists.txt`
invoking the script when `PKI_TIER0_PUBKEY_HEX` / `PKI_TIER1_CERT` cache
variables are set. The current `FATAL_ERROR` guard stays as the
fail-closed default either way.

## 3. Command path — where `pki_verify_provision()` gets called

**Insertion point:** `handle_command()` in
`firmware/components/link/link.c` (static, ~line 746) — the single
JSON command dispatcher. Follow the existing provisioning-command
pattern (`seal_provision`, `seal_unpin`, `seal_strict`, `seal_tel_seed`
at ~lines 894–955):

```c
if (strcmp(t, "pki_provision") == 0) {
    /* Serial-console ONLY, like every other provisioning command:
     * via_mqtt commands are refused (link.c ~line 771:
     * `bool via_mqtt = cJSON_IsTrue(cJSON_GetObjectItem(cmd, "_via_mqtt"));`) */
    if (via_mqtt) {
        link_respond_err(t, "serial console only");
        return;
    }
    /* transport: {"t":"pki_provision","cmd":{<watcher-provision-v1 JSON>},
     *             "sig":"<128 hex chars, Lambda DER->raw64 r||s>"} */
    const cJSON *inner = cJSON_GetObjectItem(cmd, "cmd");
    const char *sighex = opt_str(cmd, "sig");
    /* 1. re-serialize inner with cJSON_PrintUnformatted
     * 2. hex-decode sig -> sig_raw64[64]
     * 3. pki_verify_provision(bytes, len, sig_raw64, &pcmd)
     * 4. switch (pcmd.kind): apply via nvs_task broker (section 4)
     * 5. reply: link_new_event("ok") + link_send_term(o),
     *    or link_respond_err(t, pki_rc_str(rc)) */
    return;
}
```

Helpers available in `link.c` (same file, real names):
`opt_str(cmd, key)` (~line 674), `link_respond_err(t, msg)` (line 316),
`link_new_event("ok")` (line 377), `link_send_term(o)` (static, line 511),
`cmd_mid_` for `_mid` correlation.

**Boot wiring:** call once at startup (e.g. from the same place
`seal_dev_init()` is invoked):
`pki_set_device_context(<device name>, seal_dev_has_commander_key())`.
Device-name candidate: `hid_ble_device_name()` (`components/hid_ble/include/`,
line 110) — the provisioned BLE/identity name. `keyed` gates the
`device:"*"` wildcard (unkeyed devices only, per the brief's scope rule).

**Transport note:** the provisioning command JSON + signature travel as
one link command. The signature is the Lambda's raw 64-byte `r||s`
(DER→raw64 conversion happens Lambda-side per the brief); the device
feeds `r`/`s` to mbedTLS as MPIs, so no DER parsing is needed on device.

## 4. Persistence via the `nvs_task` broker

`pki_verify.c` performs **zero NVS writes** today. When the follow-up
task applies verified commands, all persistence goes through the single
FIFO `nvs_task` broker using the synchronous APIs in
`components/seal/include/seal_dev.h` (real names, implemented in
`seal_dev.c`):

- `seal_dev_nvs_write_blob_sync(ns, key, data, len, timeout_ms)`
- `seal_dev_nvs_read_blob_sync(ns, key, out, &inout_len, timeout_ms)`
- `seal_dev_nvs_write_u64_sync` / `seal_dev_nvs_read_u64_sync`
- `seal_dev_nvs_delete_sync`

Suggested NVS layout (namespace `"pki"`):

| Key | Content |
|-----|---------|
| `tier1_pub` | 65-byte rotated Tier 1 P-256 pubkey (absent = use baked) |
| `tier1_exp` / `tier1_serial` | u64 expiry / u32 serial of the stored key |
| `nonces` | blob: recent 16-byte nonces (replay store) |

Then `pki_active_tier1_pubkey()` (currently returns the baked key —
marked STUB in `pki_verify.c`) becomes "NVS `tier1_pub` if present and
its expiry/serial beat the baked cert, else baked".

Applying each verified command:
- `install_commander_key` → write the 32-byte Ed25519 pubkey to the
  seal namespace the same way `seal_dev_cmd_provision()` does
  (`seal_dev.c` ~line 1158 — reuse, don't duplicate).
- `install_telemetry_seed` → the command carries only `seed_fp`
  (fingerprint). The real seed still arrives over USB serial via the
  existing `seal_tel_seed` path (`seal_dev_cmd_tel_seed`,
  `seal_dev.c` ~line 1208); the PKI command authorizes/registers the
  fingerprint.
- `rotate_tier1` → after `pki_verify_provision()` accepts the outer
  command, persist the new pubkey/expiry/serial via the broker.
- `factory_reset_keys` → unpin commander key + delete `pki/*`
  (mirror `seal_dev_cmd_unpin`, `seal_dev.c` ~line 1178).

## 5. What remains for the full firmware task

1. **Dispatcher wiring** — the `pki_provision` branch in
   `handle_command()` (section 3), serial-only gating, `_mid` replies.
2. **Boot context** — `pki_set_device_context()` call at startup.
3. **Nonce persistence** — replace the RAM stub (`pki_nonce_seen()` /
   `pki_nonce_record()`) with the NVS-backed store via the broker.
   The interface is already what the brief mandates; only the backing
   changes.
4. **Rotated-key lookup** — upgrade `pki_active_tier1_pubkey()` to the
   NVS-with-baked-fallback described in section 4.
5. **Wall clock** — `pki_time_now()` returns 0 (device has no SNTP,
   `ts_window=0`). The absolute `issued_at <= now <= expires_at` check
   is implemented but dormant; `issued_at <= expires_at` and the 15-min
   window are always enforced. Wire SNTP (or a provisioned epoch) before
   the absolute check is meaningful — the code logs a warning on every
   verify until then so the gap stays visible.
6. **Apply logic** — section 4's per-command NVS writes.
7. **Open design items needing John's sign-off**:
   - **Rotate canonical bytes**: the brief now defines the exact
     `WATCHER-TIER1-ROTATE-v1 || 65-byte pubkey || u64(expiry) ||
     u32(serial)` form (added to `PKI_DESIGN_BRIEF.md` 2026-09-27 —
     the brief previously only sketched `rotate_tier1{new_p256_pubkey,
     expiry}`). `tools/pki` Lambda signing code MUST mirror it exactly.
   - **Rotate countersignature transport**: the brief's `rotate_tier1`
     params now REQUIRE `tier0_sig_hex` + `serial`; the firmware
     enforces this (`validate_params()` → `pki_verify_rotate_tier1()`
     verifies the Ed25519 countersignature against the baked Tier 0
     pubkey). The outer Tier 1 signature still covers transport.

## 6. Audit-tool compliance

`firmware/tools/audit_psram_flash.py` (build-time, fails the build on
violations) was considered throughout:

- **No NVS/flash calls**: `pki_verify.c` calls no `nvs_*`,
  `spi_flash_*`, `esp_ota_*`, or `esp_partition_*` APIs — directly or
  transitively. The only persistence-adjacent functions are the
  `seal_dev_nvs_*_sync()` broker APIs, which are *referenced in
  comments/docs only* until the follow-up task. (Run the audit after
  that task lands.)
- **No tasks created**: no `xTaskCreate` / `xTaskCreateStatic`, so the
  PSRAM-stack rules and the 15-char `xTaskGetHandle` rule don't apply.
  `pki_verify_provision()` is designed to run on the existing link
  worker task; its ~3.5 KB stack scratch (canonical JSON + canonical
  bytes) is sized for that.
- **No new flash-touching call paths** from any PSRAM-stack task.

Verify: `python3 firmware/tools/audit_psram_flash.py` (exit 0 = clean).

## 7. Verification evidence (2026-09-27)

- `bake_tier2_keys.py --self-test`: pure-Python Ed25519 cross-checked
  against the `cryptography` package; tampered-cert / wrong-Tier-0-key /
  expired-cert all rejected with loud `FATAL` (exit 2); provisioning
  canonical bytes sign/verify round-trip through KMS-style
  ECDSA-SHA256 → DER → raw64.
- `components/pki/host_test/run.sh`: **ALL TESTS PASSED** — C
  canonical JSON byte-identical to CPython `json.dumps(sort_keys=True,
  separators=(",",":"), ensure_ascii=True)` over 12 adversarial cases
  (nesting, key sorting, `\u` escapes, emoji/surrogate pairs, DEL,
  control chars); C provision/rotate canonical bytes byte-identical to
  the Python builders; accept path plus 15 error-path cases
  (PARSE/SCHEMA/WINDOW/BADSIG/NONCE-replay/SCOPE).
- `idf.py build`: full firmware build with the new `pki` component —
  compiles clean (mbedTLS ECDSA + Monocypher Ed25519 link correctly).
  Built against a `--self-test` TEST-VECTOR header, which was deleted
  afterwards; a plain `idf.py build` now fails at configure with the
  bake instructions (by design — never ship test vectors).
