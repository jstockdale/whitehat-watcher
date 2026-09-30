#!/usr/bin/env python3
"""seal_keygen.py — one-time commander key ceremony for the MQTT seal.

Generates:
  - Ed25519 keypair for commander "muse-01" (control-plane signing)
  - 32-byte BLAKE2b telemetry MAC key (page -> host telemetry)

Storage (0600, never printed, never logged):
  ~/.ssh/watcher_seal_muse01        Ed25519 secret key (64B raw, hex)
  ~/.ssh/watcher_seal_muse01.pub    Ed25519 public key (32B raw, hex)
  ~/.ssh/watcher_seal_telemetry.key telemetry MAC key (32B raw, hex)

Output (safe to share): public key hex + fingerprint for John to paste
into the console Setup tab.
"""
import hashlib
import os
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nacl.signing import SigningKey
from nacl.utils import random as nrandom

SSH = os.path.expanduser("~/.ssh")
SK_PATH = os.path.join(SSH, "watcher_seal_muse01")
PK_PATH = os.path.join(SSH, "watcher_seal_muse01.pub")
TK_PATH = os.path.join(SSH, "watcher_seal_telemetry.key")


def _write_private(path, data_hex):
    with open(path, "w") as f:
        f.write(data_hex + "\n")
    os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)


def main():
    for p in (SK_PATH, TK_PATH):
        if os.path.exists(p):
            print(f"REFUSING: {p} already exists — rotation is a deliberate, "
                  f"separate ceremony. Delete it yourself if you mean it.")
            sys.exit(1)

    sk = SigningKey.generate()
    # libsodium secret layout: seed(32) || pk(32); store seed only (32B).
    seed_hex = sk.encode().hex()
    pk_hex = sk.verify_key.encode().hex()
    tkey_hex = nrandom(32).hex()

    _write_private(SK_PATH, seed_hex)
    _write_private(TK_PATH, tkey_hex)
    with open(PK_PATH, "w") as f:
        f.write(pk_hex + "\n")

    fp = hashlib.sha256(bytes.fromhex(pk_hex)).hexdigest()[:16]
    print("Commander keypair generated (muse-01).")
    print(f"  private: {SK_PATH} (0600, seed hex — never share)")
    print(f"  public:  {PK_PATH}")
    print(f"  telemetry key: {TK_PATH} (0600 — provision to page + host)")
    print()
    print("PROVISION THIS PUBLIC KEY into the console Setup tab:")
    print(f"  {pk_hex}")
    print(f"  fingerprint: {fp}  (verify this matches what the page shows)")
    print()
    print("Private key was NOT printed. It lives only at " + SK_PATH)


if __name__ == "__main__":
    main()
