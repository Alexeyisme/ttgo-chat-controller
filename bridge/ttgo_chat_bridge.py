#!/usr/bin/env python3
"""
ttgo_chat_bridge.py — Hermes Chat Controller Bridge
RPi4 side. Connects TTGO T-Display (USB serial) to Hermes API server.

Full voice loop:
  BTN1 (LEFT)  → new_chat  → Hermes API (fresh session) → TTS → audio out
  BTN2 (RIGHT) → ptt_start → mic capture
                 ptt_stop  → Groq Whisper STT → Hermes API → TTS → audio out

Protocol (115200 baud, newline-delimited JSON):
  TTGO→Pi:  device_ready | new_chat | ptt_start | ptt_stop
  Pi→TTGO:  chat_started | chat_stats | ack | error
"""

import argparse
import asyncio
import json
import logging
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Optional

# Make hermes-agent importable when running this bridge standalone.
# Layout assumption (default): this repo sits next to or inside a Hermes checkout.
#   - HERMES_AGENT_ROOT env var (absolute path to the hermes-agent repo) — preferred
#   - Otherwise fall back to parents[3], which matches the layout where ttgo-chat
#     lives as a subdirectory of hermes-agent (…/hermes-agent/ttgo-chat/bridge/).
_env_root = os.getenv("HERMES_AGENT_ROOT")
if _env_root:
    HERMES_REPO_ROOT = Path(_env_root).expanduser().resolve()
else:
    HERMES_REPO_ROOT = Path(__file__).resolve().parents[2]  # ../../ from bridge/
for _p in (HERMES_REPO_ROOT, HERMES_REPO_ROOT.parent):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import requests
import serial

# ── Config ────────────────────────────────────────────────────────────────────
SERIAL_PORT   = os.getenv("TTGO_CHAT_PORT",  "/dev/ttyACM0")
SERIAL_BAUD   = int(os.getenv("TTGO_CHAT_BAUD", "115200"))

API_BASE      = os.getenv("HERMES_API_BASE",  "http://localhost:8642")
API_KEY       = os.getenv("HERMES_API_KEY",   "")
API_MODEL     = os.getenv("HERMES_API_MODEL", "hermes-agent")
CTX_WINDOW    = int(os.getenv("CTX_WINDOW_TOKENS", "200000"))

GROQ_API_KEY  = os.getenv("GROQ_API_KEY", "")

# Audio hardware
MIC_DEVICE    = os.getenv("TTGO_MIC_DEVICE", "default")          # ALSA capture device (e.g. hw:1,0)
BT_DEVICE     = os.getenv("TTGO_BT_DEVICE",  "default")          # ALSA playback device — use bluealsa:DEV=XX:XX:XX:XX:XX:XX,PROFILE=a2dp for BT
RECORD_RATE   = 16000

# TTS
TTS_VOICE     = os.getenv("TTGO_TTS_VOICE", "en-US-AvaMultilingualNeural")

log = logging.getLogger("ttgo-chat")

# ── Chat state ────────────────────────────────────────────────────────────────
class ChatState:
    def __init__(self):
        self.session_id: Optional[str] = None
        self.messages:   int = 0
        self.tokens:     int = 0
        self._lock = threading.Lock()

    def new_session(self) -> str:
        with self._lock:
            self.session_id = str(uuid.uuid4())
            self.messages = 0
            self.tokens   = 0
        return self.session_id

    def update(self, messages: int, tokens: int):
        with self._lock:
            self.messages = messages
            self.tokens   = tokens

    def increment_messages(self):
        with self._lock:
            self.messages += 1

    def context_pct(self) -> int:
        with self._lock:
            if CTX_WINDOW <= 0:
                return 0
            return min(100, int(self.tokens * 100 / CTX_WINDOW))

    @property
    def sid(self) -> Optional[str]:
        with self._lock:
            return self.session_id

state = ChatState()

# Prevent duplicate chat starts and overlapping greeting playback.
chat_start_lock = threading.Lock()
chat_starting = False
chat_block_until = 0.0


def chat_start_allowed() -> bool:
    global chat_block_until
    return time.time() >= chat_block_until


def mark_chat_block(seconds: float = 3.0):
    global chat_block_until
    chat_block_until = time.time() + seconds


# ── Serial transport ──────────────────────────────────────────────────────────
class SerialTransport:
    def __init__(self, port: str, baud: int):
        self._ser: Optional[serial.Serial] = None
        self.port = port
        self.baud = baud
        self._lock = threading.Lock()

    def connect(self):
        self._ser = serial.Serial(self.port, self.baud, timeout=0.1)
        log.info("Serial connected: %s @ %d", self.port, self.baud)

    def send(self, obj: dict):
        line = json.dumps(obj) + "\n"
        with self._lock:
            try:
                self._ser.write(line.encode())
                self._ser.flush()
                log.debug("→ %s", line.strip())
            except Exception as e:
                log.error("Serial send error: %s", e)

    def readline(self) -> Optional[str]:
        try:
            raw = self._ser.readline()
            if raw:
                return raw.decode(errors="replace").strip()
        except Exception:
            pass
        return None

    def close(self):
        if self._ser and self._ser.is_open:
            self._ser.close()

# ── TTS → BT speaker ─────────────────────────────────────────────────────────
def speak(text: str):
    """Convert text to speech and play on the configured audio output (ALSA/bluealsa)."""
    if not text.strip():
        return
    # Truncate very long responses for speech
    words = text.split()
    if len(words) > 80:
        text = " ".join(words[:80]) + "…"

    log.info("TTS: %d chars → %s", len(text), BT_DEVICE)
    try:
        import edge_tts
        fd, mp3_path = tempfile.mkstemp(suffix=".mp3")
        os.close(fd)

        async def _synthesize():
            comm = edge_tts.Communicate(text, TTS_VOICE)
            await comm.save(mp3_path)

        asyncio.run(_synthesize())

        # Play via ffplay → bluealsa
        # ffplay → pipe pcm → aplay (bluealsa can't play mp3 directly)
        fd2, wav_path = tempfile.mkstemp(suffix=".wav")
        os.close(fd2)

        # Convert mp3 → wav
        subprocess.run(
            ["ffmpeg", "-y", "-i", mp3_path, "-ar", "44100", "-ac", "2", wav_path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True
        )
        os.unlink(mp3_path)

        # Play wav on BT speaker
        subprocess.run(
            ["aplay", "-D", BT_DEVICE, wav_path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        os.unlink(wav_path)
        log.info("TTS playback done")

    except Exception as e:
        log.error("TTS error: %s", e)

# ── PTT recorder ─────────────────────────────────────────────────────────────
class PttRecorder:
    def __init__(self):
        self._recorder = None
        self._lock = threading.Lock()

    def _ensure_recorder(self):
        if self._recorder is not None:
            return self._recorder
        from tools.voice_mode import create_audio_recorder
        self._recorder = create_audio_recorder()
        return self._recorder

    def start(self):
        with self._lock:
            recorder = self._ensure_recorder()
            recorder.start()
            log.info("Voice-mode recorder started (exact Hermes CLI path)")

    def stop(self) -> Optional[str]:
        with self._lock:
            if self._recorder is None:
                return None
            wav_path = self._recorder.stop()
            log.info("Voice-mode recorder stopped: %s", wav_path)
            return wav_path

recorder = PttRecorder()

# ── STT via Hermes voice config ───────────────────────────────────────────────
def transcribe(wav_path: str) -> Optional[str]:
    try:
        from tools.transcription_tools import _transcribe_groq, DEFAULT_GROQ_STT_MODEL
        result = _transcribe_groq(wav_path, DEFAULT_GROQ_STT_MODEL)
        if not result.get("success"):
            log.warning("STT failed: %s", result.get("error", "unknown error"))
            return None
        text = (result.get("transcript") or "").strip()
        if not text:
            log.warning("STT returned empty transcript")
            return None
        log.info("STT (Groq): %r", text)
        return text
    except Exception as e:
        log.error("STT error: %s", e)
        return None

# ── Hermes API ────────────────────────────────────────────────────────────────
def hermes_chat(text: str, session_id: str) -> dict:
    """Send a message; return dict with ok, content, session_id, prompt_tokens."""
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {API_KEY}",
        "X-Hermes-Session-Id": session_id,
    }
    payload = {
        "model": API_MODEL,
        "messages": [{"role": "user", "content": text}],
        "stream": False,
    }
    try:
        r = requests.post(
            f"{API_BASE}/v1/chat/completions",
            headers=headers, json=payload, timeout=120,
        )
        r.raise_for_status()
        data = r.json()
        usage = data.get("usage", {})
        return {
            "ok":           True,
            "content":      data["choices"][0]["message"]["content"],
            "session_id":   r.headers.get("X-Hermes-Session-Id", session_id),
            "prompt_tokens": usage.get("prompt_tokens", 0),
            "total_tokens":  usage.get("total_tokens",  0),
        }
    except Exception as e:
        log.error("Hermes API error: %s", e)
        return {"ok": False, "error": str(e)}

# ── Handlers ──────────────────────────────────────────────────────────────────
def handle_new_chat(transport: SerialTransport):
    global chat_starting
    with chat_start_lock:
        if chat_starting or not chat_start_allowed():
            log.info("Ignoring duplicate new_chat while starting/blocking")
            transport.send({"type": "ack", "text": "Starting..."})
            return
        chat_starting = True

    try:
        sid = state.new_session()
        log.info("New chat — session=%s", sid)

        # Confirm to display immediately (starts the animation on TTGO)
        transport.send({"type": "chat_started", "session_id": sid})
        transport.send({"type": "ack", "text": "Connecting..."})

        result = hermes_chat(
            "Hello! I just started a new chat from my hardware chat controller. "
            "Please greet me very briefly (one sentence).",
            sid,
        )

        if result["ok"]:
            with state._lock:
                state.session_id = result["session_id"]
                state.messages   = 1
                state.tokens     = result["prompt_tokens"]

            transport.send({
                "type":        "chat_stats",
                "messages":    state.messages,
                "tokens":      state.tokens,
                "context_pct": state.context_pct(),
            })
            transport.send({"type": "ack", "text": "Ready!"})
            log.info("Session ready — %d prompt tokens", state.tokens)
            mark_chat_block(4.0)

            # Speak the greeting
            threading.Thread(
                target=speak, args=(result["content"],), daemon=True
            ).start()
        else:
            transport.send({"type": "error", "text": "API error"})
    finally:
        with chat_start_lock:
            chat_starting = False


def handle_ptt_stop(transport: SerialTransport):
    wav_path = recorder.stop()
    if not wav_path:
        transport.send({"type": "ack", "text": "No audio"})
        return

    transport.send({"type": "ack", "text": "Transcribing..."})

    text = transcribe(wav_path)
    try:
        os.unlink(wav_path)
    except Exception:
        pass

    if not text:
        transport.send({"type": "ack", "text": "Didn't catch that"})
        log.warning("No transcript; check mic capture and STT provider")
        speak("Sorry, I didn't catch that.")
        return

    sid = state.sid
    if not sid:
        transport.send({"type": "ack", "text": "No session — press LEFT"})
        return

    transport.send({"type": "ack", "text": "Thinking..."})
    log.info("Sending to Hermes: %r", text[:60])

    result = hermes_chat(text, sid)

    if result["ok"]:
        with state._lock:
            state.session_id = result["session_id"]
            state.messages  += 1
            state.tokens     = result["prompt_tokens"]

        transport.send({
            "type":        "chat_stats",
            "messages":    state.messages,
            "tokens":      state.tokens,
            "context_pct": state.context_pct(),
        })
        transport.send({"type": "ack", "text": "Done"})

        # Speak response
        threading.Thread(
            target=speak, args=(result["content"],), daemon=True
        ).start()
    else:
        transport.send({"type": "ack", "text": "API error"})
        speak("Sorry, there was an error talking to Hermes.")

# ── Main loop ─────────────────────────────────────────────────────────────────
def run(transport: SerialTransport):
    log.info("Bridge running. Serial: %s @ %d", SERIAL_PORT, SERIAL_BAUD)
    log.info("Hermes API: %s  model: %s", API_BASE, API_MODEL)
    log.info("Mic: %s  BT speaker: %s", MIC_DEVICE, BT_DEVICE)

    while True:
        line = transport.readline()
        if not line:
            time.sleep(0.01)
            continue

        log.debug("← %s", line)

        # Skip ESP32 boot ROM lines
        if not line.startswith("{"):
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        event = msg.get("event")
        if not event:
            continue

        if event == "device_ready":
            log.info("TTGO ready")
            transport.send({"type": "ack", "text": "Bridge OK"})

        elif event == "new_chat":
            threading.Thread(
                target=handle_new_chat, args=(transport,), daemon=True
            ).start()

        elif event == "ptt_start":
            log.info("PTT start — recording")
            recorder.start()

        elif event == "ptt_stop":
            log.info("PTT stop — processing")
            threading.Thread(
                target=handle_ptt_stop, args=(transport,), daemon=True
            ).start()

        else:
            log.debug("Unknown event: %s", event)

# ── Entry ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="TTGO Chat Controller Bridge")
    parser.add_argument("--port",  default=SERIAL_PORT)
    parser.add_argument("--baud",  type=int, default=SERIAL_BAUD)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    transport = SerialTransport(args.port, args.baud)
    transport.connect()

    def _shutdown(sig, frame):
        log.info("Shutting down")
        transport.close()
        sys.exit(0)

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT,  _shutdown)

    try:
        run(transport)
    finally:
        transport.close()

if __name__ == "__main__":
    main()
