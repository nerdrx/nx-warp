#!/bin/bash
# Tournament measurement driver for the "large transforms" package (xform-a).
#
#   MODE=intra444|intra420|interA|interB  run-xform-a.sh
#
# Each mode runs the before column (--xform 8, byte-identical to a build
# without the tool) and then the after column (--xform auto) on the same
# material with the same ladder, so the pair is comparable by construction.
# FRAMES defaults to 12: the machine runs several of these at once and the
# full 36-frame sequence does not fit in the time budget.  Say so in the
# results table.
set -u
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export NXQ_CPUS=4-7
export NXQ_THREADS=4
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/xform-a
export PATH=$WT/build-ref/bin:$PATH
PY=$NXQ_SCRATCH/venv/bin/python
FRAMES=${FRAMES:-12}
SEQ=${SEQ:-vr-mixed-1024-v2}
cd "$WT/tools/quality" || exit 1
mkdir -p "$NXQ_SCRATCH/results"

run_intra() {   # pixfmt  tag  extra-enc-flags
  local pf=$1 tag=$2 extra=$3
  chrt -i 0 taskset -c 4-7 nice -n 19 $PY compare.py \
    --seq "$NXQ_SCRATCH/seq/$SEQ.$pf.json" --frames "$FRAMES" \
    --codec-enc "nxv-enc --quiet $extra" --codec-dec "nxv-dec --quiet" \
    --codec-name "nxv-$tag" \
    --anchors x264-intra \
    --qp 0,4,8,12,16,20,24 --anchor-qp 8,12,16,20,24,28 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --no-vmaf \
    --out "$NXQ_SCRATCH/results/tourney-xform-a-intra-$tag-$SEQ-$pf.json"
}

run_inter() {   # pixfmt  band(A|B)  tag  extra-enc-flags
  local pf=$1 band=$2 tag=$3 extra=$4
  local qp aqp
  if [ "$band" = A ]; then qp=0,4,8,12; aqp=2,8,14,20
  else qp=18,24,30,36; aqp=26,32,38,44; fi
  chrt -i 0 taskset -c 4-7 nice -n 19 $PY compare.py \
    --seq "$NXQ_SCRATCH/seq/$SEQ.$pf.json" --frames "$FRAMES" \
    --codec-enc "nxv-enc --quiet --eyes 2 --inter on --poses $NXQ_SCRATCH/seq/$SEQ.poses.json $extra" \
    --codec-dec "nxv-dec --quiet" --codec-name "nxv-inter-$tag" \
    --anchors x265-p --qp $qp --anchor-qp $aqp --no-vmaf \
    --out "$NXQ_SCRATCH/results/tourney-xform-a-inter$band-$tag-$SEQ-$pf.json"
}

case "${MODE:-intra444}" in
  intra444) run_intra yuv444p base "--xform 8"; run_intra yuv444p auto "--xform auto" ;;
  intra420) run_intra yuv420p base "--xform 8"; run_intra yuv420p auto "--xform auto" ;;
  interA)   run_inter yuv444p A base "--xform 8"; run_inter yuv444p A auto "--xform auto" ;;
  interB)   run_inter yuv444p B base "--xform 8"; run_inter yuv444p B auto "--xform auto" ;;
esac
