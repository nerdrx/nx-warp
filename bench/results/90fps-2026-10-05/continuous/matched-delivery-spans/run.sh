#!/usr/bin/env bash
set -euo pipefail
src=${1:?source checkout}
snapshot=${2:?matching source snapshot}
out=${3:?output directory}
base=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/capacity"
flags=(-std=c++23 -O2 -I "$snapshot/server" -I "$base" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${flags[@]}" -c "$snapshot/server/driver/bitrate_controller.cpp" -o "$out/controller.o" > "$out/build.log" 2>&1
g++ "${flags[@]}" -c "$src/common/smp.cpp" -o "$out/smp.o" >> "$out/build.log" 2>&1
for fixture in noisy capacity_step span_cases; do
 g++ "${flags[@]}" "$base/$fixture.cpp" "$out/controller.o" "$out/smp.o" -lcrypto -o "$out/$fixture.bin" >> "$out/build.log" 2>&1
done
for policy in service paced; do
 "$out/noisy.bin" "$out/noisy-$policy.csv" "$policy" > "$out/noisy-$policy.log" 2>&1
done
"$out/capacity_step.bin" "$out/capacity" > "$out/capacity/run.log" 2>&1
"$out/span_cases.bin" > "$out/span_cases.csv"
sha256sum "$snapshot/server/driver/bitrate_controller.cpp" "$snapshot/server/driver/bitrate_controller.h" "$base/reference-bbr-harness.cpp" "$base/noisy.cpp" "$base/capacity_step.cpp" "$base/span_cases.cpp" > "$out/hashes.txt"
