#!/usr/bin/env bash
set -euo pipefail
repo=${1:?source checkout required}
out=${2:?output directory required}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
rtk proxy python3 "$here/extract.py" "$repo" "$out"
inc=(-I "$repo/common" -I "$repo/client/decoder" -I "$repo/external" -I "$repo/build-server/common" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
flags=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter)
rtk proxy g++ "${flags[@]}" "${inc[@]}" "$out/pump-gate.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/pump-gate" > "$out/build.log" 2>&1
rtk proxy timeout 15 "$out/pump-gate" > "$out/normal.log" 2>&1
rtk proxy g++ "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}" "$out/pump-gate.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/pump-gate-san" > "$out/build-san.log" 2>&1
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 15 "$out/pump-gate-san" > "$out/san.log" 2>&1
sha256sum "$out/pump-gate.cpp" "$repo/client/decoder/shard_accumulator.cpp" "$repo/client/decoder/shard_accumulator.h" "$repo/client/decoder/frame_window.h" "$repo/client/decoder/shard_set.h" > "$out/hashes.txt"
