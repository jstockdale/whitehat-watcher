#!/usr/bin/env python3
"""Deterministic WMS2 kind-1 ENC (D5b) test vector generator.

Writes committed vectors to vectors/WMS2/kind1enc/<case>.json, each with
the envelope, topic, original plaintext body, expected verdict, and
expected log substring. All inputs pinned: fixed test seed (NOT a real
telemetry seed), fixed nonces, fixed timestamps, fixed counters —
regeneration must be byte-identical.

Usage:
  python3 gen_tel_vectors.py            # write vectors/
  python3 gen_tel_vectors.py --check   # regen to temp, diff vs committed
"""
import base64, hashlib, json, os, shutil, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mqtt_seal as mqs

# Pinned test seed. Test-only, never deployed, never a real seed.
SEED = bytes(range(32))
SEED2 = bytes([0xA5]) * 32  # second test-only seed (wrong-key cases)

TOPIC = "watcher/telemetry"
TS = 1758850000000  # pinned (matches kind2 vectors)
DEV = "page-01"

VEC_DIR = os.path.join(HERE, "vectors", "WMS2", "kind1enc")

K_MAC, K_AEAD, FP = mqs.derive_tel_keys(SEED)
K_MAC2, K_AEAD2, _ = mqs.derive_tel_keys(SEED2)


def b64e(b):
    return base64.b64encode(b).decode()


def seal_enc(dev, ctr, ts, body, mac=K_MAC, aead=K_AEAD, nonce=None):
    if isinstance(body, str):
        body = body.encode()
    if nonce is None:
        nonce = bytes([ctr]) * 24  # deterministic per-case default
    return b64e(mqs.seal_telemetry_enc(TOPIC, dev, ctr, ts, body,
                                       mac, aead, nonce24=nonce))


def seal_mac_only(dev, ctr, ts, body):
    if isinstance(body, str):
        body = body.encode()
    return b64e(mqs.seal_telemetry(TOPIC, dev, ctr, ts, body, K_MAC))


def flip(b64s, idx):
    raw = bytearray(base64.b64decode(b64s))
    raw[idx] ^= 0x01
    return b64e(bytes(raw))


def build_cases():
    chunk_body = json.dumps(
        {"t": "snaptest_chunk", "id": 7, "seq": 3, "total": 40,
         "b64": "aGVsbG8td29ybGQtZnJhbWUtZGF0YQ==", "last": False},
        separators=(",", ":"))
    frame_body = json.dumps(
        {"t": "snaptest_chunk", "id": 42, "seq": 17, "total": 64,
         "b64": "R0lGODdhAQABAIAAAP///////ywAAAAAAQABAAACAkQBADs=" * 8,
         "last": False},
        separators=(",", ":"))
    audio_body = json.dumps(
        {"t": "audio", "seq": 11, "b64": "UklGRiQAAABXQVZFZm10IBAAAAABAAEAESsAACJWAAACABAAZGF0YQAAAAA="},
        separators=(",", ":"))

    valid = seal_enc(DEV, 1, TS, chunk_body, nonce=bytes(range(24)))
    frame = seal_enc(DEV, 5, TS, frame_body, nonce=bytes([5]) * 24)
    audio = seal_enc(DEV, 9, TS, audio_body, nonce=bytes([9]) * 24)

    # ciphertext tamper (outer MAC catches it first -> BAD_TAG)
    ct_tamper = flip(valid, mqs.HDR_LEN + 30)
    # outer tag tamper -> BAD_TAG
    tag_tamper = flip(valid, len(base64.b64decode(valid)) - 1)
    # sealed under a different seed entirely -> BAD_TAG
    wrong_seed = seal_enc(DEV, 1, TS, chunk_body, mac=K_MAC2,
                          aead=K_AEAD2, nonce=bytes(range(24)))
    # right MAC key, wrong AEAD key -> outer MAC passes, decrypt fails
    wrong_aead = mqs.seal_telemetry_enc(
        TOPIC, DEV, 1, TS, chunk_body.encode(), K_MAC, K_AEAD2,
        nonce24=bytes(range(24)))
    wrong_aead = b64e(wrong_aead)

    # hand-crafted short ENC body with a VALID outer MAC -> SHORT
    hdr = mqs._build_header(mqs.KIND_TELEM, DEV, 3, TS, flags=mqs.FLAG_ENC)
    short_body = b"too-short"
    short_tag = hashlib.blake2b(mqs._auth_bytes(TOPIC, hdr, short_body),
                                digest_size=mqs.TAG_LEN, key=K_MAC).digest()
    short_enc = b64e(hdr + short_body + short_tag)

    # replay pair: adopt ctr=2, then ctr=1 is a loud REPLAY
    first2 = seal_enc(DEV, 2, TS, chunk_body, nonce=bytes([2]) * 24)
    then1 = seal_enc(DEV, 1, TS, chunk_body, nonce=bytes([3]) * 24)

    # stale (10 min old; harness verifies with now=TS+1)
    stale = seal_enc(DEV, 4, TS - 600000, chunk_body, nonce=bytes([4]) * 24)

    # MAC-only kind-1 (flags=0) still verifies -> OK, plaintext body
    mac_only = seal_mac_only(DEV, 6, TS, '{"t":"status","uptime_s":61}')

    kdf_vec = {
        "name": "kdf", "seed_hex": SEED.hex(),
        "want_mac_hex": K_MAC.hex(), "want_aead_hex": K_AEAD.hex(),
        "want_fp": FP,
        "labels": {"mac": "WMS2-tel-mac-v1", "aead": "WMS2-tel-aead-v1",
                   "fp": "WMS2-tel-fp-v1"},
    }

    def v(name, b64s, want, want_log="", topic=TOPIC, body=None,
          seed_hex=SEED.hex()):
        d = {"name": name, "b64": b64s, "topic": topic, "want": want,
             "want_log": want_log, "seed_hex": seed_hex}
        if body is not None:
            d["body"] = body
        return d

    return [kdf_vec,
        v("valid-enc", valid, "OK", body=chunk_body),
        v("frame-roundtrip", frame, "OK", body=frame_body),
        v("audio-roundtrip", audio, "OK", body=audio_body),
        v("wrong-topic", valid, "BAD_TAG",
          "SEAL: BAD TAG from %s on 'watcher/command' - DROPPED" % DEV,
          topic="watcher/command", body=chunk_body),
        v("ct-tamper", ct_tamper, "BAD_TAG",
          "SEAL: BAD TAG from %s on '%s' - DROPPED" % (DEV, TOPIC),
          body=chunk_body),
        v("outer-tag-tamper", tag_tamper, "BAD_TAG",
          "SEAL: BAD TAG from %s on '%s' - DROPPED" % (DEV, TOPIC),
          body=chunk_body),
        v("wrong-seed", wrong_seed, "BAD_TAG",
          "SEAL: BAD TAG from %s on '%s' - DROPPED" % (DEV, TOPIC),
          body=chunk_body),
        v("wrong-aead-key", wrong_aead, "BAD_TAG",
          "SEAL: AEAD FAIL from %s on '%s' - DROPPED" % (DEV, TOPIC),
          body=chunk_body),
        v("short-enc-body", short_enc, "SHORT",
          "SEAL: SHORT enc body from %s on '%s' - DROPPED" % (DEV, TOPIC)),
        v("no-aead-key", valid, "NO_AEAD_KEY",
          "SEAL: NO AEAD KEY from %s on '%s' - DROPPED" % (DEV, TOPIC),
          body=chunk_body),
        v("replay-first", first2, "OK", body=chunk_body),
        v("replay-then", then1, "REPLAY",
          "SEAL: REPLAY ctr 1 <= 2 from %s - DROPPED" % DEV,
          body=chunk_body),
        v("dup", valid, "OK_DUP", body=chunk_body),
        v("stale", stale, "STALE", body=chunk_body),
        v("mac-only", mac_only, "OK", body='{"t":"status","uptime_s":61}'),
    ]


def write_vectors(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for c in build_cases():
        with open(os.path.join(out_dir, c["name"] + ".json"), "w") as f:
            json.dump(c, f, indent=1, sort_keys=True)
            f.write("\n")
    # pinned seed's public fingerprint (not a secret)
    with open(os.path.join(out_dir, "test-seed-fp.txt"), "w") as f:
        f.write("test-only seed fp (first 16 hex of labeled fp hash): %s\n" % FP)


def main():
    check = len(sys.argv) > 1 and sys.argv[1] == "--check"
    if check:
        tmp = tempfile.mkdtemp()
        try:
            write_vectors(tmp)
            committed = sorted(os.listdir(VEC_DIR))
            regen = sorted(os.listdir(tmp))
            assert committed == regen, "file list differs: %s vs %s" % (
                committed, regen)
            for fn in committed:
                a = open(os.path.join(VEC_DIR, fn), "rb").read()
                b = open(os.path.join(tmp, fn), "rb").read()
                assert a == b, "vector differs: %s" % fn
            print("tel vectors match committed (%d cases)" % len(committed))
        finally:
            shutil.rmtree(tmp)
    else:
        write_vectors(VEC_DIR)
        print("wrote %d vectors to %s" % (len(build_cases()), VEC_DIR))


if __name__ == "__main__":
    main()
