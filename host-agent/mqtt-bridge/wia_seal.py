#!/usr/bin/env python3
"""wia_seal.py — WIA-SEAL v1 (WIA1) session protocol, host side.

Implements the Phase 3 session-certificate protocol per
~/workspace/user/files/WIA-SEAL-v1_20260925.md (bench profile:
local mint-cert CLI instead of AWS KMS).

Wire: HEADER(64) || BODY || AUTH, little-endian, magic "WIA1".
  kind=1 TEL  (device->agent): tag16 BLAKE2b-keyed, or XChaCha20-Poly1305 when ENC
  kind=2 CTL  (agent->device): sig64 Ed25519 by the *session* key
  kind=3 CERT (agent->device): sig64 Ed25519 by the *root* key (bench root)
  kind=4 CHAL (device->agent): sig64 Ed25519 by the device identity key

Auth input:
  kinds 1,2,5 (topic-bound): S = u16le(len(topic)) || topic || header || body
  kinds 3,4 (transport-agnostic): S = header || body

Session keys (both ends): ss = X25519(own_eph_secret, peer_eph_pub);
  k_label = BLAKE2b(key=ss, out=32, msg=b"wia1:"+label+b":"+session_id)
labels: tel-mac, tel-aead, ctl-mac, ctl-aead. ss is wiped after derivation.

Transport encoding: base64 inside {"sealed":"<b64>"} — same convention
as the WMS2 path, so page/broker plumbing is untouched.

Run the self-tests:  python3 wia_seal.py
"""
import base64
import hashlib
import os
import struct
import sys

try:
    from nacl.signing import SigningKey, VerifyKey
    from nacl.public import PrivateKey
    from nacl.bindings import (
        crypto_scalarmult,
        crypto_aead_xchacha20poly1305_ietf_encrypt,
        crypto_aead_xchacha20poly1305_ietf_decrypt,
    )
    from nacl.hash import blake2b as _nacl_blake2b
    from nacl.utils import random as nrandom
    HAVE_NACL = True
except ImportError:
    HAVE_NACL = False

MAGIC = b"WIA1"
HDR_LEN = 64

KIND_TEL = 1
KIND_CTL = 2
KIND_CERT = 3
KIND_CHAL = 4
KIND_CTL_MAC = 5  # reserved

FLAG_ENC = 0x01

# Scope bitmap (§7)
SCOPE_MOUSE = 0x01
SCOPE_KEYBOARD = 0x02
SCOPE_CONSUMER = 0x04

# msg_type opcodes (§7)
MT_CTL_BATCH = 0x0001
MT_CTL_DISARM = 0x0002
MT_CTL_PING = 0x0003
MT_TEL_ARMED_ACK = 0x0001
MT_TEL_STATUS = 0x0002
MT_TEL_FRAME = 0x0003
MT_TEL_SECLOG = 0x0004

CERT_BODY_LEN = 135
CHAL_BODY_LEN = 52

SESSION_ID_LEN = 16
NONCE_LEN = 24


def _req_nacl():
    if not HAVE_NACL:
        raise RuntimeError("PyNaCl required: pip install pynacl")


def b2b_keyed(key: bytes, msg: bytes, out: int = 32) -> bytes:
    """BLAKE2b with key, arbitrary output length (libsodium generichash).

    NOTE: nacl.hash.blake2b returns hex ASCII by default; force Raw so
    tags/keys are real bytes of length `out`."""
    from nacl import encoding
    return _nacl_blake2b(msg, digest_size=out, key=key,
                         encoder=encoding.RawEncoder)


def pack_header(kind: int, flags: int, dev_id: bytes, session_id: bytes,
                ctr: int, ts_rel_ms: int, msg_type: int) -> bytes:
    assert len(dev_id) == 16 and len(session_id) == 16
    assert flags & ~FLAG_ENC == 0, "reserved flag bits must be zero"
    return struct.pack("<4sBB16s16sQQHQ",
                       MAGIC, kind, flags, dev_id, session_id,
                       ctr, ts_rel_ms, msg_type, 0)


def parse_header(hdr: bytes):
    """Returns dict or raises ValueError on structural reject (§6 steps 1-2)."""
    if len(hdr) != HDR_LEN:
        raise ValueError("MALFORMED: header len != 64")
    magic, kind, flags, dev_id, session_id, ctr, ts_rel_ms, msg_type, rsv = \
        struct.unpack("<4sBB16s16sQQHQ", hdr)
    if magic != MAGIC:
        raise ValueError("MALFORMED: bad magic")
    if kind not in (KIND_TEL, KIND_CTL, KIND_CERT, KIND_CHAL, KIND_CTL_MAC):
        raise ValueError("MALFORMED: unknown kind")
    if flags & ~FLAG_ENC:
        raise ValueError("MALFORMED: nonzero reserved flags")
    if rsv != 0:
        raise ValueError("MALFORMED: nonzero rsv")
    return {"kind": kind, "flags": flags, "dev_id": dev_id,
            "session_id": session_id, "ctr": ctr, "ts_rel_ms": ts_rel_ms,
            "msg_type": msg_type}


def auth_input_topic_bound(topic: bytes, header: bytes, body: bytes) -> bytes:
    return struct.pack("<H", len(topic)) + topic + header + body


# --------------------------------------------------------------------------
# cert_body (135 bytes, §5.3) / challenge_body (52 bytes, §5.4)
# --------------------------------------------------------------------------

def build_cert_body(scope: int, ttl_s: int, device_id: bytes, nonce: bytes,
                    agent_ed_pk: bytes, agent_x_pk: bytes,
                    device_x_pk: bytes) -> bytes:
    assert len(device_id) == 16 and len(nonce) == 16
    assert len(agent_ed_pk) == 32 and len(agent_x_pk) == 32
    assert len(device_x_pk) == 32
    return struct.pack("<BHI16s16s32s32s32s",
                       1, scope, ttl_s, device_id, nonce,
                       agent_ed_pk, agent_x_pk, device_x_pk)


def parse_cert_body(body: bytes):
    if len(body) != CERT_BODY_LEN:
        raise ValueError("BAD BODY: cert_body len != 135")
    cert_ver, scope, ttl_s, device_id, nonce, agent_ed_pk, agent_x_pk, \
        device_x_pk = struct.unpack("<BHI16s16s32s32s32s", body)
    if cert_ver != 1:
        raise ValueError("BAD BODY: cert_ver != 1")
    return {"scope": scope, "ttl_s": ttl_s, "device_id": device_id,
            "nonce": nonce, "agent_ed_pk": agent_ed_pk,
            "agent_x_pk": agent_x_pk, "device_x_pk": device_x_pk}


def build_challenge_body(scope_offered: int, nonce: bytes,
                         device_x_pk: bytes) -> bytes:
    assert len(nonce) == 16 and len(device_x_pk) == 32
    return struct.pack("<BBH16s32s", 1, 1, scope_offered, nonce, device_x_pk)


def parse_challenge_body(body: bytes):
    if len(body) != CHAL_BODY_LEN:
        raise ValueError("BAD BODY: challenge_body len != 52")
    chal_ver, proto_max, scope_offered, nonce, device_x_pk = \
        struct.unpack("<BBH16s32s", body)
    if chal_ver != 1:
        raise ValueError("BAD BODY: chal_ver != 1")
    return {"proto_max": proto_max, "scope_offered": scope_offered,
            "nonce": nonce, "device_x_pk": device_x_pk}


def session_id_of_cert(cert_body: bytes) -> bytes:
    return hashlib.blake2b(cert_body, digest_size=16).digest()


def derive_session_keys(ss: bytes, session_id: bytes):
    """Labeled KDF (§3). Returns dict of four 32-byte keys."""
    keys = {}
    for label in ("tel-mac", "tel-aead", "ctl-mac", "ctl-aead"):
        keys[label] = b2b_keyed(ss, b"wia1:" + label.encode() +
                                b":" + session_id, out=32)
    return keys


# --------------------------------------------------------------------------
# Envelope seal / verify
# --------------------------------------------------------------------------

def seal_envelope(kind: int, body: bytes, topic: bytes | None,
                 dev_id: bytes, session_id: bytes, ctr: int,
                 ts_rel_ms: int, msg_type: int,
                 sign_key: SigningKey | None = None,
                 mac_key: bytes | None = None,
                 aead_key: bytes | None = None) -> bytes:
    """Build a complete WIA1 envelope. Exactly one of sign/mac/aead applies."""
    _req_nacl()
    flags = FLAG_ENC if aead_key else 0
    if kind in (KIND_CERT, KIND_CHAL):
        assert topic is None
    hdr = pack_header(kind, flags, dev_id, session_id, ctr, ts_rel_ms,
                      msg_type)
    if kind in (KIND_CTL, KIND_CERT, KIND_CHAL):
        assert sign_key is not None
        s = (hdr + body) if kind in (KIND_CERT, KIND_CHAL) else \
            auth_input_topic_bound(topic, hdr, body)
        auth = sign_key.sign(s).signature
    elif kind == KIND_TEL:
        if aead_key is not None:
            nonce = nrandom(NONCE_LEN)
            aad = struct.pack("<H", len(topic)) + topic + hdr
            ct = crypto_aead_xchacha20poly1305_ietf_encrypt(
                body, aad, nonce, aead_key)
            return hdr + nonce + ct  # tag is the Poly1305 trailer
        else:
            assert mac_key is not None
            s = auth_input_topic_bound(topic, hdr, body)
            auth = b2b_keyed(mac_key, s, out=16)
    else:
        raise ValueError("unsupported kind for seal")
    return hdr + body + auth


def verify_envelope(env: bytes, topic: bytes | None,
                    verify_key: VerifyKey | None = None,
                    mac_key: bytes | None = None,
                    aead_key: bytes | None = None):
    """Verify a WIA1 envelope. Returns (header_dict, body).
    Raises ValueError with the §6 named log string on any rejection."""
    _req_nacl()
    if len(env) < HDR_LEN + 16:
        raise ValueError("MALFORMED: envelope too short")
    hdr = env[:HDR_LEN]
    h = parse_header(hdr)
    kind = h["kind"]
    if kind in (KIND_CTL, KIND_CERT, KIND_CHAL):
        if len(env) < HDR_LEN + 64:
            raise ValueError("MALFORMED: envelope too short for sig")
        body, sig = env[HDR_LEN:-64], env[-64:]
        s = (hdr + body) if kind in (KIND_CERT, KIND_CHAL) else \
            auth_input_topic_bound(topic, hdr, body)
        assert verify_key is not None
        try:
            verify_key.verify(s, sig)
        except Exception:
            raise ValueError("BAD SIG - DROPPED")
    elif kind == KIND_TEL:
        if h["flags"] & FLAG_ENC:
            if len(env) < HDR_LEN + NONCE_LEN + 16:
                raise ValueError("MALFORMED: ENC envelope too short")
            nonce, ct = env[HDR_LEN:HDR_LEN + NONCE_LEN], \
                env[HDR_LEN + NONCE_LEN:]
            aad = struct.pack("<H", len(topic)) + topic + hdr
            assert aead_key is not None
            try:
                pt = crypto_aead_xchacha20poly1305_ietf_decrypt(
                    ct, aad, nonce, aead_key)
            except Exception:
                raise ValueError("BAD SIG - DROPPED")
            return h, pt
        else:
            if len(env) < HDR_LEN + 16:
                raise ValueError("MALFORMED: TEL envelope too short")
            body, tag = env[HDR_LEN:-16], env[-16:]
            s = auth_input_topic_bound(topic, hdr, body)
            assert mac_key is not None
            if b2b_keyed(mac_key, s, out=16) != tag:
                raise ValueError("BAD SIG - DROPPED")
    else:
        raise ValueError("MALFORMED: kind not implemented here")
    return h, body


def b64_wrap(env: bytes) -> str:
    import json
    return json.dumps({"sealed": base64.b64encode(env).decode()})


def b64_unwrap(payload: str) -> bytes:
    import json
    return base64.b64decode(json.loads(payload)["sealed"])


# --------------------------------------------------------------------------
# Agent-side session client (established session)
# --------------------------------------------------------------------------

class WiaSession:
    """Agent side of an established WIA1 session.

    Constructed from the mint-cert outputs: session_id, scope, ttl_s,
    the agent's session keys (ed_sk, x_sk — RAM only), and the ECDH-derived
    key set. Signs kind-2 CTL (topic-bound), verifies kind-1 TEL (MAC/ENC),
    enforces strictly-monotonic counters per direction.
    """

    def __init__(self, dev_id: bytes, session_id: bytes, scope: int,
                 ttl_s: int, ed_sk: bytes, keys: dict,
                 ctl_topic: bytes, tel_topic: bytes):
        _req_nacl()
        self.dev_id = dev_id
        self.session_id = session_id
        self.scope = scope
        self.ttl_s = ttl_s
        self._ed = SigningKey(ed_sk)
        self._keys = keys
        self.ctl_topic = ctl_topic
        self.tel_topic = tel_topic
        self.tx_ctr = 0      # next kind-2 ctr to send (first = 1)
        self.rx_hw = 0       # kind-1 high-water
        self.t0_mono = None  # set from TEL_ARMED_ACK (agent clock ref)

    @property
    def ed_pk(self) -> bytes:
        return bytes(self._ed.verify_key)

    def sign_ctl(self, msg_type: int, body: bytes, ts_rel_ms: int) -> bytes:
        self.tx_ctr += 1
        return seal_envelope(KIND_CTL, body, self.ctl_topic, self.dev_id,
                             self.session_id, self.tx_ctr, ts_rel_ms,
                             msg_type, sign_key=self._ed)

    def verify_tel(self, env: bytes):
        h, body = verify_envelope(
            env, self.tel_topic,
            mac_key=self._keys["tel-mac"], aead_key=self._keys["tel-aead"])
        if h["session_id"] != self.session_id:
            raise ValueError("STALE SESSION - DROPPED")
        ctr = h["ctr"]
        if ctr == self.rx_hw:
            raise ValueError("DUP (silent) - IGNORED")
        if ctr < self.rx_hw:
            raise ValueError(f"REPLAY ctr {ctr} <= {self.rx_hw} - DROPPED")
        self.rx_hw = ctr
        return h, body

    def check_scope(self, needed: int):
        if self.scope & needed != needed:
            raise ValueError(f"SCOPE VIOLATION: need 0x{needed:02x}, "
                             f"have 0x{self.scope:02x}")


# --------------------------------------------------------------------------
# Self-tests
# --------------------------------------------------------------------------

def _t(name, fn):
    try:
        fn()
        print(f"PASS  {name}")
        return True
    except Exception as e:
        print(f"FAIL  {name}: {e}")
        return False


def self_test():
    _req_nacl()
    ok = True
    dev_id = b"wia-0123456789ab"[:16].ljust(16, b"\x00")

    # --- header round-trip + structural rejects ---
    def t_hdr():
        h = pack_header(KIND_CTL, 0, dev_id, bytes(16), 7, 1234,
                        MT_CTL_PING)
        p = parse_header(h)
        assert p["kind"] == KIND_CTL and p["ctr"] == 7
        bad = bytearray(h)
        bad[0] = ord("X")
        try:
            parse_header(bytes(bad))
            raise AssertionError("bad magic accepted")
        except ValueError as e:
            assert "MALFORMED" in str(e)
        bad = bytearray(h)
        bad[5] = 0x02  # reserved flag bit
        try:
            parse_header(bytes(bad))
            raise AssertionError("reserved flag accepted")
        except ValueError as e:
            assert "MALFORMED" in str(e)
    ok &= _t("header pack/parse + structural rejects", t_hdr)

    # --- cert_body 135 bytes, challenge_body 52 bytes ---
    def t_bodies():
        cb = build_cert_body(SCOPE_MOUSE | SCOPE_KEYBOARD, 3600, dev_id,
                             bytes(16), bytes(32), bytes(32), bytes(32))
        assert len(cb) == CERT_BODY_LEN
        c = parse_cert_body(cb)
        assert c["scope"] == 0x03 and c["ttl_s"] == 3600
        sid = session_id_of_cert(cb)
        assert len(sid) == 16
        ch = build_challenge_body(0x07, bytes(16), bytes(32))
        assert len(ch) == CHAL_BODY_LEN
        assert parse_challenge_body(ch)["scope_offered"] == 0x07
    ok &= _t("cert/challenge bodies + session_id", t_bodies)

    # --- kind 3 cert: root signs, host verifies ---
    def t_cert():
        root = SigningKey.generate()
        agent_ed = SigningKey.generate()
        agent_x = PrivateKey.generate()
        dev_x_pub = bytes(PrivateKey.generate().public_key)
        nonce = nrandom(16)
        cb = build_cert_body(SCOPE_MOUSE, 7200, dev_id, nonce,
                             bytes(agent_ed.verify_key), bytes(agent_x.public_key),
                             dev_x_pub)
        env = seal_envelope(KIND_CERT, cb, None, dev_id, bytes(16), 0, 0,
                            0, sign_key=root)
        h, body = verify_envelope(env, None, verify_key=root.verify_key)
        assert h["kind"] == KIND_CERT and parse_cert_body(body)["ttl_s"] == 7200
        # tamper -> BAD SIG
        bad = bytearray(env)
        bad[HDR_LEN + 10] ^= 1
        try:
            verify_envelope(bytes(bad), None, verify_key=root.verify_key)
            raise AssertionError("tampered cert accepted")
        except ValueError as e:
            assert "BAD SIG" in str(e)
    ok &= _t("kind-3 cert sign/verify + tamper fatal", t_cert)

    # --- kind 4 challenge: device identity signs ---
    def t_chal():
        dev_idk = SigningKey.generate()
        ch = build_challenge_body(0x07, nrandom(16),
                                  bytes(PrivateKey.generate().public_key))
        env = seal_envelope(KIND_CHAL, ch, None, dev_id, bytes(16), 0, 0,
                            0, sign_key=dev_idk)
        h, body = verify_envelope(env, None, verify_key=dev_idk.verify_key)
        assert parse_challenge_body(body)["scope_offered"] == 0x07
    ok &= _t("kind-4 challenge sign/verify", t_chal)

    # --- kind 2 control: topic-bound, cross-topic fails ---
    def t_ctl():
        sess = SigningKey.generate()
        sid = nrandom(16)
        topic = b"wia/test/ctl"
        env = seal_envelope(KIND_CTL, b"\x01\x02", topic, dev_id, sid, 1,
                            500, MT_CTL_BATCH, sign_key=sess)
        h, body = verify_envelope(env, topic, verify_key=sess.verify_key)
        assert h["ctr"] == 1 and body == b"\x01\x02"
        try:
            verify_envelope(env, b"wia/test/tel",
                            verify_key=sess.verify_key)
            raise AssertionError("cross-topic accepted")
        except ValueError as e:
            assert "BAD SIG" in str(e)
    ok &= _t("kind-2 topic binding (cross-topic dies)", t_ctl)

    # --- ECDH + labeled KDF both directions, then kind-1 MAC + ENC ---
    def t_session_keys():
        a_priv, b_priv = PrivateKey.generate(), PrivateKey.generate()
        ss_a = crypto_scalarmult(bytes(a_priv), bytes(b_priv.public_key))
        ss_b = crypto_scalarmult(bytes(b_priv), bytes(a_priv.public_key))
        assert ss_a == ss_b
        sid = nrandom(16)
        ka, kb = derive_session_keys(ss_a, sid), derive_session_keys(ss_b, sid)
        assert ka == kb and len(ka["tel-aead"]) == 32
        topic = b"wia/test/tel"
        # MAC mode
        env = seal_envelope(KIND_TEL, b"status-bytes", topic, dev_id, sid,
                            3, 900, MT_TEL_STATUS, mac_key=ka["tel-mac"])
        h, body = verify_envelope(env, topic, mac_key=kb["tel-mac"])
        assert body == b"status-bytes"
        # ENC mode
        env2 = seal_envelope(KIND_TEL, b"\xff\xd8fakejpeg", topic, dev_id,
                             sid, 4, 950, MT_TEL_FRAME, aead_key=ka["tel-aead"])
        h2, pt = verify_envelope(env2, topic, aead_key=kb["tel-aead"])
        assert pt == b"\xff\xd8fakejpeg" and h2["flags"] & FLAG_ENC
        # ENC tamper -> fatal
        bad = bytearray(env2)
        bad[-1] ^= 1
        try:
            verify_envelope(bytes(bad), topic, aead_key=kb["tel-aead"])
            raise AssertionError("tampered frame accepted")
        except ValueError as e:
            assert "BAD SIG" in str(e)
    ok &= _t("ECDH+KDF, kind-1 MAC and ENC, tamper fatal", t_session_keys)

    # --- WiaSession: sign CTL / verify TEL / replay law ---
    def t_session_client():
        from nacl.public import PrivateKey
        from nacl.bindings import crypto_scalarmult
        a_x = PrivateKey.generate()
        d_x = PrivateKey.generate()
        ss = crypto_scalarmult(bytes(a_x), bytes(d_x.public_key))
        sid = nrandom(16)
        keys = derive_session_keys(ss, sid)
        ed_sk = bytes(SigningKey.generate())
        sess = WiaSession(dev_id, sid, SCOPE_MOUSE, 3600, ed_sk, keys,
                          b"wia/t/ctl", b"wia/t/tel")
        # agent -> device: sign two CTLs, ctr 1 then 2
        e1 = sess.sign_ctl(MT_CTL_PING, b"", 100)
        e2 = sess.sign_ctl(MT_CTL_PING, b"", 200)
        h1, _ = verify_envelope(e1, b"wia/t/ctl",
                                verify_key=VerifyKey(sess.ed_pk))
        assert h1["ctr"] == 1 and h1["session_id"] == sid
        # device -> agent: MAC telemetry, replay law enforced
        t1 = seal_envelope(KIND_TEL, b"s1", b"wia/t/tel", dev_id, sid, 1,
                           50, MT_TEL_STATUS, mac_key=keys["tel-mac"])
        t2 = seal_envelope(KIND_TEL, b"s2", b"wia/t/tel", dev_id, sid, 2,
                           60, MT_TEL_STATUS, mac_key=keys["tel-mac"])
        sess.verify_tel(t1)
        sess.verify_tel(t2)
        try:
            sess.verify_tel(t2)  # dup of last -> silent ignore
            raise AssertionError("dup not detected")
        except ValueError as e:
            assert "DUP" in str(e)
        t0 = seal_envelope(KIND_TEL, b"s0", b"wia/t/tel", dev_id, sid, 1,
                           40, MT_TEL_STATUS, mac_key=keys["tel-mac"])
        # ctr 1 < hw 2 -> loud replay
        try:
            sess.verify_tel(t0)
            raise AssertionError("replay not detected")
        except ValueError as e:
            assert "REPLAY" in str(e)
        # wrong session -> stale
        other = dict(keys)
        s2 = WiaSession(dev_id, nrandom(16), SCOPE_MOUSE, 3600, ed_sk,
                        other, b"wia/t/ctl", b"wia/t/tel")
        try:
            s2.verify_tel(t2)
            raise AssertionError("stale session not detected")
        except ValueError as e:
            assert "STALE SESSION" in str(e)
        # scope check
        sess.check_scope(SCOPE_MOUSE)
        try:
            sess.check_scope(SCOPE_KEYBOARD)
            raise AssertionError("scope violation not detected")
        except ValueError as e:
            assert "SCOPE VIOLATION" in str(e)
    ok &= _t("WiaSession sign/verify/replay/scope", t_session_client)

    # --- b64 JSON wrapper round-trip ---
    def t_wrap():
        sess = SigningKey.generate()
        env = seal_envelope(KIND_CTL, b"\x00", b"t", dev_id, bytes(16), 1,
                            0, MT_CTL_PING, sign_key=sess)
        assert b64_unwrap(b64_wrap(env)) == env
    ok &= _t("base64 JSON wrapper round-trip", t_wrap)

    print("\nSELF-TEST:", "ALL PASS" if ok else "FAILURES")
    return ok


if __name__ == "__main__":
    sys.exit(0 if self_test() else 1)
