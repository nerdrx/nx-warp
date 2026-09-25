# Residual codec comparison

Scratch CPU benchmark for motion-residual bodies. Compare plain Zstd, predicted
Zstd, and the existing full selector (LZ4, plain Zstd, predicted Zstd). Inputs
are caller-supplied NXDF fixtures; no photo data is stored here.

Run with two native RGB888 NXDF files:

```sh
WIVRN_ROOT=/path/to/wivrn ./run.sh FOREST.nxdf DARK.nxdf
```

The harness synthesizes a two-half shift from the forest frame, then measures
five 523,728-byte residual inputs: each photo frame as an independent byte
stream, mixed motion with global vector zero, mixed motion with correct +8 x
vector, and forest-to-dark scene cut. It includes 8 warmups and 24 timed codec
calls per method. `results.csv` records medians, nearest-rank p95, encoded
sizes, and exact decode equality. `full_selector` uses the current selector
thresholds. `predicted_zstd` output uses the production stride-4 predictor and
Zstd envelope; when compression misses its threshold, helper returns original
bytes. All outputs decoded byte-for-byte to input.

Recorded run used local forest and dark captures on Ryzen 9 9950X3D, GCC `-O3`,
Zstd and LZ4 system libraries. Timings are host-only; not Pico or live-session
measurements. The two source captures are intentionally not included. Provide
any compatible fixtures through the command line to reproduce.
