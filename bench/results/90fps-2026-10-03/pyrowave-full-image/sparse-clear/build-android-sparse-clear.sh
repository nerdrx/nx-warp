#!/usr/bin/env bash
set -euo pipefail
repo=/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-sparse-clear-20261003
build="$repo/build-android-arm64"
deps=/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-atlas-live-client-build/_deps
ndk=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
clang="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
mkdir -p "$repo/android-objects"
for src in pyrowave_common pyrowave_encoder pyrowave_decoder pyrowave_shaders; do
  if [[ $src == pyrowave_shaders ]]; then file="$build/common/pyrowave/pyrowave_shaders.cpp"; else file="$repo/common/pyrowave/$src.cpp"; fi
  "$clang" --target=aarch64-none-linux-android29 --sysroot="$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot" -O3 -DNDEBUG -std=c++23 -DANDROID -DPYROWAVE_HAAR_FORMAT=1 -DVMA_VULKAN_VERSION=1001000 -DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS -I"$repo/common/pyrowave" -I"$repo/common" -I"$build/common" -I"$build/common/pyrowave" -I"$repo" -I"$repo/external" -I"$build/Vulkan-Headers-vulkan-sdk-1.3.268.0/include" -I"$deps/nx_zstd-src/lib" -I"$deps/boost-src/libs/pfr/include" -c "$file" -o "$repo/android-objects/$src.o"
done
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar" rcs "$repo/libpyrowave-haar-android-arm64.a" "$repo"/android-objects/*.o
