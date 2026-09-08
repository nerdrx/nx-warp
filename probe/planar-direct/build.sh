#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${PLANAR_BUILD:-$HERE/build}
HOST_VULKAN_INCLUDE=${HOST_VULKAN_INCLUDE:-/usr/include}
HOST_LIB=${PLANAR_HOST_LIB:-$ROOT/build-vkdec/vk/decoder/libnxvc_vk_decoder.a}
mkdir -p "$OUT/host" "$OUT/android"
for stage in vert frag; do
 glslc -O "$HERE/planar.$stage" -o "$OUT/host/planar.$stage.spv"
 cp "$OUT/host/planar.$stage.spv" "$OUT/android/planar.$stage.spv"
done
for stage in vert frag; do
 glslc -O "$HERE/tile.$stage" -o "$OUT/host/tile.$stage.spv"
 cp "$OUT/host/tile.$stage.spv" "$OUT/android/tile.$stage.spv"
done
glslc -O "$HERE/compact.frag" -o "$OUT/host/compact.frag.spv"
cp "$OUT/host/compact.frag.spv" "$OUT/android/compact.frag.spv"
glslc -O "$HERE/flat.frag" -o "$OUT/host/flat.frag.spv"
cp "$OUT/host/flat.frag.spv" "$OUT/android/flat.frag.spv"
glslc -O -DPLANAR_OUTPUT_YUV=1 "$HERE/planar.frag" -o "$OUT/host/planar-yuv.frag.spv"
cp "$OUT/host/planar-yuv.frag.spv" "$OUT/android/planar-yuv.frag.spv"
c++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -I"$HOST_VULKAN_INCLUDE" -I"$ROOT/include" -I"$ROOT/vk/decoder" "$HERE/main.cpp" "$HOST_LIB" -lvulkan -lpthread -ldl -o "$OUT/host/nx-planar-direct"
if [[ ${1:-} == --host-only ]]; then exit 0; fi
: "${ANDROID_NDK:?set ANDROID_NDK}"
: "${PLANAR_ANDROID_LIB:?set PLANAR_ANDROID_LIB}"
"$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -I"$ROOT/include" -I"$ROOT/vk/decoder" "$HERE/main.cpp" "$PLANAR_ANDROID_LIB" -lvulkan -llog -static-libstdc++ -o "$OUT/android/nx-planar-direct"
