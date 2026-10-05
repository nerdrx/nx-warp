#!/usr/bin/env bash
set -euo pipefail
out=${1:-results}
mkdir -p "$out"
rtk proxy g++ -std=c++23 -Wall -Wextra -Werror -pedantic gate.cpp -o "$out/gate"
rtk proxy timeout 30 "$out/gate" > "$out/normal.log" 2>&1
rtk proxy g++ -std=c++23 -Wall -Wextra -Werror -pedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer gate.cpp -o "$out/gate-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/gate-san" > "$out/san.log" 2>&1
sha256sum gate.cpp > "$out/source-sha256.txt"
