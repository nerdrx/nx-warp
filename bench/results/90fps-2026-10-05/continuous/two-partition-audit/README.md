# Two-partition ASTC 8x8 read-only audit

## Finding

Two CEM8 RGB endpoint pairs are legal in a standard 8x8, 16-byte ASTC block,
so they can represent two local color lines instead of the production's one
global endpoint pair. They keep the same block dimensions and texture sampling.
That does not mean equal GPU decoder cost; no Pico timing is available for these
modes.

One valid same-footprint example is a two-partition, matched-CEM8 block using
mode `0x0E1`: its 5x5 QUANT_2 weight grid consumes 25 bits; partition count,
10-bit partition index, matched endpoint-type signaling and block mode cost
2+10+6+11 = 29 bits;
the remaining 74 bits encode twelve CEM8 endpoint integers at QUANT_64 (72
bits). This uses 126 of 128 bits, leaving two padding bits. Each partition
therefore has its own six RGB endpoint codes. QUANT_2 means the grid symbols
have two values; ASTC decimation/interpolation combines those symbols at texels,
so texel weights can take intermediate values. The cost is still coarse 5x5
spatial control, not a strictly binary per-texel mask. The reference ASTC parser
stores partition count/index and per-partition color formats separately, then
chooses endpoint quantization from the remaining block bits. Source: bundled
`astcenc_symbolic_physical.cpp` around lines 201-285 and 390-501; the legal
weight-mode parser is `astcenc_block_sizes.cpp` around lines 36-120 and 862.

Other legal trade points exist. A 3x3 QUANT_8 grid is mode `0x1BF` under the
bundled parser: its 27 weight bits plus 72 Q64 endpoint bits and 29 bits of
mode/partition metadata use all 128 bits. Mode `0x1AE` is also 3x3, but parses
as QUANT_4 (18 weight bits) and is rejected by the ASTC minimum 24 weight-bit
rule. A 6x4 QUANT_2 grid uses mode `0x141` (24 weight bits), leaving 75 bits
for endpoints, enough for Q64 plus three spare bits. By contrast, 6x6 QUANT_2
spends 36 weight bits and leaves only 63, too little for twelve Q64 endpoints.
Thus spatial grid density, weight precision, and endpoint precision trade
against one another; no single layout is the only legal choice. Partition
assignment adds another source of frame-to-frame mode changes.

## Existing fitter and evidence

Production's `server/shaders/astc_encode.comp` fits one RGB endpoint line using
tile PCA, quantizes the endpoint pair, solves a 5x5 least-squares weight grid,
and optionally refits endpoints. Its gated dual-plane candidate is implemented
in `server/shaders/astc_encode_colour.glsl`; it still uses one endpoint pair
and two 4x4 weight planes to fit a channel-specific chroma change. Two
partitions address a distinct limitation (two color lines), but require
partition classification and a separate endpoint/weight fit for each side.
No partition-search PC timing is established here; expect materially more
encode work than the single-line path, without assigning a numerical cost.

The closest real-image comparison is evidence against that particular
one-seed fitter, not every possible partition fitter: it used 1920x1080
fixtures and emitted mode `0x053` on 82 dark and 17 forest blocks. Against the
guarded q6 dual-plane output it lost 0.5253/0.0761 dB full-frame PSNR and grew
Zstd-3 by 43/51 bytes; diagnostic high-chroma and
chroma-edge PSNR also fell in both scenes. Its earlier GPU query interval was
0.134/0.133 ms in its own harness versus 0.027 ms baseline-only, which is not
a comparable production timing. See
[retained partition comparison](../../../90fps-2026-10-04/astc-partition-followup/README.md).

The earlier mode-capacity audit separately describes a two-partition 3x3 Q8
candidate, whose dense-hair target crop was softer (+0.017 dB ROI); that
specific candidate should not be repeated. See
`overnight-recovery/astc-fine-weights-audit/findings.md`.

## Decision

Reject another broad two-partition photo gate for now. The existing real-image
comparison rejects its specific one-seed fitter; it does not settle all legal
partition/weight layouts. Mode `0x0E1` offers a plausible narrow synthetic
test for isolated two-color blobs, while `0x1BF` and `0x141` trade spatial
resolution and weight precision differently. Any future gate should first
show the complaint is a two-palette failure case, compare against the already
selected dual-plane policy, and track temporal partition/mode flips. The
existing evidence does not justify production integration or a general
partition search.

The only build here is a small CPU parser check of the extracted bundled mode
decoder; no packing, image decode, private image/payload use, GPU work, device
work, or production source changes occurred.

## Root verification

Root corrected the initial audit's invalid `0x1AE` mode, omitted padding,
overly broad layout rejection and fixture scope. The check below extracts the
actual bundled parser function at runtime and links its quantization helpers
from the actual bundled AVX2 library. It asserts all four expected results.
This verifies weight-mode parsing only: metadata/endpoint capacity arithmetic
is source-reviewed, not a complete two-partition pack/decode test.

```sh
python3 check_modes.py ASTC_SOURCE_DIRECTORY ASTC_STATIC_LIBRARY
```

Requires a compatible x86 AVX2 compiler/library build. Source/library hashes
and exact output are in `root-parser-check.log`. No private inputs are needed.
