#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 4 ]]; then printf 'Usage: %s eye0.rgba eye0.raw.astc eye1.rgba eye1.raw.astc\n' "$0" >&2; exit 2; fi
ASTC_GATE_SOURCE=${ASTC_GATE_SOURCE:-/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-hybrid-client-build/_deps/libktx-src/external/astc-encoder/Source}
ASTC_GATE_LIBRARY=${ASTC_GATE_LIBRARY:-/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-hybrid-client-build/_deps/libktx-build/external/astc-encoder/Source/libastcenc-avx2-static.a}
ASTC_GATE_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ASTC_GATE_TEMP=$(mktemp -d)
trap 'rm -rf -- "$ASTC_GATE_TEMP"' EXIT
g++ -O3 -std=c++17 -Wall -Wextra -DASTCENC_AVX=2 -DASTCENC_F16C=1 -DASTCENC_NEON=0 -DASTCENC_POPCNT=1 -DASTCENC_SSE=41 -DASTCENC_SVE=0 -DASTCENC_X86_GATHERS=1 -mavx2 -mpopcnt -mf16c -ffp-contract=off -I"$ASTC_GATE_SOURCE" "$ASTC_GATE_DIR/gate.cpp" "$ASTC_GATE_LIBRARY" -lzstd -pthread -o "$ASTC_GATE_TEMP/gate"
"$ASTC_GATE_TEMP/gate" "$@"
