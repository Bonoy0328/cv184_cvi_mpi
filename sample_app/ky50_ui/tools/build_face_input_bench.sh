#!/usr/bin/env bash
set -euo pipefail
app=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=$(cd -- "$app/../../.." && pwd)
pva="$sdk/libpva_face"
cross="$sdk/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf/bin/arm-none-linux-musleabihf-"
cd "$app"
flags=(-std=c++11 -O3 -g1 -mcpu=cortex-a53 -mfpu=neon -ffunction-sections -fdata-sections
 -DPVA_PLATFORM_CV184X -I. -Ipva/include -I"$pva/src/facerec" -I"$pva/src/core" -I"$pva/platform"
 -I"$pva/third_party/nlohmann" -I"$pva/build/cv184x-deps/install/include" -I"$sdk/cvi_mpi/include")
sources=("$pva/src/facerec/image.cpp" "$pva/src/facerec/faceRec.cpp" "$pva/platform/host/platform_host.cpp")
for method in 0 1 2; do
 "${cross}g++" "${flags[@]}" -DPVA_IMAGE_RESIZE_METHOD="$method" "$pva/tests/testNv12Alignment.cpp" "${sources[@]}" \
 -Wl,--gc-sections -static-libstdc++ -static-libgcc -pthread -o "build/face-input/nv12-align-$method"
done
"${cross}g++" "${flags[@]}" -DPVA_IMAGE_RESIZE_METHOD=1 tools/face_input_bench.cpp "${sources[@]}" build/board/scale.o \
 -Wl,--gc-sections -static-libstdc++ -static-libgcc -Wl,-Bstatic -L"$sdk/cvi_mpi/lib" \
 -Wl,--start-group -lsys -lvpss -lgdc -lvi -Wl,--end-group -Wl,-Bdynamic -pthread -ldl -lm -o build/face-input/face-input-bench
"${cross}g++" "${flags[@]}" -DPVA_IMAGE_RESIZE_METHOD=1 "$pva/tests/testCv184Preprocess.cpp" "$pva/src/facerec/image.cpp" \
 -Wl,--gc-sections -static-libstdc++ -static-libgcc -o build/face-input/nv12-color-test
