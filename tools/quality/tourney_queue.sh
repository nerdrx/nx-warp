#!/bin/bash
# The inter-efficiency package's measurement queue: base and each tool on its
# own, then all three, in both rate bands.  Serial, on the agent's core slice.
set -u
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
WT=/run/media/nerdrx/Lex/claude/nx-warp-wt/inter-b
R=$WT/tools/quality/tourney_run.sh
L=$NXQ_SCRATCH/results
SEQ=${SEQ:-vr-mixed-1024-v2.yuv444p}
SHORT=${SHORT:-m1024-444}
OFF="--refresh-drift 0 --warp-dc off --mv-quad off"
for band in B A; do
  for cfg in base all t3-mvquad t1-drift t2-warpdc; do
    case $cfg in
      base)       flags="$OFF" ;;
      t1-drift)   flags="--warp-dc off --mv-quad off" ;;
      t2-warpdc)  flags="--refresh-drift 0 --mv-quad off" ;;
      t3-mvquad)  flags="--refresh-drift 0 --warp-dc off" ;;
      all)        flags="" ;;
    esac
    tag=$SHORT-$band-$cfg
    [ -f "$L/tourney-inter-b-$tag.json" ] && continue
    echo "=== $tag  $(date +%T)"
    $R "$tag" "$SEQ" "$band" $flags > "$L/tourney-inter-b-$tag.log" 2>&1
  done
done
echo "queue done $(date +%T)"
