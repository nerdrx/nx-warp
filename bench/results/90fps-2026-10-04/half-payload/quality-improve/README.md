# ASTC endpoint quality experiment (offline)

Two endpoint-quantization ideas were tested without changing production files:

- **A, contrast gate:** current q2 weight pattern, but use six-bit endpoints when a block's maximum RGB channel span is at least 64, otherwise three-bit endpoints.
- **B, luma-preserving:** current q2 weights; quantize luma more finely than chroma, reconstruct RGB, then map to ASTC endpoints. Near-black/white endpoints bypass that transform.

Inputs: dark and forest are 2176×2176 single-eye crops; crowd is a 2176×800 crop. No resizing. All static ASTC files were host-decoded successfully. Native-scale source/decoded crops are in `results/{dark,forest,crowd}/metrics/*quality-contact.png`. The pan sheet is in `motion/dark/pan-1to1-contact.png`.

## Findings

Neither endpoint idea beats current q2 on payload size. Variant A yields a small PSNR gain but increases Zstd3 payload by 6.5–12% and uses a hard threshold: a synthetic one-pixel pan flips the endpoint-precision gate for 1,254 then 1,395 of 73,984 dark-image blocks. Reject A due to size and likely temporal threshold instability. Variant B improves dark-scene PSNR by about 1 dB, but costs 12.3–34.7% more Zstd3 bytes across these fixtures (16.4% on dark), loses PSNR slightly on forest/crowd, and still misses the half-rate target. Reject B for this bandwidth target.

| Scene | q6 LZ4 baseline | q6 Zstd3 baseline | current q2 Zstd3 | q2 savings vs q6 LZ4 |
|---|---:|---:|---:|---:|
| Dark, 2176² | 501,248 B | 416,383 B | 243,333 B | 51.5% |
| Forest, 2176² | 311,230 B | 258,375 B | 130,834 B | 58.0% |
| Crowd crop, 2176×800 | 151,943 B | 116,063 B | 92,424 B | 39.2% |

q2 full-image RGB PSNR was 27.29 dB (dark), 28.73 dB (forest), and 25.84 dB (crowd). The static comparison CSV has PSNR, MAE, bytes, and hashes for q6, q3, q2, A, and B. q4 is included for dark/forest using existing ASTC outputs; it was not re-encoded.

Higher Zstd levels save a little more, but q3 never reaches half of q6 LZ4. q2 reaches half on dark/forest, not crowd. Zstd9 costs roughly 4–11 ms per host compression for these block streams versus about 1–2 ms at Zstd3; savings over Zstd3 are scene-dependent and small relative to that extra CPU time. Exact details and 10-call reused-context timings are in `zstd-levels/levels3-6-9.json`.

The synthetic pan uses edge-clamped 0/1/2-pixel horizontal shifts. After aligning decoded frames, mean absolute changes were about 1.8–2.3 RGB levels; this is only a block-boundary stability probe, not real motion or live stream proof. The hard gate's state flips reinforce rejecting A.

## Scope and reproduction

This is an RGB-buffer Vulkan compute harness on AMD Radeon RX 7900 XTX (RADV NAVI31). It does not exercise the production YUV sampling path, the live encoder, transport, headset, or frame rate. `manifest.json` records fixture and output hashes.

Build requirements: Vulkan SDK/loader, CMake, C++ compiler, `glslangValidator`, `spirv-val`, Python with Pillow/NumPy, LZ4 source, and the Basis Universal `decode_astc` helper. Set `LZ4_DIR` and `DECODER` for local paths.

```sh
cd /run/media/nerdrx/Lex/claude/nx-scratch/astc-half-rate-20261004/quality-improve
export LZ4_DIR=/path/to/lz4/source
export DECODER=/path/to/decode_astc
./build_harness.sh
python3 run_quality_matrix.py --encoder harness/build/astc-gpu \
  --decoder "$DECODER" --scenes dark,forest,crowd \
  --qualities 6,3,2,7,8 --output-dir results
python3 compress_zstd3.py
python3 compress_existing_levels.py
python3 run_pan_check.py
```

`compress_zstd3.py` validates exact CLI decompression. `compress_existing_levels.py` uses one reused `ZSTD_CCtx`, runs 10 compression calls per level, and verifies exact decompression. Its q4 dark/forest inputs are packaged in `results/`; it skips q4 for crowd because no pre-existing q4 crowd ASTC was available. `run_pan_check.py` reconstructs shifted frames from the PNG fixture, then encodes/decodes q2, q3, A, and B.

### Zstandard level sweep

Levels 3, 6, and 9 were also applied to existing q2/q3/q4 single-eye ASTC
blocks using a reused `ZSTD_CCtx`: ten host calls per level, median time, then
exact decompression check. q4 files exist only for dark and forest; no q4 crowd
output was available and none was re-encoded. q6 comparison columns distinguish
its original LZ4 sidecar from the newer Zstd3 baseline.

| Scene | Quality | Zstd3 / Zstd6 / Zstd9 bytes | Savings vs q6 LZ4 at Zstd3 / 6 / 9 |
|---|---:|---:|---:|
| Dark | q2 | 243,333 / 222,799 / 217,613 | 51.5% / 55.6% / 56.6% |
| Dark | q3 | 288,040 / 266,911 / 261,743 | 42.5% / 46.8% / 47.8% |
| Dark | q4 | 354,499 / 336,715 / 330,574 | 29.3% / 32.8% / 34.1% |
| Forest | q2 | 130,834 / 120,375 / 117,942 | 58.0% / 61.3% / 62.1% |
| Forest | q3 | 184,734 / 172,366 / 169,240 | 40.6% / 44.6% / 45.6% |
| Forest | q4 | 213,884 / 202,000 / 199,056 | 31.3% / 35.1% / 36.0% |
| Crowd | q2 | 92,424 / 85,367 / 83,131 | 39.2% / 43.8% / 45.3% |
| Crowd | q3 | 100,890 / 95,585 / 93,679 | 33.6% / 37.1% / 38.3% |

These are per encoded image payloads; they are not bandwidth or frame-rate
measurements. q2 reaches half-size on dark and forest but misses on crowd.
Higher compression levels still miss the crowd half-size goal. q2 Zstd9
compression takes 3.9–9.7 ms per reused-context host call for these sizes;
q3 takes 4.8–10.1 ms. q6 LZ4 baselines are 501,248 B / 311,230 B / 151,943 B
for dark / forest / crowd; q6 Zstd3 baselines are 416,383 B / 258,375 B /
116,063 B. The extra Zstd levels do not change ASTC decode quality.

Exact matrices, ten timing samples, and output/source hashes are in
`zstd-levels/levels3-6-9.csv` and `.json`; q6 baselines and all q6/q3/q2/A/B
comparisons are in `results/zstd3-results.csv`.
