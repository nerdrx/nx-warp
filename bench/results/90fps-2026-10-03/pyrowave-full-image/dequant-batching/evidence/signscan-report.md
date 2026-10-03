# DQ sign-prefix candidate (offline correctness/build check)

Baseline source: `wt-pyrowave-probe` commit `f45767d346b90aa5e1a01468a7f1d718af228b59`. Only the private shader `common/pyrowave/shaders/wavelet_dequant.inc` differs; see `signscan.patch`. For \"gl_NumSubgroups <= 2\", the candidate keeps the first workgroup barrier that publishes raw subgroup sums, skips prefix scanning and its second barrier, and relies on group 0 needing no prefix and group 1 needing only sum 0. The existing <=8 scan and >8 fallback paths are unchanged. This changes no quantization/sign-bit decode math.

All three shader storage-mode wrappers compiled for Vulkan 1.1. The Android ARM64 archive was rebuilt from this candidate at NDK 29 / API 29; archive and SPIR-V hashes are in `build-artifacts.sha256`. Shader and Android build scripts/commands are retained in `build-shaders.sh` and `build-android-paired-haar-fused.sh`.

The host baseline and candidate helpers were compiled against Vulkan 1.3 and run on AMD Radeon RX 7900 XTX (RADV NAVI31, physical API 1.4), with the matching subgroup/storage features enabled. Readback planes were byte-identical and no validation errors appeared for:

- 4352×2176 4:2:0 native fixture (hash in `fixture-hashes.sha256`)
- 4352×2176 4:4:4 native fixture
- 1920×1080 4:2:0 fixture

Per-run output and exact comparisons are retained in `control-*.log`, `candidate-*.log`, and matching raw planes. These are one-frame correctness checks; the host decode timestamps are not performance measurements. The candidate was then checked on Pico using the matched 4:2:0 fixture. Control/candidate/control runs used 12 warmups + 30 timed samples each, with no readback. Control pooled GPU p50/p95 was 12.3557/12.9210 ms (n=60); candidate was 12.3164/12.9186 ms (n=30). CPU-total p50/p95 was 15.1746/16.6510 ms versus 15.0740/16.9020 ms. Differences are neutral and do not support a speedup claim. Raw sample logs, run order, binary hashes, and the pre-timing exact readback comparison are saved as `pico-control-A.log`, `pico-candidate-B.log`, `pico-control-C.log`, `pico-run-order.txt`, `pico-binaries.sha256`, and `pico-readback.sha256`. Temporary device files were removed after the run. No production edit occurred; candidate is not composed with band batching.
