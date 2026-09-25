#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "usage: WIVRN_ROOT=/path/to/wivrn $0 FOREST.nxdf DARK.nxdf" >&2
  exit 2
fi
here="$(cd -- "$(dirname -- "$0")" && pwd)"
: "${WIVRN_ROOT:?set WIVRN_ROOT to a WiVRn NX checkout containing common/nxwarp_direct.h}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CXX:-g++}" -std=c++20 -O3 -Wall -Wextra -I "$WIVRN_ROOT/common" \
  "$here/residual_codec_bench.cpp" -lzstd -llz4 -o "$tmp/residual_codec_bench"
"$tmp/residual_codec_bench" "$1" "$2"
