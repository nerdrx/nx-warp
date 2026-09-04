#!/usr/bin/env bash
# build-all.sh -- configure, build and test one preset.
#
#   scripts/build-all.sh                 # dev
#   scripts/build-all.sh asan-ubsan
#   scripts/build-all.sh release --clean
#   scripts/build-all.sh mingw-w64       # cross build, no tests (preset says so)
#   scripts/build-all.sh coverage        # + an lcov/gcovr report
#
# Options:
#   --clean         delete the build directory first
#   -j N            parallelism (default: nproc, capped at 8)
#   --no-test       configure and build only
#   --              everything after this goes to `cmake --build`
#
# Environment:
#   NXWARP_NICE=1   run the compile under SCHED_IDLE and nice 19, which is what
#                   you want on the machine you are also using for something
#                   else. Off by default: CI should get the whole core.
#
# The lavapipe ICD is located and exported before ctest, because the Vulkan
# tests are only meaningful against a known device and the path differs by
# distribution (CONTRIBUTING.md says find it, do not hard-code it).

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

PRESET="dev"
CLEAN=0
RUN_TESTS=1
JOBS=""
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)   CLEAN=1; shift ;;
        --no-test) RUN_TESTS=0; shift ;;
        -j)        JOBS="$2"; shift 2 ;;
        -j*)       JOBS="${1#-j}"; shift ;;
        --help|-h) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)        shift; EXTRA=("$@"); break ;;
        -*)        echo "unknown option: $1" >&2; exit 2 ;;
        *)         PRESET="$1"; shift ;;
    esac
done

if [ -z "$JOBS" ]; then
    JOBS="$(nproc 2>/dev/null || echo 4)"
    [ "$JOBS" -gt 8 ] && JOBS=8
fi

BUILD_DIR="$ROOT/build-$PRESET"

if ! cmake --list-presets 2>/dev/null | grep -q "\"$PRESET\""; then
    echo "no such configure preset: $PRESET" >&2
    cmake --list-presets >&2
    exit 2
fi

NICE=()
if [ "${NXWARP_NICE:-0}" = "1" ]; then
    command -v chrt >/dev/null 2>&1 && NICE+=(chrt -i 0)
    NICE+=(nice -n 19)
fi

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    say "removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

say "configure: $PRESET"
cmake --preset "$PRESET"

say "build: $PRESET (-j$JOBS)"
"${NICE[@]}" cmake --build --preset "$PRESET" --parallel "$JOBS" "${EXTRA[@]}"

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if [ "$RUN_TESTS" -eq 1 ] && ctest --list-presets 2>/dev/null | grep -q "\"$PRESET\""; then
    if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
        say "no CTest configuration in $BUILD_DIR - skipping"
    else
        for d in /usr/share/vulkan/icd.d /usr/local/share/vulkan/icd.d \
                 /etc/vulkan/icd.d "$HOME/.local/share/vulkan/icd.d"; do
            [ -d "$d" ] || continue
            icd="$(find "$d" -maxdepth 1 -name 'lvp_icd*.json' 2>/dev/null | sort | head -n1)"
            if [ -n "$icd" ]; then
                export VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd"
                say "Vulkan ICD: $icd"
                break
            fi
        done
        say "ctest: $PRESET"
        ctest --preset "$PRESET" --parallel "$JOBS"
    fi
elif [ "$RUN_TESTS" -eq 1 ]; then
    say "preset '$PRESET' has no test preset (cross build or fuzz build) - skipping ctest"
fi

# ---------------------------------------------------------------------------
# Coverage report, when that is what was asked for.
# ---------------------------------------------------------------------------
if [ "$PRESET" = "coverage" ]; then
    say "coverage report"
    if command -v gcovr >/dev/null 2>&1 && [ -n "$(find "$BUILD_DIR" -name '*.gcda' -print -quit)" ]; then
        gcovr --root "$ROOT" \
              --exclude "$ROOT/tests" --exclude "$ROOT/build.*" \
              --exclude '.*/_deps/.*' \
              --html-details "$BUILD_DIR/coverage.html" \
              --txt --print-summary \
              "$BUILD_DIR"
        echo "  html: $BUILD_DIR/coverage.html"
    elif command -v llvm-profdata >/dev/null 2>&1 && \
         [ -n "$(find "$BUILD_DIR" -name 'default.profraw' -print -quit)" ]; then
        find "$BUILD_DIR" -name '*.profraw' -print0 \
            | xargs -0 llvm-profdata merge -sparse -o "$BUILD_DIR/coverage.profdata"
        bins=()
        while IFS= read -r b; do bins+=(-object "$b"); done \
            < <(find "$BUILD_DIR/bin" -maxdepth 1 -type f -perm -u+x 2>/dev/null)
        if [ ${#bins[@]} -gt 0 ]; then
            llvm-cov report "${bins[@]}" \
                -instr-profile="$BUILD_DIR/coverage.profdata" \
                -ignore-filename-regex='(tests|build-|/usr/)'
        fi
    else
        echo "  no coverage data found. Install gcovr (gcc) or use the clang"
        echo "  compiler for this preset, and make sure the tests actually ran."
    fi
fi

say "done: $PRESET"
