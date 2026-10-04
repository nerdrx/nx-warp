#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
adb=/run/media/nerdrx/Lex/claude/tools/android-sdk/platform-tools/adb
remote=/data/local/tmp/nx-zstd-dctx-20261004
bin="$root/build/android-arm64/zstd-dctx-probe"
mkdir -p "$root/results/device"
"$adb" shell mkdir -p "$remote/fixtures"
"$adb" push "$bin" "$remote/zstd-dctx-probe" >/dev/null
for f in dark-q6.blocks.zst dark-q6.blocks forest-q6.blocks.zst forest-q6.blocks; do
  "$adb" push "$root/fixtures/$f" "$remote/fixtures/$f" >/dev/null
done
"$adb" shell chmod 700 "$remote/zstd-dctx-probe"
"$adb" shell "$remote/zstd-dctx-probe $remote/fixtures" | tee "$root/results/device/run.csv"
for f in dark-oneshot.out dark-dctx.out forest-oneshot.out forest-dctx.out; do
  "$adb" pull "$remote/fixtures/$f" "$root/results/device/$f" >/dev/null
done
sha256sum "$root/results/device/"*.out | tee "$root/results/device/output.sha256"
