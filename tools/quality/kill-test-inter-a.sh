#!/bin/bash
# The final measurement: the Phase 2 kill test on every sequence, both bands,
# the baseline against the whole inter-efficiency package, at the codec's own
# default refresh period (180) -- the configuration ref/RESULTS-inter.md
# measured, so the two documents are comparable.  The refresh scheme is
# compared separately at a period commensurate with the clip, because at 180
# on a 12- or 36-frame clip neither scheme refreshes anything.
#
# Both tags name every switch explicitly rather than relying on the encoder's
# defaults, which changed mid-tourney once the sweep had said what they should
# be.  A measurement that depends on a default is a measurement of the default.
#
# The anchors are measured once, by `fullbase`, and grafted onto `fullall`.
cd /run/media/nerdrx/Lex/claude/nx-warp-wt/inter-a/tools/quality || exit 1
OFF="--drift-refresh off --near-skip off --quad-mv off --sub-intra off"
ALL="--drift-refresh on --near-skip on --quad-mv on --sub-intra on"
for s in "m1024-444:vr-mixed-1024-v2.yuv444p:A B" \
         "m1024-420:vr-mixed-1024-v2.yuv420p:A B" \
         "turn256-444:vr-turn-256-v2.yuv444p:A" \
         "m512-420:vr-mixed-512-v2.yuv420p:A B"; do
  key=${s%%:*}; rest=${s#*:}; stem=${rest%%:*}; bands=${rest#*:}
  export SEQS="$key:$stem" BANDS="$bands"
  unset ANCHOR_FROM
  EXTRA="$OFF" ./run-inter-a.sh fullbase
  export ANCHOR_FROM=fullbase
  EXTRA="$ALL" ./run-inter-a.sh fullall
  echo "PAIR-DONE $key"
done
echo FULL-DONE
