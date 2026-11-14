#!/bin/sh
# Temporary hearing test only, with the LCD physically disconnected.
# Hold the DAC and amplifier enabled, ten seconds silent then repeated voice.
# This does not repair simultaneous LCD/reset and Speaker operation.
set -eu
test "${1:-}" = --lcd-disconnected
cd /tmp/ky50-voice-test
test -c /dev/ky50-amp
test -x ./voice_hold
test -s assets/recognition_success.s16le
base=''
for chip in /sys/class/gpio/gpiochip*; do
    if [ "$(cat "$chip/label")" = '3022000.gpio' ]; then base=$(cat "$chip/base"); break; fi
done
test -n "$base"
gpio=$((base + 25)); node=/sys/class/gpio/gpio$gpio
test ! -d "$node"
saved_mux=$(devmem 0x030011c8 32)
saved_dir=$(devmem 0x03022004 32)
saved_data=$(devmem 0x03022000 32)
exported=0
cleanup() {
    trap - EXIT
    set +e
    if [ "$exported" -eq 1 ]; then
        if [ "$((saved_dir & (1 << 25)))" -ne 0 ]; then
            if [ "$((saved_data & (1 << 25)))" -ne 0 ]; then echo high >"$node/direction";
            else echo low >"$node/direction"; fi
        else echo in >"$node/direction"; fi
        devmem 0x030011c8 32 "$saved_mux"
        echo "$gpio" >/sys/class/gpio/unexport
    fi
    echo VOICE_TEST_RESTORED
    cat /proc/uptime
    devmem 0x03022004 32
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "$gpio" >/sys/class/gpio/export
exported=1
echo in >"$node/direction"
devmem 0x030011c8 32 "$((saved_mux & ~7))"
echo VOICE_TEST_START
cat /proc/uptime
echo "C25 input, analog mux: $(devmem 0x030011c8 32)"
./voice_hold
