#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 4 || $# -gt 5 ]]; then
    echo "usage: $0 SOURCE_CHECKOUT LEFT_HEADERLESS_ASTC RIGHT_HEADERLESS_ASTC NEW_OUTPUT_DIR [system|bundled]" >&2
    exit 2
fi
report_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(realpath -- "$1")
left=$(realpath -- "$2")
right=$(realpath -- "$3")
mkdir -- "$4" # Refuse to overwrite retained evidence.
output_dir=$(realpath -- "$4")
libraries=(-lzstd -llz4)
includes=()
case "${5:-system}" in
    system) ;;
    bundled)
        libraries=("$source_dir/build-server/_deps/nx_zstd-build/lib/libzstd.a" "$source_dir/build-server/common/libwivrn-lz4.a")
        includes=("-I$source_dir/build-server/_deps/nx_zstd-src/lib" "-I$source_dir/build-server/_deps/nx_lz4-src/lib")
        ;;
    *) echo "library mode must be system or bundled" >&2; exit 2 ;;
esac
g++ -O3 -DNDEBUG -std=c++20 -pthread -I"$source_dir/common" \
    "${includes[@]}" "$report_dir/production_like.cpp" "${libraries[@]}" -o "$output_dir/production_like"
"$output_dir/production_like" "$left" "$right" "$output_dir" \
    >"$output_dir/pc.csv" 2>"$output_dir/checks.log"
g++ -O2 -std=c++23 -pthread -I"$source_dir/common" "${includes[@]}" "$report_dir/decode.cpp" \
    "${libraries[@]}" -o "$output_dir/decode-host"
"$output_dir/decode-host" decode "$left" "$right" "$output_dir" >"$output_dir/host-decode.csv"
echo "Exact ASTC round trips passed. New results in $output_dir; these are host CPU component timings."
