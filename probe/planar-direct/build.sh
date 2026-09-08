#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${PLANAR_BUILD:-$HERE/build}
HOST_VULKAN_INCLUDE=${HOST_VULKAN_INCLUDE:-/usr/include}
HOST_LIB=${PLANAR_HOST_LIB:-$ROOT/build-vkdec/vk/decoder/libnxvc_vk_decoder.a}
mkdir -p "$OUT/host" "$OUT/android"
compile_shader() {
 local src=$1 out=$2 stage=$3 defs=${4:-}
 cmake -DGLSLC="${GLSLC:-$(command -v glslc)}" -DSRC="$src" -DOUT="$out" \
   -DNAME=probe -DDEFS="$defs" -DSTYLE=raw -DSTAGE="$stage" -DTARGET_ENV=vulkan1.1 \
   -P "$ROOT/vk/common/cmake/nxvc_gen_spv.cmake"
}
for stage in vert frag; do
 compile_shader "$HERE/planar.$stage" "$OUT/host/planar.$stage.spv" "$([ "$stage" = vert ] && echo vertex || echo fragment)"
 cp "$OUT/host/planar.$stage.spv" "$OUT/android/planar.$stage.spv"
done
for stage in vert frag; do
 compile_shader "$HERE/tile.$stage" "$OUT/host/tile.$stage.spv" "$([ "$stage" = vert ] && echo vertex || echo fragment)"
 cp "$OUT/host/tile.$stage.spv" "$OUT/android/tile.$stage.spv"
done

compile_shader "$HERE/compact.frag" "$OUT/host/compact.frag.spv" fragment
cp "$OUT/host/compact.frag.spv" "$OUT/android/compact.frag.spv"
compile_shader "$HERE/flat.frag" "$OUT/host/flat.frag.spv" fragment
cp "$OUT/host/flat.frag.spv" "$OUT/android/flat.frag.spv"
compile_shader "$HERE/planar.frag" "$OUT/host/planar-yuv.frag.spv" fragment -DPLANAR_OUTPUT_YUV=1
cp "$OUT/host/planar-yuv.frag.spv" "$OUT/android/planar-yuv.frag.spv"
c++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -I"$HOST_VULKAN_INCLUDE" -I"$ROOT/include" -I"$ROOT/vk/decoder" "$HERE/main.cpp" "$HOST_LIB" -lvulkan -lpthread -ldl -o "$OUT/host/nx-planar-direct"
if [[ ${1:-} == --host-only ]]; then exit 0; fi
: "${ANDROID_NDK:?set ANDROID_NDK}"
: "${PLANAR_ANDROID_LIB:?set PLANAR_ANDROID_LIB}"
"$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -I"$ROOT/include" -I"$ROOT/vk/decoder" "$HERE/main.cpp" "$PLANAR_ANDROID_LIB" -lvulkan -llog -static-libstdc++ -o "$OUT/android/nx-planar-direct"
