#!/usr/bin/env bash
# flash_and_restart.sh — Build, flash, then restart the bridge
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$SCRIPT_DIR/.."
PORT="${1:-/dev/ttyACM0}"
ENV="${2:-usb}"

echo "=== TTGO Chat Controller: flash + restart ==="
echo "Port: $PORT  Env: $ENV"

# Stop bridge if running (releases serial port). User-scope service.
if systemctl --user is-active --quiet ttgo-chat-bridge.service 2>/dev/null; then
    echo "Stopping ttgo-chat-bridge.service..."
    systemctl --user stop ttgo-chat-bridge.service
fi

# Build & flash
cd "$PROJECT"
pio run --environment "$ENV" --target upload --upload-port "$PORT"

echo "Waiting 3s for device to boot..."
sleep 3

# Start bridge
if systemctl --user list-unit-files ttgo-chat-bridge.service >/dev/null 2>&1 \
   && [ -f "$HOME/.config/systemd/user/ttgo-chat-bridge.service" ]; then
    echo "Starting ttgo-chat-bridge.service..."
    systemctl --user start ttgo-chat-bridge.service
    sleep 2
    systemctl --user status ttgo-chat-bridge.service --no-pager | head -20
    echo ""
    echo "Logs (Ctrl+C to stop):"
    journalctl --user -u ttgo-chat-bridge.service -f -n 30
else
    echo "Service not installed. Run bridge manually (in the hermes-agent venv):"
    echo "  HERMES_AGENT_ROOT=/path/to/hermes-agent \\"
    echo "    /path/to/hermes-agent/venv/bin/python bridge/ttgo_chat_bridge.py --debug"
fi
