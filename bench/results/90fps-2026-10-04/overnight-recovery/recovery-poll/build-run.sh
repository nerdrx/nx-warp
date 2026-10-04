#!/usr/bin/env bash
set -eu
# Arguments: WiVRn NX source checkout, configured build tree, output directory.
src=$(realpath "$1")
cfg=$(realpath "$2")
out=$(realpath -m "$3")
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"
cxx=${CXX:-g++}
"$cxx" -std=c++23 -O2 -I"$src/common" -I"$src/client/decoder" -I"$cfg/common" -I"$src/external" -I"$cfg/_deps/boost-src/libs/pfr/include" "$here/adapter.cpp" "$src/common/smp.cpp" -lcrypto -pthread -o "$out/poll-adapter"
"$out/poll-adapter" > "$out/results.csv"
"$cxx" -std=c++23 -O2 -I"$src/common" -I"$src/client/decoder" -I"$cfg/common" -I"$src/external" -I"$cfg/_deps/boost-src/libs/pfr/include" "$src/tests/accumulator_test.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/accumulator-test"
"$out/accumulator-test" > "$out/checks.log"
"$cxx" -std=c++23 -O2 -I"$src/client" -I"$src/common" -I"$cfg/common" -I"$src/external" -I"$cfg/_deps/boost-src/libs/pfr/include" "$here/helper-cost.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/helper-cost"
"$out/helper-cost" > "$out/helper-cost.csv"
