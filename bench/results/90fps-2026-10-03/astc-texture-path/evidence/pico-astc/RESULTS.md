# Pico ASTC sampling benchmark

Adreno 650 reports Vulkan 1.1, `textureCompressionASTC_LDR`, sampled ASTC
8x8/12x12 formats, and timestamp queries. Both source formats render through a
full-frame 4352x2176 compute sampling pass into RGBA8. This adds one full-frame
sampling pass; results do not represent compositor presentation or total XR
frame cost.

| Fixture | Path | GPU upload median | GPU sample median | GPU total median / p95 | CPU call median / p95 |
|---|---|---:|---:|---:|---:|
| Dark q50 ASTC 12x12 | ASTC payload | 79.27 µs | 2612.03 µs | 2690.10 / 4363.96 µs | 4130.57 µs / not recorded |
| Dark q50 ASTC 12x12 | LZ4 decode + upload | 78.54 µs | 2340.99 µs | 2419.53 / 3688.02 µs | 4045.42 µs / not recorded |
| Dark q25 ASTC 8x8 | ASTC payload | 191.62 µs | 2612.50 µs | 2804.11 / 4799.69 µs | 4296.56 / 5475.42 µs |
| Dark q25 ASTC 8x8 | Alternating LZ4 frames | 192.45 µs | 2341.51 µs | 2537.92 / 3408.18 µs | 4705.47 / 5751.93 µs |

Both LZ4 runs decode into persistent cached CPU memory, copy to mapped staging,
then upload and sample. Their CPU call measurements include decompression,
staging copy, flush when needed, command recording, queue submit, and fence
wait. They do not include network transfer. At 500 Mbps, compressed payload
serialization lower bounds are 13.923 ms for q50 and 10.155/10.148 ms for the
two q25 frames.

The q25 alternating run uses a synthetic two-pixel horizontal shift, not live
headset motion. Its final sampled frame differs from the desktop ASTC decode by
at most 1 channel value, with MAE 0.01047 and zero pixels beyond tolerance 2.
The q50 ASTC output differs from its desktop decode by at most 1, with MAE
0.02907 and zero pixels beyond tolerance 2. The q25 per-sample CSV is in
`logs/q25-alternating-samples.csv`.

`build.sh` reproduces shader compilation/validation and the Android arm64 build.
The report bundle includes source, the generated SPIR-V, LZ4 license/version
provenance, aggregate logs, and q25 timings. Large RGBA fixtures and executable
are not included; fixture hashes are recorded in `INPUT-SHA256SUMS`.
