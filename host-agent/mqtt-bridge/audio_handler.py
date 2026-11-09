#!/usr/bin/env python3
"""Watcher PTT audio handler.

Reassembles base64 IMA-ADPCM chunks from the Watcher, decodes to
16kHz 16-bit mono WAV, saves to audio-captures/, and transcribes with
Whisper when available.

Firmware sends (via link -> console -> MQTT):
    {"t":"audio","id":<ms>,"seq":N,"total":M,"ms":<dur>,"data":"<b64>"}
    {"t":"audio_end","id":<ms>,"chunks":M,"ms":<dur>}

Streaming firmware (0.4.49+, docs/audio-streaming-design-2026-11-09.md)
OMITS "total" on every chunk except the final one — the count is
unknowable until the take ends. This handler therefore treats total
as optional: a slot's total stays unknown (None) until a chunk
carries one or audio_end supplies the authoritative count, and the
opportunistic completion below only fires on a KNOWN total.
Pre-0.4.49 firmware sends total on every chunk; behavior for it is
unchanged.

These arrive either on the dedicated watcher/audio topic (preferred) or
embedded as raw JSON inside watcher/telemetry events (current console).
feed() accepts both shapes.
"""

import base64
import json
import os
import time
import wave

from adpcm import adpcm_decode

CAPTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "audio-captures")
TRANSCRIPT_LOG = os.path.join(CAPTURE_DIR, "transcripts.log")

SAMPLE_RATE = 16000


def _ensure_dir():
    os.makedirs(CAPTURE_DIR, exist_ok=True)


def _write_wav(path: str, pcm: bytes) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


_whisper_model = None
_whisper_checked = False

# Local faster-whisper model (downloaded to avoid the runtime's httpx/proxy
# issue with huggingface_hub). "tiny" is fast enough for short PTT clips.
_WHISPER_MODEL_DIR = os.path.expanduser(
    "~/.cache/huggingface/hub/models--Systran--faster-whisper-tiny")


def _transcribe(path: str) -> str | None:
    """Transcribe with faster-whisper if available; else None (and log once)."""
    global _whisper_model, _whisper_checked
    if not _whisper_checked:
        _whisper_checked = True
        _ensure_dir()
        try:
            from faster_whisper import WhisperModel
            if os.path.isdir(_WHISPER_MODEL_DIR):
                _whisper_model = WhisperModel(_WHISPER_MODEL_DIR,
                                              device="cpu",
                                              compute_type="int8")
            else:
                # Fall back to Hub download (may fail behind the proxy).
                _whisper_model = WhisperModel("tiny", device="cpu",
                                              compute_type="int8")
        except Exception as e:
            _whisper_model = None
            with open(TRANSCRIPT_LOG, "a") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} "
                        f"[whisper] not available ({e}); "
                        f"WAV saved, transcription needs setup\n")
    if _whisper_model is None:
        return None
    try:
        segments, _info = _whisper_model.transcribe(path, beam_size=5)
        text = " ".join(s.text for s in segments).strip()
        return text if text else "(silence)"
    except Exception as e:
        return f"[transcription error: {e}]"


class AudioReassembler:
    """Collects audio chunks keyed by firmware id; emits WAV on audio_end."""

    def __init__(self, on_complete=None):
        # id -> {"chunks": {seq: bytes}, "total": int, "ms": int}
        self._pending: dict = {}
        self.on_complete = on_complete  # fn(wav_path, transcript|None)

    def feed(self, msg: dict) -> str | None:
        """Feed one decoded device message. Returns the WAV path when a
        complete utterance is assembled, else None."""
        t = msg.get("t")
        if t == "audio":
            uid = str(msg.get("id"))
            seq = int(msg.get("seq", 0))
            raw_total = msg.get("total")
            total = int(raw_total) if raw_total is not None else None
            try:
                raw = base64.b64decode(msg.get("data", ""))
            except Exception:
                return None
            slot = self._pending.setdefault(
                uid, {"chunks": {}, "total": None, "ms": msg.get("ms", 0)})
            slot["chunks"][seq] = raw
            if total is not None:
                slot["total"] = total
            slot["ms"] = msg.get("ms", slot["ms"])
            slot["last_at"] = time.time()
            # Opportunistic completion: all chunks arrived before
            # audio_end. Only on a KNOWN total — streaming chunks
            # carry none until the final one (0.4.49+).
            if slot["total"] is not None and \
                    len(slot["chunks"]) >= slot["total"]:
                return self._complete(uid)
            return None
        if t == "audio_end":
            uid = str(msg.get("id"))
            if uid in self._pending:
                slot = self._pending[uid]
                # audio_end's chunk count is authoritative when no
                # chunk carried a total (streaming firmware whose
                # final chunk was the one that got away).
                if slot["total"] is None and \
                        msg.get("chunks") is not None:
                    slot["total"] = int(msg["chunks"])
                return self._complete(uid)
            return None
        return None

    def sweep(self, max_age_s: float = 45.0) -> list:
        """Complete takes that have gone quiet for max_age_s seconds.

        A take whose audio_end (or some chunks) never arrives would
        otherwise sit in _pending forever and its audio would be lost
        silently — field case 2026-11-07: take 80682 parked incomplete
        while only its loss was visible in the logs. Expiry is based on
        last activity, so a slow-but-alive take (chunks still arriving)
        is never cut off; _complete already decodes partial takes and
        logs the missing chunk list. Returns the WAV paths written."""
        now = time.time()
        stale = [uid for uid, slot in self._pending.items()
                 if now - slot.get("last_at", now) >= max_age_s]
        out = []
        for uid in stale:
            print(f"[audio] id={uid}: no activity for "
                  f"{max_age_s:.0f}s, completing partial take",
                  flush=True)
            wav = self._complete(uid)
            if wav:
                out.append(wav)
        return out

    def _complete(self, uid: str) -> str | None:
        slot = self._pending.pop(uid, None)
        if not slot:
            return None
        chunks = slot["chunks"]
        # Total may still be unknown (sweep completion of a streamed
        # take whose final chunk AND audio_end both went missing):
        # report against what actually arrived rather than inventing
        # a denominator.
        total = slot["total"] if slot["total"] is not None \
            else len(chunks)
        if len(chunks) < total:
            missing = sorted(set(range(total)) - set(chunks))
            print(f"[audio] id={uid}: missing chunks {missing}, "
                  f"decoding partial", flush=True)
        adpcm = b"".join(chunks[i] for i in sorted(chunks))
        pcm = adpcm_decode(adpcm)
        _ensure_dir()
        stamp = time.strftime("%Y%m%d-%H%M%S")
        wav_path = os.path.join(CAPTURE_DIR, f"ptt-{stamp}-{uid}.wav")
        _write_wav(wav_path, pcm)
        dur_s = len(pcm) / 2 / SAMPLE_RATE
        print(f"[audio] saved {wav_path} ({dur_s:.1f}s, "
              f"{len(chunks)}/{total} chunks)", flush=True)

        transcript = _transcribe(wav_path)
        with open(TRANSCRIPT_LOG, "a") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {wav_path} "
                    f"{dur_s:.1f}s : {transcript if transcript else '(no transcription)'}\n")
        if transcript:
            print(f"[audio] transcript: {transcript}", flush=True)
        if self.on_complete:
            self.on_complete(wav_path, transcript)
        return wav_path


# Module-level reassembler for the subscriber to use.
reassembler = AudioReassembler()


def handle_device_message(raw: str) -> str | None:
    """Try to parse a raw device JSON line as audio. Returns WAV path or None."""
    try:
        msg = json.loads(raw)
    except Exception:
        return None
    if not isinstance(msg, dict) or msg.get("t") not in ("audio", "audio_end"):
        return None
    return reassembler.feed(msg)
