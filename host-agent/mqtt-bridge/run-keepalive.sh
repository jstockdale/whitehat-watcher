#!/bin/bash
# Supervisor for the Watcher keep-alive daemon.
# Restarts keepalive.py if it ever exits (it shouldn't, but belts and
# suspenders after the 2026-09-23 silent-death incident).
# Launch once:  setsid nohup ./run-keepalive.sh >/dev/null 2>&1 < /dev/null &
cd "$(dirname "$0")"
while true; do
  python3 keepalive.py --interval 90
  rc=$?
  echo "$(date '+%F %T') keepalive exited rc=$rc; restarting in 5s" >> keepalive.log
  sleep 5
done
