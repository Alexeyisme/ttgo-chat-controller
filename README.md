# TTGO Chat Controller

A tiny hardware **voice chat controller** for [Hermes Agent](https://github.com/hypernym-ai/hermes-agent). Press a button, talk to your AI. Release the button and hear it talk back. The TFT display shows session stats, token count, and a context-usage bar — no keyboard, no screen, no phone.

**Now with a premium landscape-optimized UI overhaul!**

```
┌──────────────────────┐        USB          ┌──────────────────────────┐
│   TTGO T-Display     │ ─── serial, 115200 ─│   Bridge (Python)        │
│      (LANDSCAPE)     │                     │   ttgo_chat_bridge.py    │
└──────────────────────┘                     └────────────┬─────────────┘
                                                          │
                        ┌─────────────────────────────────┼────────────────────┐
                        │ hermes-agent STT/TTS │ Hermes API│ PipeWire default   │
                        │  (config.yaml)       │ /v1/chat  │ source/sink (audio)│
                        └──────────────────────┴───────────┴────────────────────┘
```

The bridge is deliberately thin. It does **not** hold any STT/TTS keys, pick a
voice, or name an audio device:

- **STT/TTS** are delegated to **hermes-agent** (`tools.transcription_tools` /
  `tools.tts_tool`) — provider and voice come from your `~/.hermes/config.yaml`,
  the same settings as the rest of Hermes.
- **Audio** follows the **system default** devices via PipeWire (`pw-record` /
  `pw-play`). Swap the mic/speaker — or change the default sink — and the bridge
  follows. **Loudness is the system volume** (`wpctl set-volume @DEFAULT_AUDIO_SINK@ …`
  / the desktop slider); the bridge applies no gain of its own.

**Hardware:** [LilyGO TTGO T-Display](https://www.lilygo.cc/products/lilygo%C2%AE-ttgo-t-display-1-14-inch-lcd-esp32-control-board) (ESP32, 135×240 ST7789 TFT, two buttons), plus whatever mic/speaker your OS uses as defaults.

**Software:** Runs on a Linux host (Raspberry Pi, laptop, mini-PC) with [Hermes Agent](https://github.com/hypernym-ai/hermes-agent)'s API server enabled and a **PipeWire** audio session.

---

## Features

- **Landscape Dashboard** — Optimized 240x135 layout for a more professional desktop look.
- **TOP button → new chat** — Tap the top button (GPIO 0) to start a fresh Hermes session.
- **BTM button → push-to-talk** — Hold the bottom button (GPIO 35) speak, release to send.
- **Modernized Aesthetics** — Neon Cyan/Green theme with high-contrast stats and rounded context bars.
- **Animated Waveform** — Live 9-bar voice animation during recording fills the wide screen.
- **Zero WiFi config on the device** — All communication happens over USB serial.
- **No speech keys, no device config** — STT/TTS use your hermes-agent settings; audio uses the system defaults.
- **Runs as a user systemd service** — `systemctl --user restart ttgo-chat-bridge`

---

## Quick Start

### 1. Prerequisites

- A working [Hermes Agent](https://github.com/hypernym-ai/hermes-agent) install with:
  - the OpenAI-compatible **API server** enabled (see [Hermes API server](#hermes-api-server) below; defaults to `http://localhost:8642`), and
  - **STT and TTS configured** under `stt:` / `tts:` in `~/.hermes/config.yaml` (the bridge uses whatever you've set there).
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (`pip install platformio`) or the VS Code PlatformIO IDE.
- A Linux host with **PipeWire** (`pw-record`, `pw-play`), `ffmpeg`, and Python 3.11+.

### 2. Clone the repo inside a Hermes checkout

The bridge imports STT/TTS helpers (`tools.transcription_tools`, `tools.tts_tool`) directly from the Hermes Agent source tree, and runs in hermes-agent's venv. The simplest layout is to drop this repo **inside** the `hermes-agent/` directory:

```bash
cd ~/.hermes/hermes-agent        # or wherever your Hermes checkout lives
git clone https://github.com/<you>/ttgo-chat-controller.git ttgo-chat
cd ttgo-chat
```

Prefer a different layout? Set `HERMES_AGENT_ROOT=/abs/path/to/hermes-agent` in `bridge/.env` and clone wherever you want.

### 3. Run the installer

```bash
./install.sh
```

This will:

1. Create the `src/` → `firmware/` symlink PlatformIO needs
2. Ensure the hermes-agent venv has the bridge's extra deps (`pyserial`)
3. Copy `bridge/.env.example` → `bridge/.env`
4. (Optional) render and install the **user-scope** systemd unit

### 4. Fill in `bridge/.env`

```bash
$EDITOR bridge/.env
```

Required:

- `HERMES_API_KEY` — must match the `API_SERVER_KEY` the Hermes API server runs with (see [Hermes API server](#hermes-api-server)).

No audio devices and no STT/TTS keys go here — capture/playback use the system
default devices, and speech uses your hermes-agent `stt:`/`tts:` config. To choose
which mic/speaker is used, set them as your **system defaults** (e.g. `wpctl
set-default <id>` or your desktop sound settings).

### 5. Flash the firmware

```bash
# ensure the device is on /dev/ttyACM0 (or pass --upload-port)
pio run -e usb -t upload
```

### 6. Start the bridge

```bash
systemctl --user start ttgo-chat-bridge        # if you chose systemd install
# or, for foreground debugging (in the hermes-agent venv):
HERMES_AGENT_ROOT=/path/to/hermes-agent \
  /path/to/hermes-agent/venv/bin/python bridge/ttgo_chat_bridge.py --debug
```

The display should boot into the idle screen:

> `Press LEFT to start new chat`
> `Hold RIGHT to talk (PTT)`

Hold RIGHT, say "hello", release. You should hear Hermes respond over your speaker.

---

## Hermes API server

The bridge is a client of Hermes' OpenAI-compatible API server, which holds the
chat sessions. That server **won't start without an auth key**, so:

1. Pick one secret (e.g. `python3 -c 'import secrets; print(secrets.token_urlsafe(32))'`).
2. Enable the server by setting it in `~/.hermes/.env` (Hermes reads this file):

   ```bash
   API_SERVER_KEY=<your-secret>
   API_SERVER_HOST=127.0.0.1   # localhost only (default)
   API_SERVER_PORT=8642        # default
   ```

   Restart the Hermes gateway; the API server now listens on `127.0.0.1:8642`.
3. Put the **same** value in the bridge's `bridge/.env` as `HERMES_API_KEY`.

`API_SERVER_KEY` (server side) and `HERMES_API_KEY` (client/bridge side) are the
same secret under two names — the server checks the bearer token the bridge sends.

---

## Project Layout

```
.
├── firmware/                    # ESP32 Arduino firmware (PlatformIO)
│   ├── main.cpp                   # State machine, buttons, display, serial protocol
│   └── config.h                   # Pins, colors, timings, context window size
├── bridge/                      # Python bridge — USB serial ↔ Hermes API
│   ├── ttgo_chat_bridge.py        # Main bridge (serial I/O, chat state, audio glue)
│   ├── requirements.txt           # Extra deps (requests, pyserial) on top of hermes-agent
│   ├── .env.example               # Config template (copy to .env)
│   └── ttgo-chat-bridge.service.template   # systemd unit template
├── scripts/
│   └── flash_and_restart.sh       # Stop bridge → pio upload → restart + tail logs
├── platformio.ini               # PlatformIO config (board: lilygo-t-display)
├── install.sh                   # One-shot installer
└── src -> firmware/             # Created by install.sh (PlatformIO needs src/)
```

---

## Hardware

| Function        | GPIO  | Notes                                            |
|-----------------|-------|--------------------------------------------------|
| TOP button      | 0     | New chat on tap. High reliability.               |
| BTM button      | 35    | **Input only**, no pull-up. Hold ≥250 ms → PTT.  |
| TFT backlight   | 4     | Driven by TFT_eSPI                               |

**Orientation:** The device is configured for **landscape mode** with the buttons on the left.
- **GPIO 0** is the **Upper** button.
- **GPIO 35** is the **Lower** button.

Works with whatever mic/speaker your OS uses as the PipeWire defaults — a USB mic, a HAT, or the 3.5 mm jack. Tested on a WM8960 Audio HAT (both capture and playback) and on a USB mic + 3.5 mm out.

---

## Serial Protocol (115200 baud, newline-delimited JSON)

**TTGO → Bridge:**

```json
{"event": "device_ready"}    // on boot
{"event": "new_chat"}        // LEFT tapped
{"event": "ptt_start"}       // RIGHT held past threshold
{"event": "ptt_stop"}        // RIGHT released
```

**Bridge → TTGO:**

```json
{"type": "chat_started",  "session_id": "...", "name": ""}   // name empty at start
{"type": "session_name",  "name": "what time is it"}         // learned from 1st utterance
{"type": "chat_stats",    "messages": 3, "tokens": 4200, "context_pct": 2}
{"type": "status",        "text": "Searching web", "busy": true}  // STATUS card
{"type": "telemetry",     "cpu_temp": 47, "battery": 84}     // host metrics, either optional
{"type": "ack",           "text": "Thinking..."}    // green banner (≤21 chars)
{"type": "error",         "text": "No session"}     // returns to idle
```

**Live STATUS card:** During a turn the bridge streams the Hermes response (`stream: true`) and forwards each phase to the chat screen's STATUS card via `status` messages: `Transcribing → Thinking → <tool label> → Writing reply → Speaking → Ready`. Tool steps come from Hermes `event: hermes.tool.progress` SSE events (tool name mapped to a short label like *Running code*, *Searching web*). `busy: true` animates a spinner dot; `busy: false` shows a steady green dot. The status persists until replaced (no timeout), so a long "Thinking" no longer goes blank.

**Session name:** A session has no friendly name until the user speaks. On the first `ptt_stop`, the bridge takes the transcript, trims it to ~24 chars on a word boundary, and sends `session_name`. The TTGO idle **SESSION card** shows this name plus `N msg · Nk/M tok`; when no chat is active it shows *"No active session."*

**Auto-idle:** After `IDLE_TIMEOUT_MS` (2 min) with no activity — no PTT, `new_chat`, or incoming `chat_stats` — the stats screen falls back to the idle dashboard. The session stays active on the host and is shown on the SESSION card.

**Session lifecycle:** When `new_chat` fires, the bridge closes the current session on the Hermes server (`DELETE /api/sessions/{id}`) in a background thread before starting the new one. This frees server-side memory and produces a clean break in the session DB.

---

## Configuration Reference

All settings live in `bridge/.env` (or `~/.hermes/.env`). See [`bridge/.env.example`](bridge/.env.example) for the full list.

| Variable               | Default                               | Purpose                                      |
|------------------------|---------------------------------------|----------------------------------------------|
| `HERMES_API_BASE`      | `http://localhost:8642`               | Hermes API server URL                        |
| `HERMES_API_KEY`       | _(required)_                          | Must match the server's `API_SERVER_KEY`     |
| `HERMES_API_MODEL`     | `hermes-agent`                        | Model name passed to the API                 |
| `HERMES_AGENT_ROOT`    | _(auto)_                              | Path to the hermes-agent repo (STT/TTS imports) |
| `TTGO_CHAT_PORT`       | `/dev/ttyACM0`                        | USB serial port                              |
| `TTGO_CHAT_BAUD`       | `115200`                              | Baud rate (must match firmware)              |
| `TTGO_CHAT_USB_SERIAL` | _(optional)_                          | Autodetect port by USB serial number         |
| `CTX_WINDOW_TOKENS`    | `200000`                              | Used for the % bar on the display            |

STT/TTS provider, voice, and keys are **not** configured here — they come from
`~/.hermes/config.yaml` (`stt:` / `tts:`). Audio devices are **not** configured
here — they follow the system defaults via PipeWire.

---

## Development Loop

Fast iteration:

```bash
# Edit firmware/, then:
./scripts/flash_and_restart.sh

# Edit bridge/, then:
systemctl --user restart ttgo-chat-bridge
journalctl --user -u ttgo-chat-bridge -f
```

For foreground debugging without systemd (use the hermes-agent venv):

```bash
HERMES_AGENT_ROOT=/path/to/hermes-agent \
  /path/to/hermes-agent/venv/bin/python bridge/ttgo_chat_bridge.py --debug
```

---

## Troubleshooting

**"Sorry, I didn't catch that" / transcripts are empty or garbage (`'.'`, `'You'`)**
That's STT receiving **silent audio** — Whisper hallucinates short filler on silence.
The capture, not the bridge, is the problem. Record from the system default source
and check the level:

```bash
pw-record --rate 16000 --channels 1 /tmp/test.wav   # speak, then Ctrl+C
# inspect /tmp/test.wav — if it's near-silent, the mic isn't reaching the ADC
```

- Confirm the right mic is the **default source**: `wpctl status` (look for `*` under Sources).
- On a **WM8960 HAT**, the mic input path is often disabled by default. Enable it
  (mic on LINPUT1/RINPUT1) and persist:

  ```bash
  amixer -c wm8960soundcard sset 'Left Input Mixer Boost' on
  amixer -c wm8960soundcard sset 'Right Input Mixer Boost' on
  amixer -c wm8960soundcard sset 'Left Input Boost Mixer LINPUT1' 3
  amixer -c wm8960soundcard sset 'Right Input Boost Mixer RINPUT1' 3
  sudo alsactl store
  ```

**Bridge starts then exits immediately**
Usually the hermes-agent repo can't be imported. Make sure the service runs the
**hermes-agent venv** and `HERMES_AGENT_ROOT` points at the checkout.

**Speaker silent**

- Confirm the intended sink is the **default**: `wpctl status` (the `*` under Sinks).
  Set it with `wpctl set-default <id>` if needed.
- **Loudness is the system volume**: `wpctl get-volume @DEFAULT_AUDIO_SINK@` /
  `wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.8`. The bridge applies no gain.
- On a WM8960 HAT, also make sure the output mixer/`Speaker` is unmuted/up (`amixer -c wm8960soundcard`).

**No audio at all from a systemd service**
The unit must be **user-scope** (`systemctl --user`) so it shares your PipeWire
session, with linger enabled (`loginctl enable-linger $USER`). A system-scope
service has no PipeWire and audio silently fails.

**RIGHT button feels flaky / PTT starts on reset**
GPIO 0 is a boot-strap pin. Power-cycle the device without the button held. If the issue persists, swap LEFT/RIGHT roles in firmware (PTT on GPIO 35, new-chat on GPIO 0).

**`pio run` fails with "missing src directory"**
Run `./install.sh` or manually: `ln -s firmware src`.

---

## Credits & License

Built for the [Hermes Agent](https://github.com/hypernym-ai/hermes-agent) ecosystem. Uses:

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — ST7789 driver
- [ArduinoJson](https://arduinojson.org/) — firmware-side JSON
- [PipeWire](https://pipewire.org/) — audio capture/playback (`pw-record` / `pw-play`)

STT and TTS are provided by your [Hermes Agent](https://github.com/hypernym-ai/hermes-agent) configuration.

MIT. See [LICENSE](LICENSE).
