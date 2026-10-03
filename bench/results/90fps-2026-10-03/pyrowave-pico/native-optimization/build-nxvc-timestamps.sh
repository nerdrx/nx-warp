#!/usr/bin/env bash
set -euo pipefail
repo=/run/media/nerdrx/Lex/claude/nx-warp
ndk=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
target="$repo/bench/results/90fps-2026-10-03/pyrowave-pico/native-optimization/nxvc-vkdec-profile-timestamps.cpp"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" \
  --target=aarch64-none-linux-android29 \
  --sysroot="$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot" \
  -DANDROID -O3 -DNDEBUG -std=gnu++20 -fPIE -fdata-sections -ffunction-sections \
  -I"$repo/include" -I"$repo/build-vkdec-android/generated/include" \
  -I"$repo/vk/decoder" -I"$repo" \
  "$target" \
  "$repo/build-vkdec-android/vk/decoder/libnxvc_vk_decoder.a" \
  "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/29/libvulkan.so" \
  -static-libstdc++ -Wl,--build-id=sha1 -Wl,--gc-sections -Wl,--no-undefined \
  -latomic -lm \
  -o /run/media/nerdrx/Lex/claude/nx-scratch/nxvc-ts-profile-20261003/nxvc-vkdec-profile-timestamps
