#!/usr/bin/env bash
# install.sh — Bootstrap the TTGO Chat Controller on a fresh machine.
#
# What this does:
#   1. Creates the required `src/` → `firmware/` symlink (PlatformIO needs src/)
#   2. Ensures the hermes-agent venv has the bridge's extra deps (pyserial)
#   3. Creates bridge/.env from the template (edit it after)
#   4. (Optional) Renders and installs a USER-SCOPE systemd unit
#
# The bridge runs in hermes-agent's venv (it imports tools.transcription_tools /
# tools.tts_tool) and as a user service (it needs your PipeWire session for
# audio). It is NOT a system service.
#
# Usage:
#   ./install.sh                   # interactive — prompts before systemd install
#   ./install.sh --systemd         # non-interactive: install & start the service
#   ./install.sh --no-systemd      # skip systemd step
#
# Environment overrides:
#   HERMES_AGENT_ROOT  Path to the hermes-agent checkout (default: auto-detected
#                      from layout if this repo lives inside it)
#   HERMES_ENV_FILE    Path to the Hermes .env file (default: ~/.hermes/.env)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
HERMES_ENV_FILE="${HERMES_ENV_FILE:-$HOME/.hermes/.env}"

# Auto-detect hermes-agent root: if this repo sits inside it (…/hermes-agent/
# ttgo-chat/), parents[1] is the checkout. Override with HERMES_AGENT_ROOT.
HERMES_AGENT_ROOT="${HERMES_AGENT_ROOT:-$(cd "$PROJECT_DIR/.." && pwd)}"
HERMES_AGENT_VENV="$HERMES_AGENT_ROOT/venv"

echo "=== TTGO Chat Controller Installer ==="
echo "Project:            $PROJECT_DIR"
echo "hermes-agent root:  $HERMES_AGENT_ROOT"
echo "hermes-agent venv:  $HERMES_AGENT_VENV"
echo "Hermes env:         $HERMES_ENV_FILE"
echo

if [ ! -x "$HERMES_AGENT_VENV/bin/python" ]; then
    echo "ERROR: hermes-agent venv not found at $HERMES_AGENT_VENV"
    echo "Set HERMES_AGENT_ROOT to your hermes-agent checkout and re-run."
    exit 1
fi

# ── 1. src/ symlink for PlatformIO ────────────────────────────────────────
if [ ! -e "$PROJECT_DIR/src" ]; then
    echo "[1/4] Creating src/ -> firmware/ symlink (PlatformIO requirement)"
    ln -s firmware "$PROJECT_DIR/src"
else
    echo "[1/4] src/ already exists — skipping"
fi

# ── 2. Bridge deps into the hermes-agent venv ─────────────────────────────
echo "[2/4] Installing bridge deps into the hermes-agent venv"
"$HERMES_AGENT_VENV/bin/pip" install -q -r "$PROJECT_DIR/bridge/requirements.txt"

# ── 3. bridge/.env bootstrap ──────────────────────────────────────────────
if [ ! -f "$PROJECT_DIR/bridge/.env" ]; then
    echo "[3/4] Creating bridge/.env from .env.example — EDIT ME with your HERMES_API_KEY"
    cp "$PROJECT_DIR/bridge/.env.example" "$PROJECT_DIR/bridge/.env"
else
    echo "[3/4] bridge/.env already present — skipping"
fi

# ── 4. User-scope systemd unit ────────────────────────────────────────────
MODE="interactive"
for arg in "$@"; do
    case "$arg" in
        --systemd)    MODE="yes" ;;
        --no-systemd) MODE="no"  ;;
    esac
done

if [ "$MODE" = "interactive" ]; then
    read -rp "[4/4] Install user-scope systemd service? [y/N] " yn
    [[ "$yn" =~ ^[Yy] ]] && MODE="yes" || MODE="no"
fi

if [ "$MODE" = "yes" ]; then
    UNIT_DIR="$HOME/.config/systemd/user"
    mkdir -p "$UNIT_DIR"
    sed \
        -e "s|__PROJECT_DIR__|$PROJECT_DIR|g" \
        -e "s|__HERMES_AGENT_ROOT__|$HERMES_AGENT_ROOT|g" \
        -e "s|__HERMES_AGENT_VENV__|$HERMES_AGENT_VENV|g" \
        -e "s|__HERMES_ENV__|$HERMES_ENV_FILE|g" \
        "$PROJECT_DIR/bridge/ttgo-chat-bridge.service.template" \
        > "$UNIT_DIR/ttgo-chat-bridge.service"
    echo "[4/4] Installed $UNIT_DIR/ttgo-chat-bridge.service"
    # Keep the service alive across logouts (needed for a headless Pi).
    loginctl enable-linger "$(id -un)" 2>/dev/null || \
        echo "    (could not enable linger — run: sudo loginctl enable-linger $(id -un))"
    systemctl --user daemon-reload
    systemctl --user enable --now ttgo-chat-bridge.service
    echo
    systemctl --user status ttgo-chat-bridge.service --no-pager -n 10 || true
else
    echo "[4/4] Skipping systemd install. Run the bridge manually:"
    echo "    HERMES_AGENT_ROOT=$HERMES_AGENT_ROOT \\"
    echo "      $HERMES_AGENT_VENV/bin/python bridge/ttgo_chat_bridge.py --debug"
fi

echo
echo "✓ Install complete."
echo
echo "Next steps:"
echo "  • Enable the Hermes API server   (set API_SERVER_KEY in ~/.hermes/.env — see README)"
echo "  • Edit bridge/.env               (set HERMES_API_KEY to match)"
echo "  • Flash the firmware:            pio run -e usb -t upload"
echo "  • Tail logs:                     journalctl --user -u ttgo-chat-bridge -f"
