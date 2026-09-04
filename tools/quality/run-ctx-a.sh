#!/bin/bash
# Tourney ctx-a measurement runner.
#   run-ctx-a.sh intra <tag> <bindir> [encflags...]
#   run-ctx-a.sh inter <tag> <bindir> [encflags...]
set -u
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/ctx-a
PY=$NXQ_SCRATCH/venv/bin/python
OUT=$NXQ_SCRATCH/results/tourney-ctx-a
KIND=$1; TAG=$2; BIN=$3; shift 3
EXTRA="$*"
cd $WT/tools/quality
R="chrt -i 0 taskset -c 28-31 nice -n 19"
S=$NXQ_SCRATCH/seq

if [ "$KIND" = intra ]; then
for pix in yuv444p yuv420p; do
  $R $PY compare.py \
    --seq $S/vr-mixed-1024-v2.$pix.json --frames 12 \
    --codec-enc "$BIN/nxv-enc --quiet $EXTRA" --codec-dec "$BIN/nxv-dec --quiet" \
    --codec-name "nxv-$TAG" \
    --anchors x264-intra --qp 0,4,8,12,16,20,24 --anchor-qp 8,12,16,20,24,28 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --no-vmaf --no-ssim \
    --out $OUT/intra-$TAG-$pix.json > $OUT/intra-$TAG-$pix.log 2>&1
  echo "== intra $TAG $pix rc=$?"
done
else
# The kill test, ref/RESULTS-inter.md section 2: band A is the literal
# 100-300 Mbit of PAPER 2.11, band B is the paper's own bits per pixel.
# 4:4:4 is the configuration whose verdict RESULTS-inter.md quotes verbatim;
# 4:2:0 is run at band B as well, which is where a per-tile fixed cost like
# the vector bytes is the largest fraction of a frame.
inter_run() {  # <band> <pix> <qp ladder> <anchor ladder>
  $R $PY compare.py \
    --seq $S/vr-mixed-1024-v2.$2.json --frames 12 \
    --codec-enc "$BIN/nxv-enc --quiet --eyes 2 --inter on --poses $S/vr-mixed-1024-v2.poses.json $EXTRA" \
    --codec-dec "$BIN/nxv-dec --quiet" --codec-name "nxv-inter-$TAG" \
    --anchors x265-p --qp $3 --anchor-qp $4 --no-vmaf --no-ssim \
    --out $OUT/$1-$TAG-$2.json > $OUT/$1-$TAG-$2.log 2>&1
  echo "== $1 $TAG $2 rc=$?"
}
inter_run killA yuv444p 0,4,8,12 2,8,14,20
inter_run killB yuv444p 18,24,30,36 26,32,38,44
inter_run killB yuv420p 18,24,30,36 26,32,38,44
fi
