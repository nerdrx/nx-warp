#!/usr/bin/env bash
set -euo pipefail
repo=${1:?source checkout required}
out=${2:?output directory required}
mkdir -p "$out"
rtk proxy python3 "$(dirname "$0")/source_extract.py" "$repo" "$out"
flags=(-std=c++23 -Wall -Wextra -Werror -pedantic -pthread)
rtk proxy g++ "${flags[@]}" "$out/extracted-lifecycle.cpp" -o "$out/lifecycle-normal"
rtk proxy timeout 30 "$out/lifecycle-normal" > "$out/normal.log" 2>&1
rtk proxy g++ "${flags[@]}" -DINJECT_LAUNCH_FAILURE "$out/extracted-launch-failure.cpp" -o "$out/lifecycle-launch-failure"
rtk proxy timeout 30 "$out/lifecycle-launch-failure" > "$out/launch-failure.log" 2>&1
san=(-std=c++23 -Wall -Wextra -Werror -pedantic -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
rtk proxy g++ "${san[@]}" "$out/extracted-lifecycle.cpp" -o "$out/lifecycle-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/lifecycle-san" > "$out/san.log" 2>&1
rtk proxy g++ "${san[@]}" -DINJECT_LAUNCH_FAILURE "$out/extracted-launch-failure.cpp" -o "$out/lifecycle-launch-failure-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/lifecycle-launch-failure-san" > "$out/launch-failure-san.log" 2>&1
sha256sum "$repo/server/compositor/compositor.cpp" "$out/extracted-lifecycle.cpp" "$out/extracted-launch-failure.cpp" > "$out/hashes.txt"
