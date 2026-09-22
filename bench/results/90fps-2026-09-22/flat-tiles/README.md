# Lossless flat-tile compaction

Status: tested host prototype, not enabled in the streamer. LZ4 already captures much of the repetition, so these results do not justify automatic deployment.

Every decoded pixel in a tile must have exactly the same RGB value before it can become an existing mode-3 solid-colour descriptor. Remaining blocks are compacted and descriptor offsets rewritten. No reference frames, changed colour precision, new wire syntax, or Pico decoder work is required. This prototype processes encoded bytes on the host; it does not avoid the original GPU encoding work.

## Results at the 500 setting

| Fixture | Raw bytes before / after | LZ4 bytes before / after | Additional wire-payload saving | Host pass p50 |
|---|---:|---:|---:|---:|
| Photo | 511408 / 488568 | 411264 / 407969 | 0.80% | 0.289 ms |
| Scene | 511408 / 471988 | 243357 / 236616 | 2.77% | 0.383 ms |
| Edges | 511408 / 471408 | 48798 / 46702 | 4.30% | 0.316 ms |
| Noise | 511408 / 511408 | 511408 / 511408 | 0% | 0.133 ms |

The largest saving among all tested rate/image pairs was 6.89% on the edge fixture at the 160 setting. These are fixed 2176×2176 stereo fixtures, not live-stream averages. The 500 setting is a budget, not measured link throughput. [CSV](results.csv) columns: file, raw bytes, compacted bytes, LZ4 before, LZ4 after, pass p50 ms, pass p95 ms.

## Validation

- All 15 fixtures decoded pixel-for-pixel identically before and after.
- Mixed modes, a differing late block, nontrivial RGB565 endpoints, malformed inputs and descriptor offset rewriting checked by standalone ASan/UBSan test.
- Timing excludes eight warmups and includes 40 measured compaction calls per fixture.
- No claim of Pico GPU or photon-latency improvement.

Sources in the integration repository: `server/encoder/nxwarp_direct_flat.h`, `tests/direct_blocks_flat_test.cpp`, `tests/direct_blocks_flat_bench.cpp`. Build the benchmark with common/server encoder include paths and LZ4, then pass NXDF fixture paths. The prototype stays separate from the active encoder.
