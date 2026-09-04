#!/usr/bin/env bash
# NX Warp Phase 0 kernels on the host, headless.
#
# Same kernel code as the APK, no swapchain, no Android. Use it to iterate on
# the shaders without a phone. The Phase 0 gate itself is the Pico 4; these
# numbers are a regression signal, not the verdict.
#
#   ./run-host.sh                    # K1..K5 on the default ICD
#   ./run-host.sh --selftest         # bit-exactness against the CPU reference
#   ./run-host.sh --kernels k5       # just the gate kernel
#   NXB_LAVAPIPE=1 ./run-host.sh     # force the CPU ICD if one is installed
set -euo pipefail

cd "$(dirname "$0")"
HERE="$(pwd)"

BUILD="${NXB_BUILD_DIR:-$HERE/build-host}"
OUT="${NXB_OUT:-$HERE/results}"

CMAKE="${CMAKE:-/run/media/nerdrx/Lex/claude/tools/cmake-3.31.10-linux-x86_64/bin/cmake}"
command -v "$CMAKE" >/dev/null 2>&1 || CMAKE=cmake

NICE=(chrt -i 0 taskset -c 20-23 nice -n 19)
if ! command -v chrt >/dev/null 2>&1; then NICE=(nice -n 19); fi

# --- lavapipe for CI, where there is no GPU. Only forced when asked for or
# when nothing else is installed.
pick_lavapipe() {
  for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d "$HOME/.local/share/vulkan/icd.d"; do
    for f in "$d"/lvp_icd.*.json "$d"/lavapipe*.json; do
      [ -f "$f" ] && { echo "$f"; return 0; }
    done
  done
  return 1
}

if [ "${NXB_LAVAPIPE:-0}" = "1" ]; then
  if ICD="$(pick_lavapipe)"; then
    export VK_ICD_FILENAMES="$ICD"
    export VK_DRIVER_FILES="$ICD"
    echo "using lavapipe: $ICD"
  else
    echo "NXB_LAVAPIPE=1 but no lavapipe ICD is installed; using the default loader" >&2
  fi
elif [ -z "${VK_ICD_FILENAMES:-}${VK_DRIVER_FILES:-}" ]; then
  # No GPU ICD at all: fall back to lavapipe so CI still runs the kernels.
  if ! ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
    if ICD="$(pick_lavapipe)"; then
      export VK_ICD_FILENAMES="$ICD"
      export VK_DRIVER_FILES="$ICD"
      echo "no GPU ICD found; using lavapipe: $ICD"
    fi
  fi
fi

echo "==> configuring"
"${NICE[@]}" "$CMAKE" -S "$HERE" -B "$BUILD" -GNinja -DCMAKE_BUILD_TYPE=Release >/dev/null

echo "==> building"
"${NICE[@]}" "$CMAKE" --build "$BUILD" -j4

mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"
JSON="$OUT/phase0-host-$STAMP.json"

# --selftest prints its own verdict and produces no JSON.
for a in "$@"; do
  if [ "$a" = "--selftest" ] || [ "$a" = "--info" ]; then
    exec "${NICE[@]}" "$BUILD/nxbench-host" "$@"
  fi
done

echo "==> running"
"${NICE[@]}" "$BUILD/nxbench-host" --out "$JSON" "$@"

echo "==> json: $JSON"
python3 "$HERE/report.py" "$JSON"
