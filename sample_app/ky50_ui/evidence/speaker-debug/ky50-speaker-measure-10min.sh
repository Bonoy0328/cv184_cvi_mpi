#!/bin/sh
exec 9>/dev/ky50-amp || exit 1
echo TONE400_START
cat /proc/uptime
/tmp/sample_audio 5 --list -r 16000 -R 16000 -c 2 -p 320 -C 0 -V 0 -F /tmp/ky50-tone400hz-600s.raw -T 600 &
player=$!
(sleep 610; kill -INT "$player" 2>/dev/null) 9>&- &
guard=$!
wait "$player"
result=$?
kill "$guard" 2>/dev/null
exec 9>&-
echo TONE400_END_rc=$result
cat /proc/uptime
