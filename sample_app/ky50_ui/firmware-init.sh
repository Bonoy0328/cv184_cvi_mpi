#!/bin/sh
# S10auto_mount mounts the data partitions, S99user loads the media drivers.
case "$1" in
    start)
        # Keep startup diagnostics bounded on the RAM-backed /tmp.
        /system/ky50-ui/run.sh </dev/null 2>&1 | {
            head -c 65536 >/tmp/ky50-ui-boot.log
            cat >/dev/null
        } &
        ;;
    stop)
        /system/ky50-ui/stop.sh
        ;;
esac
