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
for pix in yuv444p yuv420p; do
  $R $PY compare.py \
    --seq $S/vr-mixed-1024-v2.$pix.json --frames 12 \
    --codec-enc "$BIN/nxv-enc --quiet --eyes 2 --inter on --poses $S/vr-mixed-1024-v2.poses.json $EXTRA" \
    --codec-dec "$BIN/nxv-dec --quiet" --codec-name "nxv-inter-$TAG" \
    --anchors x265-p --qp 0,4,8,12 --anchor-qp 2,8,14,20 --no-vmaf --no-ssim \
    --out $OUT/killA-$TAG-$pix.json > $OUT/killA-$TAG-$pix.log 2>&1
  echo "== killA $TAG $pix rc=$?"
  $R $PY compare.py \
    --seq $S/vr-mixed-1024-v2.$pix.json --frames 12 \
    --codec-enc "$BIN/nxv-enc --quiet --eyes 2 --inter on --poses $S/vr-mixed-1024-v2.poses.json $EXTRA" \
    --codec-dec "$BIN/nxv-dec --quiet" --codec-name "nxv-inter-$TAG" \
    --anchors x265-p --qp 18,24,30,36 --anchor-qp 26,32,38,44 --no-vmaf --no-ssim \
    --out $OUT/killB-$TAG-$pix.json > $OUT/killB-$TAG-$pix.log 2>&1
  echo "== killB $TAG $pix rc=$?"
done
fi
