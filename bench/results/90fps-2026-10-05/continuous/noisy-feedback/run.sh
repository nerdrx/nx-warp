#!/bin/bash
set -euo pipefail
src=${1:?source checkout required}
out=${2:?external output required}
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixtures="$base/../fresh-peak-confirmation"
(cd "$base"; sha256sum -c EVIDENCE_SHA256.txt >/dev/null)
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
for mode in ${3:-baseline D E}; do
 case "$mode" in baseline|D|E);; *) exit 2;; esac
 controller="$fixtures/source/baseline"
 if [ "$mode" = D ]; then controller="$base/../material-recovery-bound/source"; fi
 if [ "$mode" = E ]; then controller="$base/source-E"; fi
 mkdir -p "$out/$mode"
 flags=(-std=c++23 -O2 -I "$controller/server" -I "$fixtures" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
 g++ "${flags[@]}" "$base/noisy.cpp" "$controller/server/driver/bitrate_controller.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/$mode/noisy.bin" > "$out/$mode/build.log" 2>&1
 "$out/$mode/noisy.bin" "$out/$mode/rows.csv" > "$out/$mode/run.log" 2>&1
 sha256sum "$controller/server/driver/bitrate_controller.cpp" "$controller/server/driver/bitrate_controller.h" "$base/noisy.cpp" "$fixtures/reference-bbr-harness.cpp" "$src/common/smp.cpp" > "$out/$mode/input-hashes.txt"
done
