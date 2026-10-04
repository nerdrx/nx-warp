#!/usr/bin/env bash
set -euo pipefail
dir="$(cd "$(dirname "$0")" && pwd)"
src=/run/media/nerdrx/Lex/claude/nx-scratch/astc-temporal-dict-20261004
g++ -O3 -std=c++17 "$dir/native_delta.cpp" -lzstd -o "$dir/native_delta"
if [[ $# -gt 0 ]]; then
  "$dir/native_delta" "$@"
else
  "$dir/native_delta" "$src" "$dir/results.json"
fi
