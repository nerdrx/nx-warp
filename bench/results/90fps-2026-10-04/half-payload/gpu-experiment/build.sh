#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
LZ4_DIR="${LZ4_DIR:-$ROOT/../third_party/lz4}"
GLSLANG_VALIDATOR="${GLSLANG_VALIDATOR:-glslangValidator}"
SPIRV_VAL="${SPIRV_VAL:-spirv-val}"
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DLZ4_DIR="$LZ4_DIR"
cmake --build "$BUILD_DIR" --parallel
mkdir -p "$BUILD_DIR"
"$GLSLANG_VALIDATOR" -V --target-env vulkan1.1 "$ROOT/shaders/encode.comp" -o "$BUILD_DIR/encode.spv"
"$SPIRV_VAL" --target-env vulkan1.1 "$BUILD_DIR/encode.spv"
sha256sum "$BUILD_DIR/encode.spv" "$BUILD_DIR/astc-gpu"
