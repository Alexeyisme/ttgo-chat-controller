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
# Optional: identify the TTGO by USB serial number so we survive /dev/ttyACM{N}
# renumbering across disconnect/reconnect even without a udev symlink.
SERIAL_USB_ID = os.getenv("TTGO_CHAT_USB_SERIAL", None)

API_BASE      = os.getenv("HERMES_API_BASE",  "http://localhost:8642")
API_KEY       = os.getenv("HERMES_API_KEY",   "")
API_MODEL     = os.getenv("HERMES_API_MODEL", "hermes-agent")
CTX_WINDOW    = int(os.getenv("CTX_WINDOW_TOKENS", "200000"))

GROQ_API_KEY  = os.getenv("GROQ_API_KEY", "")

# Audio hardware — WM8960 Audio HAT is the ONLY supported output/input.
# No Bluetooth. When the WM8960 is present it is used for ALL sound flows.
MIC_DEVICE    = os.getenv("TTGO_MIC_DEVICE", "plughw:wm8960soundcard,0")   # ALSA capture device
AUDIO_DEVICE  = os.getenv("TTGO_AUDIO_DEVICE", "plughw:wm8960soundcard,0") # ALSA playback device (WM8960)
RECORD_RATE   = 16000

# TTS
TTS_VOICE     = os.getenv("TTGO_TTS_VOICE", "en-US-AvaMultilingualNeural")

# System volume — the ONLY volume control. WM8960 'Speaker' mixer (0-127).
# Enforced before EVERY playback so greeting and response are always equally
# loud regardless of what the capture cycle did to the codec. No software gain.
SYSTEM_VOLUME = os.getenv("TTGO_SYSTEM_VOLUME", "120")

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
def find_ttgo_port(preferred: str, usb_serial: Optional[str]) -> Optional[str]:
    """Locate the TTGO serial device. Prefer `preferred` if present; otherwise
    scan pyserial's list_ports for a device whose USB serial number matches
    `usb_serial`. Returns a device path or None."""
    try:
        if preferred and os.path.exists(preferred):
            return preferred
    except Exception:
        pass
    if not usb_serial:
        return None
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            sn = (getattr(p, "serial_number", None) or "").strip()
            if sn and sn == usb_serial:
                return p.device
    except Exception as e:
        log.debug("list_ports error: %s", e)
    return None


class SerialTransport:
    """Serial transport with transparent auto-reconnect.

    - `send()` and `readline()` tolerate the port being disconnected. They
      return silently (send) / return None (readline) while the port is down.
    - A background reconnect thread keeps trying to re-open the port, either
      at the configured path or via USB-serial discovery. Once reconnected,
      callers continue transparently.
    """

    def __init__(self, port: str, baud: int, usb_serial: Optional[str] = None):
        self._ser: Optional[serial.Serial] = None
        self.port = port
        self.baud = baud
        self.usb_serial = usb_serial
        self._lock = threading.Lock()          # protects self._ser for writes
        self._state_lock = threading.Lock()    # protects connected flag
        self._connected = False
        self._stop = threading.Event()
        self._on_reconnect = None  # callback(transport) after a reconnect

    def set_on_reconnect(self, cb):
        self._on_reconnect = cb

    def _open(self, path: str) -> bool:
        try:
            ser = serial.Serial(path, self.baud, timeout=0.1)
        except Exception as e:
            log.debug("open %s failed: %s", path, e)
            return False
        with self._lock:
            self._ser = ser
            self.port = path
        with self._state_lock:
            self._connected = True
        log.info("Serial connected: %s @ %d", path, self.baud)
        return True

    def connect(self, blocking: bool = True):
        """Initial connect. If blocking=True, keep retrying until we succeed
        or stop() is called. Uses port discovery by USB serial as a fallback."""
        backoff = 1.0
        while not self._stop.is_set():
            path = find_ttgo_port(self.port, self.usb_serial)
            if path and self._open(path):
                return
            if not blocking:
                return
            log.warning("TTGO not found (looking for %s or USB serial %s). "
                        "Retrying in %.1fs", self.port, self.usb_serial, backoff)
            if self._stop.wait(backoff):
                return
            backoff = min(backoff * 1.6, 10.0)

    @property
    def connected(self) -> bool:
        with self._state_lock:
            return self._connected and self._ser is not None and self._ser.is_open

    def _mark_disconnected(self, reason: str = ""):
        with self._state_lock:
            was = self._connected
            self._connected = False
        if was:
            log.warning("Serial disconnected (%s); entering reconnect loop", reason or "unknown")
        with self._lock:
            try:
                if self._ser and self._ser.is_open:
                    self._ser.close()
            except Exception:
                pass
            self._ser = None

    def _reconnect_loop(self):
        """Try to reopen the port. Prefer same path; fall back to USB-serial match."""
        backoff = 1.0
        while not self._stop.is_set() and not self.connected:
            path = find_ttgo_port(self.port, self.usb_serial)
            if path and self._open(path):
                cb = self._on_reconnect
                if cb:
                    try:
                        cb(self)
                    except Exception as e:
                        log.error("on_reconnect callback error: %s", e)
                return
            if self._stop.wait(backoff):
                return
            backoff = min(backoff * 1.6, 10.0)

    def send(self, obj: dict):
        if not self.connected:
            log.debug("send dropped (disconnected): %s", obj.get("type"))
            return
        line = json.dumps(obj) + "\n"
        with self._lock:
            ser = self._ser
            if ser is None:
                return
            try:
                ser.write(line.encode())
                ser.flush()
                log.debug("→ %s", line.strip())
            except Exception as e:
                log.error("Serial send error: %s", e)
                self._mark_disconnected(str(e))

    def readline(self) -> Optional[str]:
        if not self.connected:
            # Passive reconnect attempt driven by the main loop; cheap.
            self._reconnect_loop()
            return None
        try:
            ser = self._ser
            if ser is None:
                return None
            raw = ser.readline()
            if raw:
                return raw.decode(errors="replace").strip()
        except (serial.SerialException, OSError) as e:
            self._mark_disconnected(str(e))
        except Exception as e:
            log.debug("readline error: %s", e)
        return None

    def close(self):
        self._stop.set()
        with self._lock:
            try:
                if self._ser and self._ser.is_open:
                    self._ser.close()
            except Exception:
                pass
            self._ser = None
        with self._state_lock:
            self._connected = False


# ── TTS → WM8960 speaker ─────────────────────────────────────────────────────
def speak(text: str):
    """Convert text to speech and play on the WM8960 speaker (ALSA)."""
    if not text.strip():
        return
    # Truncate very long responses for speech
    words = text.split()
    if len(words) > 80:
        text = " ".join(words[:80]) + "…"

    log.info("TTS: %d chars → %s", len(text), AUDIO_DEVICE)
    try:
        import edge_tts
        fd, mp3_path = tempfile.mkstemp(suffix=".mp3")
        os.close(fd)

        async def _synthesize():
            comm = edge_tts.Communicate(text, TTS_VOICE)
            await comm.save(mp3_path)

        asyncio.run(_synthesize())

        # ffmpeg mp3 → wav, then aplay to the WM8960 ALSA device
        fd2, wav_path = tempfile.mkstemp(suffix=".wav")
        os.close(fd2)

        # Convert mp3 → wav
        subprocess.run(
            ["ffmpeg", "-y", "-i", mp3_path, "-ar", "44100", "-ac", "2", wav_path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True
        )
        os.unlink(mp3_path)

        # SYSTEM VOLUME ENFORCEMENT — the single source of loudness.
        # Set BEFORE every playback so greeting and post-capture response are
        # always equally loud. 'Speaker' is the system volume; 'Playback' (DAC)
        # is pinned to unity so Speaker is the only effective control.
        for ctrl, val in (("Playback", "255"), ("Speaker", SYSTEM_VOLUME)):
            r = subprocess.run(
                ["amixer", "-c", "wm8960soundcard", "sset", ctrl, val],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
            )
            if r.returncode != 0:
                log.warning("amixer sset %s %s failed (rc=%d): %s", ctrl, val,
                            r.returncode, r.stderr.decode(errors="replace") if r.stderr else "?")
        log.info("System volume enforced: Speaker=%s Playback=255", SYSTEM_VOLUME)

        # Kick the PCM device open/closed to force a fresh codec path after
        # capture. The WM8960 is half-duplex over I2S; a capture-then-playback
        # transition can leave internal routing stuck even though amixer
        # controls look correct.
        try:
            subprocess.run(
                ["aplay", "-D", AUDIO_DEVICE, "-d", "0", "/dev/zero"],
                timeout=0.3, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
        except (subprocess.TimeoutExpired, Exception):
            pass

        # Play wav on speaker. Capture device may still be settling after PTT
        # (PipeWire/ALSA half-duplex race) → retry on "device busy".
        played = False
        for attempt in range(4):
            res = subprocess.run(
                ["aplay", "-D", AUDIO_DEVICE, wav_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            if res.returncode == 0:
                played = True
                log.info("aplay attempt %d succeeded (rc=0)", attempt + 1)
                break
            err = (res.stderr or b"").decode(errors="replace").strip()
            log.warning("aplay attempt %d failed (rc=%d): %s", attempt + 1, res.returncode, err)
            time.sleep(0.4)
        os.unlink(wav_path)
        if played:
            log.info("TTS playback done")
        else:
            log.error("TTS playback FAILED after retries on %s", AUDIO_DEVICE)

    except Exception as e:
        log.error("TTS error: %s", e)

# ── PTT recorder (direct ALSA via arecord) ────────────────────────────────────
# Uses arecord on the WM8960 directly. The PipeWire-based voice_mode recorder
# left the half-duplex I2S capture stream RUNNING after stop(), which attenuated
# subsequent playback (loud greeting, quiet responses). arecord is a child
# process we fully terminate, guaranteeing the capture path is released before
# we play the TTS response back on the same codec.
class PttRecorder:
    def __init__(self):
        self._proc = None
        self._wav_path = None
        self._lock = threading.Lock()

    def start(self):
        with self._lock:
            if self._proc is not None:
                log.warning("Recorder already running; ignoring start")
                return
            fd, self._wav_path = tempfile.mkstemp(suffix=".wav", prefix="ptt_")
            os.close(fd)
            self._proc = subprocess.Popen(
                ["arecord", "-D", MIC_DEVICE, "-f", "S16_LE",
                 "-r", str(RECORD_RATE), "-c", "1", self._wav_path],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            log.info("arecord capture started → %s", self._wav_path)

    def stop(self) -> Optional[str]:
        with self._lock:
            if self._proc is None:
                return None
            # SIGINT lets arecord flush the WAV header cleanly, then ensure exit.
            try:
                self._proc.send_signal(signal.SIGINT)
                try:
                    self._proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self._proc.terminate()
                    self._proc.wait(timeout=2)
            except Exception as e:
                log.warning("arecord stop error: %s", e)
                try:
                    self._proc.kill()
                except Exception:
                    pass
            self._proc = None
            wav_path = self._wav_path
            self._wav_path = None
            log.info("arecord capture stopped: %s", wav_path)
            # Capture device is now fully closed (child exited). Half-duplex
            # I2S bus is free for playback.
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
def close_session(session_id: str):
    """Tell the Hermes API to delete (close) the given session."""
    if not session_id:
        return
    try:
        url = f"{API_BASE}/api/sessions/{session_id}"
        r = requests.delete(url, headers={"Authorization": f"Bearer {API_KEY}"}, timeout=5)
        log.info("Closed session %s — HTTP %d", session_id, r.status_code)
    except Exception as e:
        log.warning("Failed to close session %s: %s", session_id, e)


def handle_new_chat(transport: SerialTransport):
    global chat_starting
    with chat_start_lock:
        if chat_starting or not chat_start_allowed():
            log.info("Ignoring duplicate new_chat while starting/blocking — resyncing display")
            # Re-sync display so firmware doesn't time out in SCR_STARTING
            sid = state.sid
            if sid:
                transport.send({"type": "chat_started", "session_id": sid})
                transport.send({
                    "type":        "chat_stats",
                    "messages":    state.messages,
                    "tokens":      state.tokens,
                    "context_pct": state.context_pct(),
                })
                transport.send({"type": "ack", "text": "Already started"})
            else:
                transport.send({"type": "ack", "text": "Starting..."})
            return
        chat_starting = True

    # Close the old session on the server before starting a new one.
    old_sid = state.sid
    if old_sid:
        threading.Thread(target=close_session, args=(old_sid,), daemon=True).start()

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

    # Let ALSA half-duplex device settle after capture closes.
    # Without this, the WM8960 codec may still be in capture mode when
    # playback starts, routing audio to nowhere even though aplay rc=0.
    time.sleep(0.5)

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
    log.info("Mic: %s  Speaker (WM8960): %s", MIC_DEVICE, AUDIO_DEVICE)

    while True:
        line = transport.readline()
        if not line:
            # When disconnected, readline() returns quickly after a reconnect
            # attempt; sleep a bit to avoid a hot loop. When connected but
            # idle, the pyserial timeout already throttles us.
            time.sleep(0.05 if not transport.connected else 0.01)
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

    transport = SerialTransport(args.port, args.baud, usb_serial=SERIAL_USB_ID)

    # After a reconnect, send a fresh "Bridge OK" ack so the TTGO knows
    # we're back. The TTGO firmware also sends its own device_ready on
    # boot/reset, which will trigger the normal hello path too.
    def _on_reconnect(t):
        try:
            t.send({"type": "ack", "text": "Bridge OK"})
        except Exception:
            pass
    transport.set_on_reconnect(_on_reconnect)

    transport.connect(blocking=True)

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
