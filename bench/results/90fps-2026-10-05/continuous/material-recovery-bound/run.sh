#!/bin/bash
set -euo pipefail
src=${1:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR}
out=${2:?output directory required}
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixtures="$base/../fresh-peak-confirmation"
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
cd "$base"
sha256sum -c EVIDENCE_SHA256.txt > "$out/evidence-check.log"
flags=(-std=c++23 -O2 -I "$base/source/server" -I "$fixtures" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${flags[@]}" -c "$base/source/server/driver/bitrate_controller.cpp" -o "$out/controller.o" > "$out/build.log" 2>&1
g++ "${flags[@]}" -c "$src/common/smp.cpp" -o "$out/smp.o" >> "$out/build.log" 2>&1
for name in phase adversarial capacity_step probe_phase; do
  g++ "${flags[@]}" "$fixtures/$name.cpp" "$out/controller.o" "$out/smp.o" -lcrypto -o "$out/$name.bin" >> "$out/build.log" 2>&1
done
mkdir -p "$out/phase" "$out/adversarial" "$out/capacity" "$out/probe-phase"
"$out/phase.bin" "$out/phase/rows.csv" > "$out/phase/run.log" 2>&1
"$out/adversarial.bin" "$out/adversarial/rows.csv" > "$out/adversarial/run.log" 2>&1
"$out/capacity_step.bin" "$out/capacity" > "$out/capacity/run.log" 2>&1
"$out/probe_phase.bin" "$out/probe-phase/rows.csv" "$out/probe-phase/summary.csv" > "$out/probe-phase/run.log" 2>&1
sha256sum "$base/source/server/driver/bitrate_controller.cpp" "$base/source/server/driver/bitrate_controller.h" "$fixtures/reference-bbr-harness.cpp" "$src/common/smp.cpp" > "$out/input-hashes.txt"
