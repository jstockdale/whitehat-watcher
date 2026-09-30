/* SPDX-License-Identifier: MIT
 * WIA1 root Ed25519 public keys — the single trust anchor for session
 * certificates (kind 3). Baked into firmware from day one; the device
 * verifier tries PRIMARY, then BACKUP (§3).
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  STOP — PARENT (John) ACTION REQUIRED BEFORE ANY RELEASE BUILD
 * ═══════════════════════════════════════════════════════════════════════
 *  BENCH PROFILE (2026-09-25): both slots provisioned with bench
 *  root public keys generated off-cloud on the bench workstation.
 *  WIA1_ROOT_PRIMARY fp 8bc06f1a857e04a5 — private at
 *  ~/.ssh/watcher_seal_root (0600, never in firmware, never logged).
 *  WIA1_ROOT_BACKUP  fp f8cc458eb0bf9349 — private at
 *  ~/.ssh/watcher_seal_root_backup (0600). John holds the offline
 *  copy of the backup private key; it is the account-loss backstop.
 *
 *  Firmware MUST NEVER ship with zero roots except bench-test builds:
 *  with zero roots no certificate can ever verify, and — worse — a
 *  zero PRIMARY combined with a provisioned BACKUP silently changes
 *  the trust set. wia1_roots_provisioned() returns false when both
 *  slots are zero; bring-up code should refuse to arm sessions until
 *  it returns true.
 *
 *  Rotation: generate a new root key, flash firmware carrying the new
 *  key in one slot while the old still validates in the other (§11).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIA1_HOST_TEST
/* Host unit test: root keys come from the oracle-generated vectors, so
 * cert verification runs against real (throwaway) keys. */
extern const uint8_t WIA1_TEST_ROOT_PRIMARY[32];
extern const uint8_t WIA1_TEST_ROOT_BACKUP[32];
#define WIA1_ROOT_PRIMARY WIA1_TEST_ROOT_PRIMARY
#define WIA1_ROOT_BACKUP  WIA1_TEST_ROOT_BACKUP
#else
/* >>> PARENT: paste real root public keys here before release. <<< */
static const uint8_t WIA1_ROOT_PRIMARY[32] = {
    0x8a,0x9f,0x61,0xf0,0x8e,0xa4,0xe8,0x0e,
    0x62,0x68,0xe6,0x1b,0x9e,0x23,0x02,0x9d,
    0xb9,0x32,0x37,0x8e,0x24,0x7f,0x3d,0xfe,
    0xe8,0xb0,0xab,0xac,0x70,0x88,0xe7,0x1f,
};
static const uint8_t WIA1_ROOT_BACKUP[32] = {
    0x5c,0x7b,0x7b,0xc8,0xd2,0x90,0x89,0x98,
    0xb1,0xbe,0x90,0x0d,0x6c,0x7c,0xff,0xb4,
    0x21,0x20,0x92,0x67,0x9a,0x6c,0xa4,0xda,
    0x94,0xe0,0x7b,0xca,0x78,0x3c,0xe7,0xe6,
};
#endif

/* Constant-time-ish zero check over both slots (false when both zero). */
static inline bool wia1_roots_check(const uint8_t a[32], const uint8_t b[32])
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) {
        acc |= a[i] | b[i];
    }
    return acc != 0;
}

/* False when both root slots are zero (unprovisioned placeholder). */
static inline bool wia1_roots_provisioned(void)
{
    return wia1_roots_check(WIA1_ROOT_PRIMARY, WIA1_ROOT_BACKUP);
}

#ifdef __cplusplus
}
#endif
