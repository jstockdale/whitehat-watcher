#!/usr/bin/env python3
"""test_tel_seal.py — checks the committed kind-1 ENC vectors.

Each vectors/WMS2/kind1enc/<case>.json carries the envelope, topic,
expected rc string, expected log substring, and (for OK cases) the
original plaintext body. Run: python3 test_tel_seal.py
"""
import base64, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mqtt_seal as mqs

VEC_DIR = os.path.join(HERE, "vectors", "WMS2", "kind1enc")
TS = 1758850000000
NOW = TS + 1

fails = []


def check(name, cond, extra=""):
    print(("ok  " if cond else "FAIL") + "  " + name + (" " + extra if extra and not cond else ""))
    if not cond:
        fails.append(name)


def load(name):
    with open(os.path.join(VEC_DIR, name + ".json")) as f:
        return json.load(f)


def verify_vec(v, replay=None, aead=True):
    seed = bytes.fromhex(v["seed_hex"])
    k_mac, k_aead, _ = mqs.derive_tel_keys(seed)
    env = base64.b64decode(v["b64"])
    kw = dict(key32=k_mac, now_ms=NOW, replay=replay,
              policy=mqs.Policy())
    if aead:
        kw["aead_key"] = k_aead
    return mqs.verify(env, v["topic"], **kw), (k_mac, k_aead)


def expect(name, want_rc, want_log_sub="", replay=None, aead=True,
           check_body=True):
    v = load(name)
    (rc, h, body, log), _ = verify_vec(v, replay, aead)
    got = mqs.rc_str(rc).replace("-", "_")
    ok = got == want_rc
    check(name + "/rc", ok, "want %s got %s" % (want_rc, got))
    if want_log_sub:
        oklog = log is not None and want_log_sub in log
        check(name + "/log", oklog, "want %r in %r" % (want_log_sub, log))
    elif want_rc in ("OK", "OK_DUP"):
        check(name + "/log-silent", log in (None, ""),
              "want silent, got %r" % log)
    if check_body and "body" in v and want_rc == "OK":
        want_body = v["body"].encode() if isinstance(v["body"], str) else v["body"]
        check(name + "/body", body == want_body,
              "plaintext mismatch")


def main():
    # KDF byte-equality (labels pinned in the vector)
    k = load("kdf")
    k_mac, k_aead, fp = mqs.derive_tel_keys(bytes.fromhex(k["seed_hex"]))
    check("kdf/mac", k_mac.hex() == k["want_mac_hex"])
    check("kdf/aead", k_aead.hex() == k["want_aead_hex"])
    check("kdf/fp", fp == k["want_fp"])
    check("kdf/labels", k["labels"] == {"mac": "WMS2-tel-mac-v1",
                                       "aead": "WMS2-tel-aead-v1",
                                       "fp": "WMS2-tel-fp-v1"})

    expect("valid-enc", "OK")
    expect("frame-roundtrip", "OK")
    expect("audio-roundtrip", "OK")
    expect("wrong-topic", "BAD_TAG", "BAD TAG")
    expect("ct-tamper", "BAD_TAG", "BAD TAG")
    expect("outer-tag-tamper", "BAD_TAG", "BAD TAG")
    expect("wrong-seed", "BAD_TAG", "BAD TAG")
    expect("wrong-aead-key", "BAD_TAG", "AEAD FAIL", check_body=False)
    expect("short-enc-body", "SHORT", "SHORT enc body", check_body=False)
    expect("no-aead-key", "NO_AEAD_KEY", "NO AEAD KEY", aead=False,
           check_body=False)
    expect("stale", "STALE", "STALE", check_body=False)
    expect("mac-only", "OK")

    # replay pair shares one ReplayCtx
    rp = mqs.ReplayCtx()
    expect("replay-first", "OK", replay=rp)
    expect("replay-then", "REPLAY", "REPLAY ctr 1 <= 2", replay=rp,
           check_body=False)
    check("replay/hw-held", rp.high_water == 2)

    # dup: same envelope twice -> second is silent OK_DUP
    rp2 = mqs.ReplayCtx()
    expect("dup", "OK", replay=rp2)
    v = load("dup")
    (rc, h, body, log), _ = verify_vec(v, rp2)
    check("dup/second-rc", mqs.rc_str(rc).replace("-", "_") == "OK_DUP")
    check("dup/second-silent", log is None)

    # AEAD FAIL and NO-AEAD-KEY never advance high-water
    rp3 = mqs.ReplayCtx()
    expect("wrong-aead-key", "BAD_TAG", "AEAD FAIL", replay=rp3,
           check_body=False)
    check("aead-fail/hw-held", rp3.high_water == 0)

    print("RESULT: " + ("ALL PASS (%d vectors)" % 17 if not fails
                         else "FAILURES: %s" % fails))
    return not fails


if __name__ == "__main__":
    # fix the summary line: count checks via a wrapper is overkill;
    # just report fails.
    ok = main()
    sys.exit(0 if ok else 1)
