#!/usr/bin/env bash
set -euo pipefail
repo=${1:?source checkout required}
out=${2:-results}
mkdir -p "$out"
rtk proxy python3 extract-check.py "$repo" "$out"
inc=(-I "$repo/common" -I "$repo/client/decoder" -I "$repo/server/encoder" -I "$repo/build-server/common" -I "$repo/external" -I "$repo/build-server/_deps/boost-src/libs/pfr/include")
flags=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -pthread "${inc[@]}")
rtk proxy g++ "${flags[@]}" "$out/actual-method-gate.cpp" "$repo/common/smp.cpp" -lcrypto -o "$out/actual-method-gate"
rtk proxy timeout 30 "$out/actual-method-gate" > "$out/normal.log" 2>&1
san=(-std=c++23 -Wall -Wextra -Werror -Wno-error=sign-compare -Wno-error=unused-parameter -pthread -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${inc[@]}")
rtk proxy g++ "${san[@]}" "$out/actual-method-gate.cpp" "$repo/common/smp.cpp" -lcrypto -o "$out/actual-method-gate-san"
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 rtk proxy timeout 30 "$out/actual-method-gate-san" > "$out/san.log" 2>&1
sha256sum "$out/actual-method-gate.cpp" "$repo/client/decoder/shard_accumulator.cpp" "$repo/client/decoder/shard_set.h" "$repo/client/decoder/frame_window.h" "$repo/client/decoder/nack_deadline.h" > "$out/hashes.txt"
cp "$out/provenance.txt" "$out/provenance-captured.txt"
