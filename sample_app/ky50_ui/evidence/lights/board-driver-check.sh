#!/bin/sh
# Run before ky50_ui opens the device. Requires the built ky50_lights module.
set -eu
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
mask=$(( (1 << 19) | (1 << 8) ))
base_data=$(devmem 0x03020000 32)
base_dir=$(devmem 0x03020004 32)
test "$(( $(devmem 0x03001064 32) & 7 ))" -eq 3
test "$(( $(devmem 0x03001020 32) & 7 ))" -eq 3
check()
{
    data=$(devmem 0x03020000 32)
    direction=$(devmem 0x03020004 32)
    input=$(devmem 0x03020050 32)
    printf '%s DR=0x%08x DDR=0x%08x EXT=0x%08x\n' "$1" "$data" "$direction" "$input"
    test "$((data & mask))" -eq "$2"
    test "$((direction & mask))" -eq "$mask"
    test "$((input & mask))" -eq "$2"
    test "$((data & ~mask))" -eq "$((base_data & ~mask))"
    test "$direction" = "$base_dir"
}
exec 3<>/dev/ky50-lights
trap 'exec 3>&-' EXIT HUP INT TERM
read -r state <&3
test "$state" = 00
check open-off 0
if (exec 4<>/dev/ky50-lights); then
    echo 'FAIL second device owner accepted' >&2
    exit 1
fi
printf w1 >&3
check white-on $((1 << 19))
printf i1 >&3
check both-on "$mask"
printf w0 >&3
check ir-only $((1 << 8))
printf i0 >&3
check both-off 0
printf i1 >&3
check ir-on $((1 << 8))
printf w1 >&3
check both-on-reversed "$mask"
if printf w2 >&3; then
    echo 'FAIL invalid command accepted' >&2
    exit 1
fi
check invalid-command-preserved "$mask"
exec 3>&-
check close-off 0
echo 'PASS GPIO mux/direction/output/input, independent switching, exclusive owner, invalid command, close-off, unrelated pins unchanged'
