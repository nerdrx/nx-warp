# Repeated transcode and device LZ4 evidence

Host and Pico transcode helper both ran 12 warmups and 30 samples on `dark-xuastc-12x12-q50.ktx2` (4352×2176, ASTC 12×12). Raw stdout/stderr are preserved in `host-transcode.log` and `pico-transcode.log`.

- Host: init+start median 0.005 ms; transcode median 15.907 ms; full-call median 15.983 ms, p95 16.683 ms.
- Pico 4 / Android 29: init+start median 0.006 ms; transcode median 29.875 ms; full-call median 29.882 ms, p95 30.461 ms.
- Both reported repeat output byte-exact. Host ASTC, Pico ASTC, and existing CLI ASTC have identical SHA-256 `24452fe3f1ee4dec1f79b47f034806a2b649d38733ccccd238e0f7cc3865c60d`.

`pico-lz4.log` records one run of the standalone safe LZ4 benchmark using the existing dark 8x8/q25 compressed payload. It ran 12 warmups and 30 measured decodes from loaded memory into its persistent output buffer. Input was 634,704 bytes; output was 2,367,488 bytes. Median 0.458 ms, p95 0.603 ms; every sample was byte-exact to the ASTC payload.

Raw logs are in this directory. Large ASTC output snapshots remain in private scratch storage; their hashes are retained. SHA-256 values for those files, inputs, references, and Android executables are in `hash-manifest.txt`. Device files created for this rerun and the earlier helper check were removed after capture; no APK or graphics workload was used.
