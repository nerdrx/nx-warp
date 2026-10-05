#!/usr/bin/env bash
set -euo pipefail
source_dir=$(realpath "${1:?Usage: run.sh /path/to/source [output-directory]}")
report_dir=$(cd -- "$(dirname -- "$0")" && pwd)
output_dir=${2:-$(mktemp -d)}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")
cd "$source_dir"
includes=(-I common -I server/encoder -I build-server/common -I external -I build-server/_deps/boost-src/libs/pfr/include)
for mode in normal san tsan; do
 flags=(-O2)
 [[ $mode != san ]] || flags=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
 [[ $mode != tsan ]] || flags=(-O1 -g -fsanitize=thread)
 g++ -std=c++23 -pthread "${flags[@]}" "${includes[@]}" "$report_dir/endpoint_concurrency.cpp" common/smp.cpp -lcrypto -o "$output_dir/$mode" >"$output_dir/$mode-build.log" 2>&1
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1 timeout 45s "$output_dir/$mode" >"$output_dir/$mode-run.log" 2>&1
 cat "$output_dir/$mode-run.log"
done
printf 'Captured checks passed; output: %s\n' "$output_dir"
