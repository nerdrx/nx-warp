# Same-queue ASTC upload handoff check

Pico A8110 / Android 10, synthetic 2176×2176 single-eye sampler, 8×8 ASTC. Each pass submitted upload/layout-transition command A without a fence, then sampling command B to the same queue; no CPU wait or semaphore separated them. B’s fence gated timestamp retrieval, readback, and reuse.

A/B/A used 12 warmups and 30 timed ASTC samples per run. All three returned exit 0; external-decoder references had max channel error 1 and zero pixels above tolerance 2. Source fixtures contain 73,984 repeated 8×8 blocks each: baseline ordinary CEM8 and stress all legal mode 0x442. This checks same-queue handoff and decoding only; timings are reported as context, with no speedup or throughput claim.

| Run | GPU upload median | GPU sample median | GPU total median | GPU total p95 | Pixel check |
| --- | ---: | ---: | ---: | ---: | --- |
| Baseline A | 67.71 µs | 1175.89 µs | 1243.91 µs | 1248.85 µs | max error 1; 0 over tolerance 2 |
| All mode 0x442 | 66.72 µs | 1175.42 µs | 1241.98 µs | 1248.65 µs | max error 1; 0 over tolerance 2 |
| Baseline B | 67.19 µs | 1175.73 µs | 1242.71 µs | 1246.41 µs | max error 1; 0 over tolerance 2 |

The output hash for baseline A and B is identical. This is a synthetic repeated-block sampler probe, not natural-image quality, live VR, or application-level performance evidence. The isolated device scratch path was `/data/local/tmp/nx-colour-mode-same-queue-2176`; it was removed after successful validation.

`run-manifest.json`, `fixture-manifest.json`, `samples.csv`, and thermal snapshots preserve run details. Full readback rasters and ASTC fixtures remain in the scratch experiment directory and are referenced by SHA-256; private source photos are not included.
