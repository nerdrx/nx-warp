#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 ]]; then echo "usage: ASTC_SOURCE=/path/to/astc/Source ASTC_LIBRARY=/path/to/libastcenc.a $0 eye0.rgba eye0.raw.astc eye1.rgba eye1.raw.astc" >&2; exit 2; fi
: "${ASTC_SOURCE:?set ASTC_SOURCE to astc-encoder/Source}"
: "${ASTC_LIBRARY:?set ASTC_LIBRARY to libastcenc-avx2-static.a}"
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
CXX=${CXX:-g++}
common=(-std=c++17 -DASTCENC_AVX=2 -DASTCENC_F16C=1 -DASTCENC_NEON=0 -DASTCENC_POPCNT=1 -DASTCENC_SSE=41 -DASTCENC_SVE=0 -DASTCENC_X86_GATHERS=1 -mavx2 -mpopcnt -mf16c -ffp-contract=off -I"$ASTC_SOURCE" "$here/gate.cpp" "$ASTC_LIBRARY" -lzstd -pthread)
{
  date -Is
  uname -a
  "$CXX" --version | head -n 1
  pkg-config --modversion libzstd
  sha256sum "$ASTC_LIBRARY"
  sha256sum "$here/gate.cpp"
  sha256sum "$1" "$2" "$3" "$4"
} > "$here/context.txt" 2>&1
capture() {
  local label=$1 output=$2 error=$3; shift 3
  local rc=0
  "$@" >"$here/$output" 2>"$here/$error" || rc=$?
  printf '%s\n' "$rc" >"$here/$label.exit"
  if (( rc != 0 )); then cat "$here/$error" >&2; return "$rc"; fi
}
capture build-normal build-normal.log build-normal.stderr "$CXX" -O3 -Wall -Wextra "${common[@]}" -o "$here/gate"
capture normal raw.csv run.log "$here/gate" "$1" "$2" "$3" "$4"
capture build-san build-san.log build-san.stderr "$CXX" -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined "${common[@]}" -o "$here/gate_san"
capture san san.csv san.log env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$here/gate_san" "$1" "$2" "$3" "$4"
cmp "$here/raw.csv" "$here/san.csv"
