#!/bin/bash
# passb-device-rows.sh -- per-module Pass B rows from the Pico 4.
#
# One row is: gate on the headset being idle, push, check the sha256 either
# side of the push and refuse on mismatch, run the fixture while sampling
# gpuclk ON the device, and report per-module Pass B with the temperature
# either side.
#
# Rows are only comparable when they are INTERLEAVED -- run the binaries
# alternately, not all of one then all of the other.  The headset holds the GPU
# at 490 MHz and 52-57 C whenever it is awake (its SLAM tracking runs even with
# nobody wearing it), so the absolute us/tile sits ~30 % above a cold device and
# only the ratio between interleaved rows is the measurement.
#
#   ./scripts/passb-device-rows.sh <fixture-on-device> <rounds> <label>=<binary>...
#
#   ./scripts/passb-device-rows.sh ./ht.nxv 4 \
#       before=/path/nxvc-vkdec-a after=/path/nxvc-vkdec-b
#
# The decoder must be built with timestamps available; NXVC_VKD_SEG_MS is what
# makes it print the per-module line.
#
# SPDX-License-Identifier: Apache-2.0
set -u
DEVDIR=/data/local/tmp/nxwarp-vkds
ZONE=/sys/class/thermal/thermal_zone25/temp     # gpuss-max-step
CLK=/sys/class/kgsl/kgsl-3d0/gpuclk             # the only kgsl attr shell may read

FIXTURE=${1:?fixture path on the device, e.g. ./ht.nxv}; shift
ROUNDS=${1:?number of interleaved rounds}; shift
[ $# -ge 1 ] || { echo "give at least one label=binary" >&2; exit 2; }

# --- the idle gate ---------------------------------------------------------
# Nobody may be streaming: no decode lines in the recent logcat, and no
# 'Client connected' newer than the last 'Server exiting' in the live server
# log.  A run that ignores this both perturbs the numbers and interferes with
# whoever is using the headset.
idle_gate() {
  local n cc se log=${NXVC_SERVER_LOG:-/run/media/nerdrx/Lex/claude/nx-scratch/live/server31.log}
  n=$(adb logcat -d -t 400 2>/dev/null | grep -c "nxwarp\[0\]")
  [ "$n" = "0" ] || { echo "BUSY: $n nxwarp[0] decode lines in the recent logcat" >&2; return 1; }
  if [ -f "$log" ]; then
    cc=$(grep -n "Client connected" "$log" | tail -1 | cut -d: -f1)
    se=$(grep -n "Server exiting" "$log" | tail -1 | cut -d: -f1)
    if [ -n "$cc" ] && { [ -z "$se" ] || [ "$cc" -gt "$se" ]; }; then
      echo "BUSY: a client is connected (log line $cc)" >&2; return 1
    fi
  fi
  return 0
}

row() {                                   # row <label> <local-binary>
  local label=$1 bin=$2 dev=nxvcrow lh rh out
  idle_gate || return 1
  lh=$(sha256sum "$bin" | cut -d' ' -f1)
  adb push "$bin" "$DEVDIR/$dev" >/dev/null 2>&1
  rh=$(adb shell sha256sum "$DEVDIR/$dev" | cut -d' ' -f1 | tr -d '\r')
  if [ "$lh" != "$rh" ]; then
    echo "MISMATCH $label: local $lh device $rh -- REFUSING TO LAUNCH" >&2; return 1
  fi
  out=$(adb shell "cd $DEVDIR
T0=\$(cat $ZONE)
NXVC_VKD_SEG_MS=1 timeout 250 ./$dev --stats --no-out --in $FIXTURE > r.txt 2>&1 &
BP=\$!; : > c.txt
while kill -0 \$BP 2>/dev/null; do cat $CLK >> c.txt; sleep 0.1; done
wait \$BP
echo \"THERM \$T0 \$(cat $ZONE)\"
echo \"CLK \$(sort -n c.txt | head -1) \$(sort -n c.txt | tail -1)\"
cat r.txt")
  echo "=== $label"
  echo "    MATCH ${lh:0:16}  $(echo "$out" | sed -n 's/^THERM \([0-9]*\) \([0-9]*\)/gpuss \1->\2 mC/p')  $(echo "$out" | sed -n 's/^CLK \([0-9]*\) \([0-9]*\)/gpuclk \1..\2 Hz/p')"
  echo "$out" | grep segms | tail -8 | awk '
    {s+=$3; c+=$5; n++; tl=$9}
    END {gsub(/[()]/,"",tl); split(tl,a,"/");
         if (n && a[1]>0)
           printf "    skip  %.3f ms over %s tiles = %.2f us/tile\n", s/n, a[1], s/n*1000/a[1];
         if (n && a[2]>0)
           printf "    coded %.3f ms over %s tiles = %.2f us/tile   (%d frames)\n", c/n, a[2], c/n*1000/a[2], n}'
  echo "$out" | grep -E "^frame [1-9]" | tail -8 | awk '
    {for (j=1;j<=NF;j++) {if ($j=="passA") a+=$(j+1); if ($j=="passW") w+=$(j+1); if ($j=="passB") b+=$(j+1)} n++}
    END {if (n) printf "    passA %.3f  passW %.4f  passB %.4f  (W+B %.4f)\n", a/n, w/n, b/n, (w+b)/n}'
}

for r in $(seq 1 "$ROUNDS"); do
  for spec in "$@"; do
    row "r$r ${spec%%=*}" "${spec#*=}" || exit 1
  done
done
