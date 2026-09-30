#!/usr/bin/env python3
"""gen_tier0.py — Tier 0 (offline root) Ed25519 keypair generation ceremony.

Tier 0 is the Watcher PKI root: it signs Tier 1 certs and Tier 1
rotation commands, and nothing else. John holds it. It is generated
OFFLINE, never lives in AWS, never lives in this repo.

Writes (default out-dir ~/.config/watcher-pki/, NOT the repo):
    <name>_private.key   32-byte Ed25519 seed, hex, mode 0600
    <name>_public.pub    32-byte Ed25519 pubkey, hex

Prints the pubkey hex + fingerprint for the operator to record
(out-of-band), plus the ceremony checklist.

Refuses to overwrite an existing private key unless --force is given.
"""

import argparse
import hashlib
import os
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nacl.signing import SigningKey

import pki_common as P

DEFAULT_OUT_DIR = os.path.expanduser("~/.config/watcher-pki")


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate the Watcher PKI Tier 0 (offline root) Ed25519 keypair.",
        epilog="The private key is NEVER printed. Only the pubkey + fingerprint "
               "leave this machine, on paper or an out-of-band channel you trust.")
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR,
                    help="directory for key files (default: %(default)s — NOT the repo)")
    ap.add_argument("--name", default="tier0",
                    help="key name; files are <name>_private.key / <name>_public.pub "
                         "(default: %(default)s)")
    ap.add_argument("--force", action="store_true",
                    help="allow overwriting an existing private key (DANGEROUS)")
    return ap.parse_args(argv)


CEREMONY_CHECKLIST = """
TIER 0 CEREMONY CHECKLIST — read before you proceed
==================================================
[ ] AIR GAP: generate on an offline machine (live-USB boot is ideal).
    This key must never touch a networked filesystem, cloud sync, or CI.
[ ] BACKUP: copy <name>_private.key to TWO encrypted offline media
    (e.g. LUKS USB sticks). Store them in separate physical locations.
    John's custody is the mitigation for Tier 0 compromise — there is
    no technical recovery if all copies are lost.
[ ] NEVER IN AWS: the private key must never be uploaded to KMS,
    S3, DynamoDB, or any cloud service. Tier 1 lives in KMS; Tier 0
    never leaves the offline machine.
[ ] NEVER IN REPO: the private key must never be committed, pasted into
    chat/docs/logs, or emailed. tools/pki/.gitignore blocks *_private*,
    tier0*, *.key, *.pem, *.secret — but the .gitignore is a seatbelt,
    not the brakes. Keep it out of git yourself.
[ ] RECORD OUT-OF-BAND: write the pubkey hex + fingerprint below into
    the project key log (paper or the offline safe) so the firmware
    build pipeline and auditors can pin to the right Tier 0 key.
[ ] VERIFY FINGERPRINT: when the build pipeline bakes the Tier 0 pubkey
    into firmware, the fingerprint it reports MUST match this one.
"""


def main(argv=None):
    args = parse_args(argv)

    out_dir = os.path.abspath(os.path.expanduser(args.out_dir))
    os.makedirs(out_dir, mode=0o700, exist_ok=True)
    # makedirs ignores mode when the dir exists; enforce it.
    os.chmod(out_dir, 0o700)

    priv_path = os.path.join(out_dir, f"{args.name}_private.key")
    pub_path = os.path.join(out_dir, f"{args.name}_public.pub")

    if os.path.exists(priv_path) and not args.force:
        print(f"REFUSING: {priv_path} already exists.", file=sys.stderr)
        print("Tier 0 rotation is a deliberate, separate ceremony — "
              "see PKI_RUNBOOK.md.", file=sys.stderr)
        print("Pass --force only if you understand you are REPLACING the "
              "PKI root.", file=sys.stderr)
        return 1
    if os.path.exists(priv_path) and args.force:
        print(f"WARNING: --force given; OVERWRITING {priv_path}. "
              "The old Tier 0 key is now dead — every cert it signed is "
              "unverifiable against the new root. See PKI_RUNBOOK.md "
              "'Tier 0 compromise'.", file=sys.stderr)

    sk = SigningKey.generate()
    # libsodium secret layout: seed(32) || pk(32). Store the seed only —
    # it regenerates the full key and is the project convention
    # (see seal_keygen.py).
    seed_hex = sk.encode().hex()          # 32 bytes
    pub_hex = sk.verify_key.encode().hex()  # 32 bytes

    with open(priv_path, "w") as f:
        f.write(seed_hex + "\n")
    os.chmod(priv_path, stat.S_IRUSR | stat.S_IWUSR)   # 0600
    with open(pub_path, "w") as f:
        f.write(pub_hex + "\n")
    os.chmod(pub_path, 0o644)

    fp = P.fp16(bytes.fromhex(pub_hex))

    print("Tier 0 (offline root) keypair generated.")
    print(f"  private: {priv_path}  (0600 — NEVER leaves this machine)")
    print(f"  public:  {pub_path}")
    print()
    print("RECORD THESE OUT-OF-BAND (safe to share, needed by the build pipeline):")
    print(f"  pubkey hex: {pub_hex}")
    print(f"  fingerprint: {fp}  (sha256(pubkey)[:16], project convention)")
    print(CEREMONY_CHECKLIST.replace("<name>", args.name))
    print("The private key was NOT printed. It exists only at " + priv_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
