# ASTC 8x8 fine-weight read-only audit

No repo source edits, builds, GPU work, photo reconstruction, app/device changes, or Pico claims were made for this audit.

## Existing PC format

`server/shaders/astc_encode.comp` emits standard ASTC 8x8, one CEM8 RGB endpoint pair, mode `0x0F3`: a 5x5 one-plane weight grid at QUANT_8 (8 levels). Its 25 symbols use 75 bits; 17 fixed header bits leave 36 bits for six CEM8 endpoint integers, QUANT_64. Total: 11 mode + 2 partition-count + 4 CEM + 36 endpoint + 75 weight = 128 bits / 16 bytes. The shader's eight decoded weights are `{0,9,18,27,37,46,55,64}`. Endpoint precision is additionally quality-rung limited by `quantEndpoint`.

The decoder/table inspection uses the bundled astcenc reference source under `wivrn-hybrid-client-build/_deps/libktx-src/external/astc-encoder/Source`; no binary probe was run. `decode_block_mode_2d()` and `get_ise_sequence_bitcount()` accept each candidate when weight count <= 64 and weight bits 24..96. The reference parser chooses endpoint ISE from remaining block bits.

## Same-byte candidate capacity (legal by the reference decoder equations; not packed/decoded in this read-only pass)

- Balanced first gate: mode `0x108` = 6x6 QUANT_4. 36 weight symbols × 2 bits = 72. The 39 available endpoint bits choose QUANT_80 for six CEM8 values (38 encoded bits plus one pad), for 128 total. Compared with baseline: 36 vs 25 nodes, four vs eight weight levels, modestly more endpoint precision. This is the cleanest spatial/detail test, though four levels can band and may toggle under small motion.
- Aggressive crispness probe: mode `0x544` = 8x8 QUANT_2. 64 one-bit weights use 64 bits. The 47 available endpoint bits choose QUANT_192 for six CEM8 values (46 encoded bits plus one pad), for 128 total. Grid aligns one-to-one with the 8x8 texels, so expect much harder binary masks, palette/posterization loss, and greater temporal shimmer risk. Endpoint capacity is high but does not restore intermediate spatial weights.
- 6x6 binary QUANT_2 is also within limits: 36 weight bits leave 75 endpoint bits, enough for QUANT_256, but it has the same severe two-level mask risk.

Smallest recommended next experiment: build four synthetic 8x8 blocks only (flat ramp, one-pixel vertical edge swept through all eight phases, 2x2 colored blobs, and three-color boundary), pack baseline `0x0F3`, `0x108`, and `0x544` with the same endpoint fitter, and decode with astcenc's physical-to-symbolic/reference decode path. Assert 16 bytes, standard ASTC validity, exact footprint; score RGB SSE and edge spread, then shift the pattern by one source texel and measure decoded-block delta/weight-mask changes for shimmer sensitivity. Start with `0x108`; test `0x544` only if it clearly sharpens edges without unacceptable color or phase instability. No GPU timing or live/Pico claim follows from this gate.

## Color palette alternatives

The production shader already has a standard dual-plane mode `0x10442`: 4x4 grid, two QUANT_4 planes (64 weight bits), two-bit component selector, and 45 endpoint bits selecting QUANT_160. It keeps one CEM8 endpoint pair but lets one selected channel use a different weight field; this helps channel-specific chroma structure, not extra palette colors. Existing offline top-5%-chroma q6 gate measured +0.673 dB dark / +0.106 dB forest, with Zstd growth +0.006% / +0.146%; these are prior CPU/reference results, not a fresh run or Pico cost proof. The full ungated dual-plane scratch shows large compressed-byte growth, so retain the existing per-block error gate.

Two partitions can represent more colors: two matched CEM8 partitions need a 10-bit partition ID plus endpoint-type signaling and 12 endpoint integers. A 5x5 QUANT_2 field (mode `0x0E1`, 25 weight bits) leaves 74 endpoint bits, enough for 12 QUANT_64 values (72 bits); it is byte-valid but has binary weights and partition-boundary flicker/search cost. Finer 6x6 plus two partitions cannot retain Q64 endpoints at this footprint. The existing two-partition 3x3 QUANT_8 scratch is already a rejected direction for dense hair: broad ROI gains were modest and target crop details looked softer (+0.017 dB hair ROI). Do not repeat that test. Two partitions are plausible for discrete multi-color blobs but not the first gate because partition classification is more complex and temporally unstable.

Same 8x8 ASTC footprint and 16-byte block keep raw ASTC bytes and sampling dimensions fixed; Zstd packet size can still change. ASTC decoder hardware work may differ by mode; no cost equivalence is claimed without device measurement.
