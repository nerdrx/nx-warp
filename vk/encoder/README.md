# vk/encoder — the NX Warp GPU encoder

The encoder side of the codec: a chain of Vulkan compute passes, all indirect
where the tile count varies, with no CPU work between them. It runs on the
compositor's own `VkDevice` (Monado's on Linux, the helper's on Windows), so
the source frame is already a `VkImage` in the right device and the handoff is
an image memory barrier plus a timeline semaphore, not a copy.

Specification: `docs/PAPER.md` §3.6 (pipeline shape, timeline values, GPU-side
rate control, host-cached output buffer), §4.6 and §4.6.1 (rate-control inputs
and the degradation ladder), §5.2 (perceptual terms), §1.3 (colour), §3.7
(bit-exactness and vendor rules), §3.2.6 (subgroup portability).

## The pipeline

| Pass | Shape | Work | State |
|---|---|---|---|
| **E0** `convert` | one group per tile, 16×16 lanes | import-format source → tile-major coded planes. RGBA8 / RGB10A2 → YCoCg-R, or 2-plane 4:2:0 YCbCr passed through. 4:4:4 and 4:2:0. | **done** |
| E0b `warp` | fullscreen, 8×8 | warped reference from the previous reconstruction and the pose delta; writes the warped image and the per-tile corner displacements | not started |
| **E1** `stats` | one group per tile, 256 lanes | per-tile mean luma, moments, sum of squared deviations, structure tensor, warped SAD at a per-tile offset | **done** |
| **E2** `prefix` | workgroup scan + single-block second level | exclusive prefix sum over per-tile byte sizes, up to 8192 tiles | **done** |
| **E3** `forward` | one group per tile, 64 lanes | DC-plane intra prediction, residual, forward 8×8 DCT through LDS, dead-zone quantisation with the weighting matrix, sign hiding, coefficients in coding-unit order. Directional intra behind a specialization constant | **done** |
| **E4** `rans_encode` | 8 lanes per tile, 8 tiles per group, persistent | rANS backwards over the operation list into a bounded per-tile slot, over 12, 16 or 27 contexts, built-in or transmitted tables; writes the tile header and the byte count E2 scans | **done** |
| **E5** `packetize` | one group per tile | compaction into tile-row segments, tile-row and frame headers, the transmitted table area, straight into the host-cached output buffer | **done** |
| E3b `reconstruct` | one group per tile | the decoder's Pass B, *byte-identical SPIR-V*, writes the new reference | not started |

The pass letters follow the paper's table with one renaming: the paper calls
the transform pass E2 and the reconstruction pass E3, but E2 is taken here by
the prefix sum, so the transform is E3 and the reconstruction becomes E3b.
E0b is a placeholder for work the paper folds into E0.

The reconstruction pass being *byte-identical shader code to the decoder* is
the single most important rule in the project: the encoder must never hold a
reference the decoder cannot reproduce.  E3 already contains the reconstruction
the directional predictor needs (it is the same arithmetic), so E3b is a matter
of writing the reference picture out, not of writing the maths again.

## What is in the tree today

```
stats/
  tile_stats.h        the per-tile statistics record: the ABI between the GPU
                      kernels and rc/, the mode decision, and the packetizer
  rc_adapter.h        converts a record into what nxrc::TileStats wants
  nxe_common.glsl     GLSL mirror of the above plus the shared integer helpers
  E0_convert.comp     colour conversion / passthrough
  E1_stats.comp       per-tile analysis
  E2_prefix.comp      two-level exclusive prefix sum (3 passes, one source)
  stats_cpu.h/.c      bit-exact CPU models of E0, E1 and E2
forward/
  nxe_enc.h           the E3/E4/E5 contract: frame parameters, the per-tile
                      job record, the coefficient and slot geometry
  nxe_tables.h/.c     the normative constant tables, so the directory builds
                      without the reference codec
  nxe_enc_common.glsl GLSL mirror of all of the above
  forward.comp        E3
  rans_encode.comp    E4
  packetize.comp      E5 (two variants: zero the frame's bytes, then write)
  forward_cpu.h/.c    bit-exact CPU model of E3
  rans_cpu.h/.c       bit-exact CPU model of E4 and of E5's layout
tools/
  vk_min.h/.cpp       throwaway Vulkan boilerplate; delete when vk/common lands
  nxvc-stats-test.cpp GPU-vs-CPU diff harness and timing for E0/E1/E2
  nxvc-vkenc.cpp      the encoder harness: --in yuv --out nxv
  nxe_host.cpp        its frame driver (the E0 stand-in, the table-set choice)
  nxe_vk.cpp          the Vulkan backend for E3/E4/E5
  nxe_selftest.cpp    the built-in configuration table and its digests
cmake/gen_spv.cmake   glslc → SPIR-V → C array
```

## The two things in E4 that are not obvious

**A GPU cannot materialise the operation list.** `ref/src/entropy.cpp` encodes
a tile by building the whole global operation list and walking it backwards.
But `encode_units` drives the lane machines in *rounds* — one operation per
unfinished lane per round, in lane order — and a lane machine reaches `kDone`
once and never restarts. So lane *l* occupies rounds 0..nops[l]-1 and the
global order is (round ascending, lane ascending). The backward sweep is
therefore eight lanes in lockstep over descending rounds, with a running
emission counter for the byte order and no list at all.

**The bytes are produced back to front.** Their offsets are only known once the
emission count is, which would force a counting pass and then a placing pass
over the same operations. Instead the renormalisation words are anchored at the
*end* of the tile's slot, one whole word each, emission *e* at
`slot_end - 1 - e`: the position is known the moment the word is produced, and
reading that region forwards yields exactly the bitstream's order. It costs a
word per 16-bit emission and buys the whole second sweep.

The other thing worth knowing is that the sign-data-hiding decision, which the
reference makes by comparing `double` squared errors, is reproduced exactly in
integers. Multiplying the comparison by 256 and factoring the difference of
squares gives `-d * step * (32a - (2m + d)*step)`, formed as a 64-bit product
with `imulExtended`; the derivation is at `nxe_hide_sign_unit`.

## The library

`nxvc_vk_encoder` is the installable form: the C ABI in
`include/nxvc/nxvc_vk_enc.h`, built from the same sources `nxvc-vkenc` drives,
so the byte-identity the acid test pins is the byte-identity the library ships.
It is the encoder half of the pair `nxvc_vk_decoder` completes and takes the
same five adopted handles, which is what lets it run on a compositor's own
`VkDevice` — WiVRn's server selects it with the encoder option
`"backend": "vk"`.

Everything the ABI does not expose is a tool that is off. The configuration is
*fixed* at the acid test's flag set rather than defaulted to it: there is no
way through the ABI to ask for directional intra or a transform tool, and
inter, alpha, 4:4:4 and 10-bit are refused at `create()` rather than accepted
and quietly coded as something else.

What the fixed set now contains is every **entropy-side** tool: `CTX_V3` (25),
`CUSTOM_TABLES` (6) and `TAB_V2` (26) alongside `CTX_V2` (21) and `SIGN_HIDE`
(22). All five are lossless -- they change how the coefficients are coded and
never which coefficients there are -- so turning them on moves bytes and not
pixels, and `vk.encoder.acid.api` pins the whole set against `nxv-enc` at the
matching flags. They are worth **9.5 % BD-rate** on the measurement below,
which at the headset's roughly one millisecond of decode per kilobyte is
frame rate.

`nxvc_vke_tile` carries each tile's byte **offset** as well as its length,
which the reference codec's C ABI cannot report. E5 computes the layout, so
this is a read of it; a transport that has it can lose one tile per lost
datagram instead of a whole frame.

Two seams were found by joining E0 to E3 for the first time and are worth
knowing about:

* **`NXE_TS_F_CHROMA_RAW`.** E0's YCbCr passthrough zero-centres chroma, as
  the paper asks, and `nxvc_ref` with `NXVC_CT_NONE` codes it unsigned, as
  `nxvc_image` delivers it. The two are not interchangeable and the difference
  cannot be split, so the flag selects the reference's convention for a stream
  that has to match `nxv-enc`.
* **Two `nxe_frame_params`.** `stats/tile_stats.h` and `forward/nxe_enc.h`
  each define a struct with that tag and different members. They are never
  included together, and the library keeps E0 in its own translation unit for
  exactly that reason.

Tests live in `tests/vk-encoder/` and are named `vk.encoder.*`. Anything
needing a GPU exits 77 and is reported as a ctest skip when no ICD, no device,
or no device meeting the kernels' requirements is present.

`vk.encoder.acid.*` is the one that decides whether the coding passes are
right: it drives `nxv-enc` and `nxvc-vkenc` from one description of the same
synthesized picture, requires the two streams to be byte-identical, and then
requires `nxv-dec` to decode both to identical pixels. It needs the reference
tools, so it exists only in a build from the repo root; the standalone encoder
build gets `vk.encoder.forward.cpu`, which pins the same streams by digest.

The reference side of that comparison is

```
nxv-enc --no-rdo --no-custom-tables --intra-dir off \
        --split4x4 off --cfl off --tab v1 --xform 8 --entropy rans \
        --ctx v1|v2|v3
```

with every tool this pipeline does not implement **named**, not merely absent.
Three of them -- `split4x4`, `cfl` and `tab_v2` -- are on in the reference
encoder's defaults and were previously off here only as a side effect of other
flags, so the test was passing while covering a smaller stream than its own
header comment claimed. See "The minor-6 tools" below.

`vk.encoder.mirror` needs no target and no GPU: it compares every constant
`nxe_enc.h` and `nxe_enc_common.glsl` both define, 53 of them. They are two
hand-written copies of one contract, and the header used to claim static
assertions kept them in step -- which could not be true, because nothing in a
C++ translation unit can see a GLSL `#define`. `NXE_LANE_OPS_CAP` is the worst
case: the host sizes E4's operation scratch from the C header and the shader
strides it from the mirror, so a drift there points every rANS lane at another
lane's slot and writes a wrong stream with nothing logged.

## Data layout

**Tile-major packed planes.** A 64×64 tile is 4096 contiguous samples, so a
workgroup that owns a tile reads one contiguous run and never strides by the
frame width — the layout the Pass-B-style kernels want. Samples are 16-bit
two's complement, two per 32-bit word, low half first. That is the `int16`
storage width §3.7 mandates, packed by hand rather than through an `int16_t`
SSBO so nothing depends on `VK_KHR_16bit_storage` and the shader ALU stays in
`int32` throughout. A thread always owns whole words, so packing is never a
read-modify-write race. Planes are laid out Y, then Co, then Cg.

**Frames that are not a multiple of 64** are padded to the tile grid by edge
replication in E0. Those tiles carry `NXE_TS_F_PADDED`; their statistics
include the replicated samples, because those are the samples actually being
coded.

## Two source paths

On Linux the compositor hands over a `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`
image (`G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` at 10 bits), already
foveated: the colour conversion and the chroma decimation happened upstream.
On Windows the SteamVR helper hands over RGBA through the D3D11 shared texture
(§3.8). E0 has both paths and the stream header records which colour space the
planes are in — `YCoCg-R` for the RGB path, `YCbCr` for the passthrough.
Converting the compositor's YCbCr to YCoCg-R would inject a rounding error into
every frame before a single bit was coded, because the matrix is not
integer-reversible; passthrough costs nothing and loses nothing. The
passthrough subtracts the chroma midpoint so both paths hand the same shape of
data downstream: unsigned luma, signed zero-centred chroma.

E0 reads the *stored codes* through a UINT view and never applies a transfer
function. The source must therefore already be display-referred; the reasoning
is at the head of `E0_convert.comp`.

## Bit-exactness

The CPU models in `stats_cpu.c` are the specification and the shaders are
validated against them, in the same relationship §3.7 sets up for the decoder.
The argument that the GPU cannot disagree is short: every accumulator is an
integer sum, integer addition is associative (and the `u32` accumulators are
associative modulo 2³²), so the reduction tree the hardware happens to build
cannot change the answer. The kernels read `gl_NumSubgroups` and
`gl_SubgroupID` rather than assuming a width, use only `subgroupAdd` and
`subgroupExclusiveAdd` (core Vulkan 1.1, no clustered ops per §3.2.6), and
combine across subgroups through shared memory with a barrier.

That is why the harness has to run on both lavapipe and RADV: they differ by
4× to 8× in subgroup width, which is the only axis along which these kernels
could go wrong.

## The statistics record and `rc/`

`nxe_tile_stats` is an array of 52-byte integer structs, one per tile.
`nxrc::TileStats` is a struct of arrays of floats. The shapes differ on
purpose — a GPU workgroup wants to write an exact integer record, a CPU
library doing `log2` over a tile array wants floats — and `rc_adapter.h` is the
single place that reconciles them, doing the division and the logarithm on the
host where they are free and harmless.

The one thing that is *not* free to differ is the gradient operator. E1 uses
the central difference with the neighbour clamped inside the tile, which is
exactly what `nxrc::compute_one_tile_stats` uses, because rc's classifier
thresholds are calibrated in absolute units against it. E1 accumulates the
undivided difference (halving an odd difference would need a fraction), so the
record's tensor is exactly 4× rc's, and the cross term is carried as two
unsigned sums so it stays exact and in range at 10 bits as well.

## Building

The directory is self-contained. From the repo root with `-DNXWARP_BUILD_VK=ON`
it is added by `vk/CMakeLists.txt`; on its own:

```
cmake -S vk/encoder -B build-enc -DCMAKE_BUILD_TYPE=Release
cmake --build build-enc -j4
ctest --test-dir build-enc
```

`glslc` is found on `PATH` or in the NDK's `shader-tools`. If the Vulkan
headers are not in `/usr/include`, pass
`-DNXWARP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>`.

To run the diff against a specific device, or against lavapipe:

```
./build-enc/nxvc-stats-test --list
./build-enc/nxvc-stats-test --device 0
VK_ICD_FILENAMES=/path/to/lvp_icd.x86_64.json ./build-enc/nxvc-stats-test --device 0
```

The coding passes have the same shape of harness:

```
./build-enc/nxvc-vkenc --selftest --cpu          # the models and their digests
./build-enc/nxvc-vkenc --selftest --device 0     # GPU against the models
./build-enc/nxvc-vkenc --in f.yuv --w 4096 --h 2048 --eyes 2 --pix yuv420p \
                       --qp 24 --out f.nxv --check --bench 50
```

`--check` runs the CPU models alongside every frame and fails on the first
coefficient or byte that differs; `--cpu` is a complete encoder with no Vulkan
at all. The flags mirror `nxv-enc`'s, which is what lets the two be pointed at
the same input and diffed.

## The minor-6 tools

Bitstream minor 6 added seven tool bits. This is where each one stands on the
**encoder** side, and the reason is different in almost every case -- which is
why the list is here rather than in a commit message.

| bit | tool | encoder | why |
|---|---|---|---|
| 6 | `CUSTOM_TABLES` | **done** | `--custom-tables`; not a minor-6 bit, listed here because 26 needs it |
| 25 | `CTX_V3` | **done** | `--ctx v3`; byte-identical to the reference on every non-directional configuration |
| 26 | `TAB_V2` | **done** | `--custom-tables --tab v2`; byte-identical to the reference |
| 19 | `XFORM_4X4_SPLIT` | not started | needs directional intra first; see below |
| 24 | `INTRA_CFL` | not started | needs directional intra first; see below |
| 27 | `XFORM_LARGE` | not started | follow-up |
| 28 | `NEAR_SKIP` | not started | inter, Phase 2 |
| 29 | `QUAD_MV` | not started | inter, Phase 2 |
| 30 | `ENTROPY_LITE` | not started | decoder-side lever, negotiated, ships off |

**`CTX_V3` is the one that was worth doing now**, because it is the only
minor-6 tool this harness can actually *prove*. It changes entropy coding
only, so it composes with everything already here, and a `--ctx v3` stream is
not directional -- which means the acid test can compare it against `nxv-enc`
byte for byte. It does: four `v3-` configurations, byte-identical on the CPU
model, on lavapipe and on RADV.

The whole tool is three functions -- `v3_ctx_cbf`, `v3_ctx_last`,
`v3_ctx_level` in `rans_cpu.c`, mirrored in `nxe_enc_common.glsl` -- plus one
piece of per-lane state. The state is the interesting part. The neighbour
class is carried along a **rANS lane**, not along the unit list: a lane owns
units *l*, *l+N*, *l+2N*, … and the class describes the last unit *this lane*
finished, so the derivation is causal inside the lane and needs no cross-lane
read and no barrier. For the ordinary tile that is the block directly above.
E4 has to carry it through a sweep that regenerates units **backwards**, so
phase A -- the only pass that walks a lane's units forwards -- records each
unit's incoming class in the top eight bits of the per-unit slot word, next to
the operation count. A count cannot reach 2²⁴ (`NXE_UNIT_MAX_OPS` is 1411), so
the fallback path costs no extra buffer.

`vk.encoder.forward.diff` covers that fallback only by accident: it engages
when a lane exceeds `NXE_LANE_OPS_CAP` operations, which at QP 0 it does. It
was verified deliberately by rebuilding with the cap at its 1411 floor -- both
copies of the constant, which is a mistake worth making once and is now what
`vk.encoder.mirror` exists to catch -- and confirming all thirteen digests on
both ICDs.

**`XFORM_4X4_SPLIT` and `INTRA_CFL` cannot be done the way the acid test
works today**, and this is the finding that matters most for planning. Both
are on in the reference encoder's defaults, but `ref/src/codec_impl.inc` gates
them on directional intra:

```
e->fp.split4 = (cfg.split4x4 && cfg.intra_dir && !cfg.lossless);
e->fp.cfl    = (cfg.chroma_from_luma && cfg.intra_dir && cfg.ctx_v2
                && !fp.dir_layer);
```

so neither bit can appear in a stream with `--intra-dir off`. And a
directional configuration is exactly the one the acid test *skips*, because
the reference searches its own per-block intra modes and this pipeline takes
them as an input. Worse, both are **encoder decisions** made by the same
per-block rate-distortion analysis that chooses the intra mode -- a `double`
trellis -- so reproducing the coding is not the hard part; reproducing the
*decision* is, and it is the same wall the directional mode search is behind.

So the honest order is: the split and CfL coding can be implemented and
covered by pinned digests the way `dir-replace` and `dir-layer` are, but they
cannot be covered by byte-identity until the mode search itself is either
reproduced on the GPU or exported from the reference. That is a decision about
the harness, not about E3.

**`TAB_V2` and `CUSTOM_TABLES` are done, and they are where the bytes were.**
Both are entropy-side only -- the coefficients E3 produces are the same either
way, and the measured PSNR is identical to four decimal places at every QP --
so they compose with everything here and, like `CTX_V3`, a stream carrying
them is not directional and the acid test can compare it against `nxv-enc`
byte for byte.

The training is host work, and it costs no second look at the frame. The
per-tile table-set choice already builds each tile's full (context, symbol)
histogram -- that is what `select_set` minimises over -- so `train_table_sets`
keeps those histograms, pools them by the set each tile chose, quantises each
row to the 5-bit log-domain delta alphabet, and drops the rows that do not beat
their built-in default by more than the eighty bits they would cost. The
reference's Lloyd loop then reassigns every tile against the trained tables and
retrains, three times.

Two things about that are worth knowing.

* **The tables must be reset to the built-ins at the top of every frame.**
  `f.tabs` holds the previous frame's trained sets, and the reference's
  training pass scores against `deftabs`. Without the reset, frame 1 of a
  sequence is byte-identical and frame 2 is not -- the first assignment of
  every later frame is made against tables the frame does not carry, and the
  training then pools the wrong tiles. It is a one-frame test that cannot see
  it, which is why `ct-multi` in the selftest table is three frames.
* **The decision is doubles, and it stays doubles.** A row is kept or dropped
  by comparing two `std::log2` sums against 80 bits, so a different summation
  order transmits a different set of rows. `log2_prob` memoises
  `log2(v / 1024)` over the 1024 values a probability can take -- the same
  double for the same input, so it is memoisation and not an approximation,
  and it took libm back out of the largest cost of the stage.

The stream side is three fields and one binding: `table_bytes` in
`nxe_frame_params` (it took `pad1`'s word, so the record did not change size),
every E5 offset shifted by it, and an eighth binding on E5 carrying the
serialized area, which the shader lays down between the frame header and the
first tile-row header.

`XFORM_LARGE` (bit 27) is the largest single win in the tournament and is the
next real piece of work here. It is a second E3 pipeline from the same source
-- `NXE_XFORM_LOG2` is already a specialization constant and every loop bound,
LDS extent and scan lookup derives from it -- plus the `xform_size` field in
word1, the re-gridded DC plane, and `last_shift_of` in the LAST and LEVEL
banding. `v3_ctx_level` already takes `band_scan_pos` as a separate argument
for exactly that reason. The inter tools (28, 29) are Phase 2 and wait on E0b
and E3b.

## What the inter path will cost, measured before it is built

`docs/adr/0028-gpu-inter-needs-an-integer-mode-decision.md` is the decision; this
is the measurement it rests on, repeated here because it changes the order the
work should be done in.

16 frames of a 1088x1088 4:2:0 band-limited synthetic head turn (`gen_synthetic.py
--motion turn`, seed 7, peak 123 deg/s, ideal-warp ceiling 24.6 dB full-frame),
QP 30, `--no-rdo --intra-dir off --preset fast --me-effort 1 --quad-mv off
--near-skip off`, decoded back through `nxv-dec`:

| configuration | B/frame | Mbit/s per eye at 90 Hz | PSNR-Y | vs intra |
|---|---|---|---|---|
| intra only | 32339 | 23.3 | 38.42 dB | 1.00x |
| WARP_SKIP + INTRA only | 16035 | 11.5 | 36.86 dB | 2.02x |
| full inter (SKIP / STATIC_MV / WARP_MV) | 7926 | 5.7 | 36.62 dB | **4.08x** |

Tile modes over 4624 tiles of the full-inter stream: WARP_SKIP 81.0 %, INTRA
8.9 %, STATIC_MV 7.0 %, WARP_MV 3.1 %.

**WARP_SKIP alone is 2.02x and does not reach the 3x the budget needs.** The
10.1 % of tiles carrying a coded vector are worth as much again as the 81 % that
skip, so skip and the coded-vector modes belong in one increment rather than two
milestones -- and **STATIC_MV outweighs WARP_MV more than 2:1** here, so landing
WARP_MV first lands the smaller half.

Three further findings sit in the ADR and are summarised here because they are
about this directory:

* **The reference's default mode decision cannot be reproduced on a GPU.** It
  prices every candidate with a real rate from `table_set_cost`, which is a sum
  of `std::log2` terms; `log2` is not correctly rounded and is not the same
  function on a host libm and on a GPU, and every comparison downstream is a
  `double` derived from it. The GPU encoder therefore gets its own integer
  decision, added to the reference as a preset so the two stay byte-comparable.
* **E3's documented inter hook does not exist.** `nxe_enc.h` claimed E3 "already
  reads its prediction from a buffer (`pred_src`)"; there is no such buffer,
  `forward.comp` has five bindings and none of them is a predictor, and its
  prediction is recomputed by `pred_at()` and never materialised. E3 needs a
  sixth binding and a mode branch. The header has been corrected.
* **The reconstruction rule is unchanged.** The reference the encoder keeps is
  the decoder's own Pass W and Pass B as byte-identical SPIR-V (E3b above), and
  nothing in the decision change touches that.

## What the coding passes do not implement

What the coding passes do **not** implement, and refuse rather than ignore:
inter prediction, resolution levels, alpha, and
more than eight rANS lanes on the GPU path (`--nsub 4` and `5` are CPU-only;
paper 6.3 fixes v1 at eight). The RD trellis is deliberately absent: it changes
which levels are coded, never how they are decoded, and it is a `double`
trellis. `--no-rdo` is the reference configuration this pipeline reproduces.

## Measured

`nxvc-stats-test --device 0`, 2048×4096 (both eyes, 2048 tiles), RGBA8 4:2:0,
median of 50 iterations, timestamp queries around each dispatch:

| Device | E0_convert | E1_stats | sum |
|---|---|---|---|
| RX 7900 XTX (RADV) | 0.045 ms | 0.114 ms | 0.159 ms |

`nxvc-vkenc --bench 50`, the same geometry (2 × 2048², 2048 tiles), 4:2:0,
8 rANS lanes, frame matrix 1, no directional intra, RX 7900 XTX on RADV.
Milliseconds, median of 50, timestamp queries around each dispatch:

| QP | ctx | E3 forward | E4 rans_encode | E2 prefix | E5 packetize | total | bpp |
|---|---|---|---|---|---|---|---|
| 0  | v2 | 1.203 | 2.207 | 0.013 | 0.458 | **3.881** | 5.80 |
| 0  | v3 | 1.200 | 2.274 | 0.011 | 0.464 | **3.950** | 5.86 |
| 16 | v2 | 0.998 | 1.594 | 0.009 | 0.190 | **2.791** | 2.25 |
| 16 | v3 | 1.001 | 1.588 | 0.009 | 0.195 | **2.793** | 2.32 |
| 24 | v2 | 0.686 | 0.942 | 0.009 | 0.082 | **1.719** | 0.84 |
| 24 | v3 | 0.689 | 0.965 | 0.009 | 0.082 | **1.746** | 0.86 |
| 32 | v2 | 0.473 | 0.520 | 0.009 | 0.043 | **1.045** | 0.35 |
| 32 | v3 | 0.476 | 0.532 | 0.009 | 0.043 | **1.060** | 0.35 |
| 45 | v2 | 0.511 | 0.318 | 0.011 | 0.024 | **0.865** | 0.13 |
| 45 | v3 | 0.605 | 0.327 | 0.012 | 0.024 | **0.968** | 0.13 |

### The entropy tools, measured

Eight frames of a 1088x1088 4:2:0 band-limited synthetic sequence
(`gen_synthetic.py --motion mixed --eye-width 1088 --eye-height 1088 --seed 7`),
mono, `--no-rdo --intra-dir off --wm 0 --matrix 1`, RX 7900 XTX on RADV, every
stream byte-identical to `nxv-enc` at the matching flags and decoded back
through both `nxv-dec` and `nxvc-vkdec`:

| QP 30 configuration | B/frame | vs baseline | PSNR-Y |
|---|---|---|---|
| `--ctx v2` (the tools-off baseline) | 41016 | -- | 38.4292 dB |
| `--ctx v3` | 40919 | -0.24 % | 38.4292 dB |
| `--ctx v3 --custom-tables --tab v1` | 37593 | -8.35 % | 38.4292 dB |
| `--ctx v3 --custom-tables --tab v2` | **37163** | **-9.39 %** | 38.4292 dB |

**The PSNR column is the point of the table, not a footnote.** It is identical
to four decimal places across all four rows, at QP 24, 30 and 36 alike, because
entropy coding is lossless: E3's coefficients do not know which tables E4 will
use. A row here that moved would mean a bug, not a trade.

Over five quantisers (18, 24, 30, 36, 42) the full set against the baseline is
**-9.53 % BD-rate**, and the per-point saving is between 8.4 % and 9.9 %, so
the gain is flat in rate rather than concentrated at one end.

`CTX_V3` on its own is worth almost nothing here and is *negative* at QP 24
(+0.80 %). That is the same finding the bpp column below records and for the
same reason: the built-in v3 tables are trained on real content and synthetic
material is close to their worst case. It stops mattering the moment custom
tables are on, because then the frame trains its own -- and v3 with trained
tables beats v2 with trained tables by 1.6 %, which is the tool doing what it
was measured to do.

Nothing on the GPU changed, which is what the shape of the work predicts:

| QP 30, 289 tiles | E3 | E4 | E2 | E5 | total |
|---|---|---|---|---|---|
| baseline `--ctx v2` | 0.186 | 0.479 | 0.016 | 0.012 | **0.693 ms** |
| `--ctx v3 --custom-tables --tab v2` | 0.188 | 0.477 | 0.009 | 0.010 | **0.684 ms** |

The cost is host-side, in the table stage that was already there for the
per-tile choice: **0.42 ms to 1.27 ms** per frame at 289 tiles, of which the
three Lloyd iterations are 0.74 ms.

Those three iterations are the part worth arguing about. They are worth 240
bytes a frame (37403 at zero iterations, 37163 at three) -- about a quarter of
a millisecond of headset decode for three quarters of a millisecond of
encoder host time, which does not pay on its own. They are here because
`nxv-enc`'s default is three and byte-identity is the acceptance test; the
lever is `--table-iters`, and an integrator who wants the host time back
should take it there rather than by turning the tables off, which costs
thirty times as much.

**`CTX_V3` is free.** It costs E4 between nothing and 2.5 %, which is what the
shape of the tool predicts: it adds eleven rows to a table that was already
uploaded at its 27-row storage stride, and two registers of per-lane state.
Nothing about the sweep, the round structure or the byte placement changes.
E3 does not touch the entropy model at all, so its column moving at QP 45 is
run-to-run noise on a 0.5 ms kernel, not a cost.

**The bpp column is not a rate measurement of `CTX_V3` and must not be read as
one.** These rows are one synthetic 4096×2048 frame -- gradients, a block
grid and band-limited noise -- because the corpus is fetched rather than
committed and no clip was on this box. The built-in v3 tables are *trained*,
on real content, so a synthetic source is close to the worst case for them and
the tiny rate loss at low QP here says nothing about the tool. The reference's
measured BD-rate figures are in `ref/RESULTS-ctx-b.md`; these numbers replace
an earlier table taken on a different (also unrecorded) source, so the rows
are comparable to each other and not to the ones they replace.

Directional intra costs E3 8.2 ms at QP 24: the reference derivation of
SYNTAX.md 7.4 has the full 8×8 raster dependency, so the 96 blocks of a tile
are strictly serial and only the 64 lanes inside one block go wide.

### The RX 580 extrapolation

The RX 580 is the platform §3.6 budgets and it is not this box, so the number
has to be derived. Two independent ways of deriving it agree, which is the only
reason it is quoted at all.

The second RADV device here is the 9950X3D's integrated Raphael, two RDNA2 CUs
against the 7900 XTX's ninety-six. At 1024² (256 tiles, QP 24) it takes 15.8 ms
against the 7900 XTX's 1.59 ms — **ten times slower on forty-eight times fewer
CUs**, which is the measurement that says E4 is latency-bound rather than
throughput-bound, because a throughput-bound kernel would have been forty-eight
times slower. Scaled to 2048 tiles that is about 126 ms; an RX 580 has eighteen
times Raphael's CUs and a slightly lower clock, giving **11–12 ms**.

From the other direction: all 512 waves are resident on an RX 580 (GCN4 holds
forty wave64 per CU, so thirty-six CUs hold 1440), so what is left is clock —
1.34 against 2.4 GHz — and issue rate, wave64 on GCN4's SIMD16 taking four
cycles where RDNA3's SIMD32 takes two. That is 3.6× on E4, and E3 scales closer
to its 10× ALU ratio but is not saturating either. **10–17 ms** for the three
passes together.

§3.6 budgets the whole encoder at 2.5–4 ms on an RX 580 and under 1 ms on a
7900 XTX. So the coding passes are three times over on the 7900 XTX and three
to five times over on the platform that decides. The reason is structural
rather than a missing optimisation, and it is worth stating plainly.

E4 has exactly `tiles × 8` lanes of parallelism — 16384 for this frame, 512
waves, under three per SIMD on a 96-CU part — because eight rANS lanes per tile
is the bitstream, not a choice (§6.3). Every one of those lanes is a
dependent chain: rANS state feeds forward, and the operation list has to be
generated before it can be walked backwards. So E4 is latency-bound at low
occupancy and the machine is mostly idle. Three changes took it from 29.5 ms to
2.2 ms and none of them touched the inner loop: making every tile resident
rather than 1024 of them, materialising each lane's whole operation list once
instead of regenerating a unit at every boundary crossing (with an exact
fallback when it does not fit), and anchoring the emission words so one sweep
does the work of two.

What is left is the generation itself. The remaining lever is to parallelise it
over *coding units* rather than over lanes — 99 units per tile is 203k threads
instead of 16k — which needs a per-unit scratch, a prefix sum over each lane's
units, and a gather. That is the next thing to do here, and it is why the RX
580 verdict is deferred rather than claimed.
