#!/bin/sh
n=0
while [ "$n" -lt 300 ] && [ -d /proc/263 ]; do
    if grep -q 'ROTATED group=0.*864x648' /tmp/ky50-dvp-board1-return/ui.log; then
        sleep 3
        kill -USR1 263
        sleep 1
        cp /tmp/ky50-ui/screen.ppm /tmp/ky50-dvp-board1-return/dvp-entered.ppm
        cat /proc/soph/vi > /tmp/ky50-dvp-board1-return/dvp-live-vi.txt
        cat /proc/soph/vi_dbg > /tmp/ky50-dvp-board1-return/dvp-live-vi-dbg.txt
        cat /proc/soph/vpss > /tmp/ky50-dvp-board1-return/dvp-live-vpss.txt
        cat /proc/soph/vo_disp > /tmp/ky50-dvp-board1-return/dvp-live-vo-disp.txt
        cat /mnt/cfg/ky50-ui/display-source > /tmp/ky50-dvp-board1-return/selected-source.txt
        uptime
        exit 0
    fi
    sleep 1
    n=$((n+1))
done
exit 1
