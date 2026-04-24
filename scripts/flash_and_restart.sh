#!/usr/bin/env bash
# flash_and_restart.sh — Build, flash, then restart the bridge
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$SCRIPT_DIR/.."
PORT="${1:-/dev/ttyACM0}"
ENV="${2:-usb}"

echo "=== TTGO Chat Controller: flash + restart ==="
echo "Port: $PORT  Env: $ENV"

# Stop bridge if running (releases serial port)
if systemctl is-active --quiet ttgo-chat-bridge.service 2>/dev/null; then
    echo "Stopping ttgo-chat-bridge.service..."
    sudo systemctl stop ttgo-chat-bridge.service
fi

# Build & flash
cd "$PROJECT"
pio run --environment "$ENV" --target upload --upload-port "$PORT"

echo "Waiting 3s for device to boot..."
sleep 3

# Start bridge
if [ -f /etc/systemd/system/ttgo-chat-bridge.service ]; then
    echo "Starting ttgo-chat-bridge.service..."
    sudo systemctl start ttgo-chat-bridge.service
    sleep 2
    systemctl status ttgo-chat-bridge.service --no-pager | head -20
    echo ""
    echo "Logs (Ctrl+C to stop):"
    journalctl -u ttgo-chat-bridge.service -f -n 30
else
    echo "Service not installed. Run bridge manually:"
    echo "  cd bridge && python ttgo_chat_bridge.py --debug"
fi
