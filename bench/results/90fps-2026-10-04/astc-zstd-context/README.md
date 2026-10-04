# Reused Zstd context probe

Capture date: 2026-10-04 UTC; exact time of day was not recorded.

Pico CPU-only A/B/A test compared one-shot `ZSTD_decompress()` with a reused `ZSTD_DCtx` passed to `ZSTD_decompressDCtx()`. Both paths repeated production frame/content-size checks, decoded into ordinary scratch, then copied to the same output buffer. Fixtures came from the current guarded q6 selected-policy block streams, compressed as Zstd level 3. No GPU work was measured.

| Fixture | One-shot A median | Reused context B median | One-shot A repeat median | B vs mean A |
|---|---:|---:|---:|---:|
| Dark q6 | 582.344 µs | 486.354 µs | 502.656 µs | −56.1 µs (−10.3%) |
| Forest q6 | 399.739 µs | 381.302 µs | 395.990 µs | −16.6 µs (−4.2%) |

Each arm used 12 warmups and 30 timed calls. `run.csv` preserves all six aggregate observations and their order; the probe did not record individual sample latencies, so raw per-sample timing data is unavailable. The trailing one-shot p95s were 815 µs and 837 µs, preserving substantial outliers; dark A median also shifted by about 80 µs between A passes. Treat the apparent savings as a preliminary hint, not a production integration or frame-rate claim.

Both paths matched exact 518,400-byte outputs for both fixtures. `output-hashes.txt` records those hashes; output bytes are intentionally omitted. The manifest records source/build/input hashes without including photos or compressed payloads.
