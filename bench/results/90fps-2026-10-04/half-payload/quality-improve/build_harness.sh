#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
: "${LZ4_DIR:?Set LZ4_DIR to a directory containing lz4.c and lz4.h}"
cmake -S "$ROOT/harness" -B "$ROOT/harness/build" -DCMAKE_BUILD_TYPE=Release -DLZ4_DIR="$LZ4_DIR"
cmake --build "$ROOT/harness/build" --parallel
glslangValidator -V --target-env vulkan1.1 "$ROOT/harness/shaders/encode.comp" -o "$ROOT/harness/build/encode.spv"
spirv-val --target-env vulkan1.1 "$ROOT/harness/build/encode.spv"
