#!/bin/bash
# The four measurement rows of tourney/detail-a, in order, each into its own
# log.  Each row snapshots the binaries it measures (see run-detail-a.sh), so
# rebuilding the tree while this runs cannot change what is being compared.
cd "$(dirname "$0")" || exit 1
S=${LOGDIR:-/tmp/claude-1000/-run-media-nerdrx-Lex-claude/007c59b8-ef87-4187-99fc-030b64d22df2/scratchpad}
./run-detail-a.sh base    --split4x4 off --cfl off > "$S/r-base.log"    2>&1
./run-detail-a.sh split4  --split4x4 on  --cfl off > "$S/r-split4.log"  2>&1
./run-detail-a.sh cflonly --split4x4 off --cfl on  > "$S/r-cflonly.log" 2>&1
./run-detail-a.sh final   --split4x4 on  --cfl on  > "$S/r-final.log"   2>&1
echo ALLROWSDONE
