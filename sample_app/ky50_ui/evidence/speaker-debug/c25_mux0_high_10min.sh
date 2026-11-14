#!/bin/sh
# Test mux0 plus GPIOC25 high-output: measure actual LCD reset voltage. LCD unplugged.
# Needs the matching ky50_amp module, SDK sample_audio, and 600-second PCM.
# Never install this script as a boot service.
set -eu
test "${1:-}" = '--lcd-disconnected' || {
    echo 'Requires --lcd-disconnected and a physically disconnected LCD' >&2
    exit 1
}
test -c /dev/ky50-amp
test -x /tmp/sample_audio
test -f /tmp/ky50-tone400hz-600s.raw
test "$(wc -c </tmp/ky50-tone400hz-600s.raw)" -eq 38400000
gpio_base=''
for chip in /sys/class/gpio/gpiochip*; do
    if [ "$(cat "$chip/label")" = '3022000.gpio' ]; then
        gpio_base=$(cat "$chip/base")
        break
    fi
done
test -n "$gpio_base"
gpio=$((gpio_base + 25))
node=/sys/class/gpio/gpio$gpio
test ! -d "$node" || { echo 'C25 is already exported; refusing' >&2; exit 1; }
saved_mux=$(devmem 0x030011c8 32)
saved_direction=$(devmem 0x03022004 32)
saved_data=$(devmem 0x03022000 32)
exported=0
player=''
guard=''
cleanup() {
    trap - EXIT
    set +e
    if [ -n "$player" ]; then
        kill -INT "$player" 2>/dev/null
        wait "$player" 2>/dev/null
    fi
    if [ -n "$guard" ]; then kill "$guard" 2>/dev/null; fi
    exec 9>&-
    if [ "$exported" -eq 1 ]; then
        if [ "$((saved_direction & (1 << 25)))" -ne 0 ]; then
            if [ "$((saved_data & (1 << 25)))" -ne 0 ]; then
                echo high >"$node/direction"
            else
                echo low >"$node/direction"
            fi
        else
            echo in >"$node/direction"
        fi
        devmem 0x030011c8 32 "$saved_mux"
        echo "$gpio" >/sys/class/gpio/unexport
    fi
    echo C25_MUX0_HIGH_RESTORED
    cat /proc/uptime
    devmem 0x030011c8 32
    devmem 0x03022004 32
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "$gpio" >/sys/class/gpio/export
exported=1
devmem 0x030011c8 32 "$((saved_mux & ~7))"
echo high >"$node/direction"
echo "C25 saved_mux=$saved_mux saved_direction=$saved_direction saved_data=$saved_data"
exec 9>/dev/ky50-amp
echo C25_MUX0_HIGH_START
cat /proc/uptime
/tmp/sample_audio 5 --list -r 16000 -R 16000 -c 2 -p 320 -C 0 -V 0 \
    -F /tmp/ky50-tone400hz-600s.raw -T 600 &
player=$!
(trap - EXIT INT TERM HUP; sleep 610; kill -INT "$player" 2>/dev/null) 9>&- &
guard=$!
set +e
wait "$player"
result=$?
set -e
player=''
echo "C25_MUX0_HIGH_PLAYER_EXIT=$result"
exit "$result"
