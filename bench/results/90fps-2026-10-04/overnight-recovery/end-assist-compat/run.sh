#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 SOURCE_CHECKOUT OUTPUT_DIR" >&2
  exit 2
fi
src=$(realpath "$1")
out=$(realpath -m "$2")
mkdir -p "$out"
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
client_rev=c514841f87b9718a50c974e79c3e902ef38a3a82
server_rev=7b7ae3600951d90b07466053bbcda45f7dcc9b3a
cxx=${CXX:-g++}

actual_server_rev=$(git -C "$src" rev-parse HEAD)
actual_client_rev=$(git -C "$src" rev-parse "$client_rev^{commit}")
[[ "$actual_server_rev" == "$server_rev" ]] || { echo "expected server $server_rev, got $actual_server_rev" >&2; exit 3; }
[[ "$actual_client_rev" == "$client_rev" ]] || { echo "unexpected client revision $actual_client_rev" >&2; exit 4; }
for path in build-server/_deps/boost-src/libs/pfr/include build-server/common external/magic_enum.hpp common/smp.cpp; do
  [[ -e "$src/$path" ]] || { echo "missing source-build dependency: $src/$path" >&2; exit 5; }
done

work=$(mktemp -d "$out/work.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/archive"
git -C "$src" archive "$client_rev" common client/decoder/shard_set.h | tar -x -C "$work/archive"

{
  echo "client_revision=$actual_client_rev"
  echo "server_revision=$actual_server_rev"
  (cd "$work/archive" && sha256sum common/wivrn_packets.h common/fec.h client/decoder/shard_set.h)
  (cd "$src" && sha256sum common/wivrn_packets.h common/fec.h server/encoder/shard_history.h)
} > "$out/provenance.txt"

"$cxx" -std=c++23 -O2 -I "$src/common" -I "$src/build-server/common" -I "$src/external" \
  -I "$src/build-server/_deps/boost-src/libs/pfr/include" \
  -o "$work/make_server_frame" "$script_dir/make_server_frame.cpp" "$src/common/smp.cpp" -lcrypto
"$work/make_server_frame" "$work/current-server-frame.bin"

includes=(-I "$work/archive/client/decoder" -I "$work/archive/common" -I "$src/server/encoder" \
  -I "$src/build-server/common" -I "$src/build-server/_deps/boost-src/libs/pfr/include" \
  -I "$src/external")
"$cxx" -std=c++23 -O2 -pthread "${includes[@]}" -o "$work/old_client_replay" \
  "$script_dir/old_client_replay.cpp" "$src/common/smp.cpp" -lcrypto
"$work/old_client_replay" "$work/current-server-frame.bin" > "$out/normal.log"
"$cxx" -std=c++23 -O1 -g -pthread -fno-omit-frame-pointer -fsanitize=address,undefined \
  "${includes[@]}" -o "$work/old_client_replay_san" \
  "$script_dir/old_client_replay.cpp" "$src/common/smp.cpp" -lcrypto
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$work/old_client_replay_san" "$work/current-server-frame.bin" > "$out/san.log"
cat "$out/normal.log" "$out/san.log"
