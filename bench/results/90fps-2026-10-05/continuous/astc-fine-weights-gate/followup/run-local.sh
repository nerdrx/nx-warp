#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
ASTC_GATE_SOURCE=${ASTC_GATE_SOURCE:-/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-hybrid-client-build/_deps/libktx-src/external/astc-encoder/Source}
ASTC_GATE_LIBRARY=${ASTC_GATE_LIBRARY:-/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-hybrid-client-build/_deps/libktx-build/external/astc-encoder/Source/libastcenc-avx2-static.a}
ASTC_GATE_TEMP=$(mktemp -d)
trap 'rm -rf -- "$ASTC_GATE_TEMP"' EXIT
g++ -O2 -std=c++20 -DASTCENC_AVX=2 -DASTCENC_F16C=1 -DASTCENC_NEON=0 -DASTCENC_POPCNT=1 -DASTCENC_SSE=41 -DASTCENC_SVE=0 -DASTCENC_X86_GATHERS=1 -mavx2 -mpopcnt -mf16c -ffp-contract=off -I"$ASTC_GATE_SOURCE" motion_gate.cpp "$ASTC_GATE_LIBRARY" -lzstd -pthread -o "$ASTC_GATE_TEMP/gate"
(cd "$ASTC_GATE_TEMP" && ./gate > replay.csv)
diff -u raw.csv "$ASTC_GATE_TEMP/replay.csv"
cmp contact.ppm "$ASTC_GATE_TEMP/contact.ppm"
printf 'PASS: constants, translated patterns and aligned residuals reproduced.\n'
