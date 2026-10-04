#!/usr/bin/env bash
set -eu
src=$(realpath "$1")
cfg=$(realpath "$2")
out=$(realpath -m "$3")
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"
cxx=${CXX:-g++}
inc=(-I"$src/client" -I"$src/client/decoder" -I"$src/common" -I"$cfg/common" -I"$src/external" -I"$cfg/_deps/boost-src/libs/pfr/include")
"$cxx" -std=c++23 -O2 -I"$here/baseline" "${inc[@]}" "$here/cost.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/baseline"
"$cxx" -std=c++23 -O2 "${inc[@]}" "$here/cost.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/cached"
"$cxx" -std=c++23 -O2 "${inc[@]}" "$here/differential.cpp" "$src/common/smp.cpp" -lcrypto -o "$out/differential"
"$out/differential" > "$out/differential.log"
python3 "$here/run-host.py" "$out"
