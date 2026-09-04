#!/bin/bash
# Tournament measurement driver for the "inter efficiency" package (inter-b).
#
#   tourney_run.sh <tag> <seq-base> <band A|B> [extra nxv-enc flags...]
#
# <seq-base> is a name under $NXQ_SCRATCH/seq without the .json, e.g.
# vr-mixed-1024-v2.yuv444p.  Writes $NXQ_SCRATCH/results/tourney-inter-b-<tag>.json.
set -u
: "${NXQ_SCRATCH:=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp}"
export NXQ_SCRATCH
export NXQ_CPUS=16-19
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/inter-b
PY=$NXQ_SCRATCH/venv/bin/python
tag=$1; base=$2; band=$3; shift 3
poses=$NXQ_SCRATCH/seq/$(echo "$base" | sed 's/\.yuv4[24][04]p$//').poses.json
if [ "$band" = A ]; then qp=0,4,8,12; aqp=2,8,14,20; else qp=18,24,30,36; aqp=26,32,38,44; fi
cd "$WT/tools/quality" || exit 1
mkdir -p "$NXQ_SCRATCH/work-inter-b/$tag"
exec chrt -i 0 taskset -c 16-19 nice -n 19 "$PY" compare.py \
    --work "$NXQ_SCRATCH/work-inter-b/$tag" \
    --seq "$NXQ_SCRATCH/seq/$base.json" \
    --codec-enc "$WT/build-ref/bin/nxv-enc --quiet --eyes 2 --inter on --poses $poses $*" \
    --codec-dec "$WT/build-ref/bin/nxv-dec --quiet" \
    --codec-name nxv-inter \
    --anchors x265-p --qp "$qp" --anchor-qp "$aqp" --no-vmaf \
    --out "$NXQ_SCRATCH/results/tourney-inter-b-$tag.json"
