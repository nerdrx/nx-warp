#!/bin/bash
# Tournament measurement driver for the "large transforms" package (xform-a).
# Usage: run-xform-a.sh <tag> [extra nxv-enc flags...]
set -u
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export NXQ_CPUS=4-7
export NXQ_THREADS=4
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/xform-a
export PATH=$WT/build-ref/bin:$PATH
PY=$NXQ_SCRATCH/venv/bin/python
TAG=$1; shift
EXTRA="$*"
cd "$WT/tools/quality" || exit 1
mkdir -p "$NXQ_SCRATCH/results"

run_intra() {   # seq-json  pixfmt
  local seq=$1 pf=$2
  chrt -i 0 taskset -c 4-7 nice -n 19 $PY compare.py \
    --seq "$NXQ_SCRATCH/seq/$seq.$pf.json" \
    --codec-enc "nxv-enc --quiet $EXTRA" --codec-dec "nxv-dec --quiet" \
    --codec-name "nxv-$TAG" \
    --anchors x264-intra \
    --qp 0,4,8,12,16,20,24 --anchor-qp 8,12,16,20,24,28 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --no-vmaf \
    --out "$NXQ_SCRATCH/results/tourney-xform-a-intra-$TAG-$seq-$pf.json"
}

run_inter() {   # seq  pixfmt  band(A|B)
  local seq=$1 pf=$2 band=$3
  local qp aqp
  if [ "$band" = A ]; then qp=0,4,8,12; aqp=2,8,14,20
  else qp=18,24,30,36; aqp=26,32,38,44; fi
  chrt -i 0 taskset -c 4-7 nice -n 19 $PY compare.py \
    --seq "$NXQ_SCRATCH/seq/$seq.$pf.json" \
    --codec-enc "nxv-enc --quiet --eyes 2 --inter on --poses $NXQ_SCRATCH/seq/$seq.poses.json $EXTRA" \
    --codec-dec "nxv-dec --quiet" --codec-name "nxv-inter-$TAG" \
    --anchors x265-p --qp $qp --anchor-qp $aqp --no-vmaf \
    --out "$NXQ_SCRATCH/results/tourney-xform-a-inter$band-$TAG-$seq-$pf.json"
}

case "${MODE:-intra}" in
  intra) run_intra vr-mixed-1024-v2 yuv444p; run_intra vr-mixed-1024-v2 yuv420p ;;
  interA) run_inter vr-mixed-1024-v2 yuv444p A; run_inter vr-mixed-1024-v2 yuv420p A ;;
  interB) run_inter vr-mixed-1024-v2 yuv444p B ;;
esac
