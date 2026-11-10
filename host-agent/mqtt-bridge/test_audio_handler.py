#!/usr/bin/env python3
"""test_audio_handler.py — synthetic trace harness for the bridge audio
reassembler (audio_handler.AudioReassembler.feed), spec B1 of
docs/audio-reader-spec-2026-11-09.md.

Drives feed() through scripted chunk sequences — no device, no broker,
no Whisper (transcription is stubbed; WAV output is redirected to a
temp dir). Traces:
  (i)   legacy stream, constant total on every chunk (regression)
  (ii)  streaming stream, total only on the final chunk (0.4.49+)
  (iii) final chunk lost, audio_end adopts its chunk count
  (iv)  audio_end lost, 45 s sweep completes the partial take
  (v)   total: 0 chunk — the R2 trap, fixed by B3 (audio_handler.py
        total<=0 guard): a non-positive total is treated as absent, so
        the take completes once, whole, on audio_end. Pre-B3 this trace
        completed the take on chunk 0 and completed the remainder as a
        second fragment (and the fragments shared one filename, so the
        second overwrote the first — verification report D-V1).
  (vi)  duplicate chunks are idempotent
  (vii) mid-stream hole -> missing-seq list in the completion log
  (viii) two interleaved takes stay separate
  (ix)  out-of-order delivery, incl. the total-carrying chunk early

Pass bar (spec B1): exactly one completion per take in every trace
except the documented (v); WAV bytes identical across (i)/(ii)/(vi)/(ix)
for the same payload set.

Run: python3 test_audio_handler.py
"""
import base64
import contextlib
import io
import os
import random
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import audio_handler as ah
from adpcm import adpcm_decode

# ---- isolate side effects: temp capture dir, no Whisper ----
TMP = tempfile.mkdtemp(prefix="audio-harness-")
ah.CAPTURE_DIR = TMP
ah.TRANSCRIPT_LOG = os.path.join(TMP, "transcripts.log")
ah._transcribe = lambda path: None

fails = []
xfails = []


def check(name, cond, extra=""):
    print(("ok    " if cond else "FAIL  ") + name)
    if not cond:
        fails.append(name + (" :: " + extra if extra else ""))


def xfail(name, cond, evidence=""):
    # Desired behaviour that is KNOWN to fail until a specced fix lands.
    # Passing here would mean the fix arrived — report that too.
    if cond:
        print("XPASS " + name + "  <-- fix appears to have landed; "
              "promote to check()")
    else:
        print("xfail " + name + (" :: " + evidence if evidence else ""))
        xfails.append(name)


# ---- deterministic payload set, shared by traces i/ii/vi/ix ----
_rng = random.Random(20261109)
SIZES = [1536, 1536, 1536, 1536, 700]
PAYLOADS = [_rng.randbytes(n) for n in SIZES]
N = len(PAYLOADS)
MS = 5000


def expected_wav(payloads):
    """WAV bytes the handler must produce for these chunk payloads."""
    buf = os.path.join(TMP, "expected.wav")
    ah._write_wav(buf, adpcm_decode(b"".join(payloads)))
    with open(buf, "rb") as f:
        return f.read()


FULL_WAV = expected_wav(PAYLOADS)


class Take:
    """One reassembler + a completion recorder for a trace."""

    def __init__(self):
        self.done = []  # (wav_path, wav_bytes) per completion
        self.r = ah.AudioReassembler(
            on_complete=lambda p, _t: self.done.append(
                (p, open(p, "rb").read())))

    def chunk(self, uid, seq, total="absent", payloads=PAYLOADS):
        msg = {"t": "audio", "id": uid, "seq": seq, "ms": MS,
               "data": base64.b64encode(payloads[seq]).decode()}
        if total != "absent":
            msg["total"] = total
        return self.r.feed(msg)

    def end(self, uid, chunks=N):
        return self.r.feed({"t": "audio_end", "id": uid,
                            "chunks": chunks, "ms": MS})


def silent(fn, *a, **kw):
    """Run fn with stdout captured; return (result, captured_text)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        res = fn(*a, **kw)
    return res, out.getvalue()


# (i) legacy: constant total on every chunk -----------------------------
t = Take()
rets = [t.chunk("i-legacy", s, total=N) for s in range(N)]
check("i/no-early-completion", all(r is None for r in rets[:-1]),
      str(rets))
check("i/completes-on-final-chunk", rets[-1] is not None)
r, _ = silent(t.end, "i-legacy")
check("i/audio_end-after-complete-is-noop", r is None and len(t.done) == 1)
check("i/wav-matches-full-decode",
      len(t.done) == 1 and t.done[0][1] == FULL_WAV)

# (ii) streaming: total only on the final chunk -------------------------
t = Take()
rets = [t.chunk("ii-stream", s) for s in range(N - 1)]
check("ii/no-completion-without-total",
      all(r is None for r in rets) and not t.done)
r = t.chunk("ii-stream", N - 1, total=N)
check("ii/completes-on-final-chunk", r is not None and len(t.done) == 1)
r, _ = silent(t.end, "ii-stream")
check("ii/audio_end-after-complete-is-noop", r is None and len(t.done) == 1)
check("ii/wav-identical-to-legacy",
      len(t.done) == 1 and t.done[0][1] == FULL_WAV)

# (iii) final chunk lost: audio_end adopts its count --------------------
t = Take()
for s in range(N - 1):
    t.chunk("iii-finlost", s)
r, out = silent(t.end, "iii-finlost", chunks=N)
check("iii/completes-on-audio_end", r is not None and len(t.done) == 1)
check("iii/missing-seq-logged", "missing chunks [4]" in out, out.strip())
check("iii/wav-is-partial-decode",
      len(t.done) == 1 and t.done[0][1] == expected_wav(PAYLOADS[:4]))

# (iv) audio_end lost: sweep completes, denominator = arrived -----------
t = Take()
for s in range(N - 1):
    t.chunk("iv-endlost", s)
paths, out = silent(t.r.sweep, 0.0)
check("iv/sweep-completes-once", len(paths) == 1 and len(t.done) == 1)
check("iv/denominator-is-arrived-count", "4/4 chunks" in out,
      out.strip())
check("iv/no-pending-left", not t.r._pending)

# (v) total: 0 — R2 trap, fixed by B3 (total<=0 treated as absent) -----
t = Take()
r0 = t.chunk("v-zero", 0, total=0)
check("v/chunk0-with-total0-does-not-complete",
      r0 is None and not t.done,
      "total=0 must be treated as absent post-B3")
for s in range(1, N):
    t.chunk("v-zero", s)
check("v/no-completion-before-audio_end",
      not t.done, f"completions={len(t.done)}")
r1, out = silent(t.end, "v-zero", chunks=N)
check("v/exactly-one-completion",
      r1 is not None and len(t.done) == 1,
      f"completions={len(t.done)}")
check("v/single-full-wav",
      len(t.done) == 1 and t.done[0][1] == FULL_WAV)

# (vi) duplicates are idempotent ----------------------------------------
# (vi-a) duplicates while the slot is live: no effect.
t = Take()
for s in range(N - 1):
    t.chunk("vi-dup", s)
    t.chunk("vi-dup", s)  # exact re-delivery
t.chunk("vi-dup", N - 1, total=N)
r, _ = silent(t.end, "vi-dup")
check("vi/exactly-one-completion", len(t.done) == 1,
      f"completions={len(t.done)}")
check("vi/wav-identical-to-legacy",
      len(t.done) == 1 and t.done[0][1] == FULL_WAV)
# (vi-b) a duplicate of the FINAL chunk arriving after completion,
# followed by the take's audio_end (at-least-once redelivery shape):
# the popped id is unknown to the handler, so the stray chunk opens a
# fresh slot and audio_end completes it as a 1-chunk fragment. No
# current configuration double-feeds a take (audit (a) trace note 6),
# so this is a latent trap of the R2 class — documented, not fixed.
t = Take()
for s in range(N - 1):
    t.chunk("vib-redeliver", s)
t.chunk("vib-redeliver", N - 1, total=N)   # completes the take
t.chunk("vib-redeliver", N - 1, total=N)   # stray redelivery
r, out = silent(t.end, "vib-redeliver", chunks=N)
check("vib/observed-stray-fragment-completes",
      len(t.done) == 2 and "missing chunks [0, 1, 2, 3]" in out,
      f"completions={len(t.done)} out={out.strip()}")
xfail("vib/desired-exactly-one-completion", len(t.done) == 1,
      "observed a 1-chunk fragment after final-chunk redelivery; "
      "no fix specced (would need a completed-id tombstone)")

# (vii) mid-stream hole --------------------------------------------------
t = Take()
for s in (0, 1, 3):
    t.chunk("vii-hole", s)
r = t.chunk("vii-hole", 4, total=N)
check("vii/no-completion-with-hole", r is None and not t.done)
r, out = silent(t.end, "vii-hole", chunks=N)
check("vii/completes-on-audio_end", r is not None and len(t.done) == 1)
check("vii/missing-seq-logged", "missing chunks [2]" in out, out.strip())
check("vii/wav-is-present-chunks-decode",
      len(t.done) == 1 and t.done[0][1] ==
      expected_wav([PAYLOADS[i] for i in (0, 1, 3, 4)]))

# (viii) interleaved takes (distinct payloads, so cross-talk shows) -----
_rng_b = random.Random(777)
PAYLOADS_B = [_rng_b.randbytes(n) for n in SIZES]
FULL_WAV_B = expected_wav(PAYLOADS_B)
t = Take()
for s in range(N):
    t.chunk("viii-A", s, total=N if s == N - 1 else "absent")
    t.chunk("viii-B", s, total=N if s == N - 1 else "absent",
            payloads=PAYLOADS_B)
r, _ = silent(t.end, "viii-A")
r2, _ = silent(t.end, "viii-B")
check("viii/both-complete-exactly-once", len(t.done) == 2,
      f"completions={len(t.done)}")
check("viii/no-cross-talk",
      len(t.done) == 2 and
      sorted(b for _p, b in t.done) == sorted([FULL_WAV, FULL_WAV_B]))

# (ix) out-of-order, total-carrying chunk arrives early ------------------
t = Take()
order = [2, 0, 4, 1, 3]
rets = []
for s in order:
    rets.append(t.chunk("ix-ooo", s, total=N if s == 4 else "absent"))
check("ix/no-completion-before-last-needed",
      all(r is None for r in rets[:-1]) and len(t.done) == (0 if rets[-1]
      is None else 1), str([bool(r) for r in rets]))
check("ix/completes-when-set-is-full",
      rets[-1] is not None and len(t.done) == 1)
check("ix/wav-identical-to-legacy",
      len(t.done) == 1 and t.done[0][1] == FULL_WAV)

print("\nRESULT: " + ("ALL PASS" if not fails else
                      "FAILURES: %s" % fails) +
      (f"  (+{len(xfails)} expected-fail, documented traps: {xfails})"
       if xfails else ""))
sys.exit(0 if not fails else 1)
