#!/usr/bin/env bash
set -euo pipefail
src=${1:?source checkout required}
out=${2:?output dir required}
base_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
flags=(-std=c++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I "$base_dir/server" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${flags[@]}" -c "$base_dir/server/driver/bitrate_controller.cpp" -o "$out/controller.o"
g++ "${flags[@]}" -c "$src/common/smp.cpp" -o "$out/smp.o"
for name in bitrate_bbr_test bitrate_bbr_budget_test bitrate_nxwarp_test bitrate_aimd_loss_only_test bitrate_radio_test; do
  test_src="$src/tests/$name.cpp"
  [[ "$name" == bitrate_bbr_test ]] && test_src="$base_dir/tests/$name.cpp"
  g++ "${flags[@]}" "$test_src" "$out/controller.o" "$out/smp.o" -lcrypto -o "$out/$name"
  ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$out/$name" > "$out/$name.log" 2>&1
  tail -2 "$out/$name.log"
done
sha256sum "$base_dir/server/driver/bitrate_controller.cpp" "$base_dir/server/driver/bitrate_controller.h" "$base_dir/tests/bitrate_bbr_test.cpp" > "$out/source-sha256.txt"
