#!/usr/bin/env bash
# bridge-watchdog.sh — revive the MQTT bridge subscriber if it has died.
#
# Standing rule (John, 2026-09-30): the MQTT bridge should just come up
# automatically when we're on the bench together. subscriber.py died twice
# silently on 2026-10-04 with no traceback, so a cron fires this every minute.
# It stays quiet (exit 0, no output) whenever the bridge is alive.
#
# Restart credentials come from ~/.config/watcher-mqtt.env, the same file
# John created with setup-subscriber.sh and the same source used for manual
# restarts. This script never prints the file's contents.
set -u
BRIDGE_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$BRIDGE_DIR/watchdog.log"
LOCK="$BRIDGE_DIR/.watchdog.lock"

# Anchored pattern avoids matching our own shell (see AGENTS.md pkill lesson).
if pgrep -f '[s]ubscriber\.py' >/dev/null 2>&1; then
  exit 0  # bridge alive; stay quiet
fi

exec 9>"$LOCK"
if ! flock -n 9; then exit 0; fi
# Re-check after acquiring the lock in case it started meanwhile.
if pgrep -f '[s]ubscriber\.py' >/dev/null 2>&1; then
  exit 0
fi

NOW="$(date -u +%FT%TZ)"
echo "$NOW bridge dead; restarting" >> "$LOG"

set -a
# shellcheck disable=SC1091
source "$HOME/.config/watcher-mqtt.env"
set +a
cd "$BRIDGE_DIR"
PYTHON="$(command -v python3)"
# 9>&-: don't let the child inherit the lock FD; the lock is only ever held
# briefly by watchdog runs, never for the subscriber's whole lifetime.
nohup "$PYTHON" subscriber.py >> subscriber.log 2>&1 9>&- &
echo "$NOW restarted subscriber.py pid $!" >> "$LOG"
