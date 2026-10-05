#!/usr/bin/env bash
set -euo pipefail
repo=${1:?checkout}; out=${2:?output}; here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
python3 "$here/../quiet-retirement-kernel/generate.py" "$repo" "$out"
python3 "$here/prepare.py" "$repo" "$out"
inc=(-I "$repo/client/decoder" -I "$repo/common" -I "$repo/external" -I "$repo/build-server/common" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
flags=(-std=c++23 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -pthread)
g++ "${flags[@]}" "${inc[@]}" "$out/kernel-gate.cpp" "$repo/common/wivrn_sockets.cpp" "$repo/common/crypto.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/arrival-gate" >"$out/build.log" 2>&1
timeout 5 "$out/arrival-gate" >"$out/normal.log" 2>&1
if [[ ${3:-} == san ]]; then
 g++ "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}" "$out/kernel-gate.cpp" "$repo/common/wivrn_sockets.cpp" "$repo/common/crypto.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/arrival-gate-san" >"$out/build-san.log" 2>&1
 ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 10 "$out/arrival-gate-san" >"$out/san.log" 2>&1
fi
