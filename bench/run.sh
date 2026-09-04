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

echo "==> running: ${ARGS:-<defaults: K1..K5>}"
adb "${DEV[@]}" shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb "${DEV[@]}" shell run-as "$PKG" rm -f "$REMOTE_JSON" >/dev/null 2>&1 || true
adb "${DEV[@]}" logcat -c >/dev/null 2>&1 || true

if [ -n "$ARGS" ]; then
  adb "${DEV[@]}" shell am start -n "$PKG/$ACT" --es args "'$ARGS'" >/dev/null
else
  adb "${DEV[@]}" shell am start -n "$PKG/$ACT" >/dev/null
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
  if ! adb "${DEV[@]}" shell pidof "$PKG" >/dev/null 2>&1; then
    # Process gone: either it finished between polls or it crashed.
    if adb "${DEV[@]}" logcat -d -s nxwarp-bench 2>/dev/null | grep -q "bench done"; then
      DONE=1
    else
      echo "the app exited without finishing; last log lines:" >&2
      adb "${DEV[@]}" logcat -d -s nxwarp-bench DEBUG:E AndroidRuntime:E | tail -40 >&2
      exit 1
    fi
    break
  fi
  sleep 3
done

if [ "$DONE" != "1" ]; then
  echo "timed out waiting for the run" >&2
  adb "${DEV[@]}" logcat -d -s nxwarp-bench | tail -30 >&2
  exit 1
fi

mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOCAL_JSON="$OUT/phase0-$DEVNAME-$STAMP.json"
LOCAL_LOG="$OUT/phase0-$DEVNAME-$STAMP.log"

adb "${DEV[@]}" logcat -d -s nxwarp-bench > "$LOCAL_LOG" 2>/dev/null || true

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
