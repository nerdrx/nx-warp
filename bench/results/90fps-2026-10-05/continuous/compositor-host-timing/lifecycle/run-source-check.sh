#!/usr/bin/env bash
set -euo pipefail
repo=${1:?source checkout required}
out=${2:-source-check-results}
mkdir -p "$out"
rtk proxy python3 extract-check.py "$repo" "$out"
flags=(-std=c++23 -Wall -Wextra -Werror -pedantic -pthread)
rtk proxy g++ "${flags[@]}" "$out/source-guard-submit.cpp" -o "$out/source-guard-submit"
rtk proxy timeout 30 "$out/source-guard-submit" > "$out/normal.log" 2>&1
san=(-std=c++23 -Wall -Wextra -Werror -pedantic -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
rtk proxy g++ "${san[@]}" "$out/source-guard-submit.cpp" -o "$out/source-guard-submit-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/source-guard-submit-san" > "$out/san.log" 2>&1
sha256sum "$out/source-guard-submit.cpp" "$repo/server/compositor/compositor.cpp" "$repo/server/compositor/compositor.h" > "$out/hashes.txt"
cp "$out/provenance.txt" "$out/provenance-captured.txt"
