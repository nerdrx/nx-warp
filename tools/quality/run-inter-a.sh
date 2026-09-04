#!/bin/bash
# Kill-test driver for the tourney/inter-a package.
#
#   run-inter-a.sh <tag>
#
# `tag` names the encoder state being measured.  Extra encoder flags come from
# $EXTRA, the sequences from $SEQS and the rate bands from $BANDS.  With
# $ANCHOR_FROM set to a tag that has already been measured, the anchor curve is
# grafted from that run instead of being measured again: it is a property of
# the sequence and the ladder, not of our encoder (see splice_anchor.py).
#
# Every process runs on the 12-15 slice at idle priority, per TOURNEY-RULES.md.
set -u
TAG=$1
NXQ_SCRATCH=${NXQ_SCRATCH:-/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp}
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/inter-a
PY=$NXQ_SCRATCH/venv/bin/python
OUT=$NXQ_SCRATCH/results/tourney-inter-a
EXTRA=${EXTRA:-}
ANCHOR_FROM=${ANCHOR_FROM:-}
RUN="chrt -i 0 taskset -c 12-15 nice -n 19"
cd "$WT/tools/quality" || exit 1
mkdir -p "$OUT"

# Snapshot the binaries this tag measures.  Rebuilding the worktree while a
# measurement is in flight would otherwise change the encoder under it; a tag
# names a build, so the build is copied where the tag can keep it.
BIN=$NXQ_SCRATCH/bin-inter-a/$TAG
mkdir -p "$BIN"
cp -f "$WT/build-ref/bin/nxv-enc" "$WT/build-ref/bin/nxv-dec" "$BIN/" || exit 1

run_one() {
  local key=$1 stem=$2 band=$3 qp=$4 aqp=$5
  local seq=$NXQ_SCRATCH/seq/$stem.json
  local poses=$NXQ_SCRATCH/seq/${stem%%.*}.poses.json
  local dst=$OUT/$TAG-$key-$band.json
  local anchors=x265-p
  [ -n "$ANCHOR_FROM" ] && anchors=""
  $RUN $PY compare.py --seq "$seq" \
    --codec-enc "$BIN/nxv-enc --quiet --eyes 2 --inter on --poses $poses $EXTRA" \
    --codec-dec "$BIN/nxv-dec --quiet" --codec-name nxv-inter \
    --anchors "$anchors" --qp "$qp" --anchor-qp "$aqp" --no-vmaf \
    --work "$NXQ_SCRATCH/work-inter-a/$TAG-$key-$band" \
    --out "$dst" > "$OUT/$TAG-$key-$band.log" 2>&1
  local rc=$?
  if [ $rc -eq 0 ] && [ -n "$ANCHOR_FROM" ]; then
    $RUN $PY splice_anchor.py --into "$dst" \
      --anchor-from "$OUT/$ANCHOR_FROM-$key-$band.json" --anchor x265-p \
      >> "$OUT/$TAG-$key-$band.log" 2>&1
    rc=$?
  fi
  echo "$TAG $key band$band -> $rc"
}

SEQS=${SEQS:-"m1024-444:vr-mixed-1024-v2.yuv444p m1024-420:vr-mixed-1024-v2.yuv420p turn256-444:vr-turn-256-v2.yuv444p m512-420:vr-mixed-512-v2.yuv420p"}
BANDS=${BANDS:-"A B"}
for s in $SEQS; do
  key=${s%%:*}; stem=${s#*:}
  for b in $BANDS; do
    if [ "$b" = A ]; then run_one "$key" "$stem" A 0,4,8,12 2,8,14,20
    else run_one "$key" "$stem" B 18,24,30,36 26,32,38,44; fi
  done
done
