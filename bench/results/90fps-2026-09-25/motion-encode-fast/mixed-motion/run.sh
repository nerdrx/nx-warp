#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: WIVRN_ROOT=/path/to/wivrn $0 PHOTO_A.nxdf PHOTO_B.nxdf [results.csv]" >&2
    exit 2
fi
: "${WIVRN_ROOT:?set WIVRN_ROOT to a checkout containing common/nxwarp_direct.h}"

here="$(cd -- "$(dirname -- "$0")" && pwd)"
output="${3:-$here/results.csv}"
"${CXX:-g++}" -std=c++20 -O3 -Wall -Wextra -I "$WIVRN_ROOT/common" \
    "$here/mixed_motion_bench.cpp" -lzstd -llz4 -o "$here/mixed_motion_bench"
"$here/mixed_motion_bench" "$1" "$2" > "$output"
