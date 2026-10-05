#!/usr/bin/env bash
set -euo pipefail
repo=${1:?source checkout required}
out=${2:-results}
mkdir -p "$out"
rtk proxy python3 extract-check.py "$repo" "$out"
flags=(-std=c++23 -Wall -Wextra -Werror -pedantic)
rtk proxy g++ "${flags[@]}" "$out/host-stage-source.cpp" -o "$out/host-stage-source"
rtk proxy timeout 30 "$out/host-stage-source" > "$out/normal.log" 2>&1
san=(-std=c++23 -Wall -Wextra -Werror -pedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
rtk proxy g++ "${san[@]}" "$out/host-stage-source.cpp" -o "$out/host-stage-source-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/host-stage-source-san" > "$out/san.log" 2>&1
sha256sum "$out/host-stage-source.cpp" "$repo/server/compositor/compositor.cpp" "$repo/server/driver/wivrn_session.h" > "$out/hashes.txt"
cp "$out/provenance.txt" "$out/provenance-captured.txt"
