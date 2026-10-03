#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK=/run/media/nerdrx/Lex/claude/tools/android-sdk
NDK="${ASTC_PICO_NDK:-$SDK/ndk/29.0.14206865}"
CMAKE="${ASTC_PICO_CMAKE:-/run/media/nerdrx/Lex/claude/tools/cmake-3.31.5-linux-x86_64/bin/cmake}"
HEADERS="${ASTC_PICO_VULKAN_HEADERS:-/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe/build/android-arm64/Vulkan-Headers-vulkan-sdk-1.3.268.0}"
GLSLANG="${ASTC_PICO_GLSLANG:-/usr/bin/glslangValidator}"
SPIRV_VAL="${ASTC_PICO_SPIRV_VAL:-$(command -v spirv-val || true)}"
LZ4_DIR="${ASTC_PICO_LZ4_DIR:-$ROOT/third_party/lz4}"
[[ -x "$GLSLANG" && -f "$NDK/build/cmake/android.toolchain.cmake" && -d "$HEADERS/include" ]] || { echo 'missing shader compiler, NDK, or Vulkan headers' >&2; exit 1; }
[[ -n "$SPIRV_VAL" ]] || { echo 'spirv-val not found; set SPIRV_VAL' >&2; exit 1; }
mkdir -p "$ROOT/build/android-arm64/generated"
"$GLSLANG" -V --target-env vulkan1.1 "$ROOT/shaders/sample.comp" -o "$ROOT/build/sample.spv"
"$SPIRV_VAL" --target-env vulkan1.1 "$ROOT/build/sample.spv"
python3 - "$ROOT/build/sample.spv" "$ROOT/build/android-arm64/generated/sample_spv.h" <<'PY'
import pathlib, struct, sys
b = pathlib.Path(sys.argv[1]).read_bytes()
words = struct.unpack('<%dI' % (len(b) // 4), b)
pathlib.Path(sys.argv[2]).write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t sample_spv[] = {' + ','.join(f'0x{x:08x}' for x in words) + '};\n')
PY
"$CMAKE" -S "$ROOT" -B "$ROOT/build/android-arm64" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DVULKAN_HEADERS="$HEADERS" -DLZ4_DIR="$LZ4_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG'
"$CMAKE" --build "$ROOT/build/android-arm64" --parallel "$(nproc)"
cp "$ROOT/build/android-arm64/astc-pico" "$ROOT/build/astc-pico"
find "$ROOT/src" "$ROOT/shaders" "$LZ4_DIR" "$ROOT/fixtures" "$ROOT/CMakeLists.txt" "$ROOT/README.md" "$ROOT/build.sh" "$ROOT/build/sample.spv" "$ROOT/build/android-arm64/generated/sample_spv.h" "$ROOT/build/astc-pico" -type f -print0 | sort -z | xargs -0 sha256sum > "$ROOT/build/SHA256SUMS"
echo "Built $ROOT/build/astc-pico"
