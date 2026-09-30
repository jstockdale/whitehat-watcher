#!/usr/bin/env python3
"""mint-cert — bench-profile WIA-SEAL session certificate minter.

Replaces the AWS KMS auth service for bench use (WIA-SEAL v1 §11:
"Phase 1 auth service is a mint-cert CLI run with MFA'd credentials —
the human approval IS the operator running it").

The operator running this command, after seeing the Watcher's screen
show the arm challenge, IS the human approval. KEYBOARD scope should
only be minted when John explicitly intends keystroke capability.

Usage:
  mint-cert register --dev-id <id> --pubkey <hex>   # claim flow (§11)
  mint-cert mint --challenge <file|-> --scope mouse[,keyboard][,consumer]
                 --ttl 3600 [--out <file>]

The challenge is the kind-4 envelope as {"sealed":"<b64>"} (from the
device's wia/<dev_id>/arm/challenge topic or via USB console).
Output is the kind-3 envelope as {"sealed":"<b64>"} — publish to
wia/<dev_id>/arm/cert, or deliver via {"t":"seal_cert","b64":"..."}
over USB serial in bridged mode.

Security: root private key lives at ~/.ssh/watcher_seal_root (0600),
never printed. Nonces are single-use (tracked in the registry file).
"""
import argparse
import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wia_seal as w
from nacl.signing import SigningKey, VerifyKey

SSH = os.path.expanduser("~/.ssh")
ROOT_SK = os.path.join(SSH, "watcher_seal_root")
REGISTRY = os.path.expanduser("~/.config/watcher/wia_registry.json")

SCOPES = {"mouse": w.SCOPE_MOUSE, "keyboard": w.SCOPE_KEYBOARD,
          "consumer": w.SCOPE_CONSUMER}


def load_registry():
    if os.path.exists(REGISTRY):
        with open(REGISTRY) as f:
            return json.load(f)
    return {"devices": {}, "used_nonces": []}


def save_registry(reg):
    os.makedirs(os.path.dirname(REGISTRY), exist_ok=True)
    with open(REGISTRY, "w") as f:
        json.dump(reg, f, indent=2)
    os.chmod(REGISTRY, 0o600)


def load_root():
    with open(ROOT_SK) as f:
        return SigningKey(bytes.fromhex(f.read().strip()))


def parse_scope(spec: str) -> int:
    bits = 0
    for part in spec.split(","):
        part = part.strip().lower()
        if part not in SCOPES:
            raise SystemExit(f"unknown scope '{part}' (mouse,keyboard,consumer)")
        bits |= SCOPES[part]
    if not bits:
        raise SystemExit("scope is empty")
    return bits


def cmd_register(args):
    reg = load_registry()
    pk = bytes.fromhex(args.pubkey)
    if len(pk) != 32:
        raise SystemExit("pubkey must be 32 bytes hex")
    dev_id = args.dev_id.encode().ljust(16, b"\x00")[:16]
    reg["devices"][args.dev_id] = {"pubkey": args.pubkey,
                                   "dev_id_hex": dev_id.hex()}
    save_registry(reg)
    print(f"registered {args.dev_id} -> {args.pubkey[:16]}...")


def cmd_mint(args):
    reg = load_registry()
    root = load_root()

    raw = sys.stdin.read() if args.challenge == "-" else \
        open(args.challenge).read()
    env = w.b64_unwrap(raw)

    # Verify challenge against the registry (closes fake-nonce minting):
    # parse first, look up the identity key, then verify the signature.
    h = w.parse_header(env[:w.HDR_LEN])
    if h["kind"] != w.KIND_CHAL:
        raise SystemExit("not a kind-4 challenge envelope")
    dev_id_raw = h["dev_id"]
    dev_id_str = dev_id_raw.split(b"\x00")[0].decode()
    entry = reg["devices"].get(dev_id_str)
    if not entry:
        raise SystemExit(f"unknown device '{dev_id_str}' — register first")
    idkey = VerifyKey(bytes.fromhex(entry["pubkey"]))
    h, body = w.verify_envelope(env, None, verify_key=idkey)
    ch = w.parse_challenge_body(body)
    nonce_hex = ch["nonce"].hex()
    if nonce_hex in reg.get("used_nonces", []):
        raise SystemExit("nonce already used — refusing to mint twice")
    if h["ctr"] != 0 or h["ts_rel_ms"] != 0 or h["session_id"] != bytes(16):
        raise SystemExit("challenge must have ctr=0, ts=0, zero session_id")

    scope = parse_scope(args.scope)
    if scope & ~ch["scope_offered"]:
        raise SystemExit(f"scope 0x{scope:02x} exceeds device offer "
                         f"0x{ch['scope_offered']:02x}")
    if args.ttl > 4 * 3600:
        raise SystemExit("ttl exceeds device policy max (4h)")
    if scope & w.SCOPE_KEYBOARD:
        print("NOTE: KEYBOARD scope — keystroke capability. "
              "Mint only with John's explicit intent.", file=sys.stderr)

    # Agent ephemeral keys (RAM only, never serialized).
    from nacl.public import PrivateKey
    agent_ed = SigningKey.generate()
    agent_x = PrivateKey.generate()

    cert_body = w.build_cert_body(scope, args.ttl, dev_id_raw, ch["nonce"],
                                  bytes(agent_ed.verify_key),
                                  bytes(agent_x.public_key),
                                  ch["device_x_pk"])
    session_id = w.session_id_of_cert(cert_body)
    cert_env = w.seal_envelope(w.KIND_CERT, cert_body, None, dev_id_raw,
                               bytes(16), 0, 0, 0, sign_key=root)

    reg.setdefault("used_nonces", []).append(nonce_hex)
    save_registry(reg)

    out = w.b64_wrap(cert_env)
    if args.out:
        with open(args.out, "w") as f:
            f.write(out)
        print(f"cert written to {args.out}")
    else:
        print(out)
    print(f"session_id={session_id.hex()} scope=0x{scope:02x} ttl={args.ttl}s",
          file=sys.stderr)
    print("AGENT SESSION KEYS (RAM only — the agent keeps these):",
          file=sys.stderr)
    print(f"  ed_sk={bytes(agent_ed).hex()}", file=sys.stderr)
    print(f"  x_sk={bytes(agent_x).hex()}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description="WIA-SEAL bench mint-cert")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("register", help="claim flow: register device identity")
    r.add_argument("--dev-id", required=True)
    r.add_argument("--pubkey", required=True, help="32-byte hex identity pubkey")
    m = sub.add_parser("mint", help="mint a kind-3 session certificate")
    m.add_argument("--challenge", required=True,
                   help="file with {\"sealed\":\"<b64>\"} or - for stdin")
    m.add_argument("--scope", default="mouse",
                   help="comma list: mouse,keyboard,consumer")
    m.add_argument("--ttl", type=int, default=3600, help="seconds, max 14400")
    m.add_argument("--out", help="output file (default stdout)")
    args = ap.parse_args()
    if args.cmd == "register":
        cmd_register(args)
    else:
        cmd_mint(args)


if __name__ == "__main__":
    main()
