# Pair variant experiment

This pair compares the scalar and pair implementation variants on the same
native full-resolution checkerboard/cubes capture. Runtime telemetry indicates
that the pair variant was active, but neither capture carries an explicit
variant banner; this is therefore an inferred activation and a weak causal
comparison.

The canonical stream-0, post-warmup arrival-to-render-selection percentiles
(p50/p95/p99, ms) were **20.146/28.238/30.061** for scalar (3,602 paired
frames) and **23.435/30.890/32.349** for pair (2,951 paired frames). The pair
variant regressed the measured selection timing in this run. These are reported
frame timings, not compositor, photon, or FPS measurements; no retained
optimization or speed claim follows. The screenshots are retained for visual inspection only: [scalar](scalar-screen-06.png) and [pair](pair-screen-06.png). The implementation and test patches are [atlas-view8-pair.patch](atlas-view8-pair.patch), [client](atlas-view8-pair-client.patch), and [test](atlas-view8-pair-test.patch). The capture APK hash and arm provenance are in [provenance.json](provenance.json).

Reproduce the percentiles with `python3 reproduce.py`. The normalized archive
contains relative timestamps only. The source patch is archived when the
corresponding implementation artifact is available; no private logs or paths
are included here.
