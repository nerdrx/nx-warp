#!/bin/bash
# Measurement driver for tourney/detail-a.
#   run-detail-a.sh <row-name> [extra nxv-enc flags...]
# The binaries are snapshotted per row so that a rebuild during a run cannot
# change what is being measured.
set -e
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export NXQ_CPUS=20-23
export NXQ_THREADS=4
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/detail-a
NAME=$1; shift
FLAGS="$*"
SNAP=$NXQ_SCRATCH/bin-detail-a/$NAME
mkdir -p "$SNAP"
cp -f "$WT/build-ref/bin/nxv-enc" "$WT/build-ref/bin/nxv-dec" "$SNAP/"
export PATH="$SNAP:$PATH"
PY=$NXQ_SCRATCH/venv/bin/python
OUT=$NXQ_SCRATCH/results/tourney-detail-a
mkdir -p "$OUT"
cd "$WT/tools/quality"
for PIX in yuv444p yuv420p; do
  chrt -i 0 taskset -c 20-23 nice -n 19 $PY compare.py \
    --seq "$NXQ_SCRATCH/seq/vr-mixed-1024-v2.$PIX.json" --frames 6 \
    --codec-enc "nxv-enc $FLAGS" --codec-dec nxv-dec --codec-name "$NAME" \
    --anchors x264-intra \
    --qp 16,20,24,28,32 --anchor-qp 24,28,32,36,40 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --work "$NXQ_SCRATCH/work-detail-a/$NAME-$PIX" \
    --out "$OUT/$NAME-$PIX.json" 2>&1 | tail -30
done
