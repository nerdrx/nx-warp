#!/usr/bin/env bash
# Cross-build the decoder's conformance suite for arm64, push it to an
# attached device and run it there.  No APK, no Java, no NativeActivity: the
# test is a plain executable and adb shell is the whole harness.
#
#   ./vk/decoder/tools/run-android.sh                  # the full sweep
#   ./vk/decoder/tools/run-android.sh --quick          # a subset, for a smoke test
#   ./vk/decoder/tools/run-android.sh --bench 10       # the timing table
#   ./vk/decoder/tools/run-android.sh --unorm 1        # opt into the UNORM store
#   ./vk/decoder/tools/run-android.sh --no-build       # reuse what is there
#
# Everything after `--` is passed straight to the test binary.
#
# Device etiquette, which is not optional on a headset: the GPU parks at
# 305 MHz while the screen is off and every number taken then is fiction, so
# the device is woken and pinned awake before each run and the GPU clock is
# recorded either side of it (bench/README.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${NXVC_ANDROID_BUILD:-$ROOT/build-vkdec-android}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865}}"
ABI="${ANDROID_ABI:-arm64-v8a}"
PLATFORM="${ANDROID_PLATFORM:-android-29}"
DEST="${NXVC_ANDROID_DEST:-/data/local/tmp/nxwarp}"
JOBS="${NXVC_JOBS:-4}"

BUILD_IT=1
UNORM=""
MODE=(--verbose)
EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) BUILD_IT=0; shift ;;
    --quick)    MODE=(--quick); shift ;;
    --bench)    MODE=(--bench "${2:-10}"); shift 2 ;;
    --unorm)    UNORM="$2"; shift 2 ;;
    --)         shift; EXTRA=("$@"); break ;;
    *)          EXTRA+=("$1"); shift ;;
  esac
done

ADB=(adb)
[[ -n "${ANDROID_SERIAL:-}" ]] && ADB=(adb -s "$ANDROID_SERIAL")

if ! "${ADB[@]}" get-state >/dev/null 2>&1; then
  echo "no adb device (set ANDROID_SERIAL to pick one of several)" >&2
  exit 77
fi

if [[ ! -d "$NDK" ]]; then
  echo "NDK not found at $NDK (set ANDROID_NDK_HOME)" >&2
  exit 77
fi

if [[ $BUILD_IT -eq 1 ]]; then
  echo "== configuring $BUILD for $ABI"
  # NXWARP_BUILD_TESTS is what brings in tests/vk-decoder; the transport and
  # the examples are not wanted on the device and only slow the build down.
  cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="$PLATFORM" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNXWARP_BUILD_VK=ON -DNXWARP_BUILD_TESTS=ON -DNXWARP_BUILD_TOOLS=ON \
    -DNXWARP_BUILD_EXAMPLES=OFF -DNXWARP_BUILD_TRANSPORT=OFF >/dev/null
  echo "== building"
  cmake --build "$BUILD" -j"$JOBS" \
        --target test_vk_decoder_conformance nxvc-vkdec >/dev/null
fi

BIN="$BUILD/bin/test_vk_decoder_conformance"
[[ -x "$BIN" ]] || { echo "not built: $BIN" >&2; exit 1; }

echo "== pushing to $DEST"
"${ADB[@]}" shell "mkdir -p $DEST/vectors" >/dev/null
"${ADB[@]}" push "$BIN" "$BUILD/bin/nxvc-vkdec" "$DEST/" >/dev/null
# The UNORM round-trip proof rides along when it was built; it is what says
# whether the UNORM store is allowed on this device at all.
[[ -x "$BUILD/bin/test_unorm_roundtrip" ]] &&
  "${ADB[@]}" push "$BUILD/bin/test_unorm_roundtrip" "$DEST/" >/dev/null
"${ADB[@]}" push "$ROOT/tests/vectors/." "$DEST/vectors/" >/dev/null
"${ADB[@]}" shell "chmod +x $DEST/test_vk_decoder_conformance $DEST/nxvc-vkdec" >/dev/null

# The GPU parks at 305 MHz with the screen off; wake it and keep it awake.
"${ADB[@]}" shell "input keyevent KEYCODE_WAKEUP; svc power stayon usb" >/dev/null
CLK_BEFORE=$("${ADB[@]}" shell "cat /sys/class/kgsl/kgsl-3d0/gpuclk" 2>/dev/null | tr -d '\r' || true)

ENVSTR=""
[[ -n "$UNORM" ]] && ENVSTR="NXVC_VKD_UNORM=$UNORM "

echo "== running: ${ENVSTR}test_vk_decoder_conformance ${MODE[*]} ${EXTRA[*]:-}"
set +e
"${ADB[@]}" shell "cd $DEST && ${ENVSTR}./test_vk_decoder_conformance ${MODE[*]} --vectors ./vectors ${EXTRA[*]:-}; echo NXVC_EXIT=\$?" \
  | tee /tmp/nxvc-android-run.$$
set -e
RC=$(grep -o 'NXVC_EXIT=[0-9]*' /tmp/nxvc-android-run.$$ | tail -1 | cut -d= -f2)
rm -f /tmp/nxvc-android-run.$$

CLK_AFTER=$("${ADB[@]}" shell "cat /sys/class/kgsl/kgsl-3d0/gpuclk" 2>/dev/null | tr -d '\r' || true)
[[ -n "$CLK_BEFORE$CLK_AFTER" ]] &&
  echo "== gpuclk ${CLK_BEFORE:-?} -> ${CLK_AFTER:-?} Hz"

case "${RC:-1}" in
  0)  echo "== PASS" ;;
  77) echo "== SKIP (no usable Vulkan ICD on the device)" ;;
  *)  echo "== FAIL (exit ${RC:-?})" ;;
esac
exit "${RC:-1}"
