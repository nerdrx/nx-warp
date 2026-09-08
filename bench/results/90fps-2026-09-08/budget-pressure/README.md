# Artificially tight admission budgets

The single-pass scheduler meets the 90 Hz deadline in these two runs, but its
prefix-only policy **starves the outermost pixels** when the admission budget
is reduced to 2 or 3 ms. Those pixels reach 719 source frames of age (7.989 s).
This is a measured limitation, not a speed improvement.

| Admission budget | Median completion ms | p99 ms | Maximum ms | Misses / 720 at 90 Hz | Oldest tile, frames |
|---|---:|---:|---:|---:|---:|
| 2 ms | 1.336 | 4.474 | 6.629 | 0 | 719 |
| 3 ms | 1.286 | 3.973 | 7.491 | 0 | 719 |

The admission budget is a prediction threshold, not a hard bound: the centre
always runs and CPU work is not cancelled. Missing the artificial 2/3 ms
threshold and missing the actual 11.111 ms cadence are different quantities.
The scene is the same 720-frame native stereo camera motion fixture generated
at 90 FPS used in [single-pass](../single-pass/README.md). No warmup rows are
removed. This remains offscreen parsing/upload/rendering, with no network,
compositor, physical head movement or display timing included.

The device executable remains `nx-planar-single`, SHA-256
`e2af78c22d6adb04327876ef2e69ab71a4b635f00eb5cce9aa4587e8da52b3f7`.
The source fixture SHA-256 is
`dc55e3e1f5ad1f1c08b53d136ef9755c5ada562dc4614410c9718834795a98d4`.
Both identities were recorded in the preceding single-pass test; neither
file was replaced between those runs and these pressure tests.

Reproduce with the archived single-pass run command, setting
`NX_PLANAR_FOVEATED_BUDGET_MS=2` or `3`, while retaining
`NX_PLANAR_PACE_FPS=90`, `NX_PLANAR_FOVEATED=1`,
`NX_PLANAR_FOVEATED_SINGLE_PASS=1`, TILE, SPIN, command reuse, HIGH queue
priority and `taskset 80`. `raw-inputs.tar.gz` contains both complete CSVs and
logs. The shared `../centre-first/summarize.py` reproduces `summary.json`.

A future overload policy needs age-aware admission of peripheral corrections
and current-pose reprojection, while maintaining encoder/receiver reference
agreement. Merely shortening the completion time is insufficient.
