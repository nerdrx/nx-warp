# Atlas vertex-warp v1 evidence

The [final cleaned implementation check](v3-final/README.md) reproduced
**3.10 ms enabled / 6.30 ms disabled** with the same final APK, full resolution,
and explicit activation evidence. Prototype results below remain separate.

![Ordered vertex-warp, control and repeated vertex-warp GPU measurements](render-comparison.png)

Rebuild the figure with `python3 plot.py` (Matplotlib required). The diamonds are
estimates excluding cache-hit iterations, not confidence intervals.

These are diagnostic client-render captures at native 2160x2160 per eye: `3a` vertex-warp profile, `0` original control, and `3b` reverse vertex-warp arm. The APK used for v1 has SHA-256 `9e04da71760c37600133444d7393956bfbb0d474a765b29cd097b9484ea67aca`.

The parser excludes the first 10 seconds from each capture using the embedded capture-start timestamp. It reports medians of periodic GPU window means and does not report frame-level p95/p99 or infer FPS. Reproduce filtered summaries with:

    python3 summarize-render-prof.py '*-render-filtered.log' --warmup 10

Results: `3a` 2.9 ms/iteration, 261 ms/s duty, adjusted non-cache estimate 3.285 ms; control `0` 6.25 ms, 560.95 ms/s, 6.302 ms; `3b` 3.15 ms, 281.925 ms/s, 3.204 ms. These are separate diagnostic captures; the 3a selection metric was about 11.7 ms and is not presented as a large latency gain. Screenshots are retained as capture checkpoints without an artifact-free quality claim.

`reviewed-followup.patch` is the current source diff for review and includes an extent guard added after the v1 APK was built, so it is not exact v1 source provenance. The standalone float32 homography check and summary are included for numerical context, not bit-exact shader proof. SHA-256 records cover every packaged file.

## Pipeline timing CSV

The normalized `.csv.gz` files retain every timing row and stream flag, with column 2 shifted by that file's common minimum timestamp. They contain protocol frame numbers needed for unambiguous joins but no device identifiers or network addresses. `summarize_pipeline_latency.py` accepts the gzip files directly. With 10 seconds warmup, encode-to-feedback results were:

| arm | receive p50/p95/p99 ms | decode-end p50/p95/p99 ms | blit/selection p50/p95/p99 ms | samples (receive/blit) |
|---|---:|---:|---:|---:|
| control 0 | 3.601 / 4.309 / 4.861 | 8.191 / 9.207 / 11.018 | 11.093 / 12.009 / 16.069 | 1558 / 1532 |
| vertex 3a | 3.909 / 4.569 / 4.948 | 6.226 / 7.443 / 9.099 | 11.702 / 16.161 / 17.280 | 1427 / 1360 |
| vertex 3b | 3.672 / 4.247 / 4.717 | 5.992 / 7.444 / 9.467 | 8.292 / 15.099 / 17.029 | 1477 / 1446 |

`blit` is the compositor selection event, not photon latency. `pipeline-summary.json` records the machine-readable output; these stage percentiles are separate from the GPU window means above and are from separate diagnostic captures.
