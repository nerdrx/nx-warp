#!/usr/bin/env bash
# Cross-build the NX Warp Windows interop probe on Linux with llvm-mingw.
#
#   platform/win/build.sh              shared-fence build (default, paper 3.8)
#   platform/win/build.sh --keyed      keyed-mutex fallback build
#   platform/win/build.sh --clean      wipe the build dir first
#
# The build dir is <repo>/build-win (gitignored). Compiles are pinned to the
# idle scheduling class and four cores so a long build never fights the desktop.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

CMAKE="${CMAKE:-/run/media/nerdrx/Lex/claude/tools/cmake-3.31.10-linux-x86_64/bin/cmake}"
command -v "$CMAKE" >/dev/null 2>&1 || CMAKE="$(command -v cmake)"

keyed=OFF
build_dir="$repo/build-win"
clean=0
for arg in "$@"; do
  case "$arg" in
    --keyed) keyed=ON; build_dir="$repo/build-win-keyed" ;;
    --clean) clean=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

[ "$clean" = 1 ] && rm -rf "$build_dir"

# nice/chrt so a build never competes with the compositor or a VR session.
NICE=(nice -n 19)
command -v chrt >/dev/null 2>&1 && NICE=(chrt -i 0 "${NICE[@]}")
command -v taskset >/dev/null 2>&1 && NICE=(taskset -c 0-3 "${NICE[@]}")

# Extra -D flags can be passed through NXWARP_CMAKE_EXTRA (space separated).
read -r -a extra <<<"${NXWARP_CMAKE_EXTRA:-}"

"${NICE[@]}" "$CMAKE" \
  -S "$repo/platform" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$repo/platform/cmake/llvm-mingw-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DNXWARP_WIN_KEYED_MUTEX="$keyed" \
  ${extra[@]+"${extra[@]}"}

"${NICE[@]}" "$CMAKE" --build "$build_dir" -j4

echo
echo "built: $build_dir/win/nxvc-d3dinterop.exe  (interop mode: $([ "$keyed" = ON ] && echo keyed-mutex || echo shared-fence))"
