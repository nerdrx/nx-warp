#!/usr/bin/env bash
set -euo pipefail
repo=${1:?checkout}; out=${2:?output}; here=$(cd "$(dirname "$0")" && pwd); mkdir -p "$out"
python3 "$here/generate.py" "$repo" "$out"
inc=(-I "$repo/client/decoder" -I "$repo/common" -I "$repo/external" -I "$repo/build-server/common" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
flags=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -Wno-error=missing-field-initializers -pthread)
g++ "${flags[@]}" "${inc[@]}" "$out/kernel-gate.cpp" "$repo/common/wivrn_sockets.cpp" "$repo/common/crypto.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/kernel-gate" >"$out/build.log" 2>&1
timeout 10 "$out/kernel-gate" >"$out/normal.log" 2>&1
g++ "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}" "$out/kernel-gate.cpp" "$repo/common/wivrn_sockets.cpp" "$repo/common/crypto.cpp" "$repo/common/smp.cpp" -lcrypto -lspdlog -lfmt -o "$out/kernel-gate-san" >"$out/build-san.log" 2>&1
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 15 "$out/kernel-gate-san" >"$out/san.log" 2>&1
sha256sum "$out/kernel-gate.cpp" "$repo/client/wivrn_client.h" "$repo/client/scenes/stream_network.cpp" "$repo/client/decoder/shard_accumulator.cpp" "$repo/client/decoder/frame_window.h" "$repo/client/decoder/shard_set.h" "$repo/client/decoder/nack_deadline.h" "$repo/common/wivrn_sockets.cpp" >"$out/hashes.txt"
