#!/bin/bash
# tourney-merge.sh <winner> [<winner> ...]
#
# Merge the winning tournament branches onto the current branch, apply the
# global tool-bit renumbering from docs/TOOLBITS.md, regenerate the conformance
# vectors, and run the ref.* and fuzz.* suites under the asan-ubsan preset.
# Stops at the first failure with a message saying what to do about it.
#
#   scripts/tourney-merge.sh xform-b detail-a ctx-b inter-a rdo-b
#
# Winners are given in any order; the script sorts them into the merge order
# argued in docs/MERGE-PLAN.md section 5 (ctx, xform, detail, inter, rdo).
#
# It will NOT run on merge-main or main: run it on a scratch branch.
#
# CPU: every compile and test is pinned per the tournament throttle --
# chrt -i 0, cores 20-23, nice 19, -j2, one build at a time.  Cores 0-15 are
# the user's desktop and are never touched.

set -u -o pipefail

CORES=${NXW_CORES:-20-23}
JOBS=${NXW_JOBS:-2}
NICE="chrt -i 0 taskset -c $CORES nice -n 19"
CMAKE=${NXW_CMAKE:-/run/media/nerdrx/Lex/claude/tools/cmake-3.31.10-linux-x86_64/bin/cmake}
CTEST=${NXW_CTEST:-/run/media/nerdrx/Lex/claude/tools/cmake-3.31.10-linux-x86_64/bin/ctest}
PRESET=asan-ubsan
ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "not a git worktree"; exit 2; }
cd "$ROOT" || exit 2

step=0
say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
info() { printf '   %s\n' "$*"; }
die()  {
    printf '\n\033[1;31mFAILED at step %s: %s\033[0m\n' "$step" "$1" >&2
    shift
    for l in "$@"; do printf '   %s\n' "$l" >&2; done
    exit 1
}

# ---------------------------------------------------------------- guard rails
BRANCH=$(git rev-parse --abbrev-ref HEAD)
case "$BRANCH" in
    main|merge-main)
        die "refusing to run on '$BRANCH'" \
            "This script rewrites the tool-bit allocation and regenerates every" \
            "conformance vector.  Make a scratch branch first:" \
            "    git checkout -b integ-scratch merge-main" ;;
esac
if ! git diff --quiet || ! git diff --cached --quiet; then
    die "the working tree is dirty" \
        "Commit or stash first; the script needs a clean index to detect conflicts."
fi
[ $# -ge 1 ] || die "no winners given" "usage: scripts/tourney-merge.sh <winner> [<winner> ...]"

# --------------------------------------------------------------- merge order
# docs/MERGE-PLAN.md section 5: entropy first, then the transform it codes
# through, then detail, then the near-disjoint inter package, then the
# encoder-only rdo package last so vectors are regenerated exactly once.
order_key() {
    case "$1" in
        ctx-*)    echo 1 ;;
        xform-*)  echo 2 ;;
        detail-*) echo 3 ;;
        inter-*)  echo 4 ;;
        rdo-*)    echo 5 ;;
        percept|sparse) echo 9 ;;
        *)        echo 8 ;;
    esac
}

WINNERS=()
for w in "$@"; do
    w=${w#tourney/}
    if ! git rev-parse --verify -q "tourney/$w" >/dev/null; then
        die "no such branch tourney/$w" "Known: $(git for-each-ref --format='%(refname:short)' refs/heads/tourney | tr '\n' ' ')"
    fi
    # percept/sparse are already ancestors of merge-main; merging them is a no-op.
    if git merge-base --is-ancestor "tourney/$w" HEAD 2>/dev/null; then
        info "skipping tourney/$w: already an ancestor of $BRANCH (no-op merge)"
        continue
    fi
    WINNERS+=("$(order_key "$w") $w")
done
[ ${#WINNERS[@]} -gt 0 ] || die "every branch given is already merged" "Nothing to do."
mapfile -t SORTED < <(printf '%s\n' "${WINNERS[@]}" | sort -n | awk '{print $2}')

say "merge order: ${SORTED[*]}"
info "onto branch: $BRANCH"
info "cpu: $NICE, -j$JOBS"

# ------------------------------------------------------------------- merging
for b in "${SORTED[@]}"; do
    step=$((step+1))
    say "step $step: git merge tourney/$b"
    if git merge --no-edit --no-ff "tourney/$b" >/dev/null 2>&1; then
        info "clean"
        continue
    fi

    conflicts=$(git diff --name-only --diff-filter=U)
    info "conflicts:"; printf '     %s\n' $conflicts

    # -- scripted resolution 1: the quality harness (docs/MERGE-PLAN.md 2).
    if grep -qx 'tools/quality/compare.py' <<<"$conflicts"; then
        python3 scripts/resolve-compare-py.py tools/quality/compare.py \
            || die "resolve-compare-py.py failed" "Resolve tools/quality/compare.py by hand."
        git add tools/quality/compare.py
    fi

    # -- scripted resolution 2: conformance vectors are regenerated, never
    #    merged (docs/MERGE-PLAN.md 4.3).  Clear them from the index now and
    #    let the regeneration pass below be the authority.
    for f in $conflicts; do
        case "$f" in
            tests/vectors/*.nxv|tests/vectors/vectors.md5|tests/vectors/rejects.md5)
                git checkout --theirs -- "$f" 2>/dev/null || git checkout --ours -- "$f" 2>/dev/null
                git add "$f" ;;
        esac
    done

    # -- anything still conflicted needs a human; say precisely which class.
    left=$(git diff --name-only --diff-filter=U)
    if [ -n "$left" ]; then
        msgs=()
        for f in $left; do
            case "$f" in
              docs/SYNTAX.md)      msgs+=("$f -- three colliding tables + section numbering: docs/MERGE-PLAN.md 4.1") ;;
              include/nxvc/nxvc.h) msgs+=("$f -- union NXVC_TOOLS_SUPPORTED, renumber per docs/TOOLBITS.md 2: MERGE-PLAN 4.2") ;;
              python/src/nxvc/_ffi.py) msgs+=("$f -- Tool.<NAME>, the names table AND Tool.RESERVED_FROM: MERGE-PLAN 4.2") ;;
              ref/src/transform.*) msgs+=("$f -- SEMANTIC: fold detail's 4x4 into xform's fdct_2d/idct_2d family: MERGE-PLAN 4.4") ;;
              tests/ref/vectors.cpp) msgs+=("$f -- keep both branches' vectors, renumber so no v57 repeats: MERGE-PLAN 4.3") ;;
              *)                   msgs+=("$f") ;;
            esac
        done
        die "tourney/$b left $(wc -w <<<"$left") file(s) conflicted" \
            "${msgs[@]}" \
            "" \
            "Resolve, 'git add' them, then 'git commit --no-edit' and re-run with the" \
            "remaining winners: scripts/tourney-merge.sh ${SORTED[*]}" \
            "(already-merged branches are detected and skipped)."
    fi

    git commit --no-edit -q || die "commit of tourney/$b failed"
    info "resolved and committed"
done

# -------------------------------------------------- tool-bit renumbering pass
step=$((step+1))
say "step $step: apply the docs/TOOLBITS.md bit allocation"
python3 scripts/retool-bits.py \
    || die "retool-bits.py failed" "Fix include/nxvc/nxvc.h and python/src/nxvc/_ffi.py by hand."

# --------------------------------------------------------------------- build
step=$((step+1))
say "step $step: configure + build ($PRESET, -j$JOBS, cores $CORES)"
$NICE $CMAKE --preset $PRESET -DNXWARP_BUILD_VK=OFF >/dev/null 2>&1 \
    || die "cmake configure failed" "Re-run without the redirect to see it."
if ! $NICE $CMAKE --build --preset $PRESET -j"$JOBS" 2>&1 | tail -40; then
    die "build failed" \
        "The most likely cause is docs/MERGE-PLAN.md 4.4: xform's fdct_2d(n,...)" \
        "and detail's fdct8x8 cannot both exist.  Unify them before re-running."
fi

# --------------------------------------------------------- regenerate vectors
step=$((step+1))
say "step $step: regenerate conformance vectors and rejects"
GEN=$(find "build-$PRESET" -name nxv-vectors -type f | head -1)
[ -n "$GEN" ] || die "nxv-vectors was not built" "Expected it under build-$PRESET/."
$NICE "$GEN" --generate tests/vectors \
    || die "nxv-vectors --generate failed" \
           "A generator crash here is a real encoder bug, not a merge artifact."
info "regenerated: $(git diff --name-only tests/vectors | wc -l) file(s) changed"

# Renumbering two branches' vectors onto one sequence (docs/MERGE-PLAN.md 4.3)
# orphans the blobs that carried the old names.  They are still tracked, still
# decode, and are no longer named by either md5 list -- so ref.vectors passes
# while the tree carries dead conformance vectors.  Drop them.
mapfile -t live < <(awk '{print $NF}' tests/vectors/vectors.md5 tests/vectors/rejects.md5 2>/dev/null | xargs -rn1 basename)
stale=0
for f in tests/vectors/*.nxv; do
    b=$(basename "$f")
    printf '%s\n' "${live[@]}" | grep -qx -- "$b" && continue
    printf '%s\n' "${live[@]}" | grep -qx -- "${b%.nxv}" && continue
    info "dropping orphaned vector $b"
    git rm -q --ignore-unmatch -- "$f" || rm -f "$f"
    stale=$((stale+1))
done
[ "$stale" -eq 0 ] || info "dropped $stale orphaned vector(s)"

# ---------------------------------------------------------------------- test
step=$((step+1))
say "step $step: ctest ref.* and fuzz.* under $PRESET"
if ! $NICE $CTEST --preset $PRESET -R '^(ref|fuzz)\.' -j"$JOBS" 2>&1 | tail -40; then
    die "conformance tests failed" \
        "Baseline on merge-main is 17/17.  A ref.vectors failure right after a" \
        "regeneration means the generator and the decoder disagree -- a real bug." \
        "A ref.codec/ref.inter failure is usually a half-merged tool bit."
fi

step=$((step+1))
say "step $step: commit the regenerated vectors"
if git diff --quiet tests/vectors; then
    info "vectors unchanged"
else
    git add tests/vectors
    git commit -q -m "tests: regenerate conformance vectors for the merged tool set

Generated by nxv-vectors --generate after merging ${SORTED[*]}.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
    info "committed"
fi

say "done: ${SORTED[*]} merged, vectors regenerated, ref.* and fuzz.* green"
