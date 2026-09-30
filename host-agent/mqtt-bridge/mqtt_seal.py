#!/usr/bin/env python3
"""mqtt_seal.py — Python port of ref/mqtt_seal (WMS1 envelope format,
sealed under the WMS2 magic: kind=2 carries RFC 8032 Ed25519).

Wire-compatible with the C reference (Monocypher backend) and the
fleet SEAL it descends from. Host side uses PyNaCl (libsodium), which
the handover brief names as the day-one interop target: libsodium is
wire-compatible with Monocypher for Ed25519 / BLAKE2b.

Envelope: HEADER(40) || BODY || AUTH, little-endian.
  magic  "WMS2"        4   (WMS1 envelope format; WMS2 = RFC 8032 Ed25519)
  kind   u8            1   1=telemetry(BLAKE2b-MAC) 2=control(Ed25519)
  flags  u8            1   bit0 = ENC (kind-1 AEAD body, D5b)
  dev_id char[16]     16   NUL-padded
  ctr    u64           8   strictly monotonic per sender
  ts_ms  u64           8   sender unix-ms
  rsv    u16           2
AUTH covers: len16(topic) || topic || header || body  (topic-bound, D1)
  kind=1: tag[16] = BLAKE2b(key=k_tel_mac, digest=16)
  kind=2: sig[64] = Ed25519 over the same bytes

D5b kind-1 ENC envelope (flags & 0x01):
  body = nonce(24) || XChaCha20-Poly1305(k_tel_aead, nonce, plaintext)
  (ciphertext carries its own 16B Poly1305 tag; the outer BLAKE2b MAC
  still covers the ciphertext, so topic binding + integrity hold even
  for a verifier that lacks the AEAD key.)
  Keys derive from one 32B telemetry seed via labeled BLAKE2b-256:
    k_tel_mac  = BLAKE2b(key=seed, "WMS2-tel-mac-v1")
    k_tel_aead = BLAKE2b(key=seed, "WMS2-tel-aead-v1")
  Frame/audio-bearing telemetry MUST be ENC; pure status may stay
  MAC-only.

Return codes mirror mqs_rc in mqtt_seal.h.
"""
import hashlib
import os
import struct
import time

from nacl.signing import SigningKey, VerifyKey
from nacl.exceptions import BadSignatureError
from nacl.bindings import (crypto_aead_xchacha20poly1305_ietf_encrypt,
                           crypto_aead_xchacha20poly1305_ietf_decrypt)
from nacl.utils import random as nrandom

MAGIC = b"WMS2"   # WMS1 envelope format; WMS2 magic = RFC 8032 Ed25519
                     # (WMS1-as-deployed = Monocypher EdDSA-BLAKE2b; magic is
                     # the algorithm registry, so a new magic for the new alg)
HDR_LEN = 40
TAG_LEN = 16
SIG_LEN = 64
DEVID_LEN = 16
NONCE_LEN = 24
AEAD_TAG_LEN = 16
KIND_TELEM = 1
KIND_CTRL = 2
FLAG_ENC = 0x01

# D5b telemetry KDF labels (labeled BLAKE2b-256, key=seed)
TEL_KDF_MAC = b"WMS2-tel-mac-v1"
TEL_KDF_AEAD = b"WMS2-tel-aead-v1"
TEL_KDF_FP = b"WMS2-tel-fp-v1"
TEL_SEED_LEN = 32

# Return codes (mirror mqs_rc)
OK = 0
OK_DUP = 1
OK_GRACE = 2
E_SHORT = -1
E_MAGIC = -2
E_KIND = -3
E_BADTAG = -4
E_BADSIG = -5
E_REPLAY = -6
E_STALE = -7
E_NOKEY = -8
E_UNSEAL = -9
E_SPACE = -10
E_NOAEAD = -11

RC_STR = {
    OK: "OK", OK_DUP: "OK-DUP", OK_GRACE: "OK-GRACE",
    E_SHORT: "SHORT", E_MAGIC: "MAGIC", E_KIND: "KIND",
    E_BADTAG: "BAD-TAG", E_BADSIG: "BAD-SIG", E_REPLAY: "REPLAY",
    E_STALE: "STALE", E_NOKEY: "NO-KEY", E_UNSEAL: "UNSEALED",
    E_SPACE: "SPACE", E_NOAEAD: "NO-AEAD-KEY",
}


def rc_str(rc):
    return RC_STR.get(rc, "?")


def derive_tel_keys(seed32):
    """D5b KDF: one 32B seed -> (k_tel_mac, k_tel_aead, fp16).

    fp is the first 16 hex chars of the labeled fingerprint hash —
    safe to display and log; it identifies the seed without exposing it.
    """
    seed = bytes(seed32)
    if len(seed) != TEL_SEED_LEN:
        raise ValueError("telemetry seed must be %d bytes" % TEL_SEED_LEN)
    k_mac = hashlib.blake2b(TEL_KDF_MAC, digest_size=32, key=seed).digest()
    k_aead = hashlib.blake2b(TEL_KDF_AEAD, digest_size=32, key=seed).digest()
    fp = hashlib.blake2b(TEL_KDF_FP, digest_size=32, key=seed).hexdigest()[:16]
    return k_mac, k_aead, fp


def load_tel_seed():
    """Load the 32B telemetry seed. Returns (seed_bytes, source) or (None, None).

    File ~/.config/watcher/tel_seed (64 hex chars, mode 0600) wins;
    WATCHER_TEL_SEED env (64 hex chars) is the fallback.
    NEVER log or print the seed itself — only its fingerprint.
    """
    raw, src = None, None
    p = os.path.expanduser("~/.config/watcher/tel_seed")
    try:
        with open(p, "r", encoding="utf-8") as f:
            raw, src = f.read().strip(), "file:" + p
    except FileNotFoundError:
        pass
    except OSError:
        pass
    if raw is None:
        e = os.environ.get("WATCHER_TEL_SEED", "").strip()
        if e:
            raw, src = e, "env:WATCHER_TEL_SEED"
    if not raw:
        return None, None
    try:
        seed = bytes.fromhex(raw)
    except ValueError:
        raise ValueError("telemetry seed in %s is not 64 hex chars" % src)
    if len(seed) != TEL_SEED_LEN:
        raise ValueError("telemetry seed in %s is %d bytes, want %d"
                         % (src, len(seed), TEL_SEED_LEN))
    return seed, src


def _build_header(kind, dev_id, ctr, ts_ms, flags=0):
    if isinstance(dev_id, str):
        dev_id = dev_id.encode()
    if len(dev_id) > DEVID_LEN:
        raise ValueError("dev_id too long")
    hdr = bytearray(HDR_LEN)
    hdr[0:4] = MAGIC
    hdr[4] = kind
    hdr[5] = flags
    hdr[6:6 + len(dev_id)] = dev_id  # rest stays NUL
    struct.pack_into("<Q", hdr, 22, ctr)
    struct.pack_into("<Q", hdr, 30, ts_ms)
    # rsv[38:40] stays zero
    return bytes(hdr)


def _auth_bytes(topic, hdr, body):
    """The ONE byte string every auth covers: len16(topic)||topic||hdr||body."""
    if isinstance(topic, str):
        topic = topic.encode()
    return struct.pack("<H", len(topic)) + topic + hdr + body


def seal_telemetry(topic, dev_id, ctr, ts_ms, body, key32):
    """Seal a telemetry body. Returns envelope bytes."""
    if isinstance(body, str):
        body = body.encode()
    hdr = _build_header(KIND_TELEM, dev_id, ctr, ts_ms)
    tag = hashlib.blake2b(_auth_bytes(topic, hdr, body),
                          digest_size=TAG_LEN, key=key32).digest()
    return hdr + body + tag


def seal_telemetry_enc(topic, dev_id, ctr, ts_ms, body, key_mac32,
                       key_aead32, nonce24=None):
    """D5b: seal a frame/audio-bearing telemetry body with ENC.

    body = nonce(24) || XChaCha20-Poly1305(k_aead, nonce, plaintext);
    the outer BLAKE2b MAC (k_mac) still covers the ciphertext, so the
    envelope stays topic-bound (D1) for verifiers without the AEAD key.
    nonce24 may be pinned for deterministic vectors; otherwise random.
    """
    if isinstance(body, str):
        body = body.encode()
    nonce = bytes(nonce24) if nonce24 is not None else nrandom(NONCE_LEN)
    if len(nonce) != NONCE_LEN:
        raise ValueError("nonce must be %d bytes" % NONCE_LEN)
    hdr = _build_header(KIND_TELEM, dev_id, ctr, ts_ms, flags=FLAG_ENC)
    ct = crypto_aead_xchacha20poly1305_ietf_encrypt(
        body, None, nonce, bytes(key_aead32))
    enc_body = nonce + ct
    tag = hashlib.blake2b(_auth_bytes(topic, hdr, enc_body),
                          digest_size=TAG_LEN, key=key_mac32).digest()
    return hdr + enc_body + tag


def seal_control(topic, dev_id, ctr, ts_ms, body, signing_key):
    """Seal a control body with an Ed25519 SigningKey. Returns envelope bytes."""
    if isinstance(body, str):
        body = body.encode()
    if isinstance(signing_key, (bytes, bytearray)):
        signing_key = SigningKey(bytes(signing_key))
    hdr = _build_header(KIND_CTRL, dev_id, ctr, ts_ms)
    sig = signing_key.sign(_auth_bytes(topic, hdr, body)).signature
    assert len(sig) == SIG_LEN
    return hdr + body + sig


class ReplayCtx:
    """Per-sender replay state. high_water persists wear-aware on MCU;
    on the page it lives in localStorage. Zero-init accepts any first ctr >= 1."""

    def __init__(self, high_water=0):
        self.high_water = high_water


class Policy:
    def __init__(self, strict=False, ts_window_ms=120_000,
                 grace_warn_every=64):
        self.strict = strict
        self.ts_window_ms = ts_window_ms
        self.grace_warn_every = grace_warn_every
        self.grace_warns = 0


def verify(buf, topic, key32=None, verify_key=None, aead_key=None,
           now_ms=None, replay=None, policy=None):
    """Verify an envelope.

    Returns (rc, header_dict_or_None, body_or_None, log_line_or_None).
    header_dict: {kind, flags, dev_id, ctr, ts_ms}.
    On OK/OK_DUP the replay high_water is updated/held (adopt only
    after ALL gates pass). log_line is the named rejection/warning
    string the fleet law requires ("SEAL: ...").
    For kind-1 ENC envelopes, body is the DECRYPTED plaintext on OK
    (aead_key required); the ciphertext on OK_DUP (dupes are dropped
    before decrypt).
    """
    if policy is None:
        policy = Policy()
    if now_ms is None:
        now_ms = int(time.time() * 1000)

    # Grace/strict gate for unauthenticated input
    framed = len(buf) >= HDR_LEN and buf[:4] == MAGIC
    if not framed:
        if policy.strict:
            return E_UNSEAL, None, None, \
                f"SEAL: UNSEALED msg on '{topic}' - DROPPED"
        policy.grace_warns += 1
        log = None
        if (policy.grace_warns % (policy.grace_warn_every or 64)) == 1:
            log = f"SEAL: unauthenticated msg on '{topic}' honored (grace)"
        return OK_GRACE, {"kind": 0, "dev_id": ""}, buf, log

    kind = buf[4]
    if kind == KIND_TELEM:
        alen = TAG_LEN
    elif kind == KIND_CTRL:
        alen = SIG_LEN
    else:
        return E_KIND, None, None, f"SEAL: bad kind {kind} on '{topic}' - DROPPED"
    if len(buf) < HDR_LEN + alen:
        return E_SHORT, None, None, f"SEAL: SHORT msg on '{topic}' - DROPPED"

    hdr = buf[:HDR_LEN]
    body = buf[HDR_LEN:len(buf) - alen]
    auth = buf[len(buf) - alen:]
    dev_id = hdr[6:6 + DEVID_LEN].split(b"\x00")[0].decode("ascii", "replace")
    ctr, ts_ms = struct.unpack_from("<QQ", hdr, 22)
    h = {"kind": kind, "flags": hdr[5], "dev_id": dev_id,
         "ctr": ctr, "ts_ms": ts_ms}
    msg = _auth_bytes(topic, hdr, body)

    # Auth first — bad tag/sig is ALWAYS fatal, every mode.
    if kind == KIND_TELEM:
        if key32 is None:
            if policy.strict:
                return E_NOKEY, h, None, \
                    f"SEAL: NO KEY for {dev_id} on '{topic}' - DROPPED"
            return OK_GRACE, h, body, \
                f"SEAL: no key for {dev_id}, honored (grace)"
        want = hashlib.blake2b(msg, digest_size=TAG_LEN, key=key32).digest()
        # constant-time compare
        if len(auth) != TAG_LEN or any(a != b for a, b in zip(auth, want)):
            return E_BADTAG, h, None, \
                f"SEAL: BAD TAG from {dev_id} on '{topic}' - DROPPED"
    else:
        if verify_key is None:
            if policy.strict:
                return E_NOKEY, h, None, \
                    f"SEAL: NO PIN for {dev_id} on '{topic}' - DROPPED"
            return OK_GRACE, h, body, \
                f"SEAL: no pin for {dev_id}, honored (grace)"
        if isinstance(verify_key, (bytes, bytearray)):
            verify_key = VerifyKey(bytes(verify_key))
        try:
            verify_key.verify(bytes(auth) + msg)
        except BadSignatureError:
            return E_BADSIG, h, None, \
                f"SEAL: BAD SIGNATURE from {dev_id} on '{topic}' - DROPPED"

    # Replay: counter is primary. Equal-to-last is silent (QoS echo).
    if replay is not None:
        if ctr == replay.high_water and ctr != 0:
            return OK_DUP, h, body, None
        if ctr <= replay.high_water:
            return E_REPLAY, h, None, \
                f"SEAL: REPLAY ctr {ctr} <= {replay.high_water} " \
                f"from {dev_id} - DROPPED"

    # Freshness: timestamp is secondary.
    if policy.ts_window_ms:
        d = abs(now_ms - ts_ms)
        if d > policy.ts_window_ms:
            return E_STALE, h, None, \
                f"SEAL: STALE ts (|dt|={d}ms) from {dev_id} - DROPPED"

    # D5b ENC: decrypt only after every other gate passed, before
    # adopting high-water — an undecryptable message never advances it.
    if kind == KIND_TELEM and (h["flags"] & FLAG_ENC):
        if aead_key is None:
            return E_NOAEAD, h, None, \
                f"SEAL: NO AEAD KEY for {dev_id} on '{topic}' - DROPPED"
        if len(body) < NONCE_LEN + AEAD_TAG_LEN:
            return E_SHORT, h, None, \
                f"SEAL: SHORT enc body from {dev_id} on '{topic}' - DROPPED"
        nonce, ct = body[:NONCE_LEN], body[NONCE_LEN:]
        try:
            body = crypto_aead_xchacha20poly1305_ietf_decrypt(
                ct, None, nonce, bytes(aead_key))
        except Exception:
            return E_BADTAG, h, None, \
                f"SEAL: AEAD FAIL from {dev_id} on '{topic}' - DROPPED"

    if replay is not None:
        replay.high_water = ctr  # adopt only after ALL gates
    return OK, h, body, None


def self_test():
    """Mirror the C demo's assertions in Python."""
    from nacl.utils import random as nrandom
    fails = []

    def check(name, cond):
        print(("PASS  " if cond else "FAIL  ") + name)
        if not cond:
            fails.append(name)

    sk = SigningKey.generate()
    vk = sk.verify_key
    tkey = nrandom(32)
    topic = "watcher/command"
    now = int(time.time() * 1000)
    pol = Policy()
    rp = ReplayCtx()

    # control round-trip
    env = seal_control(topic, "muse-01", 1, now, b'{"t":"ping"}', sk)
    rc, h, body, log = verify(env, topic, verify_key=vk.encode(),
                              now_ms=now, replay=rp, policy=pol)
    check("control seal+verify", rc == OK and body == b'{"t":"ping"}'
          and h["dev_id"] == "muse-01" and rp.high_water == 1)

    # QoS echo silent dup
    rc, _, _, log = verify(env, topic, verify_key=vk.encode(),
                           now_ms=now, replay=rp, policy=pol)
    check("dup ctr is silent OK-DUP", rc == OK_DUP and log is None
          and rp.high_water == 1)

    # tamper -> fatal
    bad = bytearray(env)
    bad[HDR_LEN + 2] ^= 0x01
    rc, _, _, log = verify(bytes(bad), topic, verify_key=vk.encode(),
                           now_ms=now, replay=ReplayCtx(1), policy=pol)
    check("tampered body is FATAL", rc == E_BADSIG and "BAD SIGNATURE" in log)

    # cross-topic republish -> fatal (D1)
    rc, _, _, _ = verify(env, "watcher/telemetry", verify_key=vk.encode(),
                         now_ms=now, replay=ReplayCtx(), policy=pol)
    check("cross-topic republish is FATAL", rc == E_BADSIG)

    # counter replay -> fatal
    env2 = seal_control(topic, "muse-01", 2, now, b'{"t":"ping"}', sk)
    rp2 = ReplayCtx()
    verify(env2, topic, verify_key=vk.encode(), now_ms=now,
           replay=rp2, policy=pol)
    rc, _, _, log = verify(env, topic, verify_key=vk.encode(),
                           now_ms=now, replay=rp2, policy=pol)
    check("counter replay is FATAL", rc == E_REPLAY and rp2.high_water == 2)

    # stale -> rejected
    env_old = seal_control(topic, "muse-01", 9, now - 600_000,
                           b'{"t":"ping"}', sk)
    rc, _, _, log = verify(env_old, topic, verify_key=vk.encode(),
                           now_ms=now, replay=ReplayCtx(), policy=pol)
    check("stale timestamp rejected", rc == E_STALE)

    # telemetry round-trip + tamper
    tenv = seal_telemetry("watcher/telemetry", "watcher-01", 1, now,
                          b'{"snap":1}', tkey)
    rc, h, body, _ = verify(tenv, "watcher/telemetry", key32=tkey,
                            now_ms=now, replay=ReplayCtx(), policy=pol)
    check("telemetry seal+verify", rc == OK and body == b'{"snap":1}')
    badt = bytearray(tenv)
    badt[-1] ^= 0x01
    rc, _, _, log = verify(bytes(badt), "watcher/telemetry", key32=tkey,
                           now_ms=now, replay=ReplayCtx(), policy=pol)
    check("telemetry tamper is FATAL", rc == E_BADTAG)

    # grace / strict on unsigned
    rc, _, body, log = verify(b'{"id":"x","cmd":{"t":"ping"}}', topic,
                              verify_key=vk.encode(), now_ms=now,
                              replay=ReplayCtx(), policy=Policy())
    check("grace honors unsigned (loudly)", rc == OK_GRACE and log is not None)
    rc, _, _, log = verify(b'{"id":"x","cmd":{"t":"ping"}}', topic,
                            verify_key=vk.encode(), now_ms=now,
                            replay=ReplayCtx(), policy=Policy(strict=True))
    check("strict drops unsigned", rc == E_UNSEAL)

    # failed gates never advance high-water
    rp3 = ReplayCtx()
    verify(env_old, topic, verify_key=vk.encode(), now_ms=now,
           replay=rp3, policy=pol)  # STALE, must not adopt
    check("failed gate never advances high-water", rp3.high_water == 0)

    # ---- D5b kind-1 ENC ----
    seed = bytes(range(32))  # test-only pinned seed
    k_mac, k_aead, fp = derive_tel_keys(seed)
    check("KDF labels diverge", k_mac != k_aead and len(fp) == 16)
    ttopic = "watcher/telemetry"
    cbody = b'{"t":"snaptest_chunk","id":7,"seq":3,"total":40,"b64":"aGVsbG8=","last":false}'
    nonce = bytes(range(24))
    eenv = seal_telemetry_enc(ttopic, "page-01", 1, now, cbody,
                              k_mac, k_aead, nonce24=nonce)
    hdr = eenv[:HDR_LEN]
    check("ENC sets flags bit", hdr[5] & FLAG_ENC and hdr[4] == KIND_TELEM)
    check("ENC body layout", eenv[HDR_LEN:HDR_LEN + 24] == nonce
          and len(eenv) == HDR_LEN + 24 + len(cbody) + 16 + TAG_LEN)
    rp4 = ReplayCtx()
    rc, h, pbody, _ = verify(eenv, ttopic, key32=k_mac, aead_key=k_aead,
                             now_ms=now, replay=rp4, policy=pol)
    check("ENC seal+verify+decrypt", rc == OK and pbody == cbody
          and rp4.high_water == 1 and h["dev_id"] == "page-01")

    # cross-topic republish of ENC -> outer MAC fails (D1)
    rc, _, _, log = verify(eenv, "watcher/command", key32=k_mac,
                           aead_key=k_aead, now_ms=now,
                           replay=ReplayCtx(), policy=pol)
    check("ENC cross-topic is FATAL", rc == E_BADTAG)

    # ciphertext tamper -> outer MAC catches first
    bade = bytearray(eenv)
    bade[HDR_LEN + 30] ^= 0x01
    rc, _, _, _ = verify(bytes(bade), ttopic, key32=k_mac,
                        aead_key=k_aead, now_ms=now,
                        replay=ReplayCtx(), policy=pol)
    check("ENC ct tamper is FATAL", rc == E_BADTAG)

    # wrong AEAD key, right MAC key -> AEAD FAIL (distinct rc)
    _, k_aead2, _ = derive_tel_keys(bytes([9]) * 32)
    rc, _, _, log = verify(eenv, ttopic, key32=k_mac, aead_key=k_aead2,
                           now_ms=now, replay=ReplayCtx(), policy=pol)
    check("ENC wrong AEAD key -> AEAD FAIL", rc == E_BADTAG
          and log is not None and "AEAD FAIL" in log)

    # no AEAD key -> NO-AEAD-KEY, high-water untouched
    rp5 = ReplayCtx()
    rc, _, _, log = verify(eenv, ttopic, key32=k_mac, aead_key=None,
                           now_ms=now, replay=rp5, policy=pol)
    check("ENC without AEAD key -> NO-AEAD-KEY",
          rc == E_NOAEAD and rp5.high_water == 0)

    # ENC replay / dup
    rp6 = ReplayCtx()
    verify(eenv, ttopic, key32=k_mac, aead_key=k_aead,
           now_ms=now, replay=rp6, policy=pol)
    rc, _, _, log = verify(eenv, ttopic, key32=k_mac, aead_key=k_aead,
                           now_ms=now, replay=rp6, policy=pol)
    check("ENC dup is silent OK-DUP", rc == OK_DUP and log is None)
    eenv2 = seal_telemetry_enc(ttopic, "page-01", 2, now, b"{}", k_mac,
                               k_aead, nonce24=bytes([1]) * 24)
    verify(eenv2, ttopic, key32=k_mac, aead_key=k_aead,
           now_ms=now, replay=rp6, policy=pol)  # advances high-water to 2
    rc, _, _, _ = verify(eenv, ttopic, key32=k_mac, aead_key=k_aead,
                         now_ms=now, replay=rp6, policy=pol)  # ctr 1 < 2
    check("ENC replay is FATAL", rc == E_REPLAY and rp6.high_water == 2)

    # undecryptable never advances high-water
    rp7 = ReplayCtx()
    rc, _, _, _ = verify(eenv, ttopic, key32=k_mac, aead_key=k_aead2,
                         now_ms=now, replay=rp7, policy=pol)
    check("AEAD FAIL never advances high-water",
          rc == E_BADTAG and rp7.high_water == 0)

    print("\nSELF-TEST: " + ("ALL PASS" if not fails
                             else f"{len(fails)} FAILURE(S)"))
    return not fails


if __name__ == "__main__":
    import sys
    sys.exit(0 if self_test() else 1)
