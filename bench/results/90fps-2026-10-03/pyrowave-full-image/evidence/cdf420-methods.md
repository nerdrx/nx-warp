# Standard CDF 4:2:0 Pico check

Offline test only; no production files, application installs, or device configuration were changed. The Pico was released after this run.

## Fixture and source

- Source is the existing private 4352x2176 side-by-side planar 4:2:0 frame, SHA-256 `9c0dd82a1597e2c7b9a03ab5055456966e235b635bd7f2d31713f43eecf66516`, 14,204,928 bytes. `cmp` confirmed it is byte-identical to the source used by the paired-Haar 4:2:0 fixture.
- Provenance recorded with that source: duplicate a 2176x2176 eye resized from `private-dark-s0-source.png`, RGB to full-range BT.709 Y'CbCr, then 2x2 Cb/Cr averaging with `floor((sum+2)/4)`; Y stays full-resolution. Source plane MAE vs the generated CDF decode is listed below.
- The private CDF fixture was freshly encoded from this exact 4:2:0 input with the standard CDF source (no `PYROWAVE_HAAR_FORMAT` define), 4352x2176, header magic `PYROWAVE`, chroma field 0. Encoder target parameter: 694,328 bytes; resulting one-packet file: 694,352 bytes. Fixture SHA-256 `34baff52fbecf8e22c0d0b8b7b528793f9fc330d3ac9f2735f96f35463367655`.

## Correctness

The standard host fragment decoder produced planes of 9,469,952 / 2,367,488 / 2,367,488 bytes. Its mean absolute source errors (Y/Cb/Cr) were 1.211457 / 1.082256 / 0.950094; maxima were 106 / 60 / 73. Existing Haar 4:2:0 output from the same source had MAEs 1.481642 / 1.310970 / 1.100527 (maxima 98 / 69 / 60).

Pico Adreno 650 fragment readback completed. Compared with the host fragment reference, per-plane output differed by at most one code value: Y MAE 0.036816, Cb 0.051165, Cr 0.039667. The Pico output's source MAEs were 1.228221 / 1.077580 / 0.947913 (maxima 106 / 60 / 73).

## Pico timing

The existing feature-enabled standard native harness used the fragment decoder path (`fragment_path=true`), with storage-image write/extended-format features and subgroup-size/full-subgroup support enabled. One fixture was looped through 12 warmups and 30 measured decodes; readback was disabled.

- GPU p50 16.8462 ms, p95 17.2323 ms
- CPU total p50 21.1093 ms, p95 21.5177 ms

For context, root's fused Haar 4:2:0 candidate was measured at 12.417 / 12.923 ms GPU p50/p95. These runs indicate standard CDF is about 4.43 ms slower at p50 on this fixture and harness; they do not establish a quality preference or live playback result.

## Reproduction artifacts

All files remain in this private scratch directory. Encoder source delta is `cdf420-encoder.patch`; source helper is `encode-frame-host.cpp`; command script is `build-host-cdf420.sh`; logs are `host-encode.log`, `host-decode.log`, `pico-readback.log`, and `pico-bench.log`. Readback and bench commands used ADB serial `PA8150MGGB110166G` with `/data/local/tmp/cdf420-20261003.pyrowave`.

Important file hashes:

- Host CDF encoder: `8335fca610df33e58a0419d35816cba39c5deb2edd9a89ee77276d4a8aac5125`
- Standard Android CDF library used for benchmark: `6a81e10a2e8cb5db4d09626a030c6875a1d9a2956efa1c4e4e95b3051f1c61ae`
- Pico native benchmark binary: `0c113ded0218fe07e3ab1ea09df4f539c22461be18bd5bcff545e20c427db50e`
- Pico fragment readback binary: `97d5069d9f4583608b753d741958b2265343a9eca0985f42590b112fa6ce5def`
- Benchmark source: `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/pyrowave-pico-native-feature-enabled.cpp`, SHA-256 `70ef42b4f1860615f16cb67d0891099ce206960648651a3de9b78d4bb7910f9f`
- Readback source: `/run/media/nerdrx/Lex/claude/nx-scratch/pyrowave-matched444-20261003/pico-fragment-copyfix.cpp`, SHA-256 `79060a2b04213565929d4ad43bf6bf9e1ce31b6b330f991a2517ae296dcfe243`
