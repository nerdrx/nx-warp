#!/bin/bash
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=${1:?source checkout required}
out=${2:?output directory required}
mkdir -p "$out"
flags=(-std=c++23 -O2 -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${flags[@]}" -c "$src/server/driver/bitrate_controller.cpp" -o "$out/controller.o"
g++ "${flags[@]}" -c "$src/common/smp.cpp" -o "$out/smp.o"
for name in bitrate_bbr_test bitrate_bbr_budget_test bitrate_nxwarp_test bitrate_aimd_loss_only_test bitrate_radio_test; do
  g++ "${flags[@]}" "$src/tests/$name.cpp" "$out/controller.o" "$out/smp.o" -lcrypto -o "$out/$name"
  timeout 30 "$out/$name" > "$out/$name.log" 2>&1
  tail -3 "$out/$name.log"
done
sha256sum "$src/server/driver/bitrate_controller.cpp" "$src/server/driver/bitrate_controller.h" "$src/tests/bitrate_bbr_test.cpp" > "$out/source-sha256.txt"
g++ "${flags[@]}" -I "$src/tests" "$script_dir/root-extra.cpp" "$out/controller.o" "$out/smp.o" -lcrypto -o "$out/extra"
timeout 30 "$out/extra" > "$out/extra-run.log" 2>&1
cat "$out/extra-run.log"
