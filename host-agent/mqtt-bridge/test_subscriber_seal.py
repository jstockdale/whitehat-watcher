#!/usr/bin/env python3
"""test_subscriber_seal.py — brokerless test of the subscriber's D5b
sealed_chunk unwrap path: seed loading, decrypt, tamper rejection,
no-seed drop, non-chunk passthrough. Run: python3 test_subscriber_seal.py
"""
import base64, json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# test-only seed via env (never touches ~/.config/watcher/tel_seed)
os.environ["WATCHER_TEL_SEED"] = bytes(range(32)).hex()

import mqtt_seal as mqs
import subscriber as sub

fails = []


def check(name, cond, extra=""):
    print(("ok  " if cond else "FAIL") + "  " + name)
    if not cond:
        fails.append(name + (" :: " + extra if extra else ""))


# capture log lines instead of writing telemetry.log
logged = []
sub.log_line = logged.append

sub._tel_keys_init()
check("seed/loaded", sub._TEL["aead"] is not None and sub._TEL["fp"] is not None)
check("seed/fp-only-logged",
      all("000102" not in l and bytes(range(32)).hex() not in l for l in logged),
      str(logged))

TOPIC = "watcher/telemetry"
k_mac, k_aead, _ = mqs.derive_tel_keys(bytes(range(32)))
now = int(time.time() * 1000)


def sealed_chunk(body_obj, **kw):
    env = mqs.seal_telemetry_enc(
        TOPIC, "page-01", kw.get("ctr", 1), kw.get("ts", now),
        json.dumps(body_obj).encode(), k_mac, k_aead)
    return json.dumps({"t": "sealed_chunk", "enc": base64.b64encode(env).decode()})


chunk = {"t": "snaptest_chunk", "id": 3, "seq": 0, "total": 1,
         "b64": "aGVsbG8=", "last": True}

# valid chunk decrypts to the original JSON
raw, drop = sub._unwrap_sealed_chunk(sealed_chunk(chunk), TOPIC)
check("unwrap/ok", not drop and json.loads(raw) == chunk, repr(raw))

# tampered ciphertext is dropped with a named log line
bad = bytearray(base64.b64decode(json.loads(sealed_chunk(chunk))["enc"]))
bad[mqs.HDR_LEN + 30] ^= 0x01
badraw = json.dumps({"t": "sealed_chunk",
                     "enc": base64.b64encode(bytes(bad)).decode()})
n0 = len(logged)
raw, drop = sub._unwrap_sealed_chunk(badraw, TOPIC)
check("unwrap/tamper-drop", drop and raw is None)
check("unwrap/tamper-logged",
      any("SEAL: BAD TAG" in l for l in logged[n0:]), str(logged[n0:]))

# replay of the same chunk is dropped silently-ish (dup)
raw, drop = sub._unwrap_sealed_chunk(sealed_chunk(chunk), TOPIC)
check("unwrap/dup-drop", drop and raw is None)

# non-chunk lines pass through untouched
plain = '{"t":"heartbeat","uptime_s":61}'
raw, drop = sub._unwrap_sealed_chunk(plain, TOPIC)
check("unwrap/passthrough", not drop and raw == plain)
raw, drop = sub._unwrap_sealed_chunk("not json", TOPIC)
check("unwrap/nonjson-passthrough", not drop and raw == "not json")

# no seed provisioned -> sealed chunk dropped, plaintext still flows
sub._TEL["aead"] = None
sub._TEL["mac"] = None
n1 = len(logged)
raw, drop = sub._unwrap_sealed_chunk(
    sealed_chunk(chunk, ctr=99), TOPIC)
check("unwrap/no-seed-drop", drop and raw is None)
check("unwrap/no-seed-logged",
      any("SEAL: NO TEL SEED" in l for l in logged[n1:]))
raw, drop = sub._unwrap_sealed_chunk(plain, TOPIC)
check("unwrap/no-seed-plain-flows", not drop and raw == plain)

print("\nRESULT: " + ("ALL PASS" if not fails else "FAILURES: %s" % fails))
sys.exit(0 if not fails else 1)
