# Compression/cache provisional report

This directory contains sanitized numeric CSV/JSON data and a self-contained `build_report.py`. Rebuilding reads only the included `runs.csv` and `pico_decode.csv`; it does not require private logs, photos, paths, or the original benchmark outputs.

The predictor comparison measures **complete full-frame codec payload** (detail+safety), excluding transport, FEC, and optional padding. It covers four runs (forest/dark, shift 0/8) and saves 8–11% across all four frames; this is not the detail-only 10.7% figure. Four raw byte-exact hashes were verified by the fixture harness.

CPU predictor encoding adds about 0.4–0.6 ms. Reusing the encode cache saves about 0.6–1.0 ms on cache-hit samples. The Pico table is from the latest production Zstd benchmark: `pointer-local-v2` is the production pointer-local decoder and `ordinary-zstd-v1` is the ordinary Zstd v1 baseline. NEON timings are excluded.

The fixture uses CPU NV12 conversion and **two identical photo eyes**. It is a bounded codec benchmark, not real game content or physical motion, and does not prove photon latency or end-to-end streaming behavior.
