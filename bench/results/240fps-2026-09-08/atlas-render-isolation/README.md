# Atlas render isolation diagnostic

This folder records four 30-second headset captures isolating client render diagnostics. `0` is the original path, `2b` enables the gamma diagnostic, `1c` enables the homography diagnostic, and `0c` is the restored control. The labels describe diagnostics, not production modes.

The parser excludes the first 10 seconds and reports medians of periodic window means. It does not calculate frame-level p95/p99 or infer FPS. Reproduce the summaries with:

    python3 summarize-render-prof.py '*-render-filtered.log' --warmup 10

The filtered logs retain the render and `nxwarp[0]` report lines; server banner files retain only encoder/configuration and lifecycle lines. Screenshots are checkpoint artifacts, not a claim of artifact-free output. The diagnostic outcomes were: original `0` 6.25 ms/iteration, restored `0c` 6.15 ms/iteration, `1c` 3.8 ms/iteration with broken homography behavior, and `2b` 5.9 ms/iteration with incorrect gamma behavior. These are GPU window means from separate diagnostic runs and are not a matched performance claim.

`diagnostic-source.patch` is the active worktree source diff used for the diagnostic. `summary.json` records the complete parsed window data and source hashes are in `SHA256SUMS`.
