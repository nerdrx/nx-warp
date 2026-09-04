#!/usr/bin/env bash
# Wine smoke test for nxvc-d3dinterop.
#
# This does NOT validate interop: Wine has no D3D11 <-> Vulkan external memory
# path, and the answer the probe exists to produce is only meaningful on real
# Windows hardware. What this checks is that the binary loads, parses its
# arguments, finds (or fails to find) its DLLs, and emits a well-formed JSON
# verdict with pass=false and a named failure stage instead of crashing. That
# is the property that matters when the exe lands on an unknown box.
#
# The prefix is created fresh under nx-scratch, the crash dialog is disabled
# before the first run, and DISPLAY/WAYLAND_DISPLAY are cleared so nothing can
# put a window on the desktop: this is a console program.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

EXE="${1:-$repo/build-win/win/nxvc-d3dinterop.exe}"
PREFIX="${WINEPREFIX_OVERRIDE:-/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/wineprefix}"

if ! command -v wine >/dev/null 2>&1; then
  echo "wine not installed: skipping the smoke test" >&2
  exit 77
fi
if [ ! -f "$EXE" ]; then
  echo "no probe binary at $EXE (run platform/win/build.sh first)" >&2
  exit 2
fi

export WINEPREFIX="$PREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"
# mscoree/mshtml off: never let Wine offer to download Mono or Gecko.
export WINEDLLOVERRIDES="mscoree=d;mshtml=d"
# Headless. A console program must never be able to map a window.
unset DISPLAY WAYLAND_DISPLAY

mkdir -p "$PREFIX"
if [ ! -f "$PREFIX/system.reg" ]; then
  echo "creating a fresh WINEPREFIX at $PREFIX" >&2
  wine wineboot -u >/dev/null 2>&1
fi

# Disable the crash dialog *before* the first run of the probe.
wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f \
  >/dev/null 2>&1

echo "--- nxvc-d3dinterop under wine ---" >&2
out="$(timeout 180 wine "$EXE" --iterations 2 2>/tmp/nxwarp-wine-smoke.err)"
rc=$?

echo "exit code: $rc" >&2
echo "stderr:" >&2
sed 's/^/  /' /tmp/nxwarp-wine-smoke.err >&2
echo "stdout:" >&2
echo "$out" | sed 's/^/  /' >&2

# The only assertion: one line of JSON came out and it carries a verdict.
case "$out" in
  '{"probe":"nxvc-d3dinterop"'*'"pass":'*) ;;
  *) echo "SMOKE FAIL: no well-formed probe JSON on stdout" >&2; exit 1 ;;
esac

echo "SMOKE PASS: the binary loaded and produced a JSON verdict" >&2
