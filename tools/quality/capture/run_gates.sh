#!/bin/bash
# Every gate, on one ingested WiVRn capture.
#
# `tools/quality/reports/gates-v2-2026-09-04.md` ran the four gates on synthetic
# material and said the quiet part out loud in its own header: synthetic
# sequences prove the plumbing, and "the Phase 1 and Phase 2 gates are stated on
# VR captures, not on synthetic material" (corpus/README.md, content classes).
# This script is the same four gates with the sequence swapped, so that the
# moment a real recording exists the numbers that decide the phases exist too,
# and nobody has to reconstruct a command line out of a document.
#
#   tools/quality/capture/run_gates.sh $NXW_CORPUS/wivrn-vrchat-1440.yuv420p.json
#
# It writes tools/quality/reports/capture-<name>-<date>.md with every verdict
# block quoted verbatim, and leaves the full console output of every run beside
# the result JSON in $NXQ_SCRATCH/results/capture-<name>/.
#
# The four gates, and where each is stated:
#
#   1. Phase 1 intra   PAPER.md 3.11 -- within 1.0 dB of x264 intra, 100-400 Mbit
#   2. Phase 2 kill    PAPER.md 2.11 item 1 -- within 10 % at rest, 30 % better
#                      on motion; both rate bands of ref/RESULTS-inter.md 1
#   3. Warp chain      PAPER.md 2.11 item 2 -- 35 dB held for 30 warped frames
#   4. Honest anchors  every anchor that runs on this pixel format, plain and
#                      foveated, for the intra and the inter row of the codec
#
# CPU discipline is the harness's own (nxq/cpu.py): NXQ_CPUS and NXQ_THREADS are
# exported below and every heavy child lands on that slice at idle priority.

set -u

usage() {
	cat >&2 <<'EOF'
usage: run_gates.sh [options] <sequence .json sidecar>

  --frames N        cap every run at N frames (default: the whole sequence)
  --out FILE        report path (default tools/quality/reports/capture-<name>-<date>.md)
  --work DIR        scratch for results and logs (default $NXQ_SCRATCH/results/capture-<name>)
  --qp LIST         nxv QP ladder for the Phase 1 gate (default 0,4,8,12,16,20,24)
  --anchor-qp LIST  anchor ladder for the Phase 1 gate (default 8,12,16,20,24,28)
  --no-anchors      skip gate 4 (the long one)
  --dry-run         print what would run and exit

The sidecar is what ingest_wivrn.py printed. Its pose log must sit beside it as
<name>.poses.json -- the harness derives that name, it is not a free choice.
EOF
	exit 2
}

FRAMES=0
REPORT=""
WORK=""
QP="0,4,8,12,16,20,24"
AQP="8,12,16,20,24,28"
ANCHORS=1
DRY=0
SEQ=""

while [ $# -gt 0 ]; do
	case "$1" in
	--frames) FRAMES="$2"; shift 2 ;;
	--out) REPORT="$2"; shift 2 ;;
	--work) WORK="$2"; shift 2 ;;
	--qp) QP="$2"; shift 2 ;;
	--anchor-qp) AQP="$2"; shift 2 ;;
	--no-anchors) ANCHORS=0; shift ;;
	--dry-run) DRY=1; shift ;;
	-h|--help) usage ;;
	-*) echo "unknown option $1" >&2; usage ;;
	*) [ -n "$SEQ" ] && usage; SEQ="$1"; shift ;;
	esac
done
[ -n "$SEQ" ] || usage
[ -f "$SEQ" ] || { echo "no such sidecar: $SEQ" >&2; exit 2; }

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$HERE/../../.." && pwd)
Q="$REPO/tools/quality"
SEQ=$(cd -- "$(dirname -- "$SEQ")" && pwd)/$(basename -- "$SEQ")
SEQDIR=$(dirname -- "$SEQ")
BASE=$(basename -- "$SEQ")
NAME=${BASE%%.*}                 # <name>.<pix>.json -> <name>
PIX=${BASE#"$NAME".}; PIX=${PIX%.json}
POSES="$SEQDIR/$NAME.poses.json"
DATE=$(date +%F)

[ -f "$POSES" ] || {
	echo "no pose log at $POSES." >&2
	echo "Every gate but the intra one needs it, and ref/warp_chain.py derives" >&2
	echo "that exact name from the sidecar. Re-run ingest_wivrn.py." >&2
	exit 2
}

# The harness's CPU slice. Cores 0-15 are the interactive session's and the
# compositor's; a gate run is hours of full-tilt encoding and must not touch them.
export NXQ_CPUS="${NXQ_CPUS:-16-17}"
export NXQ_THREADS="${NXQ_THREADS:-2}"
export NXQ_SCRATCH="${NXQ_SCRATCH:-/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp}"
export PATH="$REPO/build-ref/bin:$PATH"

PY="${NXQ_PYTHON:-$NXQ_SCRATCH/venv/bin/python}"
[ -x "$PY" ] || PY=python3
for tool in nxv-enc nxv-dec; do
	command -v "$tool" >/dev/null || {
		echo "$tool is not on PATH. Build it first:" >&2
		echo "  cmake -S $REPO -B $REPO/build-ref -DCMAKE_BUILD_TYPE=Release" >&2
		echo "  cmake --build $REPO/build-ref -j2" >&2
		exit 2
	}
done

WORK="${WORK:-$NXQ_SCRATCH/results/capture-$NAME}"
LOGS="$WORK/logs"
REPORT="${REPORT:-$REPO/tools/quality/reports/capture-$NAME-$DATE.md}"
mkdir -p "$LOGS" "$(dirname -- "$REPORT")"

FRAME_ARG=()
[ "$FRAMES" != "0" ] && FRAME_ARG=(--frames "$FRAMES")

# What the ingester recorded about this capture, so the report can say what the
# numbers are numbers about without the reader opening a second file.
META=$("$PY" - "$SEQ" "$POSES" <<'PYEOF'
import json, sys
side = json.load(open(sys.argv[1])); doc = json.load(open(sys.argv[2]))
cap = doc.get("capture", {})
av = [f["angular_velocity_deg_s"] for f in doc["frames"][1:]] or [0.0]
print(side.get("frames", 0), f"{side['width']}x{side['height']}", side.get("fps", 0),
      cap.get("gate_material", "?"), cap.get("foveation", {}).get("identity", "?"),
      round(cap.get("fov", {}).get("asymmetry_deg", 0.0), 3),
      cap.get("input_bit_depth", "?"), cap.get("dither", "?"),
      f"{min(av):.1f}-{max(av):.1f}")
PYEOF
) || { echo "cannot read $SEQ / $POSES" >&2; exit 2; }
read -r CAP_FRAMES CAP_RES CAP_FPS CAP_GATE CAP_FOVEA CAP_ASYM CAP_DEPTH CAP_DITHER CAP_AV <<<"$META"

echo "sequence : $SEQ"
echo "poses    : $POSES"
echo "geometry : $CAP_RES, $CAP_FRAMES frames @ $CAP_FPS, $PIX"
echo "capture  : ${CAP_DEPTH}-bit in, dither=$CAP_DITHER, foveation identity=$CAP_FOVEA,"
echo "           frustum asymmetry ${CAP_ASYM} deg, head motion ${CAP_AV} deg/s"
echo "gate material: $CAP_GATE"
echo "work     : $WORK"
echo "report   : $REPORT"
echo

if [ "$CAP_GATE" != "True" ]; then
	echo "WARNING: ingest_wivrn.py marked this capture as NOT gate material." >&2
	echo "  Its pixels are not on a uniform angular grid, so the pose homography" >&2
	echo "  does not describe them. The gates will still run and their numbers" >&2
	echo "  will still be wrong by an unknown amount. See CAPTURE.md." >&2
	echo >&2
fi

run() { # $1 log name, rest: command
	local log="$LOGS/$1.log"; shift
	echo "### $(date +%T)  $*" | tee -a "$LOGS/commands.log"
	if [ "$DRY" = "1" ]; then return 0; fi
	chrt -i 0 taskset -c "$NXQ_CPUS" nice -n 19 "$@" >"$log" 2>&1
	local rc=$?
	tail -5 "$log"
	[ $rc -ne 0 ] && echo "  (exit $rc -- full output in $log)"
	return 0
}

# ---------------------------------------------------- 1. Phase 1 intra gate
run phase1 "$PY" "$Q/compare.py" --seq "$SEQ" --codec-cmd nxv --anchors x264-intra \
	--qp "$QP" --anchor-qp "$AQP" \
	--phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
	--no-vmaf --out "$WORK/phase1.json" "${FRAME_ARG[@]}"

# ---------------------------------------------------- 2. Phase 2 kill test
# Both rate bands of ref/RESULTS-inter.md section 1: A is the high-rate end
# where the intra core is not yet the bottleneck, B the operating band.
for band in A B; do
	if [ "$band" = "A" ]; then kqp=0,4,8,12; kaqp=2,8,14,20; else kqp=18,24,30,36; kaqp=26,32,38,44; fi
	run "kill-$band" "$PY" "$Q/compare.py" --seq "$SEQ" \
		--codec-enc "nxv-enc --quiet --eyes 2 --inter on --poses $POSES" \
		--codec-dec "nxv-dec --quiet" --codec-name nxv-inter \
		--anchors x265-p --qp "$kqp" --anchor-qp "$kaqp" --no-vmaf \
		--out "$WORK/kill-$band.json" "${FRAME_ARG[@]}"
done
run verdicts "$PY" "$REPO/ref/phase2_verdict.py" --results "$WORK"/kill-A.json "$WORK"/kill-B.json

# ---------------------------------------------------- 3. the warp-only chain
run chain "$PY" "$REPO/ref/warp_chain.py" --seq "$SEQ" --poses "$POSES" \
	--enc "$REPO/build-ref/bin/nxv-enc" --dec "$REPO/build-ref/bin/nxv-dec" \
	--eyes 2 --qp 8 --work "$WORK" --json "$WORK/chain.json" "${FRAME_ARG[@]}"

# ---------------------------------------------------- 4. the honest anchors
if [ "$ANCHORS" = "1" ]; then
	ANCH=x264-intra,x265-intra,x265-p-refresh,hevc-vulkan,av1-svt-p
	run anchors-intra "$PY" "$Q/compare.py" --seq "$SEQ" --codec-cmd nxv \
		--anchors "$ANCH" --qp 10,14,18,22,26,30 --anchor-qp 8,14,20,26,32,38,44 \
		--foveated-psnr --no-vmaf --out "$WORK/anchors-intra.json" "${FRAME_ARG[@]}"
	run anchors-inter "$PY" "$Q/compare.py" --seq "$SEQ" \
		--codec-enc "nxv-enc --quiet --eyes 2 --inter on --poses $POSES" \
		--codec-dec "nxv-dec --quiet" --codec-name nxv-inter \
		--anchors "$ANCH" --qp 10,14,18,22,26,30 --anchor-qp 8,14,20,26,32,38,44 \
		--foveated-psnr --no-vmaf --out "$WORK/anchors-inter.json" "${FRAME_ARG[@]}"
fi

[ "$DRY" = "1" ] && { echo; echo "dry run: nothing was executed"; exit 0; }

# ------------------------------------------------------------------- report
#
# Verdicts are quoted verbatim rather than re-formatted. A verdict this project
# transcribed by hand is a verdict a reader has to trust; a verdict pasted out
# of the log is one they can check against the log next to it.

quote() { # $1 log, $2 grep -A window, $3.. pattern
	local log="$LOGS/$1.log" ctx="$2"; shift 2
	if [ ! -s "$log" ]; then echo '```'; echo "(no output: $log is empty)"; echo '```'; return; fi
	echo '```'
	if [ -n "$*" ] && grep -qF -- "$*" "$log"; then
		grep -F -A "$ctx" -- "$*" "$log"
	else
		tail -n "$ctx" "$log"
	fi
	echo '```'
}

{
	echo "# Every gate, on a real WiVRn capture: \`$NAME\`"
	echo
	echo "**$DATE.** Produced by \`tools/quality/capture/run_gates.sh\`, which is the"
	echo "only thing that produced it: every block below is quoted out of the console"
	echo "output in \`$LOGS\`, not transcribed."
	echo
	echo "This is the first time these four gates have been stated on material that is"
	echo "not synthetic. \`tools/quality/reports/gates-v2-2026-09-04.md\` ran all four on"
	echo "band-limited synthetic sequences and was explicit that they only prove the"
	echo "plumbing: \`corpus/README.md\` says the Phase 1 and Phase 2 gates are stated on"
	echo "VR captures. This is that material."
	echo
	echo "## What the numbers are about"
	echo
	echo "| | |"
	echo "|---|---|"
	echo "| sequence | \`$SEQ\` |"
	echo "| pose log | \`$POSES\` |"
	echo "| coded picture | $CAP_RES, $PIX, side by side, eye 0 left |"
	echo "| frames | $CAP_FRAMES @ $CAP_FPS fps |"
	if [ "$CAP_DEPTH" = "10" ]; then
		echo "| captured at | 10-bit, down-converted to 8-bit with dither \`$CAP_DITHER\` |"
	else
		echo "| captured at | 8-bit, no depth conversion |"
	fi
	echo "| foveation degenerate | $CAP_FOVEA |"
	echo "| frustum asymmetry | $CAP_ASYM deg (dropped by \`fov_deg\`; see below) |"
	echo "| head angular velocity | $CAP_AV deg/s |"
	echo "| gate material | **$CAP_GATE** |"
	echo
	if [ "$CAP_GATE" != "True" ]; then
		echo "> **These numbers do not decide anything.** \`ingest_wivrn.py\` marked this"
		echo "> capture as not gate material: the foveation resample means its pixels are"
		echo "> not on a uniform angular grid, and the pose homography of \`docs/WARP.md\` 4"
		echo "> assumes they are. Re-record with foveation off (\`CAPTURE.md\`) before"
		echo "> quoting anything here."
		echo
	fi
	echo "Caveats that apply to a capture and to none of the synthetic material:"
	echo
	echo "* **The tap is the encoder's input, so it is post-colour-conversion.** These"
	echo "  are BT.709 samples the compositor produced, not a linear render target, and"
	echo "  they are what the encoder would have compressed -- which is the point."
	if awk "BEGIN{exit !($CAP_ASYM > 0.05)}"; then
		echo "* **The frustum is asymmetric by $CAP_ASYM deg and \`nxv-enc\` builds a centred"
		echo "  \`K\`.** The sidecar's \`fov_deg\` is the symmetric equivalent, which is what"
		echo "  the encoder reads; the measured half-angles are in \`fov_rad\` and"
		echo "  \`capture.fov\`. Whatever that projection error costs the warp, the codec is"
		echo "  charged for it in every number below."
	else
		echo "* The frustum is symmetric ($CAP_ASYM deg of residual), so the centred \`K\`"
		echo "  \`nxv-enc\` builds from \`fov_deg\` is the projection this capture was"
		echo "  rendered with. That is not true of every headset."
	fi
	echo
	echo "## 1. The Phase 1 intra gate"
	echo
	echo "> PAPER.md 3.11: within 1.0 dB of x264 intra over 100-400 Mbit/s."
	echo
	echo "\`compare.py --phase1-anchor x264-intra --phase1-band 100,400"
	echo "--phase1-tolerance 1.0\`, \`nxv\` at its shipped default, QP ladder \`$QP\`"
	echo "against \`$AQP\`."
	echo
	quote phase1 6 "Phase 1 gate"
	echo
	echo "## 2. The Phase 2 kill test"
	echo
	echo "> PAPER.md 2.11 item 1: within 10 percent at rest and at least 30 percent"
	echo "> better on the motion frames."
	echo
	echo "\`nxv-enc --inter on --poses\` against \`x265-p\`, both rate bands of"
	echo "\`ref/RESULTS-inter.md\` section 1. The velocity split is at the 20th percentile"
	echo "of this capture's own angular velocity, which is the reason the recording has"
	echo "to contain both rest and motion."
	echo
	quote verdicts 400 ""
	echo
	echo "## 3. The warp-only chain"
	echo
	echo "> PAPER.md 2.11 item 2: if the Full profile filter does not hold above 35 dB"
	echo "> for 30 frames on textured content the per-tile refresh rate must rise and the"
	echo "> bit budget in 2.4 is wrong."
	echo
	echo "\`ref/warp_chain.py\`, QP 8, \`--eyes 2\`: frame 0 is an ordinary intra frame and"
	echo "every frame after it is nothing but the pose warp of its predecessor."
	echo
	quote chain 8 "PAPER.md 2.11 item 2"
	echo
	if [ "$ANCHORS" = "1" ]; then
		echo "## 4. The honest anchors"
		echo
		echo "Every anchor that runs on $PIX, foveated scoring on, for the intra row and"
		echo "the inter row of the codec. BD-rate on PSNR-Y; negative is better."
		echo
		echo "### \`nxv\` intra"
		echo
		quote anchors-intra 40 "BD-rate"
		echo
		echo "### \`nxv-inter\`"
		echo
		quote anchors-inter 40 "BD-rate"
		echo
	fi
	echo "## Reproducing this"
	echo
	echo '```sh'
	echo "export NXQ_SCRATCH=$NXQ_SCRATCH"
	echo "export NXW_CORPUS=\$NXQ_SCRATCH/corpus"
	echo "cmake --build $REPO/build-ref -j2"
	echo "tools/quality/capture/ingest_wivrn.py --dump <WIVRN_RAW_DUMP dir> --name $NAME"
	echo "tools/quality/capture/run_gates.sh $SEQ"
	echo '```'
	echo
	echo "Result JSON and the full console output of every run: \`$WORK\`."
	echo "The recording recipe -- which server, which env vars, what to play and for how"
	echo "long -- is \`tools/quality/capture/CAPTURE.md\`."
} >"$REPORT"

echo
echo "wrote $REPORT"
