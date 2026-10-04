#!/usr/bin/env bash
set -eu
# Source, configured host build (PFR/OpenXR headers), Android OpenSSL prefix, NDK, output.
src=$(realpath "$1")
cfg=$(realpath "$2")
ssl=$(realpath "$3")
ndk=$(realpath "$4")
out=$(realpath -m "$5")
here=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$out"
cxx=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++
inc=(-I"$src/client" -I"$src/common" -I"$cfg/common" -I"$src/external" -I"$cfg/_deps/boost-src/libs/pfr/include" -I"$cfg/_deps/monado-src/src/external/openxr_includes" -I"$ssl/include")
"$cxx" -O2 -std=c++23 -static-libstdc++ "${inc[@]}" "$here/cost.cpp" "$src/common/smp.cpp" "$ssl/lib/libcrypto.a" -ldl -lm -o "$out/count"
"$cxx" -O2 -std=c++23 -static-libstdc++ -I"$here/baseline" "${inc[@]}" "$here/cost.cpp" "$src/common/smp.cpp" "$ssl/lib/libcrypto.a" -ldl -lm -o "$out/baseline"
