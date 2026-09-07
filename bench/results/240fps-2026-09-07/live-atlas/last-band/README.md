# Last-band ATLAS evidence

This package records the matched QP40/pace60 `before` and `after` captures for the last-band change. A reverse-order `before` capture is included separately to expose short-run ordering effects.

All decoder and render values below are medians over every measured report window. The captures were warmed before measurement; only the first boundary report is omitted when computing aligned wall-rate spans. The chart plots each reported fresh-source rate and pose-age mean; it does not add error bars or treat window means as frame latency.

| capture | decoder GPU ms | wall ms | passA / passB ms | fresh source/s | pose age ms |
|---|---:|---:|---:|---:|---:|
| before | 1.9 | 3.0 | 0.4 / 1.4 | 34.0 | 70.5 |
| before-reverse | 1.85 | 2.9 | 0.4 / 1.4 | 34.0 | 71.1 |
| after | 1.2 | 2.2 | 0.2 / 1.0 | 56.75 | 50.35 |

The after screenshots at both requested times are black, so this package makes no visual live-scene claim for that capture. Raw logs and manifests are retained. The startup banner reports an older cached version; measured executable hashes and the recorded source commit are authoritative.

```sh
python3 summarize.py before/*measure.log before-reverse/*measure.log after/*measure.log --out summary.json
MPLCONFIGDIR=/tmp/mpl python3 plot.py --before before/live-lastband-before-pace60-measure.log --reverse before-reverse/live-lastband-before-reverse-pace60-measure.log --after after/live-lastband-after-pace60-measure.log --out last-band-windows
```
