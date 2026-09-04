#!/usr/bin/env bash
# release.sh -- cut a release of nxvc.
#
#   scripts/release.sh 1.2.0              DRY RUN. Checks, builds, packs, and
#                                         prints exactly what it would do.
#   scripts/release.sh 1.2.0 --publish    Actually tag, push the tag, and draft
#                                         a GitHub release.
#
# The default is a dry run on purpose. A release is the one operation in this
# repository that is visible outside it and awkward to undo, so it takes an
# explicit flag, every time, from a human.
#
# --publish does, in order:
#   1. all the dry-run checks, again
#   2. git tag -a vX.Y.Z
#   3. git push origin vX.Y.Z
#   4. gh release create vX.Y.Z --draft   with the artifacts attached
#
# It creates a DRAFT. Nothing is public until someone opens the release on
# GitHub, reads the notes and presses the button. See RELEASE.md for the
# checklist that belongs around this script.
#
# Options:
#   --publish            do it for real (default: dry run)
#   --skip-tests         build and package but do not run ctest (not advised)
#   --allow-dirty        proceed with uncommitted changes (never with --publish)
#   --notes FILE         release notes body; default is generated from the log
#   --preset NAME        build preset to package from (default: release)

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

VERSION=""
PUBLISH=0
SKIP_TESTS=0
ALLOW_DIRTY=0
NOTES_FILE=""
PRESET="release"

while [ $# -gt 0 ]; do
    case "$1" in
        --publish)     PUBLISH=1; shift ;;
        --skip-tests)  SKIP_TESTS=1; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        --notes)       NOTES_FILE="$2"; shift 2 ;;
        --preset)      PRESET="$2"; shift 2 ;;
        --help|-h)     sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)            echo "unknown option: $1" >&2; exit 2 ;;
        *)             VERSION="$1"; shift ;;
    esac
done

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
plan() { printf '\033[36m   would run:\033[0m %s\n' "$*"; }
ok()   { printf '   \033[32mok\033[0m  %s\n' "$*"; }

[ -n "$VERSION" ] || die "no version given.  usage: scripts/release.sh X.Y.Z [--publish]"
VERSION="${VERSION#v}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] \
    || die "'$VERSION' is not SemVer X.Y.Z[-prerelease]"
TAG="v$VERSION"

if [ "$PUBLISH" -eq 1 ]; then
    say "RELEASE $TAG  (publishing)"
else
    say "RELEASE $TAG  (dry run -- pass --publish to do it for real)"
fi

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
say "checks"

git rev-parse --git-dir >/dev/null 2>&1 || die "not a git repository"

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$BRANCH" != "main" ]; then
    if [ "$PUBLISH" -eq 1 ]; then
        die "on branch '$BRANCH'; releases are cut from main"
    fi
    printf '   \033[33mwarn\033[0m on branch %s, not main\n' "$BRANCH"
else
    ok "on main"
fi

if ! git diff --quiet HEAD 2>/dev/null || [ -n "$(git status --porcelain)" ]; then
    if [ "$PUBLISH" -eq 1 ]; then
        die "working tree is dirty. A tag must name a commit that exists."
    elif [ "$ALLOW_DIRTY" -eq 0 ]; then
        printf '   \033[33mwarn\033[0m working tree is dirty (fine for a dry run)\n'
    fi
else
    ok "working tree clean"
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    die "tag $TAG already exists. Releases are immutable; pick the next version."
fi
ok "tag $TAG is free"

if [ -f RELEASE.md ] && ! grep -q "$VERSION" RELEASE.md 2>/dev/null; then
    printf '   \033[33mwarn\033[0m RELEASE.md does not mention %s\n' "$VERSION"
fi

if [ "$PUBLISH" -eq 1 ]; then
    command -v gh >/dev/null 2>&1 || die "gh not found; --publish needs the GitHub CLI"
    gh auth status >/dev/null 2>&1 || die "gh is not authenticated (gh auth login)"
    ok "gh authenticated"
fi

# ---------------------------------------------------------------------------
# Format
# ---------------------------------------------------------------------------
say "format check"
if [ -x scripts/format.sh ]; then
    if scripts/format.sh --check >/dev/null 2>&1; then
        ok "clang-format clean"
    else
        printf '   \033[33mwarn\033[0m clang-format reports drift (advisory, see CONTRIBUTING.md)\n'
    fi
fi

# ---------------------------------------------------------------------------
# Build and test
# ---------------------------------------------------------------------------
BUILD_DIR="$ROOT/build-$PRESET"

say "build: preset $PRESET"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET" --parallel "$(nproc 2>/dev/null || echo 4)"
ok "built into $BUILD_DIR"

if [ "$SKIP_TESTS" -eq 0 ]; then
    say "tests"
    if [ -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
        ctest --preset "$PRESET"
        ok "ctest passed"
    else
        printf '   \033[33mwarn\033[0m no tests registered yet\n'
    fi
else
    printf '   \033[33mwarn\033[0m tests skipped by request\n'
fi

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------
say "package"
( cd "$BUILD_DIR" && cpack )
( cd "$BUILD_DIR" && cpack --config CPackSourceConfig.cmake ) || \
    printf '   \033[33mwarn\033[0m source package failed\n'

mapfile -t ARTIFACTS < <(find "$BUILD_DIR" -maxdepth 1 -type f \
    \( -name 'nxvc-*.tar.gz' -o -name 'nxvc-*.zip' -o -name 'nxvc*.deb' \) | sort)

if [ ${#ARTIFACTS[@]} -eq 0 ]; then
    die "cpack produced nothing in $BUILD_DIR"
fi
for a in "${ARTIFACTS[@]}"; do
    ok "$(basename "$a")  ($(du -h "$a" | cut -f1))"
done

# Checksums travel with the release; a codec artifact nobody can verify is a
# codec artifact nobody should run.
SUMS="$BUILD_DIR/SHA256SUMS"
( cd "$BUILD_DIR" && sha256sum "$(basename -a "${ARTIFACTS[@]}")" > SHA256SUMS )
ok "SHA256SUMS"

# ---------------------------------------------------------------------------
# Notes
# ---------------------------------------------------------------------------
say "release notes"
NOTES="$BUILD_DIR/release-notes-$TAG.md"
if [ -n "$NOTES_FILE" ]; then
    cp "$NOTES_FILE" "$NOTES"
else
    PREV="$(git describe --tags --abbrev=0 2>/dev/null || true)"
    {
        echo "## nxvc $TAG"
        echo
        echo "Bitstream version: $(grep -oP 'NXWARP_BITSTREAM_VERSION \K[0-9]+' \
            cmake/nxwarp_version.cmake | head -n1 || echo '?')"
        echo
        if [ -n "$PREV" ]; then
            echo "### Changes since $PREV"
            echo
            git log --no-merges --pretty='- %s' "$PREV..HEAD"
        else
            echo "### Changes"
            echo
            git log --no-merges --pretty='- %s' -n 50
        fi
        echo
        echo "### Artifacts"
        echo
        for a in "${ARTIFACTS[@]}"; do echo "- \`$(basename "$a")\`"; done
        echo "- \`SHA256SUMS\`"
    } > "$NOTES"
fi
ok "$NOTES"

# ---------------------------------------------------------------------------
# Tag and publish
# ---------------------------------------------------------------------------
say "tag and publish"

if [ "$PUBLISH" -eq 0 ]; then
    plan "git tag -a $TAG -m 'nxvc $TAG'"
    plan "git push origin $TAG"
    plan "gh release create $TAG --draft --title 'nxvc $TAG' --notes-file $NOTES ${ARTIFACTS[*]} $SUMS"
    echo
    echo "Dry run complete. Nothing was tagged, pushed or published."
    echo "Read $NOTES, then re-run with --publish."
    exit 0
fi

git tag -a "$TAG" -m "nxvc $TAG"
ok "tagged $TAG"

git push origin "$TAG"
ok "pushed $TAG"

gh release create "$TAG" \
    --draft \
    --title "nxvc $TAG" \
    --notes-file "$NOTES" \
    "${ARTIFACTS[@]}" "$SUMS"

say "DRAFT release created for $TAG"
echo "It is a draft. Open it on GitHub, read the notes, and publish it there."
