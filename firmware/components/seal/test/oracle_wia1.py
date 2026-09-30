#!/usr/bin/env python3
"""WIA1 golden-vector oracle (PyNaCl).

Generates test/vectors.h: every envelope kind built independently with
PyNaCl, cross-checked against the C implementation in wia1.c (which the
host test asserts byte-for-byte).

Deterministic RNG: replicates wia1.c's host-test RNG exactly —
splitmix64, 32-bit words consumed little-endian (see wia1_random_bytes).
The C test reseeds before each phase with the RNG_S* constants below,
so both sides draw identical bytes.

All keys are deterministic throwaways derived from the RNG_S* streams:
regenerating the oracle always emits the same vectors (stable golden
test data), but no key here is real or provisioned anywhere.
"""
import os
import sys

from nacl.signing import SigningKey
from nacl.public import PrivateKey
from nacl.bindings import (
    crypto_scalarmult,
    crypto_aead_xchacha20poly1305_ietf_encrypt,
)
from nacl.hash import blake2b
from nacl.encoding import RawEncoder as Raw


def H(data, digest_size, key=b""):
    """Raw (not hex) BLAKE2b."""
    return blake2b(data, digest_size=digest_size, key=key, encoder=Raw)

HERE = os.path.dirname(os.path.abspath(__file__))

# RNG seeds shared with test_wia1.c (must match wia1_test_rng_seed calls)
RNG_S0_IDENT = 0x123456789ABCDEF0  # identity mint (8 words -> seed32)
RNG_S1_ARM = 0x0FEDCBA987654321    # arm tap (4 words nonce16 + 8 words x_sk32)
RNG_S2_ENC = 0x7777777777777777    # TEL ENC nonce24 (6 words)
RNG_S3_ROOTS = 0x3C3C3C3C3C3C3C3C  # roots (primary 32 B seed + backup 32 B seed)
RNG_S4_AGENT = 0x5A5A5A5A5A5A5A5A  # agent keys (32 B ed seed + 32 B x seed)

M64 = (1 << 64) - 1


def splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & M64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return state, (z ^ (z >> 31)) & 0xFFFFFFFF


def rand_bytes(state, n):
    """Replicates wia1_random_bytes: 32-bit words, little-endian fill."""
    out = b""
    while len(out) < n:
        state, w = splitmix64(state)
        out += w.to_bytes(4, "little")
    return state, out[:n]


def u16le(v):
    return v.to_bytes(2, "little")


def u32le(v):
    return v.to_bytes(4, "little")


def u64le(v):
    return v.to_bytes(8, "little")


def header(kind, flags, dev_id, session_id, ctr, ts, msg_type):
    h = b"WIA1"
    h += bytes([kind, flags])
    h += dev_id  # 16
    h += session_id  # 16
    h += u64le(ctr) + u64le(ts) + u16le(msg_type) + b"\x00" * 8
    assert len(h) == 64
    return h


def c_array(name, data, per_line=12):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    # ---- device identity (minted from RNG_S0, like wia1_init) ----
    st, id_seed = rand_bytes(RNG_S0_IDENT, 32)
    id_sk = SigningKey(id_seed)
    id_pk = bytes(id_sk.verify_key)
    fp8 = H(id_pk, 8)
    dev_id = b"wia-" + fp8[:6].hex().encode("ascii")
    assert len(dev_id) == 16
    fp16 = fp8.hex()

    # ---- arm challenge (minted from RNG_S1, like wia1_arm_tap) ----
    st, nonce = rand_bytes(RNG_S1_ARM, 16)
    st, x_sk_raw = rand_bytes(st, 32)
    x_sk = PrivateKey(x_sk_raw)
    x_pk = bytes(x_sk.public_key)
    chal_body = b"\x01\x01" + u16le(0x07) + nonce + x_pk
    assert len(chal_body) == 52
    chal_hdr = header(4, 0, dev_id, b"\x00" * 16, 0, 0, 0)
    chal_sig = id_sk.sign(chal_hdr + chal_body).signature
    chal_env = chal_hdr + chal_body + chal_sig
    assert len(chal_env) == 180
    shortcode = H(b"\x01" + nonce + x_pk, 16)[:3].hex()

    # ---- roots (deterministic throwaways) ----
    st, root_seed = rand_bytes(RNG_S3_ROOTS, 32)
    root_sk = SigningKey(root_seed)
    root_pk = bytes(root_sk.verify_key)
    st, backup_seed = rand_bytes(st, 32)
    backup_sk = SigningKey(backup_seed)
    backup_pk = bytes(backup_sk.verify_key)
    # monocypher secret form = seed(32) || pub(32)
    root_sk64 = bytes(root_sk) + root_pk
    backup_sk64 = bytes(backup_sk) + backup_pk

    # ---- agent session keys (deterministic throwaways) ----
    st, agent_ed_seed = rand_bytes(RNG_S4_AGENT, 32)
    agent_ed = SigningKey(agent_ed_seed)
    agent_ed_pk = bytes(agent_ed.verify_key)
    agent_ed_sk64 = bytes(agent_ed) + agent_ed_pk
    st, agent_x_seed = rand_bytes(st, 32)
    agent_x = PrivateKey(agent_x_seed)
    agent_x_pk = bytes(agent_x.public_key)

    # ---- kind-3 cert (mouse scope) ----
    scope = 0x01
    ttl = 3600
    cert_body = (
        b"\x01" + u16le(scope) + u32le(ttl) + dev_id + nonce + agent_ed_pk + agent_x_pk + x_pk
    )
    assert len(cert_body) == 135
    session_id = H(cert_body, 16)
    cert_hdr = header(3, 0, dev_id, b"\x00" * 16, 0, 0, 0)
    cert_sig = root_sk.sign(cert_hdr + cert_body).signature
    cert_env = cert_hdr + cert_body + cert_sig
    assert len(cert_env) == 263

    # ---- §3 key schedule (device side: X25519(x_sk, agent_x_pk)) ----
    ss = crypto_scalarmult(x_sk_raw, agent_x_pk)

    def kdf(label):
        return H(b"wia1:" + label.encode() + b":" + session_id, 32, ss)

    k_tel_mac = kdf("tel-mac")
    k_tel_aead = kdf("tel-aead")
    k_ctl_mac = kdf("ctl-mac")
    k_ctl_aead = kdf("ctl-aead")

    # ---- kind-2 CTL: batch move+button+wheel+dwell ----
    ctl_topic = b"wia/" + dev_id + b"/ctl"
    ctl_body = (
        b"\x01" + (10).to_bytes(2, "little", signed=True) + (-20).to_bytes(2, "little", signed=True)
        + b"\x02\x01\x02"
        + b"\x03\x05"
        + b"\x04" + u16le(100)
    )
    assert len(ctl_body) == 13
    ctl_hdr = header(2, 0, dev_id, session_id, 1, 0, 0x0001)
    s = u16le(len(ctl_topic)) + ctl_topic + ctl_hdr + ctl_body
    ctl_sig = agent_ed.sign(s).signature
    ctl_env = ctl_hdr + ctl_body + ctl_sig

    # ---- kind-1 TEL MAC: armed ack ----
    tel_topic = b"wia/" + dev_id + b"/tel"
    ack_body = b"\x01" + u16le(0x0001)  # proto_ver=1, scope_granted=MOUSE
    tel_hdr = header(1, 0, dev_id, session_id, 1, 0, 0x0001)
    s = u16le(len(tel_topic)) + tel_topic + tel_hdr + ack_body
    tag = H(s, 16, k_tel_mac)
    tel_mac_env = tel_hdr + ack_body + tag

    # ---- kind-1 TEL ENC: frame (nonce from RNG_S2) ----
    st, enc_nonce = rand_bytes(RNG_S2_ENC, 24)
    frame_body = bytes((i * 7) & 0xFF for i in range(64))  # fake jpeg bytes
    enc_hdr = header(1, 0x01, dev_id, session_id, 2, 0, 0x0003)
    aad = u16le(len(tel_topic)) + tel_topic + enc_hdr
    ct = crypto_aead_xchacha20poly1305_ietf_encrypt(frame_body, aad, enc_nonce, k_tel_aead)
    tel_enc_env = enc_hdr + enc_nonce + ct

    # ---- standalone KDF check vector (fixed ss + session_id) ----
    kdf_ss = bytes([0x42]) * 32
    kdf_sid = bytes([0x11]) * 16

    def kdf2(label):
        return H(b"wia1:" + label.encode() + b":" + kdf_sid, 32, kdf_ss)

    # ------------------------------------------------------------------
    # RFC 8032 section 7.1 TEST 1 (transcribed from the RFC text; verified
    # here with PyNaCl before being emitted — the C test then checks
    # Monocypher against this independently-confirmed vector).
    # ------------------------------------------------------------------
    rfc_seed = bytes.fromhex(
        "9d61b19deffd5a60ba844af492ec2cc4"
        "4449c5697b326919703bac031cae7f60")
    rfc_pk = bytes.fromhex(
        "d75a980182b10ab7d54bfed3c964073a"
        "0ee172f3daa62325af021a68f707511a")
    rfc_sig = bytes.fromhex(
        "e5564300c360ac729086e2cc806e828a"
        "84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46b"
        "d25bf5f0595bbe24655141438e7a100b")
    rfc_sk = SigningKey(rfc_seed)
    assert bytes(rfc_sk.verify_key) == rfc_pk, "RFC transcription: pk mismatch"
    rfc_sk.verify_key.verify(b"", rfc_sig)  # raises on transcription error
    print("RFC 8032 TEST 1 transcription verified with PyNaCl")

    # ---- emit vectors.h ----
    out = []
    out.append("/* GENERATED by oracle_wia1.py — do not edit.")
    out.append(" * Deterministic throwaway test keys (RNG_S* streams).")
    out.append(" */")
    out.append("#pragma once")
    out.append("#ifndef WIA1_VECTORS_H")
    out.append("#define WIA1_VECTORS_H")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f"#define VEC_RNG_S0_IDENT 0x{RNG_S0_IDENT:016X}ULL")
    out.append(f"#define VEC_RNG_S1_ARM   0x{RNG_S1_ARM:016X}ULL")
    out.append(f"#define VEC_RNG_S2_ENC   0x{RNG_S2_ENC:016X}ULL")
    out.append("")
    out.append(c_array("VEC_ID_SEED", id_seed))
    out.append(c_array("VEC_ID_PK", id_pk))
    out.append(f'static const char VEC_DEV_ID[17] = "{dev_id.decode("ascii")}";')
    out.append(f'static const char VEC_FP16[17] = "{fp16}";')
    out.append(f'static const char VEC_SHORTCODE[7] = "{shortcode}";')
    out.append("")
    out.append(c_array("VEC_ROOT_PRIMARY", root_pk))
    out.append(c_array("VEC_ROOT_BACKUP", backup_pk))
    out.append(c_array("VEC_ROOT_PRIMARY_SK64", root_sk64))
    out.append(c_array("VEC_ROOT_BACKUP_SK64", backup_sk64))
    out.append("")
    out.append(c_array("VEC_AGENT_ED_PK", agent_ed_pk))
    out.append(c_array("VEC_AGENT_ED_SK64", agent_ed_sk64))
    out.append(c_array("VEC_AGENT_X_PK", agent_x_pk))
    out.append("")
    out.append(c_array("VEC_CHAL", chal_env))
    out.append(c_array("VEC_CERT", cert_env))
    out.append(c_array("VEC_SESSION_ID", session_id))
    out.append(c_array("VEC_K_TEL_MAC", k_tel_mac))
    out.append(c_array("VEC_K_TEL_AEAD", k_tel_aead))
    out.append(c_array("VEC_K_CTL_MAC", k_ctl_mac))
    out.append(c_array("VEC_K_CTL_AEAD", k_ctl_aead))
    out.append("")
    out.append(c_array("VEC_CTL", ctl_env))
    out.append(f"#define VEC_CTL_BODY_LEN {len(ctl_body)}")
    out.append("")
    out.append(c_array("VEC_TEL_MAC", tel_mac_env))
    out.append(c_array("VEC_TEL_ENC", tel_enc_env))
    out.append(c_array("VEC_FRAME_BODY", frame_body))
    out.append("")
    out.append(c_array("VEC_KDF_SS", kdf_ss))
    out.append(c_array("VEC_KDF_SID", kdf_sid))
    out.append(c_array("VEC_KDF_TEL_MAC", kdf2("tel-mac")))
    out.append(c_array("VEC_KDF_TEL_AEAD", kdf2("tel-aead")))
    out.append(c_array("VEC_KDF_CTL_MAC", kdf2("ctl-mac")))
    out.append(c_array("VEC_KDF_CTL_AEAD", kdf2("ctl-aead")))
    out.append("")
    out.append("/* seal_roots.h WIA1_HOST_TEST hook definitions (throwaway). */")
    out.append("const uint8_t WIA1_TEST_ROOT_PRIMARY[32] = {")
    out.append("    %s" % ", ".join("0x%02x" % b for b in root_pk))
    out.append("};")
    out.append("const uint8_t WIA1_TEST_ROOT_BACKUP[32] = {")
    out.append("    %s" % ", ".join("0x%02x" % b for b in backup_pk))
    out.append("};")
    out.append("")
    out.append("/* RFC 8032 section 7.1 TEST 1 (Ed25519, empty message).")
    out.append(" * Transcription confirmed by the oracle with PyNaCl before emit. */")
    out.append("static const uint8_t VEC_RFC8032_T1_SEED[32] = {")
    out.append("    %s" % ", ".join("0x%02x" % b for b in rfc_seed))
    out.append("};")
    out.append("static const uint8_t VEC_RFC8032_T1_PK[32] = {")
    out.append("    %s" % ", ".join("0x%02x" % b for b in rfc_pk))
    out.append("};")
    out.append("static const uint8_t VEC_RFC8032_T1_SIG[64] = {")
    out.append("    %s" % ", ".join("0x%02x" % b for b in rfc_sig))
    out.append("};")
    out.append("")
    out.append("#endif /* WIA1_VECTORS_H */")

    path = os.path.join(HERE, "vectors.h")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {path}")
    print(f"dev_id={dev_id.decode()} fp={fp16} shortcode={shortcode}")
    print(f"session_id={session_id.hex()}")


if __name__ == "__main__":
    sys.exit(main())
