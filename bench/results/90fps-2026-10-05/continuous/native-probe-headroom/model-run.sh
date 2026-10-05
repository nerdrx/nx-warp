#!/usr/bin/env bash
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=${1:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR}
out=${2:?usage: run.sh SOURCE_CHECKOUT OUTPUT_DIR}
mkdir -p "$out"
base="$script_dir/baseline"
flags=(-std=c++23 -O2 -I "$src/tests" -I "$src/server" -I "$src/common" -I "$src/build-server/common" -I "$src/build-server/_deps/monado-src/src/xrt/include" -I "$src/build-server/_deps/monado-src/src/xrt/auxiliary" -I "$src/build-server/_deps/monado-src/src/external/openxr_includes" -isystem "$src/external" -isystem "$src/build-server/_deps/boost-src/libs/pfr/include")
for mode in baseline candidate; do
 if [[ "$mode" == baseline ]]; then
  controller="$base/server/driver/bitrate_controller.cpp"
  inc=(-I "$base/server" -I "$base/server/driver")
 else
  controller="$script_dir/candidate/server/driver/bitrate_controller.cpp"
  inc=(-I "$script_dir/candidate/server" -I "$script_dir/candidate/server/driver")
 fi
 g++ "${inc[@]}" "${flags[@]}" "$script_dir/native_late_probe.cpp" "$controller" "$src/common/smp.cpp" -lcrypto -o "$out/$mode" > "$out/$mode-build.log" 2>&1
 "$out/$mode" "$mode" "$out" > "$out/$mode-run.log" 2>&1
 cat "$out/$mode-run.log"
done
python3 "$script_dir/check.py" "$out"
