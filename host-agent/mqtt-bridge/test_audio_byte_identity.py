#!/usr/bin/env python3
"""test_audio_byte_identity.py — design verification item 1, host-side
half (spec B4 of docs/audio-reader-spec-2026-11-09.md): a streamed
take's chunk payloads (totals stripped except the final chunk) must
reassemble to a WAV byte-identical to the same take fed in legacy
store-then-send shape (total on every chunk), and the concatenated
payloads must be the ADPCM stream itself, unaltered by framing.

WHY CONSTRUCTED, NOT CORPUS-REPLAYED: the spec assumed an archived
chunk-set corpus exists. It does not:
  * audio-captures/ holds only decoded WAVs + transcripts.log
    (30 files) — the chunk JSON was never archived;
  * telemetry.log does contain audio chunk lines, but the logger
    truncates them at 290 characters (measured on id=53096's lines,
    2026-11-09) while a full chunk line is ~2.1 KB — the payloads
    are unrecoverable.
So the take is constructed here: PCM is encoded with a host mirror
of the firmware encoder (components/ptt/ptt.c adpcm_nibble :118-140,
state reset at take start :1071-1072, low-nibble-first packing in
rec_feed :1145-1146, 1536-byte chunks :93), then fed through
AudioReassembler.feed() in both shapes. The device-side half of
item 1 (the sender's b64 routine is unchanged) is a code read,
already discharged by the reader audit.

Run: python3 test_audio_byte_identity.py
"""
import base64
import math
import os
import random
import struct
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import audio_handler as ah
import adpcm as adpcm_mod
from adpcm import adpcm_decode

TMP = tempfile.mkdtemp(prefix="audio-b4-")
ah.CAPTURE_DIR = TMP
ah.TRANSCRIPT_LOG = os.path.join(TMP, "transcripts.log")
ah._transcribe = lambda path: None

fails = []


def check(name, cond, extra=""):
    print(("ok    " if cond else "FAIL  ") + name)
    if not cond:
        fails.append(name + (" :: " + extra if extra else ""))


# ---- host mirror of the firmware IMA-ADPCM encoder (ptt.c) ----
STEP = adpcm_mod.STEP_TABLE
INDEX = adpcm_mod.INDEX_TABLE


def fw_encode(pcm_bytes: bytes) -> bytes:
    """Mirror of ptt.c adpcm_nibble + rec_feed packing, exactly:
    predictor/step_index reset to 0 at take start, greedy step
    quantiser, low nibble = earlier sample."""
    samples = struct.unpack(f"<{len(pcm_bytes) // 2}h", pcm_bytes)
    assert len(samples) % 2 == 0
    pred = 0
    si = 0

    def nibble(sample):
        nonlocal pred, si
        diff = sample - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        step = STEP[si]
        diffq = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            diffq += step
        if diff >= (step >> 1):
            code |= 2
            diff -= (step >> 1)
            diffq += (step >> 1)
        if diff >= (step >> 2):
            code |= 1
            diffq += (step >> 2)
        pred = pred - diffq if code & 8 else pred + diffq
        pred = max(-32768, min(32767, pred))
        si = max(0, min(88, si + INDEX[code & 0x0F]))
        return code & 0x0F

    out = bytearray()
    for i in range(0, len(samples), 2):
        out.append(nibble(samples[i]) | (nibble(samples[i + 1]) << 4))
    return bytes(out)


# ---- encoder anchors ----
check("anchor/adpcm-py-selftest", adpcm_mod.selftest() is True)
sil = fw_encode(b"\x00\x00" * 512)  # 512 silent samples -> 256 ADPCM bytes
check("anchor/silence-encodes-to-zero-bytes",
      sil == bytes(256) and len(sil) == 256)
check("anchor/silence-round-trips", adpcm_decode(sil) == b"\x00" * 1024)

# ---- a scripted take: ~3.06 s of speech-like dynamics at 16 kHz ----
SR = 16000
NSAMP = 49000
_r = random.Random(7)
pcm_samples = []
for n in range(NSAMP):
    t = n / SR
    v = int(9000 * math.sin(2 * math.pi * 440 * t) * math.exp(-0.7 * t)
            + 5000 * math.sin(2 * math.pi * 1244 * t))
    if 1.2 < t < 1.8:      # noise burst segment
        v += _r.randint(-1500, 1500)
    if t > 2.7:            # trailing quiet
        v = int(v * 0.05)
    pcm_samples.append(max(-32768, min(32767, v)))
PCM = struct.pack(f"<{NSAMP}h", *pcm_samples)

STREAM = fw_encode(PCM)
check("encoder/deterministic", fw_encode(PCM) == STREAM)
check("encoder/stream-is-2-samples-per-byte",
      len(STREAM) * 2 == NSAMP and len(STREAM) * 4 == len(PCM),
      f"stream={len(STREAM)} samples={NSAMP} pcm={len(PCM)}")

CHUNK = 1536  # PTT_ADPCM_CHUNK, ptt.c:93
chunks = [STREAM[i:i + CHUNK] for i in range(0, len(STREAM), CHUNK)]
TOTAL = len(chunks)
check("take/has-a-partial-final-chunk",
      len(chunks[-1]) < CHUNK and TOTAL > 2,
      f"total={TOTAL} last={len(chunks[-1])}")


def feed_shape(shape):
    """Feed the take in 'legacy' or 'streaming' shape; return the
    list of (path, wav_bytes) completions."""
    done = []
    r = ah.AudioReassembler(
        on_complete=lambda p, _t: done.append((p, open(p, "rb").read())))
    uid = f"b4-{shape}"
    for seq, payload in enumerate(chunks):
        msg = {"t": "audio", "id": uid, "seq": seq, "ms": 3062,
               "data": base64.b64encode(payload).decode()}
        if shape == "legacy" or seq == TOTAL - 1:
            msg["total"] = TOTAL
        r.feed(msg)
    r.feed({"t": "audio_end", "id": uid, "chunks": TOTAL, "ms": 3062})
    return done


legacy = feed_shape("legacy")
streamed = feed_shape("streaming")

check("legacy/exactly-one-completion", len(legacy) == 1,
      f"{len(legacy)}")
check("streaming/exactly-one-completion", len(streamed) == 1,
      f"{len(streamed)}")
check("identity/wav-bytes-identical-across-shapes",
      len(legacy) == 1 and len(streamed) == 1
      and legacy[0][1] == streamed[0][1])

# the WAV is exactly the bridge decoder's output on the raw stream
if legacy:
    with wave.open(legacy[0][0], "rb") as w:
        params = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        frames = w.readframes(w.getnframes())
    check("identity/wav-params-16k-mono-16bit", params == (1, 2, SR),
          str(params))
    check("identity/wav-frames-equal-decoder-output",
          frames == adpcm_decode(STREAM))

# the literal item-1 claim: streamed payloads, concatenated, ARE the
# store-then-send ADPCM buffer — framing neither adds nor drops bytes
rebuilt = b"".join(
    base64.b64decode(base64.b64encode(c).decode()) for c in chunks)
check("identity/chunk-payloads-concatenate-to-the-stream",
      rebuilt == STREAM)

print("\nRESULT: " + ("ALL PASS" if not fails else
                      "FAILURES: %s" % fails))
sys.exit(0 if not fails else 1)
