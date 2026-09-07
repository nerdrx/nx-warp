# ATLAS skip receipt feedback evidence

These four matched pace60 captures use the same QP40 configuration and fixture. Directory names describe the server/client feedback variant: `baseline`, `baseline-reverse`, `fixed`, and `queue2`. The two baseline runs reverse capture order to expose short-run ordering effects. Screenshots are retained as raw evidence. Baseline and fixed screenshots were visually checked; both queue2 screenshots are black, so queue2 has no visual proof of a live scene and is reported from logs only.

The portable parser reports medians of all measured decoder two-second **window means**. The captures were warmed before measurement; no boundary report is discarded. Fresh-source is the median of the render report's new-source count divided by its printed report duration. Pose age is the median displayed pose-age mean.

| capture | decoder GPU ms | wall ms | passA / passB ms | fresh source/s | pose age ms |
|---|---:|---:|---:|---:|---:|
| baseline (two runs pooled) | 8.5 | 9.98 | 1.8 / 6.75 | 46.75 | 66.0 |
| fixed | 2.3 | 3.15 | 0.6 / 1.65 | 30.0 | 75.6 |
| queue2 (rejected) | 2.4 | 3.4 | 0.6 / 1.8 | 30.5 | 77.4 |

The fixed change lowers decoder work but delivery cadence is also lower than baseline. Queue2 was rejected: it does not improve the fixed result and has slightly higher decoder wall/GPU and pose age. These short captures do not establish FPS or a feedback correctness cause.

Reproduce from the integration worktree with the same QP40/pace60 config and the corresponding APK/server pair. The fixed server integration was commit `a85ccc46`; baseline server SHA256 is `508ff68bce2f01532d09d205b001e3057f537b4ef3a020caa10821f33b7aa95c`; fixed server SHA256 is `1cfcf0573e47aef8c319e23ffdfac5ae4fc318fec0e488b238ad3c8b57d68db0`. Recorded APK SHA256 values are fixed `964dd243e20a140be8f46ad463770a126190adfcbfa80d01235bb6094b58eb06` and queue2 `711d5fd6bef3348b80d32f1f5889a38c6e7309c13945f2a71bb81690975288c2`.

```sh
python3 summarize.py baseline/*measure.log baseline-reverse/*measure.log fixed/*measure.log queue2/*measure.log --out summary.json
```
