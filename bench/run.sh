#!/usr/bin/env bash
# NX Warp Phase 0 gate, on-device.
#
# Builds the APK, installs it on the adb device named by $ANDROID_SERIAL (or
# the first device attached), runs the benchmark, pulls the result JSON and
# prints the PAPER section 3.4 table with PASS/FAIL and the decision rule.
#
#   ./run.sh                       # K1..K5, 120 warm-up + 600 measured each
#   ./run.sh --kernels k5          # just the gate kernel
#   ./run.sh --kernels all         # include K6 hybrid
#   ./run.sh --thermal 600         # the 10-minute thermal run
#   ./run.sh --selftest            # bit-exactness against the CPU reference
#
# Everything after the options is passed through to the app as intent extras.
set -euo pipefail

cd "$(dirname "$0")"
HERE="$(pwd)"

PKG=org.nxwarp.bench
ACT=android.app.NativeActivity
APK=build/outputs/apk/debug/bench-debug.apk
REMOTE_JSON="files/nxwarp-phase0.json"
OUT="${NXB_OUT:-$HERE/results}"

# --- toolchain, no sudo on this machine
export JAVA_HOME="${JAVA_HOME:-/run/media/nerdrx/Lex/claude/tools/jdk-21.0.12+8}"
export ANDROID_HOME="${ANDROID_HOME:-/run/media/nerdrx/Lex/claude/tools/android-sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"

# Every compile stays out of the user's way: idle scheduling class, four cores.
NICE=(chrt -i 0 taskset -c 20-23 nice -n 19)
if ! command -v chrt >/dev/null 2>&1; then NICE=(nice -n 19); fi

ARGS="$*"
SKIP_BUILD="${NXB_SKIP_BUILD:-0}"

# --- device selection
if [ -n "${ANDROID_SERIAL:-}" ]; then
  DEV=(-s "$ANDROID_SERIAL")
  DEVNAME="$ANDROID_SERIAL"
else
  DEVNAME="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
  if [ -z "$DEVNAME" ]; then
    echo "no adb device attached (and ANDROID_SERIAL is unset)" >&2
    echo "attached:" >&2
    adb devices -l >&2
    exit 1
  fi
  DEV=(-s "$DEVNAME")
fi
echo "device: $DEVNAME"

# --- local.properties is gitignored; regenerate it so a fresh clone builds
if [ ! -f local.properties ]; then
  echo "sdk.dir=$ANDROID_HOME" > local.properties
fi

# K6 needs an HEVC base layer in the APK assets. The binary is not in git.
if [ ! -s assets/base.hevc ]; then
  echo "==> generating the K6 base layer asset"
  ./gen-asset.sh || echo "    (continuing without it: K6 Pass C uses a synthetic base)"
fi

if [ "$SKIP_BUILD" != "1" ]; then
  echo "==> building APK"
  "${NICE[@]}" ./gradlew --no-daemon assembleDebug
fi
[ -f "$APK" ] || { echo "APK not found at $APK" >&2; exit 1; }

echo "==> installing"
adb "${DEV[@]}" install -r -g "$APK" >/dev/null

# The gate wants the display active for the whole run (PAPER 3.4), and a
# 10-minute thermal pass outlives any sane screen timeout.
adb "${DEV[@]}" shell svc power stayon usb >/dev/null 2>&1 || true

mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOCAL_JSON="$OUT/phase0-$DEVNAME-$STAMP.json"
LOCAL_LOG="$OUT/phase0-$DEVNAME-$STAMP.log"

# Saves whatever the device can still tell us, on every exit path. Takes the
# crash buffer too: a native abort leaves nothing under our own log tag.
save_logs() {
  if [ "$(adb "${DEV[@]}" get-state 2>/dev/null)" = "device" ]; then
    {
      echo "--- logcat: nxwarp-bench ---"
      adb "${DEV[@]}" logcat -d -s nxwarp-bench 2>/dev/null || true
      echo
      echo "--- logcat: crash buffer ---"
      adb "${DEV[@]}" logcat -d -b crash 2>/dev/null | tail -100 || true
      echo
      echo "--- logcat: anything mentioning the package or Vulkan ---"
      adb "${DEV[@]}" logcat -d 2>/dev/null \
        | grep -iE "nxwarp|org\.nxwarp|vulkan|adreno|mali|DEBUG|AndroidRuntime|libc" \
        | tail -200 || true
    } > "$LOCAL_LOG" 2>&1
  fi
}

echo "==> running: ${ARGS:-<defaults: K1..K5>}"
adb "${DEV[@]}" shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb "${DEV[@]}" shell run-as "$PKG" rm -f "$REMOTE_JSON" >/dev/null 2>&1 || true
adb "${DEV[@]}" logcat -c >/dev/null 2>&1 || true
adb "${DEV[@]}" logcat -c -b crash >/dev/null 2>&1 || true

# am start reports failures on stdout with a zero exit code, so it has to be
# read rather than trusted.
if [ -n "$ARGS" ]; then
  START_OUT="$(adb "${DEV[@]}" shell am start -n "$PKG/$ACT" --es args "'$ARGS'" 2>&1)"
else
  START_OUT="$(adb "${DEV[@]}" shell am start -n "$PKG/$ACT" 2>&1)"
fi
if echo "$START_OUT" | grep -qiE "error|exception"; then
  echo "am start failed:" >&2
  echo "$START_OUT" >&2
  exit 1
fi

# --- wait for the run to finish, echoing progress
echo "==> waiting for the run to finish (Ctrl-C to give up; the app keeps going)"
DEADLINE=$(( $(date +%s) + ${NXB_TIMEOUT:-3600} ))
DONE=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if adb "${DEV[@]}" logcat -d -s nxwarp-bench 2>/dev/null | grep -q "bench done"; then
    DONE=1
    break
  fi
  # A disconnected device is not a crashed app. Distinguish them, or every
  # unplug looks like a kernel bug.
  STATE="$(adb "${DEV[@]}" get-state 2>/dev/null || true)"
  if [ "$STATE" != "device" ]; then
    echo "device $DEVNAME went away mid-run (adb state: '${STATE:-offline}')" >&2
    echo "nothing was measured; re-attach it and run again" >&2
    exit 2
  fi

  if ! adb "${DEV[@]}" shell pidof "$PKG" >/dev/null 2>&1; then
    # Process gone: either it finished between polls or it died.
    if adb "${DEV[@]}" logcat -d -s nxwarp-bench 2>/dev/null | grep -q "bench done"; then
      DONE=1
    else
      echo "the app exited without finishing" >&2
      save_logs
      echo "diagnostics saved to $LOCAL_LOG" >&2
      sed -n '1,60p' "$LOCAL_LOG" >&2 2>/dev/null || true
      exit 1
    fi
    break
  fi
  sleep 3
done

if [ "$DONE" != "1" ]; then
  echo "timed out waiting for the run" >&2
  save_logs
  echo "diagnostics saved to $LOCAL_LOG" >&2
  exit 1
fi

save_logs

# Debug APK, so run-as reaches the app's files dir without root.
if ! adb "${DEV[@]}" exec-out run-as "$PKG" cat "$REMOTE_JSON" > "$LOCAL_JSON" 2>/dev/null \
   || [ ! -s "$LOCAL_JSON" ]; then
  echo "could not pull $REMOTE_JSON via run-as; trying the app's external path" >&2
  adb "${DEV[@]}" exec-out cat "/sdcard/Android/data/$PKG/files/nxwarp-phase0.json" \
      > "$LOCAL_JSON" 2>/dev/null || true
fi
if [ ! -s "$LOCAL_JSON" ]; then
  echo "no result JSON was produced; see $LOCAL_LOG" >&2
  exit 1
fi

echo "==> json: $LOCAL_JSON"
echo "==> log : $LOCAL_LOG"

python3 "$HERE/report.py" "$LOCAL_JSON"
