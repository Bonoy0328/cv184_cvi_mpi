#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
make TARGET=host build/host/vendor/sqlite/sqlite3.o
mkdir -p build/tests
${CC:-gcc} -I. -Ipva/include -Ivendor/sqlite -std=gnu11 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer tools/async_biometric_test.c \
    access.c access_store.c biometric_worker.c ir_exposure.c app_config.c \
    build/host/vendor/sqlite/sqlite3.o -lm -lpthread -o build/tests/async_biometric_test
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} UBSAN_OPTIONS=halt_on_error=1 build/tests/async_biometric_test
