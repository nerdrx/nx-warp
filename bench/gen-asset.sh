#!/usr/bin/env bash
# Generates the K6 hybrid base layer: an HEVC elementary stream at the frame
# geometry of PAPER 3.1 (2 views of 2048^2, stacked), shipped in the APK as
# assets/base.hevc.
#
# The binary is deliberately not in git. Run this once; run.sh calls it
# automatically when the asset is missing and ffmpeg is available.
#
#   ./gen-asset.sh            # 1 second at 90 fps
#   ./gen-asset.sh 3          # 3 seconds
set -euo pipefail
cd "$(dirname "$0")"

DUR="${1:-1}"
OUT=assets/base.hevc

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg not found; K6 will run Pass C against a synthetic base" >&2
  exit 1
fi
if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
  echo "this ffmpeg has no libx265; K6 will run Pass C against a synthetic base" >&2
  exit 1
fi

mkdir -p assets

# shellcheck source=../scripts/cpu-discipline.sh
. "$(dirname "$(readlink -f "$0")")/../scripts/cpu-discipline.sh"
nx_cpu_prefix 20-23

# zerolatency shape: no B-frames, headers repeated on every IDR so the decoder
# can be fed from any keyframe when the clip loops.
"${NICE[@]}" ffmpeg -y -v warning \
  -f lavfi -i "testsrc2=size=2048x4096:rate=90:duration=$DUR" \
  -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
  -x265-params "bframes=0:keyint=30:min-keyint=30:repeat-headers=1:annexb=1:log-level=error" \
  -f hevc "$OUT"

ls -la "$OUT"
