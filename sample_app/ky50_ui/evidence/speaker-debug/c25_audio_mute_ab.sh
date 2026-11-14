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
    /tmp/ky50-dac-left-mute 0
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
exec 9>/dev/ky50-amp
echo CAPTURE_START
cat /proc/uptime
/tmp/sample_audio 4 --list -r 16000 -R 16000 -c 2 -p 320 -C 0 -V 0 \
    -F /tmp/ky50-c25-mute-ab.raw -T 50 >/tmp/ky50-c25-mute-ab-capture.log 2>&1 &
recorder=$!
sleep 2
/tmp/sample_audio 5 --list -r 16000 -R 16000 -c 2 -p 320 -C 0 -V 0 \
    -F /tmp/ky50-tone400hz-600s.raw -T 600 >/tmp/ky50-c25-mute-ab-playback.log 2>&1 &
player=$!
(trap - EXIT INT TERM HUP; sleep 65; kill -INT "$player" "$recorder" 2>/dev/null;
 sleep 5; kill -KILL "$player" "$recorder" 2>/dev/null) 9>&- &
guard=$!
sleep 3
/tmp/ky50-dac-left-mute 0
stage A_ANALOG_INPUT
echo high >"$node/direction"
devmem 0x030011c8 32 "$(((saved_l & ~7) | 3))"
stage B_GPIO_HIGH_UNMUTED
/tmp/ky50-dac-left-mute 1
stage C_GPIO_HIGH_LEFT_MUTED
/tmp/ky50-dac-left-mute 0
stage B2_GPIO_HIGH_UNMUTED
/tmp/ky50-dac-left-mute 1
stage C2_GPIO_HIGH_LEFT_MUTED
echo in >"$node/direction"
devmem 0x030011c8 32 "$((saved_l & ~7))"
/tmp/ky50-dac-left-mute 0
stage A2_ANALOG_INPUT
kill -INT "$player"
set +e
wait "$player"; play_result=$?; player=''
wait "$recorder"; capture_result=$?; recorder=''
set -e
echo "PLAYER_EXIT=$play_result CAPTURE_EXIT=$capture_result"
test "$play_result" -eq 0
test "$capture_result" -eq 0
