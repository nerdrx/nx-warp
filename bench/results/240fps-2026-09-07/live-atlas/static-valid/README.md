# Valid static-only pair

The preflight banner confirms that both arms actually ran at 2160x2160 with
the same full-resolution static checkerboard/cubes scene. The **static-only
restriction** measured decoder GPU 13.8 ms, decoder wall 17.5 ms, renderer GPU
3.8 ms, and 36 fresh sources/s. The **unrestricted optimistic** arm measured
5.1 ms, 9.9 ms, 5.9 ms, and 82 fresh sources/s. These are medians of active-window means;
they are not FPS or causal claims. The restriction was a test condition and
was removed afterward; it is not retained as an experimental default.

Per-frame arrival-to-render-selection latency (p50/p95/p99) was
28.931/67.190/88.402 ms with the restriction and 20.268/28.382/30.414 ms
unrestricted. The shared latency parser excludes the first 10 seconds and
uses observed stream-0 frames; these are not photon-latency measurements.

The preflight checker verifies runtime setup; it does not calculate latency.
The retained relative-time CSV permits analysis without publishing absolute
clock values or private raw logs.

The unrestricted optimistic screenshot has visibly worse block trails around cube edges than
the static-only screenshot. These are static images and cannot establish motion
correctness. Unrestricted optimistic admission remains opt-in; the static-only restriction was removed. The timing
archive stores relative timestamps only; `optimistic-static-only.patch` records
the source experiment. Preflight JSON records the verified banner, dimensions,
and binary hash without private paths.
