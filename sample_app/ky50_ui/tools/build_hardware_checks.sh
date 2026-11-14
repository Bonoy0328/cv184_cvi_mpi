#!/usr/bin/env bash
# Synthetic board checks only. Run the resulting VPSS tool after stopping UI.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
sdk=$(cd ../../.. && pwd)
cc="$sdk/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf/bin/arm-none-linux-musleabihf-gcc"
mkdir -p build/tests
flags=(-I. -Ipva/include -I"$sdk/cvi_mpi/include" -I"$sdk/osal/include"
    -O2 -mcpu=cortex-a53 -mfpu=neon-vfpv4 -Wall -Wextra -Werror -static)
"$cc" "${flags[@]}" tools/vpss_csc_test.c scale.c pixels.c hand_overlay.c \
    -L"$sdk/cvi_mpi/lib" -Wl,--start-group -lvpss -lsys -lgdc -Wl,--end-group \
    -lpthread -lm -o build/tests/vpss_csc_test
"$cc" "${flags[@]}" tools/hand_overlay_test.c hand_overlay.c -lm -o build/tests/hand_overlay_test_arm
gcc -I. -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    tools/relay_control_test.c -o build/tests/relay_control_test
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} build/tests/relay_control_test
