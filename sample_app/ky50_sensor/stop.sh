#!/bin/sh
set -eu
cd "$(dirname "$0")"
[ -f capture.pid ] || exit 0
pid=$(cat capture.pid)
case "$pid" in ''|*[!0-9]*) echo "Invalid capture.pid" >&2; exit 1 ;; esac
expected="$(pwd -P)/ky50_sensor_capture"
exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
if [ "$exe" != "$expected" ]; then
    echo "Recorded sensor process is no longer running."
    exit 0
fi
kill -TERM "$pid"
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    sleep 1
    if [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" != "$expected" ]; then
        echo "Sensor test stopped."
        exit 0
    fi
done
echo "Sensor process did not exit; preserve logs before recovery." >&2
exit 1
