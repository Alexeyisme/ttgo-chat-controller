# TTGO Chat Controller

A tiny hardware **voice chat controller** for [Hermes Agent](https://github.com/hypernym-ai/hermes-agent). Press a button, talk to your AI. Release the button and hear it talk back. The TFT display shows session stats, token count, and a context-usage bar — no keyboard, no screen, no phone.

```
┌──────────────────────┐        USB          ┌──────────────────────────┐
│   TTGO T-Display     │ ─── serial, 115200 ─│   Bridge (Python)        │
│   ESP32 + TFT + 2 btn│                     │   ttgo_chat_bridge.py    │
└──────────────────────┘                     └────────────┬─────────────┘
                                                          │
                               ┌──────────────┬───────────┼──────────────────┐
                               │ Groq Whisper │ Hermes API│ edge-tts + ALSA  │
                               │    (STT)     │ /v1/chat  │  (TTS + speaker) │
                               └──────────────┴───────────┴──────────────────┘
```

**Hardware:** [LilyGO TTGO T-Display](https://www.lilygo.cc/products/lilygo%C2%AE-ttgo-t-display-1-14-inch-lcd-esp32-control-board) (ESP32, 135×240 ST7789 TFT, two buttons), a USB mic, and any ALSA or Bluetooth A2DP speaker.

**Software:** Runs on a Linux host (Raspberry Pi, laptop, mini-PC) with [Hermes Agent](https://github.com/hypernym-ai/hermes-agent)'s API server enabled.

---

## Features

- **LEFT button → new chat** — fresh Hermes session, one tap
- **RIGHT button → push-to-talk** — hold, speak, release; transcribed and sent to Hermes, reply spoken back
- **TFT display states** — idle hints → starting animation → live stats (messages · tokens · context %) → listening waveform
- **Zero WiFi config on the device** — all talk happens over USB serial
- **Runs as a systemd service** — `sudo systemctl restart ttgo-chat-bridge`

---

## Quick Start

### 1. Prerequisites

- A working [Hermes Agent](https://github.com/hypernym-ai/hermes-agent) install with the OpenAI-compatible **API server** enabled (defaults to `http://localhost:8642`). Your `~/.hermes/config.yaml` must set an `API_SERVER_KEY`.
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (`pip install platformio`) or the VS Code PlatformIO IDE.
- Linux host with ALSA tools (`arecord`, `aplay`, `ffmpeg`) and Python 3.11+.
- A free [Groq](https://console.groq.com/keys) API key for speech-to-text (Whisper).

### 2. Clone the repo inside a Hermes checkout

The bridge imports a few helpers (`tools/voice_mode`, `tools/transcription_tools`) directly from the Hermes Agent source tree. The simplest layout is to drop this repo **next to or inside** the `hermes-agent/` directory:

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
2. Create `bridge/.venv` and install Python deps
3. Copy `bridge/.env.example` → `bridge/.env`
4. (Optional) render and install the systemd unit

### 4. Fill in `bridge/.env`

```bash
$EDITOR bridge/.env
```

Required:

- `HERMES_API_KEY` — must match `API_SERVER_KEY` in `~/.hermes/config.yaml`
- `GROQ_API_KEY` — from https://console.groq.com/keys

Audio (adjust to your hardware — `arecord -l` / `aplay -l`):

- `TTGO_MIC_DEVICE` (e.g. `hw:1,0`, `default`)
- `TTGO_BT_DEVICE` (e.g. `default`, `plughw:0,0`, `bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp`)

### 5. Flash the firmware

```bash
# ensure the device is on /dev/ttyACM0 (or pass --upload-port)
pio run -e usb -t upload
```

### 6. Start the bridge

```bash
sudo systemctl start ttgo-chat-bridge          # if you chose systemd install
# or, for foreground debugging:
cd bridge && .venv/bin/python ttgo_chat_bridge.py --debug
```

The display should boot into the idle screen:

> `Press LEFT to start new chat`
> `Hold RIGHT to talk (PTT)`

Hold RIGHT, say "hello", release. You should hear Hermes respond over your speaker.

---

## Project Layout

```
.
├── firmware/                    # ESP32 Arduino firmware (PlatformIO)
│   ├── main.cpp                   # State machine, buttons, display, serial protocol
│   └── config.h                   # Pins, colors, timings, context window size
├── bridge/                      # Python bridge — USB serial ↔ Hermes API
│   ├── ttgo_chat_bridge.py        # Main bridge (serial I/O, STT, TTS, chat state)
│   ├── requirements.txt           # Runtime deps
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
| LEFT button     | 35    | **Input only**, no pull-up — new chat on tap     |
| RIGHT button    | 0     | Has internal pull-up. Hold ≥250 ms → PTT start   |
| TFT backlight   | 4     | Driven by TFT_eSPI                               |

GPIO 0 is an ESP32 boot-strap pin. Holding it during reset enters the ROM bootloader, so we use polling + debounce rather than interrupts and a 20 ms raw settle window before treating the edge as pressed.

Works with any USB microphone. Tested extensively with a Logitech C920 (as `hw:3,0`) and a generic ATOP6868 Bluetooth A2DP speaker via `bluealsa`. A 3.5 mm jack + `default` also works fine.

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
{"type": "chat_started", "session_id": "..."}
{"type": "chat_stats",   "messages": 3, "tokens": 4200, "context_pct": 2}
{"type": "ack",          "text": "Thinking..."}    // green banner (≤21 chars)
{"type": "error",        "text": "No session"}     // returns to idle
```

---

## Configuration Reference

All settings live in `bridge/.env` (or `~/.hermes/.env`). See [`bridge/.env.example`](bridge/.env.example) for the full list.

| Variable               | Default                               | Purpose                                      |
|------------------------|---------------------------------------|----------------------------------------------|
| `HERMES_API_BASE`      | `http://localhost:8642`               | Hermes API server URL                        |
| `HERMES_API_KEY`       | _(required)_                          | Must match `API_SERVER_KEY` in Hermes config |
| `HERMES_API_MODEL`     | `hermes-agent`                        | Model name passed to the API                 |
| `GROQ_API_KEY`         | _(required)_                          | Speech-to-text (Whisper large v3 turbo)      |
| `TTGO_CHAT_PORT`       | `/dev/ttyACM0`                        | USB serial port                              |
| `TTGO_CHAT_BAUD`       | `115200`                              | Baud rate (must match firmware)              |
| `TTGO_MIC_DEVICE`      | `default`                             | ALSA capture device                          |
| `TTGO_BT_DEVICE`       | `default`                             | ALSA playback device (or bluealsa:…)         |
| `TTGO_TTS_VOICE`       | `en-US-AvaMultilingualNeural`         | edge-tts voice                               |
| `CTX_WINDOW_TOKENS`    | `200000`                              | Used for the % bar on the display            |
| `HERMES_AGENT_ROOT`    | _(auto)_                              | Override path to the Hermes repo             |

---

## Development Loop

Fast iteration:

```bash
# Edit firmware/, then:
./scripts/flash_and_restart.sh

# Edit bridge/, then:
sudo systemctl restart ttgo-chat-bridge
journalctl -u ttgo-chat-bridge -f
```

For foreground debugging without systemd:

```bash
cd bridge
source .venv/bin/activate
python ttgo_chat_bridge.py --debug
```

---

## Troubleshooting

**"Sorry, I didn't catch that" after every PTT release**
STT failed, not the mic. Check logs for `STT failed:` — the most common cause is a missing `openai` package in the bridge venv (Whisper calls go through the OpenAI SDK pointed at Groq's endpoint). Fix:

```bash
bridge/.venv/bin/pip install 'openai>=1.40.0'
```

**Bridge starts then exits immediately**
Usually means the Hermes repo can't be imported. Set `HERMES_AGENT_ROOT=/abs/path/to/hermes-agent` in `bridge/.env`.

**BT speaker silent** (check in this order)

1. **Bridge routing** — the most common cause. Check the bridge log:

   ```bash
   journalctl -u ttgo-chat-bridge -n 20 | grep "BT speaker"
   ```

   If it says `BT speaker: default`, the bridge is playing to onboard ALSA — not to the BT sink. Set `TTGO_BT_DEVICE` in `bridge/.env` and restart:

   ```bash
   echo 'TTGO_BT_DEVICE=bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp' >> bridge/.env
   sudo systemctl restart ttgo-chat-bridge
   ```

2. **BT volume** — bluealsa sink volume tracks the remote device and can end up very low (e.g. 6/127). Check and bump:

   ```bash
   bluealsa-cli volume /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink       # read
   bluealsa-cli volume /org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink 90    # set
   ```

   Note: volume alone is never the fix if routing is wrong — the PCM never opens, so the change is inaudible regardless.

3. **Device reachable** — `bluealsa` cannot play MP3 directly; the bridge already converts to 44100 Hz stereo WAV via `ffmpeg`. Verify the sink:

   ```bash
   aplay -D bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp --dump-hw-params /dev/null
   ```

**RIGHT button feels flaky / PTT starts on reset**
GPIO 0 is a boot-strap pin. Power-cycle the device without the button held. If the issue persists, swap LEFT/RIGHT roles in firmware (PTT on GPIO 35, new-chat on GPIO 0).

**`pio run` fails with "missing src directory"**
Run `./install.sh` or manually: `ln -s firmware src`.

**Mic capture works but transcripts are empty**
Check the WAV written to `/tmp/hermes_voice/` — if it's 0 bytes or pure silence, it's a mic routing issue. Try `arecord -D "$TTGO_MIC_DEVICE" -f S16_LE -r 16000 -d 3 /tmp/test.wav`.

---

## Credits & License

Built for the [Hermes Agent](https://github.com/hypernym-ai/hermes-agent) ecosystem. Uses:

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — ST7789 driver
- [ArduinoJson](https://arduinojson.org/) — firmware-side JSON
- [Groq Whisper](https://console.groq.com/) — free-tier STT
- [edge-tts](https://github.com/rany2/edge-tts) — free cloud TTS

MIT. See [LICENSE](LICENSE).
