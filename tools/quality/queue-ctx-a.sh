#!/bin/bash
set -u
NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/ctx-a
BASE=$NXQ_SCRATCH/tourney-ctx-a/base-src/build-ref/bin
NEW=$WT/build-ref/bin
cd $WT/tools/quality
./run-ctx-a.sh intra base   $BASE
./run-ctx-a.sh intra tool3  $NEW --ctx v2
./run-ctx-a.sh intra ctxv3  $NEW --ctx v3
./run-ctx-a.sh inter base   $BASE
./run-ctx-a.sh inter ctxv3  $NEW --ctx v3
./run-ctx-a.sh inter vecent $NEW --ctx v3 --vec-ent
echo "QUEUE DONE"
