#!/bin/bash
# Setup script for Watcher MQTT subscriber auto-restart.
# Creates the credentials file (600 perms) and enables the systemd service.
#
# Usage: ./setup-subscriber.sh
# You will be prompted for the MQTT password.

set -e

ENV_FILE="$HOME/.config/watcher-mqtt.env"
SERVICE_SRC="$HOME/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/watcher-subscriber.service"
SERVICE_DST="$HOME/.config/systemd/user/watcher-subscriber.service"

echo "Watcher MQTT Subscriber Setup"
echo "=============================="
echo

# Prompt for password (not echoed)
read -sp "MQTT password (user=muse): " MQTT_PASS
echo

if [ -z "$MQTT_PASS" ]; then
    echo "Error: password cannot be empty" >&2
    exit 1
fi

# Create config dir with restricted perms
mkdir -p "$HOME/.config"
chmod 700 "$HOME/.config"

# Write credentials file (600 perms - only owner can read)
cat > "$ENV_FILE" <<EOF
MQTT_HOST=mqtt.offx1.com
MQTT_PORT=8883
MQTT_USER=muse
MQTT_PASS=$MQTT_PASS
EOF
chmod 600 "$ENV_FILE"
echo "Credentials written to $ENV_FILE (600 perms)"

# Install systemd service
mkdir -p "$HOME/.config/systemd/user"
cp "$SERVICE_SRC" "$SERVICE_DST"
echo "Service installed to $SERVICE_DST"

# Reload, enable, and start
systemctl --user daemon-reload
systemctl --user enable watcher-subscriber.service
systemctl --user restart watcher-subscriber.service

echo
echo "Done! Subscriber is now running under systemd and will auto-restart."
echo "Check status: systemctl --user status watcher-subscriber.service"
echo "View logs: tail -f ~/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/subscriber.log"
