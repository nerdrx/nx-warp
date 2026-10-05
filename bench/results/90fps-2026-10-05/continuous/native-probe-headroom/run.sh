#!/usr/bin/env bash
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=${1:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR}
out=${2:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR}
mkdir -p "$out"
base="$script_dir/baseline"
flags=(-std=c++23 -O2 -I "$src/tests" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
# Historical controller comes before the current header path; tests use public API only.
g++ -I "$base/server" -I "$base/server/driver" "${flags[@]}" "$script_dir/root.cpp" "$base/server/driver/bitrate_controller.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/baseline-root" > "$out/baseline-root-build.log" 2>&1
set +e
"$out/baseline-root" > "$out/baseline-root-run.log" 2>&1
result=$?
set -e
cat "$out/baseline-root-run.log"
# The baseline must fail the no-upward-probe assertion, not build or crash.
test "$result" = 1
g++ -I "$script_dir/candidate/server" "${flags[@]}" "$script_dir/root.cpp" "$script_dir/candidate/server/driver/bitrate_controller.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/candidate-root" > "$out/candidate-root-build.log" 2>&1
"$out/candidate-root" > "$out/candidate-root-run.log" 2>&1
cat "$out/candidate-root-run.log"
sha256sum "$script_dir/candidate/server/driver/bitrate_controller.cpp" "$script_dir/candidate/server/driver/bitrate_controller.h" "$script_dir/candidate/tests/bitrate_bbr_test.cpp" > "$out/source-sha256.txt"
