#!/usr/bin/env python3
"""Deterministic WMS2 kind-2 test vector generator.

Writes committed vectors to vectors/WMS2/kind2/<case>.json, each with
the envelope, topic, expected verdict, and expected log string.
All inputs pinned: fixed test keypair (NOT a real commander key),
fixed timestamps, fixed counters — regeneration must be byte-identical.

Usage:
  python3 gen_seal_vectors.py            # write vectors/
  python3 gen_seal_vectors.py --check    # regen to temp, diff vs committed
"""
import base64, hashlib, json, os, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mqtt_seal as mqs
from nacl.signing import SigningKey

# Pinned test keys (32-byte seeds -> keypairs). Test-only, never deployed.
SEED_A = hashlib.sha256(b"wms2-test-commander-A").digest()
SEED_B = hashlib.sha256(b"wms2-test-commander-B").digest()
KEY_A = SigningKey(SEED_A)
KEY_B = SigningKey(SEED_B)

TOPIC = "watcher/command"
TS = 1758850000000  # pinned
DEV = "muse-01"

VEC_DIR = os.path.join(HERE, "vectors", "WMS2", "kind2")


def seal(topic, dev, ctr, ts, body, key, raw_body=False):
    body_b = body if raw_body else json.dumps(body).encode()
    env = mqs.seal_control(topic, dev, ctr, ts, body_b, bytes(key))
    return base64.b64encode(env).decode()


def build_cases():
    body = {"id": "t1", "cmd": {"t": "dbg", "cmd": "version"}}
    valid = seal(TOPIC, DEV, 1, TS, body, KEY_A)

    raw = bytearray(base64.b64decode(valid))
    raw[45] ^= 0x01
    tampered = base64.b64encode(bytes(raw)).decode()

    raw2 = bytearray(base64.b64decode(valid))
    raw2[0:4] = b"XXXX"
    badmagic = base64.b64encode(bytes(raw2)).decode()

    wrongkey = seal(TOPIC, DEV, 1, TS, body, KEY_B)

    # kind byte flipped (checked before signature verification)
    raw3 = bytearray(base64.b64decode(valid))
    raw3[4] = 3
    badkind = base64.b64encode(bytes(raw3)).decode()

    # counter forced out of JS-safe integer range
    raw4 = bytearray(base64.b64decode(valid))
    raw4[22:30] = b"\xff" * 8
    bigctr = base64.b64encode(bytes(raw4)).decode()

    # stale timestamp (5 min old; harness pins Date.now = TS+1)
    stale = seal(TOPIC, DEV, 1, TS - 300000, body, KEY_A)

    # replay pair: adopt ctr=2, then ctr=1 must be a loud REPLAY
    first2 = seal(TOPIC, DEV, 2, TS, body, KEY_A)
    then1 = seal(TOPIC, DEV, 1, TS, body, KEY_A)

    # valid signature over a non-JSON body
    badinner = seal(TOPIC, DEV, 1, TS, b"this is not json", KEY_A, raw_body=True)

    short = base64.b64encode(b"\x00" * 10).decode()

    siglog = "SEAL: BAD SIGNATURE from %s on '%s' - DROPPED" % (DEV, TOPIC)
    return [
        {"name": "valid", "b64": valid, "topic": TOPIC,
         "want": "OK", "want_log": ""},
        {"name": "tampered-body", "b64": tampered, "topic": TOPIC,
         "want": "BAD_SIG", "want_log": siglog},
        {"name": "cross-topic", "b64": valid, "topic": "watcher/telemetry",
         "want": "BAD_SIG",
         "want_log": "SEAL: BAD SIGNATURE from %s on 'watcher/telemetry' - DROPPED" % DEV},
        {"name": "wrong-key", "b64": wrongkey, "topic": TOPIC,
         "want": "BAD_SIG", "want_log": siglog},
        {"name": "bad-magic", "b64": badmagic, "topic": TOPIC,
         "want": "MAGIC",
         "want_log": "SEAL: bad magic on '%s' - DROPPED" % TOPIC},
        # duplicate: same bytes as valid; the test verifies the envelope
        # twice and expects the second guard call to be a silent dup.
        {"name": "duplicate", "b64": valid, "topic": TOPIC,
         "want": "OK_THEN_DUP", "want_log": ""},
        {"name": "stale", "b64": stale, "topic": TOPIC,
         "want": "STALE",
         "want_log": "SEAL: STALE ts (|dt|=300001ms) from %s - DROPPED" % DEV},
        {"name": "kind", "b64": badkind, "topic": TOPIC,
         "want": "KIND",
         "want_log": "SEAL: bad kind 3 on '%s' - DROPPED" % TOPIC},
        {"name": "short", "b64": short, "topic": TOPIC,
         "want": "SHORT",
         "want_log": "SEAL: SHORT msg on '%s' - DROPPED" % TOPIC},
        {"name": "ctr-range", "b64": bigctr, "topic": TOPIC,
         "want": "SHORT",
         "want_log": "SEAL: counter/timestamp out of JS-safe range on '%s' - DROPPED" % TOPIC},
        {"name": "replay", "b64_first": first2, "b64": then1, "topic": TOPIC,
         "want": "REPLAY",
         "want_log": "SEAL: REPLAY ctr 1 <= 2 from %s - DROPPED" % DEV},
        {"name": "bad-inner", "b64": badinner, "topic": TOPIC,
         "want": "BAD_INNER",
         "want_log": "SEAL: verified envelope, bad inner JSON - DROPPED"},
    ]


def write_vectors(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for c in build_cases():
        p = os.path.join(out_dir, c["name"] + ".json")
        with open(p, "w") as f:
            json.dump(c, f, indent=1)
            f.write("\n")
    # pinned public key for the test (page pins this in the harness)
    with open(os.path.join(out_dir, "test-pubkey.hex"), "w") as f:
        f.write(KEY_A.verify_key.encode().hex() + "\n")


def main():
    if "--check" in sys.argv:
        tmp = tempfile.mkdtemp()
        try:
            d = os.path.join(tmp, "kind2")
            write_vectors(d)
            bad = []
            for c in build_cases():
                name = c["name"] + ".json"
                a = open(os.path.join(VEC_DIR, name)).read()
                b = open(os.path.join(d, name)).read()
                if a != b:
                    bad.append(name)
            if bad:
                print("DRIFT:", ", ".join(bad))
                return 1
            print("vectors match committed (%d cases)" % len(build_cases()))
            return 0
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    write_vectors(VEC_DIR)
    print("wrote %d vectors to %s" % (len(build_cases()), VEC_DIR))


if __name__ == "__main__":
    sys.exit(main())
