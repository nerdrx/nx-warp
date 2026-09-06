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
#   ./vk/decoder/tools/run-android.sh --atlas          # the ATLAS kernels only
#   ./vk/decoder/tools/run-android.sh --build-only     # cross-build, touch no device
#
# Everything after `--` is passed straight to the test binary.
#
# Device etiquette, which is not optional on a headset: the GPU parks at
# 305 MHz while the screen is off and every number taken then is fiction, so
# the device is woken and pinned awake before each run and the GPU clock is
# recorded either side of it (bench/README.md).  The gpuss-max-step thermal
# zone is recorded with it, because the absolutes in any table taken at the end
# of a long session are a number about the die temperature and the ratios are
# what carry.
#
# And the pushed binary's sha256 is checked against the local one BEFORE it is
# launched.  adb push reports success on a short write; running a binary that
# is not the one that was built produces a result about nothing, and it is one
# shell command to rule out.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${NXVC_ANDROID_BUILD:-$ROOT/build-vkdec-android}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865}}"
ABI="${ANDROID_ABI:-arm64-v8a}"
PLATFORM="${ANDROID_PLATFORM:-android-29}"
DEST="${NXVC_ANDROID_DEST:-/data/local/tmp/nxwarp}"
JOBS="${NXVC_JOBS:-4}"

BUILD_IT=1
BUILD_ONLY=0
ATLAS=0
UNORM=""
MODE=(--verbose)
EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) BUILD_IT=0; shift ;;
    # Cross-build and STOP.  The headset is a shared resource and is often
    # held by someone else for an hour at a time; this stages the arm64
    # binaries so a row can be taken the moment it frees up, without touching
    # adb at all.  It is also the only mode that is safe to run while a live
    # session is up.
    --build-only) BUILD_ONLY=1; shift ;;
    --quick)    MODE=(--quick); shift ;;
    --bench)    MODE=(--bench "${2:-10}"); shift 2 ;;
    --unorm)    UNORM="$2"; shift 2 ;;
    --atlas)    ATLAS=1; shift ;;
    --)         shift; EXTRA=("$@"); break ;;
    *)          EXTRA+=("$1"); shift ;;
  esac
done

ADB=(adb)
[[ -n "${ANDROID_SERIAL:-}" ]] && ADB=(adb -s "$ANDROID_SERIAL")

if [[ $BUILD_ONLY -eq 0 ]] && ! "${ADB[@]}" get-state >/dev/null 2>&1; then
  echo "no adb device (set ANDROID_SERIAL to pick one of several)" >&2
  exit 77
fi

if [[ ! -d "$NDK" ]]; then
  echo "NDK not found at $NDK (set ANDROID_NDK_HOME)" >&2
  exit 77
fi

# The GPU clock and the die temperature, either side of every run.
clocks() {
  local when="$1"
  local clk temp
  clk=$("${ADB[@]}" shell "cat /sys/class/kgsl/kgsl-3d0/gpuclk" 2>/dev/null | tr -d '\r' || true)
  temp=$("${ADB[@]}" shell 'for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type 2>/dev/null); case $t in *gpuss-max-step*) cat $z/temp;; esac; done' 2>/dev/null | tr -d '\r' | head -1 || true)
  echo "== $when: gpuclk ${clk:-?} Hz, gpuss-max-step ${temp:-?} mC"
}

# Push one binary and REFUSE to launch it unless the sha256 matches.
push_checked() {
  local bin="$1" name
  name=$(basename "$bin")
  local want got
  want=$(sha256sum "$bin" | cut -d' ' -f1)
  "${ADB[@]}" push "$bin" "$DEST/" >/dev/null
  "${ADB[@]}" shell "chmod +x $DEST/$name" >/dev/null
  got=$("${ADB[@]}" shell "sha256sum $DEST/$name" 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
  if [[ "$want" != "$got" ]]; then
    echo "sha256 mismatch for $name: local $want, device ${got:-none}" >&2
    return 1
  fi
  echo "== $name sha256 $want (verified on device)"
}

# ---------------------------------------------------------------- ATLAS
# The ATLAS kernels have their own harness and no conformance vectors yet, so
# they get their own path rather than a flag on the big one.  The device leg is
# MANDATORY for them and not a formality: a subgroup scan in Pass A once
# validated byte-exactly on RADV AND lavapipe and silently miscomputed on this
# very part.
if [[ $ATLAS -eq 1 ]]; then
  if [[ $BUILD_IT -eq 1 ]]; then
    echo "== configuring $BUILD for $ABI"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="$PLATFORM" \
      -DCMAKE_BUILD_TYPE=Release \
      -DNXWARP_BUILD_VK=ON -DNXWARP_BUILD_TESTS=ON -DNXWARP_BUILD_TOOLS=ON \
      -DNXWARP_BUILD_EXAMPLES=OFF -DNXWARP_BUILD_TRANSPORT=OFF >/dev/null
    echo "== building"
    cmake --build "$BUILD" -j"$JOBS" --target nxvc-atlas-gpu-test >/dev/null
  fi
  ABIN="$BUILD/bin/nxvc-atlas-gpu-test"
  [[ -x "$ABIN" ]] || { echo "not built: $ABIN" >&2; exit 1; }
  if [[ $BUILD_ONLY -eq 1 ]]; then
    echo "== built (device untouched): $ABIN"
    echo "== sha256 $(sha256sum "$ABIN" | cut -d' ' -f1)"
    exit 0
  fi
  "${ADB[@]}" shell "mkdir -p $DEST" >/dev/null
  push_checked "$ABIN" || exit 1
  "${ADB[@]}" shell "input keyevent KEYCODE_WAKEUP; svc power stayon usb" >/dev/null
  clocks before
  set +e
  "${ADB[@]}" shell "cd $DEST && ./nxvc-atlas-gpu-test ${MODE[*]} ${EXTRA[*]:-}; echo NXVC_EXIT=\$?" \
    | tee /tmp/nxvc-atlas-run.$$
  set -e
  RC=$(grep -o 'NXVC_EXIT=[0-9]*' /tmp/nxvc-atlas-run.$$ | tail -1 | cut -d= -f2)
  rm -f /tmp/nxvc-atlas-run.$$
  clocks after
  case "${RC:-1}" in
    0)  echo "== PASS" ;;
    77) echo "== SKIP (no usable Vulkan ICD on the device)" ;;
    *)  echo "== FAIL (exit ${RC:-?})" ;;
  esac
  exit "${RC:-1}"
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

if [[ $BUILD_ONLY -eq 1 ]]; then
  echo "== built (device untouched)"
  for b in "$BIN" "$BUILD/bin/nxvc-vkdec"; do
    [[ -x "$b" ]] && echo "== $(basename "$b") sha256 $(sha256sum "$b" | cut -d' ' -f1)"
  done
  exit 0
fi

echo "== pushing to $DEST"
"${ADB[@]}" shell "mkdir -p $DEST/vectors" >/dev/null
# Every binary's sha256 is checked on the device before anything is launched.
# adb push reports success on a short write, and a run of the wrong binary is a
# result about nothing.
push_checked "$BIN" || exit 1
push_checked "$BUILD/bin/nxvc-vkdec" || exit 1
# The UNORM round-trip proof rides along when it was built; it is what says
# whether the UNORM store is allowed on this device at all.
[[ -x "$BUILD/bin/test_unorm_roundtrip" ]] &&
  { push_checked "$BUILD/bin/test_unorm_roundtrip" || exit 1; }
"${ADB[@]}" push "$ROOT/tests/vectors/." "$DEST/vectors/" >/dev/null

# The GPU parks at 305 MHz with the screen off; wake it and keep it awake.
"${ADB[@]}" shell "input keyevent KEYCODE_WAKEUP; svc power stayon usb" >/dev/null
clocks before
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

clocks after
CLK_AFTER=$("${ADB[@]}" shell "cat /sys/class/kgsl/kgsl-3d0/gpuclk" 2>/dev/null | tr -d '\r' || true)
[[ -n "$CLK_BEFORE$CLK_AFTER" ]] &&
  echo "== gpuclk ${CLK_BEFORE:-?} -> ${CLK_AFTER:-?} Hz"

case "${RC:-1}" in
  0)  echo "== PASS" ;;
  77) echo "== SKIP (no usable Vulkan ICD on the device)" ;;
  *)  echo "== FAIL (exit ${RC:-?})" ;;
esac
exit "${RC:-1}"
