# Matched PyroWave 4:4:4 timing probe

The feature-enabled native decoder was compiled for Android arm64/API 29 against the saved `wt-pyrowave-probe` Android build, then run on Pico serial `PA8150MGGB110166G`. It decoded the one-frame 4352×2176 PyroWave 4:4:4 fixture 12 warmup times plus 30 measured samples. The harness reported `shaderStorageImageWriteWithoutFormat` and `shaderStorageImageExtendedFormats` supported and enabled.

| Metric | p50 | p95 |
|---|---:|---:|
| PyroWave 4:4:4 host total | 32.601 ms | 33.599 ms |
| PyroWave 4:4:4 GPU timestamps | 27.673 ms | 28.431 ms |

The available NXVC Q22 4:4:4 repeat profile reports host total around 184.837/198.983 ms p50/p95. The resulting timing ratio is about 5.7×. This is timing evidence only: the saved PyroWave encoder fixture's enabled extension-feature state and quality against the original source were not established, and the original uncompressed source is unavailable here. Do not describe this as a quality-matched codec win or retain the earlier 9× 4:2:0-vs-4:4:4 comparison as matched.

An additional one-frame readback used `fragment_path=true`, matching the timed decoder. It produced Y/Cb/Cr planes of 9,469,952 bytes each; all had multiple values (Y min/max 0/246, Cb 53/236, Cr 41/230). This rules out a blank output but does not establish fidelity. The earlier compute-path readback is excluded because that reconstruction path is known to be incorrect.

## Reproduction and provenance

- Harness source: `pyrowave-pico-native-feature-enabled.cpp`, SHA-256 `70ef42b4f1860615f16cb67d0891099ce206960648651a3de9b78d4bb7910f9f`.
- Android archive: `build/android-arm64/common/pyrowave/libpyrowave.a`, SHA-256 `1efcbb20e4be3d5c2eb5fd167934aa8c141d73b515902059632834248428c472`.
- Built executable SHA-256 `5cf1be78b1bd7611bccdbdee4a4b84e381de5b2df96beb6a36530996817ea652`.
- Fixture: `host-encoder/dark-native-stereo-444-cdf.pyrowave`, SHA-256 `852b66e075364cc42fb4dbe40a1b1170f5fff5c966baebde9b3904d68d399fbc`.
- Raw timings: `matched-444-feature-enabled-run.log`.
- Fragment readback source/binary/log and captured planes are retained in `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/`.
- Rebuild script: `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/build-harness.sh`.

No production source, install, or server state was changed.
