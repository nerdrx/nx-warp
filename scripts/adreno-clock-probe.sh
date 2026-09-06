#!/bin/bash
# adreno-clock-probe.sh -- what clock the Adreno 650 actually runs at, and
# which of the kgsl attributes a non-root adb shell can read at all.
#
# The question this answers: if the GPU sits below its top frequency bin while
# a stream is decoding, that is part of the wall and no amount of kernel work
# recovers it.  The client can then ask for a performance level
# (XR_EXT_performance_settings, or Pico's own level API) -- but that is the
# WiVRn integrator's change to make, not this repo's.  This script only
# measures and reports.
#
#   ./scripts/adreno-clock-probe.sh load     with a stream running
#   ./scripts/adreno-clock-probe.sh idle     with nothing streaming
#
# `load` deliberately does NOT gate on the headset being idle -- it is the one
# measurement here that needs the stream up.  `idle` gates the other way and
# refuses if a client is connected, so the two rows cannot be taken in the
# wrong state by accident.
#
# Root is not assumed anywhere.  passb-device-rows.sh carries the note that
# gpuclk is "the only kgsl attr shell may read"; that was an observation on one
# build, so this reports readability per attribute rather than repeating it.
#
# SPDX-License-Identifier: Apache-2.0
set -u

MODE=${1:?usage: adreno-clock-probe.sh load|idle}
SECS=${2:-20}
KGSL=/sys/class/kgsl/kgsl-3d0
ZONE=/sys/class/thermal/thermal_zone25/temp     # gpuss-max-step

# The attributes worth asking for, in the order they explain each other:
# what it is running at now, what it is allowed to run at, and who decides.
ATTRS="
$KGSL/gpuclk
$KGSL/devfreq/cur_freq
$KGSL/devfreq/min_freq
$KGSL/devfreq/max_freq
$KGSL/devfreq/available_frequencies
$KGSL/devfreq/governor
$KGSL/devfreq/available_governors
$KGSL/max_gpuclk
$KGSL/thermal_pwrlevel
$KGSL/num_pwrlevels
$KGSL/min_pwrlevel
$KGSL/max_pwrlevel
$KGSL/gpu_busy_percentage
$KGSL/temp
"

streaming() {
  [ "$(adb logcat -d -t 200 2>/dev/null | grep -c 'nxwarp\[0\]')" != "0" ]
}

case "$MODE" in
  load)
    streaming || { echo "REFUSING: mode 'load' but no nxwarp[0] decode lines in the recent logcat -- nothing is streaming" >&2; exit 1; }
    ;;
  idle)
    streaming && { echo "REFUSING: mode 'idle' but a stream is decoding right now" >&2; exit 1; }
    ;;
  *) echo "mode must be load or idle" >&2; exit 2 ;;
esac

echo "=== readability (non-root adb shell) ==="
for a in $ATTRS; do
  v=$(adb shell "cat $a 2>/dev/null" 2>/dev/null | tr -d '\r')
  if [ -z "$v" ]; then
    printf '  %-46s UNREADABLE\n' "${a#$KGSL/}"
  else
    printf '  %-46s %s\n' "${a#$KGSL/}" "$(echo "$v" | head -2 | tr '\n' ' ')"
  fi
done

echo
echo "=== $MODE: gpuclk / cur_freq sampled for ${SECS}s ==="
# Sampled ON the device in one shell, so the adb round trip is not the sample
# period.  A dropped attribute prints as an empty field rather than aborting.
adb shell "
  end=\$(( \$(date +%s) + $SECS ))
  while [ \$(date +%s) -lt \$end ]; do
    printf '%s %s %s\n' \
      \"\$(cat $KGSL/gpuclk 2>/dev/null)\" \
      \"\$(cat $KGSL/devfreq/cur_freq 2>/dev/null)\" \
      \"\$(cat $ZONE 2>/dev/null)\"
    sleep 0.25
  done
" 2>/dev/null | tr -d '\r' > /tmp/adreno-probe-$MODE.txt

awk '
  NF >= 1 && $1 != "" { c[$1]++; n++ }
  NF >= 3 && $3 != "" { if (t0 == "") t0 = $3; t1 = $3 }
  END {
    if (n == 0) { print "  no gpuclk samples -- attribute unreadable"; exit }
    printf "  %d samples\n", n
    for (f in c) printf "    %12s Hz  %5.1f %% of the run\n", f, 100.0 * c[f] / n
    if (t0 != "") printf "  gpuss %s -> %s mC\n", t0, t1
  }
' /tmp/adreno-probe-$MODE.txt | sort -k2 -n

echo
echo "raw samples: /tmp/adreno-probe-$MODE.txt"
