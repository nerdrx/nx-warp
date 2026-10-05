#!/usr/bin/env bash
set -eu
src=${1:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR BASELINE_DIR}
out=${2:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR BASELINE_DIR}
base=${3:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR BASELINE_DIR}
if [[ $# -ne 3 || ! -d "$src" || ! -d "$base" ]]; then
  echo "usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR BASELINE_DIR (both source dirs must exist)" >&2
  exit 2
fi
mkdir -p "$out"
flags=(-std=c++23 -O2 -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
# Keep the archived controller first on the baseline include path; pacing helper remains current and source-confirmed.
baseflags=(-std=c++23 -O2 -I "$base/server" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
g++ "${baseflags[@]}" -o "$out/baseline" "$(dirname "$0")/coupled.cpp" "$base/server/driver/bitrate_controller.cpp" "$src/common/smp.cpp" -lcrypto > "$out/baseline-build.log" 2>&1
"$out/baseline" baseline "$out/baseline-500.csv" "$out/baseline-2g.csv"
g++ "${flags[@]}" -o "$out/patched" "$(dirname "$0")/coupled.cpp" "$src/server/driver/bitrate_controller.cpp" "$src/common/smp.cpp" -lcrypto > "$out/patched-build.log" 2>&1
"$out/patched" patched "$out/patched-500.csv" "$out/patched-2g.csv"
