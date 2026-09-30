#!/usr/bin/env python3
"""Watcher PTT audio handler.

Reassembles base64 IMA-ADPCM chunks from the Watcher, decodes to
16kHz 16-bit mono WAV, saves to audio-captures/, and transcribes with
Whisper when available.

Firmware sends (via link -> console -> MQTT):
    {"t":"audio","id":<ms>,"seq":N,"total":M,"ms":<dur>,"data":"<b64>"}
    {"t":"audio_end","id":<ms>,"chunks":M,"ms":<dur>}

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
            total = int(msg.get("total", 1))
            try:
                raw = base64.b64decode(msg.get("data", ""))
            except Exception:
                return None
            slot = self._pending.setdefault(
                uid, {"chunks": {}, "total": total, "ms": msg.get("ms", 0)})
            slot["chunks"][seq] = raw
            slot["total"] = total
            slot["ms"] = msg.get("ms", slot["ms"])
            # opportunistic completion: all chunks arrived before audio_end
            if len(slot["chunks"]) >= total:
                return self._complete(uid)
            return None
        if t == "audio_end":
            uid = str(msg.get("id"))
            if uid in self._pending:
                return self._complete(uid)
            return None
        return None

    def _complete(self, uid: str) -> str | None:
        slot = self._pending.pop(uid, None)
        if not slot:
            return None
        total = slot["total"]
        chunks = slot["chunks"]
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
