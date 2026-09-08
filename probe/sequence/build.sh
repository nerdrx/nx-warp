#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${NX_SEQUENCE_BUILD:-$HERE/build}
HOST_DECODER_BUILD=${NX_SEQUENCE_HOST_DECODER_BUILD:-$ROOT/build-vkdec}
ANDROID_DECODER_BUILD=${NX_SEQUENCE_ANDROID_DECODER_BUILD:-}
HOST_VULKAN_INCLUDE=${NX_SEQUENCE_HOST_VULKAN_INCLUDE:-}
NDK=${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
GLSLC=${GLSLC:-$(command -v glslc || true)}
HOST_CXX=${CXX:-c++}
RENDERER=${NX_SEQUENCE_RENDERER:-$HERE/renderer.cpp}
VERT=${NX_SEQUENCE_VERT:-$HERE/atlas.glsl}
FRAG=${NX_SEQUENCE_FRAG:-$HERE/atlas.glsl}
HOST_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --host-only) HOST_ONLY=1 ;;
        *) echo "sequence: unknown option $arg (use --host-only)" >&2; exit 2 ;;
    esac
done

if [[ -z "$HOST_VULKAN_INCLUDE" ]]; then
    for candidate in "${VULKAN_SDK:-}/include" /usr/include /usr/local/include; do
        if [[ -f "$candidate/vulkan/vulkan.h" ]]; then
            HOST_VULKAN_INCLUDE=$candidate
            break
        fi
    done
fi
[[ -n "$HOST_VULKAN_INCLUDE" && -f "$HOST_VULKAN_INCLUDE/vulkan/vulkan.h" ]] || {
    echo "sequence: Vulkan headers not found; set NX_SEQUENCE_HOST_VULKAN_INCLUDE" >&2
    exit 2
}

[[ -n "$GLSLC" && -x "$GLSLC" ]] || { echo "sequence: glslc not found" >&2; exit 2; }
[[ -f "$RENDERER" && -f "$VERT" && -f "$FRAG" ]] || {
    echo "sequence: expected renderer.cpp and atlas.glsl" >&2
    exit 2
}
[[ -f "$HOST_DECODER_BUILD/vk/decoder/libnxvc_vk_decoder.a" ]] || {
    echo "sequence: host decoder library not found: $HOST_DECODER_BUILD" >&2
    exit 2
}
if [[ "$HOST_ONLY" == 0 ]]; then
    [[ -n "$NDK" && -x "$TC/bin/aarch64-linux-android29-clang++" ]] || {
        echo "sequence: Android NDK not found; set ANDROID_NDK or use --host-only" >&2
        exit 2
    }
    [[ -n "$ANDROID_DECODER_BUILD" && -f "$ANDROID_DECODER_BUILD/vk/decoder/libnxvc_vk_decoder.a" ]] || {
        echo "sequence: set NX_SEQUENCE_ANDROID_DECODER_BUILD or use --host-only" >&2
        exit 2
    }
fi

mkdir -p "$OUT/host" "$OUT/android"

echo "== shaders (copied native vertex/fragment source)"
"$GLSLC" -fshader-stage=vert -DVERT_SHADER=1 "$VERT" -o "$OUT/host/atlas.vert.spv"
"$GLSLC" -fshader-stage=frag -DFRAG_SHADER=1 "$FRAG" -o "$OUT/host/atlas.frag.spv"
cp "$OUT/host/atlas.vert.spv" "$OUT/android/atlas.vert.spv"
cp "$OUT/host/atlas.frag.spv" "$OUT/android/atlas.frag.spv"

echo "== host"
"$HOST_CXX" -std=c++20 -O2 -Wall -Wextra \
    -I"$ROOT/include" -I"$HERE" -I"$HOST_VULKAN_INCLUDE" \
    -c "$HERE/sequence.cpp" -o "$OUT/host/sequence.o"
"$HOST_CXX" -std=c++20 -O2 -Wall -Wextra \
    -I"$ROOT/include" -I"$HERE" -I"$HOST_VULKAN_INCLUDE" \
    -c "$RENDERER" -o "$OUT/host/renderer.o"
"$HOST_CXX" "$OUT/host/sequence.o" "$OUT/host/renderer.o" \
    "$HOST_DECODER_BUILD/vk/decoder/libnxvc_vk_decoder.a" \
    -lvulkan -ldl -lpthread -o "$OUT/host/nx-sequence-bench"

if [[ "$HOST_ONLY" == 1 ]]; then
    file "$OUT/host/nx-sequence-bench"
    exit 0
fi

echo "== Android arm64"
"$TC/bin/aarch64-linux-android29-clang++" -std=c++20 -O2 -Wall -Wextra \
    -I"$ROOT/include" -I"$HERE" -I"$TC/sysroot/usr/include" \
    -c "$HERE/sequence.cpp" -o "$OUT/android/sequence.o"
"$TC/bin/aarch64-linux-android29-clang++" -std=c++20 -O2 -Wall -Wextra \
    -I"$ROOT/include" -I"$HERE" -I"$TC/sysroot/usr/include" \
    -c "$RENDERER" -o "$OUT/android/renderer.o"
"$TC/bin/aarch64-linux-android29-clang++" \
    "$OUT/android/sequence.o" "$OUT/android/renderer.o" \
    "$ANDROID_DECODER_BUILD/vk/decoder/libnxvc_vk_decoder.a" \
    -lvulkan -llog -static-libstdc++ -o "$OUT/android/nx-sequence-bench"

file "$OUT/host/nx-sequence-bench" "$OUT/android/nx-sequence-bench"
