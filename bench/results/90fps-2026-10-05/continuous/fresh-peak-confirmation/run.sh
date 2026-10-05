#!/bin/bash
set -euo pipefail
src=${1:?usage: run.sh SOURCE_CHECKOUT MODE OUTPUT_DIR}
mode=${2:?mode required}
out=${3:?output directory required}
case "$mode" in baseline|exploratory-A|confirmed-B|growth-bounded-C);; *) exit 2;; esac
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
cd "$base"
sha256sum -c SOURCE_SHA256.txt > "$out/source-check.log"
controller="$base/source/$mode"
flags=(-std=c++23 -O2 -I "$controller/server" -I "$base" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${flags[@]}" -c "$controller/server/driver/bitrate_controller.cpp" -o "$out/controller.o" > "$out/build.log" 2>&1
g++ "${flags[@]}" -c "$src/common/smp.cpp" -o "$out/smp.o" >> "$out/build.log" 2>&1
for fixture in phase adversarial capacity_step probe_phase; do
  g++ "${flags[@]}" "$base/$fixture.cpp" "$out/controller.o" "$out/smp.o" -lcrypto -o "$out/$fixture.bin" >> "$out/build.log" 2>&1
done
mkdir -p "$out/phase" "$out/adversarial" "$out/capacity" "$out/probe-phase"
"$out/phase.bin" "$out/phase/rows.csv" > "$out/phase/run.log" 2>&1
"$out/adversarial.bin" "$out/adversarial/rows.csv" > "$out/adversarial/run.log" 2>&1
"$out/capacity_step.bin" "$out/capacity" > "$out/capacity/run.log" 2>&1
"$out/probe_phase.bin" "$out/probe-phase/rows.csv" "$out/probe-phase/summary.csv" > "$out/probe-phase/run.log" 2>&1
sha256sum "$controller/server/driver/bitrate_controller.cpp" "$controller/server/driver/bitrate_controller.h" "$src/common/smp.cpp" "$base/reference-bbr-harness.cpp" "$base/phase.cpp" "$base/adversarial.cpp" "$base/capacity_step.cpp" "$base/probe_phase.cpp" > "$out/input-hashes.txt"
