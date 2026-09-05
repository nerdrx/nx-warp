#!/bin/bash
# The three rows the first pass did not reach: cflonly 4:2:0 and both final
# rows.  Same driver, same snapshot discipline as all-detail-a.sh.
cd "$(dirname "$0")" || exit 1
S=${LOGDIR:-/tmp/claude-1000/-run-media-nerdrx-Lex-claude/007c59b8-ef87-4187-99fc-030b64d22df2/scratchpad}
PIX_ONLY=yuv420p ./run-detail-a.sh cflonly --split4x4 off --cfl on >> "$S/r-cflonly.log" 2>&1
./run-detail-a.sh final --split4x4 on --cfl on > "$S/r-final.log" 2>&1
echo RESTDONE
