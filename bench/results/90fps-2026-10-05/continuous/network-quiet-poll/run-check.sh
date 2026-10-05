#!/usr/bin/env bash
set -euo pipefail
repo=${1:?checkout required}; out=${2:?output required}; here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
python3 "$here/extract.py" "$repo" "$out"
inc=(-I "$here/baseline" -I "$repo/client/decoder" -I "$repo/common" -I "$repo/external" -I "$repo/build-server/common" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
src=("$repo/common/wivrn_sockets.cpp" "$repo/common/crypto.cpp" "$repo/common/smp.cpp")
g++ -std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -Wno-error=missing-field-initializers -pthread "${inc[@]}" "$out/poll-gate.cpp" "${src[@]}" -lcrypto -lspdlog -lfmt -o "$out/poll-gate" 2>"$out/build.log"
timeout 30 "$out/poll-gate" >"$out/normal.log" 2>&1
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 g++ -std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -Wno-error=missing-field-initializers -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}" "$out/poll-gate.cpp" "${src[@]}" -lcrypto -lspdlog -lfmt -o "$out/poll-gate-san" 2>"$out/build-san.log"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$out/poll-gate-san" >"$out/san.log" 2>&1
sha256sum "$out/poll-gate.cpp" "$repo/client/wivrn_client.h" "$repo/common/wivrn_sockets.h" "$repo/common/wivrn_packets.h" "$repo/common/wivrn_sockets.cpp" "$repo/client/decoder/nack_deadline.h" "$here/baseline/nack_deadline.h" >"$out/hashes.txt"
