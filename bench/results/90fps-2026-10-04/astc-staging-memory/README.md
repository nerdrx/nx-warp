# Pico ASTC staging memory probe

**Result:** Keep the ordinary CPU scratch decode followed by one forward copy into the existing VMA upload buffer. Direct codec output into the mapped VMA allocation was substantially slower for every measured case. On this Pico, the existing sequential-write allocation already lands in cached coherent host memory, so changing allocation intent did not produce a different memory type.

The harness measured 12 warmups and 30 samples per configuration on Adreno 650. It compared (1) current scratch decode + copy, (2) direct decode into that same sequential-write buffer, and (3) direct decode into an AUTO random-access buffer. All three VMA allocations selected memory type 4, heap 0, with `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED`. The VMA-selected actual flags are the device result; creation flags alone do not guarantee the allocation type.

Median CPU decode-path time, microseconds:

| Input | Codec | Scratch + copy | Direct to current | Direct to random |
|---|---|---:|---:|---:|
| Repeated 2176×2176 ASTC mode 0x442 | LZ4 | 245 | 1,681 | 1,639 |
| Repeated 2176×2176 ASTC mode 0x442 | Zstd 3 | 402 | 3,565 | 3,505 |
| Dark 1920×1080 q6 ASTC | LZ4 | 242 | 3,472 | 3,475 |
| Dark 1920×1080 q6 ASTC | Zstd 3 | 1,730 | 3,935 | 4,064 |
| Forest 1920×1080 q6 ASTC | LZ4 | 235 | 3,360 | 3,679 |
| Forest 1920×1080 q6 ASTC | Zstd 3 | 1,254 | 4,062 | 3,778 |

The queue behavior stayed identical: ASTC upload and sampling were separate submissions to one queue, with no intermediate CPU wait or semaphore; the final sampling fence gated readback and buffer reuse. GPU upload/sample medians were effectively unchanged across the three CPU paths. Each fixture/codec produced identical GPU readback hashes across all paths. Hardware sampling differed from the external RGBA references by at most one channel value, with no values over tolerance 2.

These are bounded harness timings, not production-stream throughput or an app performance claim. The synthetic mode-0x442 image is repeated texture content; the two q6 cases use exact 1920×1080 ASTC outputs. No full source photos or payloads are included.

`summary.csv` has per-run medians and hashes; `samples.csv` preserves all 540 measured samples; `readbacks.csv` lists input, payload, and output hashes plus pixel deltas. `manifest.json` records source paths/hashes and allocation properties. `probe.cpp` and `run_vma_probe.py` preserve the harness method.
