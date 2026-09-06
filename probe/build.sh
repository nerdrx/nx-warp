#!/usr/bin/env bash
# Build the standalone texture-native probe for arm64 and push it to
# /data/local/tmp/nxtex. Nothing here installs a package, touches
# wivrn-server, or touches the installed org.meumeu.wivrn.nx.warp app.
set -euo pipefail
cd "$(dirname "$0")"

. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7

NDK=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TC/bin/aarch64-linux-android29-clang

mkdir -p build
echo "== display.spv"
"${NICE[@]}" glslc -fshader-stage=comp -O src/display.comp -o build/display.spv
"${NICE[@]}" glslc -fshader-stage=comp -O src/display3.comp -o build/display3.spv

echo "== nxtexnative"
"${NICE[@]}" "$CC" -O2 -Wall -Wextra -Wno-unused-parameter \
    -o build/nxtexnative src/nxtexnative.c -lvulkan -llog -lm
"$TC/bin/llvm-strip" build/nxtexnative
ls -la build/nxtexnative build/display.spv

adb shell mkdir -p /data/local/tmp/nxtex
adb push build/nxtexnative build/display.spv build/display3.spv /data/local/tmp/nxtex/ >/dev/null
adb shell chmod 755 /data/local/tmp/nxtex/nxtexnative
echo "pushed to /data/local/tmp/nxtex"
