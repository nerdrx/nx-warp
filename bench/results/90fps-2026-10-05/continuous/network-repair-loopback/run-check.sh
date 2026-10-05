#!/usr/bin/env bash
set -euo pipefail
repo=${1:?checkout required}; out=${2:-results}; here=$(cd "$(dirname "$0")" && pwd);mkdir -p "$out"
rtk proxy python3 "$here/generate.py" "$repo" "$out"
inc=(-I "$repo/client" -I "$repo/client/decoder" -I "$repo/server/encoder" -I "$repo/common" -I "$repo/external" -I "$repo/build-server/common" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
src=("$repo/common/wivrn_sockets.cpp" "$repo/common/crypto.cpp" "$repo/common/smp.cpp")
flags=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -Wno-error=missing-field-initializers -Wno-error=misleading-indentation -pthread "${inc[@]}")
rtk proxy g++ "${flags[@]}" "$out/loopback-gate.cpp" "${src[@]}" -lcrypto -lspdlog -lfmt -o "$out/loopback-gate" 2>"$out/build.log"
rtk proxy timeout 30 "$out/loopback-gate" >"$out/normal.log" 2>&1
san=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -Wno-error=missing-field-initializers -Wno-error=misleading-indentation -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}")
rtk proxy g++ "${san[@]}" "$out/loopback-gate.cpp" "${src[@]}" -lcrypto -lspdlog -lfmt -o "$out/loopback-gate-san" 2>"$out/build-san.log"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/loopback-gate-san" >"$out/san.log" 2>&1
sha256sum "$out/loopback-gate.cpp" "$repo/client/decoder/shard_accumulator.cpp" "$repo/client/wivrn_client.h" "$repo/server/encoder/shard_history.h" "$repo/common/wivrn_sockets.cpp" "$repo/client/decoder/nack_deadline.h" "$repo/client/decoder/shard_set.h" "$repo/client/decoder/frame_window.h" "$repo/common/fec.h" "$repo/common/wivrn_sockets.h" "$repo/common/wivrn_packets.h" >"$out/hashes.txt"
