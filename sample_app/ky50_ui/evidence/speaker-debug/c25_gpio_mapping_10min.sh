#!/bin/sh
# Physical mapping check, not a production change. LCD must be unplugged.
# DAC remains OFF. Hold GPIOC25 high for 600s, then restore latch/dir/mux.
set -eu
test "${1:-}" = --lcd-disconnected
test "$(( $(devmem 0x0300a000 32) & 3 ))" -eq 0 || {
    echo 'DAC must be off before the GPIO mapping check' >&2; exit 1;
}
base=''
for chip in /sys/class/gpio/gpiochip*; do
    if [ "$(cat "$chip/label")" = '3022000.gpio' ]; then
        base=$(cat "$chip/base"); break
    fi
done
test -n "$base"
gpio=$((base + 25)); node=/sys/class/gpio/gpio$gpio
test ! -d "$node" || { echo 'C25 already exported; refusing' >&2; exit 1; }
saved_mux=$(devmem 0x030011c8 32)
saved_dir=$(devmem 0x03022004 32)
saved_data=$(devmem 0x03022000 32)
exported=0
cleanup() {
    trap - EXIT
    set +e
    if [ "$exported" -eq 1 ]; then
        if [ "$((saved_dir & (1 << 25)))" -ne 0 ]; then
            if [ "$((saved_data & (1 << 25)))" -ne 0 ]; then
                echo high >"$node/direction"
            else echo low >"$node/direction"; fi
        else echo in >"$node/direction"; fi
        devmem 0x030011c8 32 "$saved_mux"
        echo "$gpio" >/sys/class/gpio/unexport
    fi
    echo GPIO_MAPPING_RESTORED
    cat /proc/uptime
    devmem 0x030011c8 32
    devmem 0x03022000 32
    devmem 0x03022004 32
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "$gpio" >/sys/class/gpio/export
exported=1
devmem 0x030011c8 32 "$(((saved_mux & ~7) | 3))"
echo high >"$node/direction"
echo GPIO_MAPPING_C25_HIGH_DAC_OFF_START
cat /proc/uptime
devmem 0x030011c8 32
devmem 0x030011cc 32
devmem 0x03022000 32
devmem 0x03022004 32
devmem 0x0300a000 32
sleep 600
