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
# argued in docs/MERGE-PLAN.md section 5 (detail, ctx, xform, inter, rdo).
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
# docs/MERGE-PLAN.md section 5.  detail goes first because JUDGE-detail.md
# landed first and fixes its bits (19 split, 24 CfL); every later package
# renumbers around them.  Then the entropy layer the rest codes through, then
# the transform, then the near-disjoint inter package, then the encoder-only
# rdo package last so vectors are regenerated exactly once.
order_key() {
    case "$1" in
        detail-*) echo 1 ;;
        ctx-*)    echo 2 ;;
        xform-*)  echo 3 ;;
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
# Every branch already merged is the *resume* case, not an error: the operator
# hand-resolved a step, committed it, and re-ran with the same winner list as
# the failure message says to.  Fall through to the renumber, regenerate and
# test phases, which still have to happen.
SORTED=()
if [ ${#WINNERS[@]} -gt 0 ]; then
    mapfile -t SORTED < <(printf '%s\n' "${WINNERS[@]}" | sort -n | awk '{print $2}')
else
    info "every branch given is already merged; resuming at the post-merge phases"
fi

say "merge order: ${SORTED[*]:-(none, resuming)}"
info "onto branch: $BRANCH"
info "cpu: $NICE, -j$JOBS"

# ------------------------------------------------------------------- merging
for b in ${SORTED[@]+"${SORTED[@]}"}; do
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
              ref/src/transform.*) msgs+=("$f -- DECIDED, see MERGE-PLAN 4.4: ONE family fdct_2d(n)/idct_2d(n)"
                                          "     for n in {4,8,16,32} on xform-b's recursion.  Invariant: 2D gain"
                                          "     exactly 2^20 at every size, one qstep table, one weighting rule"
                                          "     w_N[u][v] = w_8[u>>s][v>>s].  detail-a's 4x4 constants stay only if"
                                          "     they already meet it, else rescale.  A 2x scale slip does NOT fail"
                                          "     loudly -- it shifts effective QP by 6 -- so the ctest checking every"
                                          "     size's 2D gain against a float DCT to 0.1% is MANDATORY.") ;;
              ref/src/codec.cpp|ref/src/entropy.*) msgs+=("$f -- DECIDED, see MERGE-PLAN 4.5: merge to"
                                          "     uc.level(scan_pos, prev_class, band_min), UnitCtx carrying ucls,"
                                          "     ctx_v3 AND band_min.  CTX_V3 conditions per CODING UNIT (the 8x8"
                                          "     coefficient group), NEVER per transform block: a 32x32 block is"
                                          "     sixteen units, each conditioned on the previous unit its own rANS"
                                          "     lane decoded, exactly as 8x8 blocks are.") ;;
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

# Commit the renumber immediately.  If a later step fails -- and the build step
# is exactly where a half-merged tool shows up -- an uncommitted renumber makes
# the tree dirty, and the dirty-tree guard then refuses the re-run the failure
# message just told the operator to do.
if ! git diff --quiet; then
    git add -A include/nxvc/nxvc.h python/src/nxvc/_ffi.py docs/SYNTAX.md 2>/dev/null
    git commit -q -m "tools: apply the docs/TOOLBITS.md bit allocation

Every tournament branch claimed bit 24; this moves each tool to its allocated
slot and bumps the bitstream minor.  Generated by scripts/retool-bits.py.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>" \
        && info "renumber committed"
fi

# --------------------------------------------------------------------- build
step=$((step+1))
say "step $step: configure + build ($PRESET, -j$JOBS, cores $CORES)"
$NICE $CMAKE --preset $PRESET -DNXWARP_BUILD_VK=OFF >/dev/null 2>&1 \
    || die "cmake configure failed" "Re-run without the redirect to see it."
if ! $NICE $CMAKE --build --preset $PRESET -j"$JOBS" 2>&1 | tail -40; then
    die "build failed" \
        "The most likely cause is docs/MERGE-PLAN.md 4.4: xform's fdct_2d(n,...)" \
        "and detail's fdct8x8 cannot both exist.  The decision is ONE family over" \
        "n in {4,8,16,32} with a 2D gain of exactly 2^20 at every size; unify them" \
        "onto it, and do not skip the 0.1% gain ctest -- a 2x scale slip is silent" \
        "and shifts the effective QP by 6."
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
# The vector name is the FIRST field of each non-comment line in both lists.
# Reading the last field instead yields a list of md5s, every vector then looks
# orphaned, and the cleanup deletes the entire conformance suite -- which is
# exactly what happened before this comment existed.
mapfile -t live < <(awk '!/^#/ && NF {print $1}' \
    tests/vectors/vectors.md5 tests/vectors/rejects.md5 2>/dev/null)
if [ ${#live[@]} -lt 2 ]; then
    die "could not read the vector name lists" \
        "tests/vectors/vectors.md5 and rejects.md5 must exist and be non-empty;" \
        "refusing to run the orphan cleanup against an empty list."
fi
stale=0
for f in tests/vectors/*.nxv; do
    b=$(basename "$f")
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

Generated by nxv-vectors --generate after merging the tournament winners.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
    info "committed"
fi

say "done: vectors regenerated, ref.* and fuzz.* green"
