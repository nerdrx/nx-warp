#!/usr/bin/env bash
# Deploy nxvc-d3dinterop to the NX-WIN box, run it, and fetch the JSON.
#
#   platform/win/run-on-nxwin.sh                 build if needed, deploy, run, fetch
#   platform/win/run-on-nxwin.sh --dry-run       print every remote command, touch nothing
#   platform/win/run-on-nxwin.sh --keyed         deploy the keyed-mutex build instead
#   platform/win/run-on-nxwin.sh --iterations 2000
#
# The JSON lands in platform/win/results/nxwin-<mode>-<timestamp>.json and the
# newest run is symlinked as results/latest.json.
#
# NOTE: this script is the only thing in the tree that touches the Windows box.
# Nothing else in platform/win/ opens a network connection.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

HOST="${NXWIN_HOST:-xlerm@192.168.1.215}"
KEY="${NXWIN_KEY:-$HOME/.ssh/wivrnnx_windows}"
REMOTE_DIR='C:\wivrnnx\nxwarp-probe'
ITERATIONS=600
ADAPTER=""
KEYED=0
DRY=0
NO_BUILD=0
EXE=""
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --key) KEY="$2"; shift 2 ;;
    --dir) REMOTE_DIR="$2"; shift 2 ;;
    --iterations) ITERATIONS="$2"; shift 2 ;;
    --adapter) ADAPTER="$2"; shift 2 ;;
    --exe) EXE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --keyed) KEYED=1; shift ;;
    --no-build) NO_BUILD=1; shift ;;
    --dry-run|-n) DRY=1; shift ;;
    -h|--help)
      awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

mode=$([ "$KEYED" = 1 ] && echo keyed-mutex || echo shared-fence)
build_dir=$([ "$KEYED" = 1 ] && echo "$repo/build-win-keyed" || echo "$repo/build-win")
[ -n "$EXE" ] || EXE="$build_dir/win/nxvc-d3dinterop.exe"

results_dir="$here/results"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
[ -n "$OUT" ] || OUT="$results_dir/nxwin-$mode-$stamp.json"

# --- local: make sure there is something to deploy ---------------------------
if [ ! -f "$EXE" ] && [ "$NO_BUILD" = 0 ] && [ "$DRY" = 0 ]; then
  echo "==> no binary at $EXE, building" >&2
  if [ "$KEYED" = 1 ]; then
    "$here/build.sh" --keyed
  else
    "$here/build.sh"
  fi
fi
if [ ! -f "$EXE" ] && [ "$DRY" = 0 ]; then
  echo "no probe binary at $EXE" >&2
  exit 2
fi
if [ ! -f "$KEY" ] && [ "$DRY" = 0 ]; then
  echo "ssh key not found: $KEY" >&2
  exit 2
fi

# Windows-style remote paths for cmd, forward-slash form for scp.
remote_exe_win="$REMOTE_DIR\\nxvc-d3dinterop.exe"
remote_json_win="$REMOTE_DIR\\probe.json"
remote_dir_scp="$(printf '%s' "$REMOTE_DIR" | tr '\\' '/')"

SSH=(ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=10 "$HOST")
SCP=(scp -i "$KEY" -o BatchMode=yes -o ConnectTimeout=10)

mkdir_cmd="cmd /c \"if not exist \\\"$REMOTE_DIR\\\" mkdir \\\"$REMOTE_DIR\\\"\""
run_cmd="cmd /c \"\\\"$remote_exe_win\\\" --iterations $ITERATIONS${ADAPTER:+ --adapter $ADAPTER} --out \\\"$remote_json_win\\\"\""

show() { printf '  %s\n' "$*"; }

if [ "$DRY" = 1 ]; then
  cat <<EOF
dry run: nothing was sent to $HOST

  local binary   : $EXE   ($([ -f "$EXE" ] && echo present || echo MISSING))
  interop mode   : $mode
  remote dir     : $REMOTE_DIR
  iterations     : $ITERATIONS
  local JSON out : $OUT

commands that would run:
EOF
  show "${SSH[*]} $mkdir_cmd"
  show "${SCP[*]} '$EXE' '$HOST:$remote_dir_scp/nxvc-d3dinterop.exe'"
  show "${SSH[*]} $run_cmd"
  show "${SCP[*]} '$HOST:$remote_dir_scp/probe.json' '$OUT'"
  exit 0
fi

mkdir -p "$results_dir"

echo "==> ensuring $REMOTE_DIR exists on $HOST" >&2
"${SSH[@]}" "$mkdir_cmd"

echo "==> uploading $(basename "$EXE") ($(du -h "$EXE" | cut -f1))" >&2
"${SCP[@]}" "$EXE" "$HOST:$remote_dir_scp/nxvc-d3dinterop.exe"

echo "==> running the probe ($mode, $ITERATIONS iterations per size)" >&2
set +e
"${SSH[@]}" "$run_cmd"
probe_rc=$?
set -e

echo "==> fetching the JSON" >&2
"${SCP[@]}" "$HOST:$remote_dir_scp/probe.json" "$OUT.crlf"
tr -d '\r' <"$OUT.crlf" >"$OUT"
rm -f "$OUT.crlf"
ln -sfn "$(basename "$OUT")" "$results_dir/latest.json"

echo >&2
if command -v python3 >/dev/null 2>&1; then
  python3 - "$OUT" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)
a = d.get("adapter", {})
p = d.get("profile", {})
print(f"adapter    : {a.get('name')}  (driver {a.get('driver_version_umd')})")
print(f"interop    : {d.get('interop_mode')}")
print(f"profile    : {p.get('id')}  verdict={p.get('verdict')}")
for b in p.get("blockers", []):
    print(f"  blocker  : {b}")
for s in d.get("sizes", []):
    h = s.get("handoff_ms", {})
    v = s.get("verify", {})
    print(f"{s['width']}x{s['height']}  verify={'ok' if v.get('pass') else 'FAIL'}  "
          f"p50={h.get('p50')} ms  p99={h.get('p99')} ms")
amf = d.get("amf", {})
print(f"AMF        : {'present ' + str(amf.get('version')) if amf.get('present') else 'absent'}")
print(f"PASS       : {d.get('pass')}")
if d.get("error"):
    print(f"error      : {d['error'].get('stage')}: {d['error'].get('message')}")
PY
else
  cat "$OUT"
fi

echo >&2
echo "JSON: $OUT" >&2
exit $probe_rc
