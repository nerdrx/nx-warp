#!/usr/bin/env bash
set -euo pipefail
repo=/run/media/nerdrx/Lex/claude/nx-scratch/wt-pyrowave-probe
scratch=/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-batched-integrated-20261003
ndk=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
clang=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++
sysroot=$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot
build=$repo/build/android-arm64
deps=/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-atlas-live-client-build/_deps
flags=(--target=aarch64-none-linux-android29 --sysroot=$sysroot -DVMA_VULKAN_VERSION=1001000 -DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS -O3 -DNDEBUG -std=c++23 -DANDROID -DPYROWAVE_HAAR_FORMAT=1 -I$repo/common/pyrowave -I$repo/common -I$build/common -I$build/common/pyrowave -I$repo -I$repo/external -I$build/Vulkan-Headers-vulkan-sdk-1.3.268.0/include -I$deps/nx_zstd-src/lib -I$deps/boost-src/libs/pfr/include)
for name in bench readback motion-readback; do
 $clang "${flags[@]}" -static-libstdc++ $scratch/fused-haar-pico-$name.cpp $scratch/libhaar.a $build/common/libwivrn-common.a $build/common/libwivrn-common-base.a $sysroot/usr/lib/aarch64-linux-android/29/libvulkan.so $deps/openssl/lib/libcrypto.a -latomic -lm -o $scratch/$name
done
sha256sum $scratch/libhaar.a $scratch/bench $scratch/readback > $scratch/artifacts.sha256
