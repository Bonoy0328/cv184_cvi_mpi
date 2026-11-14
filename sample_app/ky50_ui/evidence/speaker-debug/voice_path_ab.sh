#!/bin/sh
# Short software-recorded comparison; LCD must remain physically unplugged.
set -eu
test "${1:-}" = --lcd-disconnected
test -c /dev/ky50-amp
test -x /tmp/sample_audio
base=''
for chip in /sys/class/gpio/gpiochip*; do
    if [ "$(cat "$chip/label")" = '3022000.gpio' ]; then base=$(cat "$chip/base"); break; fi
done
test -n "$base"
gpio=$((base + 25)); node=/sys/class/gpio/gpio$gpio
test ! -d "$node"
saved_l=$(devmem 0x030011c8 32)
saved_rin=$(devmem 0x030011c4 32)
saved_dir=$(devmem 0x03022004 32)
saved_data=$(devmem 0x03022000 32)
test "$((saved_dir & (1 << 22)))" -eq 0
exported=0; player=''; recorder=''; guard=''
cleanup() {
    trap - EXIT
    set +e
    for pid in "$player" "$recorder"; do
        if [ -n "$pid" ]; then kill -INT "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; fi
    done
    if [ -n "$guard" ]; then kill "$guard" 2>/dev/null; fi
    exec 9>&-
    devmem 0x030011c4 32 "$saved_rin"
    if [ "$exported" -eq 1 ]; then
        if [ "$((saved_dir & (1 << 25)))" -ne 0 ]; then
            if [ "$((saved_data & (1 << 25)))" -ne 0 ]; then echo high >"$node/direction";
            else echo low >"$node/direction"; fi
        else echo in >"$node/direction"; fi
        devmem 0x030011c8 32 "$saved_l"
        echo "$gpio" >/sys/class/gpio/unexport
    fi
    echo AB_RESTORED
    cat /proc/uptime
    devmem 0x030011c8 32
    devmem 0x030011c4 32
    devmem 0x03022004 32
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
stage() {
    echo "STAGE=$1"
    cat /proc/uptime
    devmem 0x030011c8 32
    devmem 0x03022000 32
    devmem 0x03022004 32
    sleep 5
}
echo "$gpio" >/sys/class/gpio/export
exported=1
echo in >"$node/direction"
devmem 0x030011c8 32 "$((saved_l & ~7))"
devmem 0x030011c4 32 "$((saved_rin & ~7))"
cd /tmp/ky50-voice-test
echo CAPTURE_START
cat /proc/uptime
/tmp/sample_audio 4 --list -r 16000 -R 16000 -c 2 -p 320 -C 0 -V 0 -F /tmp/ky50-voice-path.raw -T 40 >/tmp/ky50-voice-path-capture.log 2>&1 &
recorder=$!
sleep 2
echo FACTORY16_START
cat /proc/uptime
./probe16
sleep 2
echo FACTORY32_START
cat /proc/uptime
./probe32
sleep 2
echo SDK_VOICE_START
cat /proc/uptime
exec 9>/dev/ky50-amp
/tmp/sample_audio 5 --list -r 16000 -R 16000 -c 2 -p 320 -C 0 -V 0 -F /tmp/ky50-voice-stereo.raw -T 5 >/tmp/ky50-voice-path-playback.log 2>&1 &
player=$!
sleep 7
kill -INT "$player" 2>/dev/null || true
sleep 1
exec 9>&-
wait
player=''; recorder=''
echo VOICE_PATH_DONE
