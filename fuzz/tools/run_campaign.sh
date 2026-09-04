#!/bin/sh
# Bounded fuzz campaign: run every libFuzzer target for SECONDS.
#
#   fuzz/tools/run_campaign.sh <build-dir> <out-dir> [seconds]
#
# The working corpus lives under <out-dir>, never in the source tree: seeds from
# fuzz/corpus/ and reproducers from fuzz/regressions/ are copied in, and nothing
# is written back, so the checked-in corpus stays small and curated.
# Crashers land in <out-dir>/artifacts/<target>/.
#
# Scheduling, for a workstation that is doing something else at the same time:
#   NXFUZZ_CPUS=12-15   pin to those CPUs (taskset) -- unset means no pinning,
#                       which is what CI wants on a 2-core runner
#   NXFUZZ_NICE=19      nice level, default 19; empty disables
#   NXFUZZ_IDLE=1       run at SCHED_IDLE via chrt, default on when available
#   NXFUZZ_WORKERS=4    libFuzzer -jobs/-workers, default 4
set -eu

BUILD=${1:?usage: run_campaign.sh <build-dir> <out-dir> [seconds]}
OUT=${2:?usage: run_campaign.sh <build-dir> <out-dir> [seconds]}
SECS=${3:-300}
SRC=$(cd "$(dirname "$0")/.." && pwd)
WORKERS=${NXFUZZ_WORKERS:-4}

# ---------------------------------------------------------------------------
# Build the scheduling prefix from what this machine actually has.  A hard-coded
# `taskset -c 12-15` fails outright on a 2-core CI runner, so every part of the
# prefix is probed before it is used.
# ---------------------------------------------------------------------------
PREFIX=""
if [ "${NXFUZZ_IDLE:-1}" = "1" ] && command -v chrt >/dev/null 2>&1; then
    if chrt -i 0 true >/dev/null 2>&1; then PREFIX="chrt -i 0"; fi
fi
if [ -n "${NXFUZZ_CPUS:-}" ] && command -v taskset >/dev/null 2>&1; then
    if taskset -c "$NXFUZZ_CPUS" true >/dev/null 2>&1; then
        PREFIX="$PREFIX taskset -c $NXFUZZ_CPUS"
    else
        echo "note: NXFUZZ_CPUS=$NXFUZZ_CPUS is not usable here, not pinning"
    fi
fi
if [ -n "${NXFUZZ_NICE-19}" ] && command -v nice >/dev/null 2>&1; then
    PREFIX="$PREFIX nice -n ${NXFUZZ_NICE:-19}"
fi
echo "scheduling prefix: ${PREFIX:-none}; ${WORKERS} worker(s); ${SECS}s per target"

mkdir -p "$OUT"
rc=0
for t in nxvc_decode_fuzz nxvc_headers_fuzz nxvc_rans_fuzz \
         transport_depacketize_fuzz transport_rs_fuzz \
         transport_feedback_fuzz warp_tile_fuzz; do
    bin="$BUILD/bin/$t"
    [ -x "$bin" ] || bin=$(find "$BUILD" -type f -perm -u+x -name "$t" 2>/dev/null | head -n1 || true)
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        echo "skip $t (not built)"
        continue
    fi

    work="$OUT/corpus/$t"
    art="$OUT/artifacts/$t"
    mkdir -p "$work" "$art"
    cp -n "$SRC/corpus/$t/"* "$work/" 2>/dev/null || true
    cp -n "$SRC/regressions/$t/"* "$work/" 2>/dev/null || true

    dict=""
    case $t in
        nxvc_*)      dict="-dict=$SRC/dict/nxvc.dict" ;;
        transport_*) dict="-dict=$SRC/dict/nxt.dict" ;;
        warp_*)      dict="-dict=$SRC/dict/warp.dict" ;;
    esac

    echo "=== $t for ${SECS}s ==="
    # libFuzzer writes its per-worker fuzz-N.log into the working directory.
    # $PREFIX and $dict are deliberately word-split (shellcheck SC2086).
    ( cd "$OUT" && \
      $PREFIX "$bin" "$work" $dict \
        -max_total_time="$SECS" -jobs="$WORKERS" -workers="$WORKERS" \
        -rss_limit_mb=4096 -timeout=25 -max_len=65536 \
        -print_final_stats=1 -artifact_prefix="$art/" ) || rc=$?
done

if [ "$rc" -ne 0 ]; then
    echo "=== a target reported a crash; artifacts under $OUT/artifacts/ ==="
    find "$OUT/artifacts" -type f | sort || true
fi
exit "$rc"
