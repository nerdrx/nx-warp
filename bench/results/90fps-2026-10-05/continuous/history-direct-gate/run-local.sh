#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then echo "usage: $0 /path/to/wt-pyrowave-probe" >&2; exit 2; fi
src=$(cd "$1" && pwd -P)
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
out="$here/reproduced"
mkdir -p "$out"
CXX=${CXX:-g++}

run_logged() {
    local label=$1; shift
    local rc=0
    "$@" >"$out/$label.log" 2>&1 || rc=$?
    printf '%s\n' "$rc" >"$out/$label.exit"
    if (( rc != 0 )); then cat "$out/$label.log" >&2; return "$rc"; fi
}

{
    date -Is
    uname -a
    uptime
    printf 'source_commit='
    git -C "$src" rev-parse HEAD
    printf 'source_status='
    git -C "$src" status --short
    printf 'compiler='
    "$CXX" --version | head -n 1
} >"$out/context.txt" 2>&1

common=(-std=c++23 -pthread "$here/direct_gate.cpp" "$src/common/smp.cpp"
        -I"$here" -I"$src/common" -I"$src/server/encoder" -I"$src/build-server/common"
        -I"$src/external" -I"$src/build-server/_deps/boost-src/libs/pfr/include" -lcrypto)
run_logged build-normal "$CXX" -O2 "${common[@]}" -o "$out/direct_gate"
run_logged correctness-normal "$out/direct_gate"
run_logged build-asan "$CXX" -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined "${common[@]}" -o "$out/direct_gate_asan"
run_logged correctness-asan env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$out/direct_gate_asan"
run_logged build-tsan "$CXX" -O1 -g -fsanitize=thread "${common[@]}" -o "$out/direct_gate_tsan"
run_logged correctness-tsan env TSAN_OPTIONS=halt_on_error=1 "$out/direct_gate_tsan"
rc=0
"$out/direct_gate" time >"$out/timing.raw.csv" 2>"$out/timing.summary.log" || rc=$?
printf '%s\n' "$rc" >"$out/timing.exit"
if (( rc != 0 )); then cat "$out/timing.summary.log" >&2; exit "$rc"; fi
