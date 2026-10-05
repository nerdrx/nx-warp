#!/usr/bin/env bash
set -euo pipefail
repo=${1:?source checkout required}
out=${2:?output directory required}
here=$(cd "$(dirname "$0")" && pwd)
bash "$here/run-check.sh" "$repo" "$out/methods"
rtk proxy python3 "$here/caller-check.py" "$repo" "$out/methods/quiet-retirement-gate.cpp" "$out/caller"
bash "$here/build-caller.sh" "$repo" "$out/caller"
