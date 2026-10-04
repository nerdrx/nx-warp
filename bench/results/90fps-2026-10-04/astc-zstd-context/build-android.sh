#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
ndk=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
cmake=/run/media/nerdrx/Lex/claude/tools/cmake-3.31.5-linux-x86_64/bin/cmake
zstd_include=/run/media/nerdrx/Lex/claude/nx-scratch/wivrn-atlas-live-client-build/_deps/nx_zstd-src/lib
zstd_library=/run/media/nerdrx/Lex/claude/nx-scratch/20260923-until8/zstd-android-build/lib/libzstd.a
"$cmake" -S "$root" -B "$root/build/android-arm64" -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DZSTD_INCLUDE="$zstd_include" -DZSTD_LIBRARY="$zstd_library" -DCMAKE_BUILD_TYPE=Release
"$cmake" --build "$root/build/android-arm64" --parallel "$(nproc)"
