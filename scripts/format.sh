#!/usr/bin/env bash
# format.sh -- clang-format the tree, and optionally run clang-tidy.
#
#   scripts/format.sh              format every tracked C/C++ file in place
#   scripts/format.sh --check      report drift, change nothing (what CI does)
#   scripts/format.sh --staged     only what is staged, for a pre-commit hook
#   scripts/format.sh --tidy       also run clang-tidy over compile_commands.json
#   scripts/format.sh --tidy-fix   ... and apply its fixes
#
# .clang-format at the root is the only style authority. The GLSL kernels are
# deliberately excluded: clang-format does not understand layout qualifiers and
# reflows them into nonsense.
#
# tests/vectors/ is excluded too. Those files are the bitstream contract; a
# formatter must never touch them.

set -euo pipefail

cd "$(dirname "$0")/.."

MODE=fix
TIDY=0
TIDY_FIX=0
SCOPE=tracked

while [ $# -gt 0 ]; do
    case "$1" in
        --check)     MODE=check ;;
        --staged)    SCOPE=staged ;;
        --tidy)      TIDY=1 ;;
        --tidy-fix)  TIDY=1; TIDY_FIX=1 ;;
        --help|-h)   sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not found; scripts/bootstrap.sh lists how to get it" >&2
    exit 1
fi

if [ "$SCOPE" = staged ]; then
    mapfile -t FILES < <(git diff --cached --name-only --diff-filter=ACMR \
        -- '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.inc')
else
    mapfile -t FILES < <(git ls-files '*.c' '*.h' '*.cc' '*.cpp' '*.hpp' '*.inc')
fi

# Drop anything that is not ours to format.
KEEP=()
for f in "${FILES[@]:-}"; do
    [ -n "$f" ] || continue
    [ -f "$f" ] || continue
    case "$f" in
        tests/vectors/*|*/third_party/*|*/external/*|build*/*) continue ;;
    esac
    KEEP+=("$f")
done

if [ ${#KEEP[@]} -eq 0 ]; then
    echo "nothing to format"
else
    echo "clang-format: ${#KEEP[@]} files ($(clang-format --version))"
    if [ "$MODE" = check ]; then
        rc=0
        clang-format --dry-run --Werror --style=file "${KEEP[@]}" || rc=$?
        if [ "$rc" -ne 0 ]; then
            echo
            echo "Formatting drift above. Fix it with: scripts/format.sh"
            exit "$rc"
        fi
        echo "clean"
    else
        clang-format -i --style=file "${KEEP[@]}"
        if ! git diff --quiet -- "${KEEP[@]}" 2>/dev/null; then
            echo "reformatted:"
            git diff --name-only -- "${KEEP[@]}" | sed 's/^/  /'
        else
            echo "already clean"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# clang-tidy. Advisory: it needs a compile database, and a tree where half the
# components are mid-landing will have opinions about all of them.
# ---------------------------------------------------------------------------
if [ "$TIDY" -eq 1 ]; then
    echo
    if ! command -v clang-tidy >/dev/null 2>&1; then
        echo "clang-tidy not found; skipping" >&2
        exit 0
    fi

    DB=""
    for d in build-dev build-clang build-gcc build-release build; do
        if [ -f "$d/compile_commands.json" ]; then DB="$d"; break; fi
    done
    if [ -z "$DB" ]; then
        echo "no compile_commands.json found. Run:  scripts/build-all.sh dev --no-test"
        exit 1
    fi
    echo "clang-tidy: using $DB/compile_commands.json"

    ARGS=(-p "$DB" --quiet)
    [ "$TIDY_FIX" -eq 1 ] && ARGS+=(--fix --fix-errors --format-style=file)

    # Only sources that are actually in the compile database; headers are
    # reached through HeaderFilterRegex in .clang-tidy.
    SRC=()
    for f in "${KEEP[@]:-}"; do
        case "$f" in
            *.c|*.cc|*.cpp) SRC+=("$f") ;;
        esac
    done
    if [ ${#SRC[@]} -eq 0 ]; then
        echo "no translation units to check"
        exit 0
    fi

    if command -v run-clang-tidy >/dev/null 2>&1 && [ "$TIDY_FIX" -eq 0 ]; then
        run-clang-tidy -p "$DB" -quiet -j "$(nproc)" "${SRC[@]}" || true
    else
        for f in "${SRC[@]}"; do
            clang-tidy "${ARGS[@]}" "$f" || true
        done
    fi
    echo
    echo "clang-tidy output above is advisory; it is not a CI gate."
fi
