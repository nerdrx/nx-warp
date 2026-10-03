# Typed dequant store check: Pico

Fixture: native dark stereo 4:2:0 Haar, 4352×2176, SHA-256 in `pico-artifacts.sha256`. The first located fixture was 4:4:4 and was not used for timing. The final readback logs report plane sizes Y/Cb/Cr = 9,469,952 / 2,367,488 / 2,367,488 bytes; control and typed-candidate planes compare byte-for-byte.

Run order: control A → typed DQ candidate → control C. Each timed run used 12 warmups + 30 samples, no readback. All runs reported Adreno 650, Vulkan 1.1. Exact per-sample logs: `bench-control-A.log`, `bench-candidate-B.log`, `bench-control-C.log`. The control p50/p95 are pooled across both control runs (n=60); candidate n=30.

| GPU decode | p50 | p95 |
|---|---:|---:|
| Control pooled | 12.4297 ms | 13.0479 ms |
| Typed DQ | 12.3668 ms | 12.9406 ms |

Typed DQ changed the pooled GPU medians by −0.063 ms at p50 (−0.5%) and −0.107 ms at p95 (−0.8%). Treat this as neutral; it does not support a speedup claim. CPU totals and command-record times are in `pico-summary.csv`.

Readback validation: `readback-control-420.log`, `readback-candidate-420.log`, `readbacks.sha256`, and the pulled output planes. Build script: `build-pico-harnesses.sh`; host candidate shaders/archive and source patch remain alongside this report. Temporary `/data/local/tmp/typed-*` files were removed after the run; no app was installed and no production files were edited.
