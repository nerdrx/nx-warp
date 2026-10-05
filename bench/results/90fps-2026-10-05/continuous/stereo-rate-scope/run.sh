#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=${1:?usage: run.sh SOURCE_CHECKOUT [OUTPUT_DIR]}
out=${2:-$(dirname "$0")}
mkdir -p "$out"
g++ -std=c++23 -O2 -I "$src/server" -I "$src/common" -I "$src/build-server/common" \
  -I "$src/build-server/_deps/monado-src/src/xrt/include" \
  -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" \
  -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" \
  -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include" \
  -o "$out/replay" "$script_dir/replay.cpp" "$src/server/driver/bitrate_controller.cpp" \
  "$src/common/smp.cpp" -lcrypto > "$out/build.log" 2>&1
echo 'build_exit=0' > "$out/status.txt"
"$out/replay" > "$out/output.csv"
echo 'run_exit=0' >> "$out/status.txt"
