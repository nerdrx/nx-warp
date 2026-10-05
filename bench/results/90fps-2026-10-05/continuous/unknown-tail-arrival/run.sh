#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 ]]; then echo "usage: $0 SOURCE_CHECKOUT OUTPUT_DIR" >&2; exit 2; fi
src=$(realpath "$1")
out=$(realpath -m "$2")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
rev=7b7ae3600951d90b07466053bbcda45f7dcc9b3a
[[ $(git -C "$src" rev-parse HEAD) == "$rev" ]] || { echo "wrong source revision" >&2; exit 3; }
for path in build-server/common build-server/_deps/boost-src/libs/pfr/include external; do
  [[ -e "$src/$path" ]] || { echo "missing dependency $path" >&2; exit 4; }
done
mkdir -p "$out"
common=(-std=c++23 -pthread -I "$src" -I "$src/common" -I "$src/client/decoder" -I "$src/server/encoder" -I "$src/build-server/common" -I "$src/external" -I "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${common[@]}" -O2 -o "$out/replay" "$script_dir/replay.cpp" "$src/common/smp.cpp" -lcrypto > "$out/build-normal.log" 2>&1
"$out/replay" > "$out/results.csv" 2> "$out/checks-normal.log"
g++ "${common[@]}" -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -o "$out/replay-sanitized" "$script_dir/replay.cpp" "$src/common/smp.cpp" -lcrypto > "$out/build-sanitizer.log" 2>&1
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/replay-sanitized" > "$out/results-sanitizer.csv" 2> "$out/checks-sanitizer.log"
printf 'normal: '; cat "$out/checks-normal.log"
printf 'sanitizer: '; cat "$out/checks-sanitizer.log"
