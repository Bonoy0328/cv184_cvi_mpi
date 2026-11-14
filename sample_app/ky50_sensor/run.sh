#!/bin/sh
# Foreground NFS test; defaults match the physically verified dual display.
set -eu
cd "$(dirname "$0")"
base=$(pwd -P)
if [ -f capture.pid ]; then
    pid=$(cat capture.pid)
    case "$pid" in ''|*[!0-9]*) pid=0 ;; esac
    if [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" = "$base/ky50_sensor_capture" ]; then
        echo "Sensor test already running (PID $pid); use ./stop.sh first." >&2
        exit 1
    fi
fi
if [ -x /etc/init.d/S99z_ky50_lcd ]; then
    /etc/init.d/S99z_ky50_lcd stop
fi
# A separate NFS character test must release shared media buffers first.
for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    case "$exe" in
        */ky50_lcd/sample_ky50_lcd)
            echo "LCD character test still running (${proc##*/}); stop it before sensor display." >&2
            exit 1 ;;
    esac
done
printf '%s\n' "$$" > capture.pid
exec ./ky50_sensor_capture "${1:-both}" "${2:-0}" --display
