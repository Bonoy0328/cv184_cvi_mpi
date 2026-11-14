#!/bin/sh
# Run locally on the board; never writes to NFS during cable-unplug tests.
set -eu
pid=${1:-$(cat /run/ky50-ui.pid)}
duration=${2:-1800}
case "$pid:$duration" in *[!0-9:]*|:*) echo 'Usage: soak.sh PID SECONDS' >&2;exit 2;; esac
start=$(cut -d. -f1 /proc/uptime)
while :; do
    now=$(cut -d. -f1 /proc/uptime)
    elapsed=$((now-start))
    if [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" != /tmp/ky50-ui/ky50_ui ]; then
        echo "FAIL process exited at elapsed=$elapsed";exit 1
    fi
    printf 'SAMPLE elapsed=%s pid=%s utc=' "$elapsed" "$pid"
    date -u '+%Y-%m-%dT%H:%M:%SZ'
    awk '/VmRSS:|VmHWM:|VmSize:|Threads:/' "/proc/$pid/status"
    awk '/err_fwr_yuv|bw fail/' /proc/soph/vo_disp
    tail -1 /tmp/ky50-ui.log
    if [ "$elapsed" -ge "$duration" ]; then echo "PASS process alive for ${elapsed}s";break;fi
    sleep 30
done
