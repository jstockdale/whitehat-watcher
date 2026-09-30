#!/bin/bash
# Supervisor for the Watcher MQTT bridge subscriber.
# Restarts subscriber.py if it ever exits (it died silently ~23:15 on 2026-09-23).
# Credentials come from ~/.config/watcher-mqtt.env (600 perms, written by setup-subscriber.sh).
# Launch once:  setsid nohup ./run-subscriber.sh >/dev/null 2>&1 < /dev/null &
cd "$(dirname "$0")"
set -a
# shellcheck disable=SC1090
. "$HOME/.config/watcher-mqtt.env"
set +a
while true; do
  python3 subscriber.py
  rc=$?
  echo "$(date '+%F %T') subscriber exited rc=$rc; restarting in 5s" >> subscriber.log
  sleep 5
done
