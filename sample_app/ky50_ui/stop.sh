#!/bin/sh
set -eu
pid=$(cat /run/ky50-ui.pid 2>/dev/null || true)
case "$pid" in ''|*[!0-9]*) echo 'ky50_ui is not running';exit 0;; esac
if [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" != /tmp/ky50-ui/ky50_ui ]; then
    echo 'Stale PID file; no signal sent';exit 0
fi
kill -TERM "$pid"
i=0
while [ "$i" -lt 10 ]; do
    if [ "$(readlink "/proc/$pid/exe" 2>/dev/null || true)" != /tmp/ky50-ui/ky50_ui ]; then
        rm -f /run/ky50-ui.pid;exit 0
    fi
    sleep 1;i=$((i+1))
done
echo 'Normal exit timed out; inspect logs. No force kill or VO unload.' >&2
exit 1
