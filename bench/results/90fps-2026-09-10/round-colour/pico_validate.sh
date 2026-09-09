#!/usr/bin/env bash
set -euo pipefail
ROOT="${REPO_ROOT:-$(git rev-parse --show-toplevel)}"
P="${ROUND_COLOUR_DIR:-$ROOT/nx-scratch/round-colour}"
ADB=(/home/nerdrx/.local/bin/adb -s PA8150MGGB110166G)
D=/data/local/tmp/round-colour
"${ADB[@]}" shell 'am force-stop org.meumeu.wivrn.nx.warp'
"${ADB[@]}" shell "mkdir -p $D"
"${ADB[@]}" push "$ROOT/nx-warp/build-vkdec-android/bin/nxvc-vkdec" "$D/"
"${ADB[@]}" push "$P/control.nxv" "$P/colour.nxv" "$P/round-colour.nxv" "$D/"
for n in control colour round-colour; do
  "${ADB[@]}" shell "cd $D && NXVC_VKD_PLANAR_FLAT=1 NXVC_VKD_PLANAR_EXACTREUSE=0 ./nxvc-vkdec --in $n.nxv --out $n-gpu.yuv --pix yuv420p --frames 8 --stats --quiet" > "$P/$n-pico.log" 2>&1
  "${ADB[@]}" pull "$D/$n-gpu.yuv" "$P/$n-gpu.yuv" >/dev/null
  "${ADB[@]}" shell "cd $D && NXVC_VKD_PLANAR_FLAT=1 NXVC_VKD_PLANAR_EXACTREUSE=0 ./nxvc-vkdec --in $n.nxv --out $n-compact.nv12 --nv12 --compact-centre --frames 8 --stats --quiet" > "$P/$n-compact-pico.log" 2>&1
  "${ADB[@]}" pull "$D/$n-compact.nv12" "$P/$n-compact.nv12" >/dev/null
done
python3 "$P/validate_pico.py" > "$P/pico-compare.json"
