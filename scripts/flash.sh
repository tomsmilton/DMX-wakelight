#!/usr/bin/env bash
# Kill the serial tail, flash, restart the tail.
# Optional arg: env name (defaults to `wakelight`). Use `diagnostic` for
# the bus-diagnostic firmware with per-second TX stats.
set -e
cd "$(dirname "$0")/.."

ENV="${1:-wakelight}"

pkill -f serial_tail.py || true
sleep 0.3

pio run -e "$ENV" -t upload

nohup python3 scripts/serial_tail.py >/dev/null 2>&1 &
disown
echo "tail restarted, pid $!"
