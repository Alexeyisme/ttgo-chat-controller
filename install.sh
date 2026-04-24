#!/usr/bin/env bash
# install.sh — Bootstrap the TTGO Chat Controller on a fresh machine.
#
# What this does:
#   1. Creates the required `src/` → `firmware/` symlink (PlatformIO needs src/)
#   2. Creates a Python venv under bridge/.venv and installs requirements
#   3. Renders the systemd unit from the template with your actual paths/user
#   4. (Optional) Enables and starts the service
#
# Usage:
#   ./install.sh                   # interactive — prompts before systemd install
#   ./install.sh --systemd         # non-interactive: install & start the service
#   ./install.sh --no-systemd      # skip systemd step
#
# Environment overrides:
#   HERMES_ENV_FILE   Path to the Hermes .env file (default: ~/.hermes/.env)
#   TARGET_USER       System user that owns the service (default: current user)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"

TARGET_USER="${TARGET_USER:-$(id -un)}"
HERMES_ENV_FILE="${HERMES_ENV_FILE:-$HOME/.hermes/.env}"

echo "=== TTGO Chat Controller Installer ==="
echo "Project:     $PROJECT_DIR"
echo "User:        $TARGET_USER"
echo "Hermes env:  $HERMES_ENV_FILE"
echo

# ── 1. src/ symlink for PlatformIO ────────────────────────────────────────
if [ ! -e "$PROJECT_DIR/src" ]; then
    echo "[1/4] Creating src/ -> firmware/ symlink (PlatformIO requirement)"
    ln -s firmware "$PROJECT_DIR/src"
else
    echo "[1/4] src/ already exists — skipping"
fi

# ── 2. Python venv + deps ─────────────────────────────────────────────────
cd "$PROJECT_DIR/bridge"
if [ ! -d ".venv" ]; then
    echo "[2/4] Creating bridge/.venv"
    python3 -m venv .venv
fi
echo "[2/4] Installing Python dependencies"
./.venv/bin/pip install --upgrade pip >/dev/null
./.venv/bin/pip install -r requirements.txt

# ── 3. bridge/.env bootstrap ──────────────────────────────────────────────
if [ ! -f "$PROJECT_DIR/bridge/.env" ]; then
    echo "[3/4] Creating bridge/.env from .env.example — EDIT ME with your keys/devices"
    cp "$PROJECT_DIR/bridge/.env.example" "$PROJECT_DIR/bridge/.env"
else
    echo "[3/4] bridge/.env already present — skipping"
fi

# ── 4. Systemd unit ───────────────────────────────────────────────────────
MODE="interactive"
for arg in "$@"; do
    case "$arg" in
        --systemd)    MODE="yes" ;;
        --no-systemd) MODE="no"  ;;
    esac
done

if [ "$MODE" = "interactive" ]; then
    read -rp "[4/4] Install systemd service (sudo required)? [y/N] " yn
    [[ "$yn" =~ ^[Yy] ]] && MODE="yes" || MODE="no"
fi

if [ "$MODE" = "yes" ]; then
    TMP_UNIT="$(mktemp)"
    sed \
        -e "s|__USER__|$TARGET_USER|g" \
        -e "s|__PROJECT_DIR__|$PROJECT_DIR|g" \
        -e "s|__HERMES_ENV__|$HERMES_ENV_FILE|g" \
        "$PROJECT_DIR/bridge/ttgo-chat-bridge.service.template" > "$TMP_UNIT"
    echo "[4/4] Installing /etc/systemd/system/ttgo-chat-bridge.service"
    sudo install -m 0644 "$TMP_UNIT" /etc/systemd/system/ttgo-chat-bridge.service
    rm -f "$TMP_UNIT"
    sudo systemctl daemon-reload
    sudo systemctl enable --now ttgo-chat-bridge.service
    echo
    sudo systemctl status ttgo-chat-bridge.service --no-pager -n 10 || true
else
    echo "[4/4] Skipping systemd install. Run the bridge manually:"
    echo "    cd bridge && source .venv/bin/activate && python ttgo_chat_bridge.py --debug"
fi

echo
echo "✓ Install complete."
echo
echo "Next steps:"
echo "  • Flash the firmware:   pio run -e usb -t upload"
echo "  • Edit bridge/.env      (HERMES_API_KEY, GROQ_API_KEY, audio devices)"
echo "  • Tail logs:            journalctl -u ttgo-chat-bridge -f"
