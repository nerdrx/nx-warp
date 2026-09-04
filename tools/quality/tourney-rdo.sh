#!/bin/bash
# Measurement driver for the tourney/rdo-a package.  One argument: the tag that
# names the run (e.g. "base", "lambda", "final").  Optional second argument:
# extra nxv-enc flags applied to every codec run.
set -e
TAG=${1:?tag}
EXTRA=${2:-}
export NXQ_SCRATCH=${NXQ_SCRATCH:-/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp}
export NXQ_CPUS=${NXQ_CPUS:-8-11}
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/rdo-a
PY=$NXQ_SCRATCH/venv/bin/python
BIN=${NXQ_BIN:-$WT/build-ref/bin}
OUT=$NXQ_SCRATCH/results/tourney
S=$NXQ_SCRATCH/seq
cd $WT/tools/quality
# Idempotent: a leg whose --out already exists is skipped, so the script can be
# resumed after an interruption without re-running the anchors.
run() {
  local out=""
  for a in "$@"; do [ "$prev" = "--out" ] && out=$a; prev=$a; done
  if [ -n "$out" ] && [ -f "$out" ]; then echo "### skip (have $out)"; return 0; fi
  echo "### $*"
  chrt -i 0 taskset -c $NXQ_CPUS nice -n 19 $PY "$@"
}

# ---- Phase 1 gate: intra, x264-intra anchor, vr-mixed-1024-v2
for PIX in yuv444p yuv420p; do
  run compare.py --seq $S/vr-mixed-1024-v2.$PIX.json --frames 6 \
    --codec-enc "$BIN/nxv-enc --quiet $EXTRA" --codec-dec "$BIN/nxv-dec --quiet" \
    --codec-name nxv-$TAG --anchors x264-intra \
    --qp 0,4,8,12,16,20,24 --anchor-qp 8,12,16,20,24,28 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --no-vmaf --out $OUT/p1-$TAG-$PIX.json
done

# ---- Phase 2 kill test: inter, x265-p anchor, band A and band B
for PIX in yuv444p yuv420p; do
  run compare.py --seq $S/vr-mixed-1024-v2.$PIX.json --frames  8 \
    --codec-enc "$BIN/nxv-enc --quiet --eyes 2 --inter on --poses $S/vr-mixed-1024-v2.poses.json $EXTRA" \
    --codec-dec "$BIN/nxv-dec --quiet" --codec-name nxv-inter-$TAG \
    --anchors x265-p --qp 0,4,8,12 --anchor-qp 2,8,14,20 --no-vmaf \
    --out $OUT/kA-$TAG-$PIX.json
done
run compare.py --seq $S/vr-mixed-1024-v2.yuv420p.json --frames  8 \
  --codec-enc "$BIN/nxv-enc --quiet --eyes 2 --inter on --poses $S/vr-mixed-1024-v2.poses.json $EXTRA" \
  --codec-dec "$BIN/nxv-dec --quiet" --codec-name nxv-inter-$TAG \
  --anchors x265-p --qp 18,24,30,36 --anchor-qp 26,32,38,44 --no-vmaf \
  --out $OUT/kB-$TAG-yuv420p.json
