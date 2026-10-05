#!/usr/bin/env bash
set -euo pipefail
repo=${1:?}
out=${2:?}
inc=(-I "$repo/common" -I "$repo/client/decoder" -I "$repo/external" -I "$repo/build-server/common" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
flags=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter)
rtk proxy g++ "${flags[@]}" "${inc[@]}" "$out/caller-gate.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/caller-gate" > "$out/build.log" 2>&1
rtk proxy timeout 15 "$out/caller-gate" > "$out/normal.log" 2>&1
rtk proxy g++ "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}" "$out/caller-gate.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/caller-gate-san" > "$out/build-san.log" 2>&1
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 15 "$out/caller-gate-san" > "$out/san.log" 2>&1
