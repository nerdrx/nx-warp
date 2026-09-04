#!/bin/sh
# Tourney detail-b measurement driver.  One row of the before/after table:
#   run-b.sh <tag> <pix> <bindir> [extra nxv-enc flags ...]
# Writes $NXQ_SCRATCH/results/tourney-detail-b-<tag>-<pix>.json.
#
# The QP ladder is chosen for the v2 (band-limited) sequences, whose rates are
# about 4x the v1 ones: it puts six nxv points and six anchor points across the
# Phase 1 band of 100-400 Mbit/s.
set -e
: "${NXQ_SCRATCH:=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp}"
export NXQ_SCRATCH
export NXQ_CPUS=24-27
tag=$1; pix=$2; bin=$3; shift 3
seq=$NXQ_SCRATCH/seq/vr-mixed-1024-v2.$pix.json
out=$NXQ_SCRATCH/results/tourney-detail-b-$tag-$pix.json
mkdir -p "$NXQ_SCRATCH/results"
exec chrt -i 0 taskset -c 24-27 nice -n 19 \
  "$NXQ_SCRATCH/venv/bin/python" compare.py --seq "$seq" --frames 6 \
    --codec-enc "$bin/nxv-enc $*" --codec-dec "$bin/nxv-dec" \
    --codec-name "$tag" --anchors x264-intra \
    --qp 20,24,28,32,36,40 --anchor-qp 20,24,28,32,36,40 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --work "$NXQ_SCRATCH/work-b/$tag-$pix" \
    --out "$out"
