# Pass B: reconstruction

One workgroup per 64x64 tile, 256 threads. Reads the int16 coefficient buffer
and the tile records that Pass A produced, dequantizes, inverse-transforms,
predicts, and writes the display image. `docs/PAPER.md` 3.2.1, 3.2.3, 3.2.4,
3.2.5, 3.2.6.

```
reconstruct.comp        the kernel (GLSL 4.60 -> SPIR-V, vulkan1.1), built as
                        four modules over NXVW_INTRA_DIR x NXVW_XFORM_LARGE
syntax_constants.h      every normative constant, shared by GLSL and C++
passB_layout.h          the Pass A -> Pass B SSBO layout, shared by both
passB_model.{h,cpp}     line-for-line CPU model of the kernel
tools/nxvc-passB-test   headless harness: CPU-vs-GPU compare, timing, traffic
(the SPIR-V build rule is the tree-wide vk/common/cmake/nxvc_gen_spv.cmake)
```

Tests live in `tests/vk-decoder/passB/` and are registered as `vk.passB.*`.

## What the kernel does

Per plane (Y/R, Co/G, Cg/B, and alpha when `alpha_mode == 2`):

0. **Geometry.** [minor 6] The tile's `xform_size` names a transform edge, 8,
   16 or 32, capped by the plane's own coded extent, and `bsize` and
   `nb = size / bsize` drive everything below (SYNTAX.md 6.7). Without
   `XFORM_LARGE` this is `bsize == 8` and `nb == size / 8` exactly as it was --
   see "Large transforms" below.
1. **DC plane.** `nb*nb` DC coefficients are dequantized at `QP - 6` with a
   flat weight. When the plane is 8 blocks wide they pass through a
   second-level 8x8 IDCT (PAPER 3.2.4/6.4). The result plus the plane's DC
   offset, clamped, is the array of block means.
2. **Planar intra prediction.** Every pixel is a Q4 bilinear interpolation of
   the four nearest block means, sampled at `(2x - 7, 2y - 7)`, which puts the
   sample grid on the block centres -- or, at `bsize` 16 and 32, at SYNTAX.md
   7.2's general form of the same mapping. No wavefront, no barrier beyond the
   transform's.
   With `INTRA_DIR` (stream tool bit 17) each 8x8 block instead names one of
   nine modes, of which mode 0 *is* this predictor, and the blocks of a plane
   are reconstructed **in raster order** — see "Directional intra" below. A
   plane whose modes are *all* 0 takes this parallel path instead, bit
   identically, because mode 0 reads no neighbour.
3. **Residual.** Either transform-skip (dequantized coefficients *are* the
   residual, flat weight, raster order) or dequantize with the weighting
   matrix and run the 8x8 integer IDCT: 4 threads per block, 2 rows each in
   the row pass, 2 columns each in the column pass, transposed through local
   memory in between. A block whose published length is 0 coded nothing, so
   its residual is zero and neither pass runs — see "The sparse coefficient
   layout" below.

   [minor 6] With `XFORM_4X4_SPLIT` (tool bit 19) a block may instead be four
   4x4 sub-blocks — see "The 4x4 split" below. With `XFORM_LARGE` (tool bit
   27) the block is `bsize x bsize` and the transform is the N-point member of
   the same family — see "Large transforms" below.
4. **Add and clamp** into the plane's sample store.

Then, once per tile:

5. **Resample** each plane from its coded edge to display resolution. A
   `res_level` 1 or 2 tile is coded at 32 or 16 and upsampled by the fixed
   half-phase Q4 bilinear; a 4:2:0 chroma plane gets a second half-phase
   doubling (the 3/4, 1/4 taps of PAPER 1.3) on the RGB output paths.
6. **Colour and store**, see below.

### Output formats (specialization constant 0)

| value | format | notes |
|---|---|---|
| `kOutRgba8` | `rgba8ui`, binding 3 | YCoCg-R inverse when `colorTransform == 1` |
| `kOutRgb10A2` | `rgb10_a2ui`, binding 4 | 8-bit samples replicated to 10 bits; a real 10-bit profile will widen the sample path instead |
| `kOutYcbcr420` | `r8ui` luma binding 5 + `rg8ui` CbCr binding 6 | two-plane 4:2:0 passthrough, no colour transform, no chroma upsampling. Default on Android: it is what the WiVRn NX client's decoder already consumes and it halves reference-slot memory on the headset |

**Specialization constant 3, `kOutSecond`,** names a *second* format written
from the same reconstruction, or `kOutNone`. Two stores of one tile is the
shape the reference ring slot will have when the inter path lands, and the
shape a 4:2:0 stream with a coded alpha plane already has, because the
two-plane store has nowhere to put alpha. Doing both from one dispatch is 26 %
cheaper than reconstructing the frame twice on RADV and 46–63 % cheaper on
lavapipe (`../README.md`). The store loop is over specialization constants, so
a one-store pipeline compiles to exactly the kernel it did before.

### The sparse coefficient layout

**Specialization constant 4, `kSparse`** (default 1), and binding 9. Pass A
stores a unit's coefficients in *scan* order at slots `[0, LAST]` of the same
reserved region the dense layout used, and publishes `LAST + 1` per unit;
`passA/syntax_constants.h` section 8 is the normative description. Pass B
wants a coefficient by its raster position, so it inverts the scan
(`nxvw_scan_pos()`) and reads zero past the length.

Two things fall out of the length that matter more than the bytes:

* a **unit with length 0** coded nothing, so its dequantize and both passes of
  its IDCT are skipped. A tile with no coded unit — which is what `WARP_SKIP`
  and `STATIC_MV` will be when the inter hook lands, and what a static region
  under rolling refresh already is — therefore runs no transform stage at all;
* a **DC unit with length 0** makes the block means the plane's DC offset, so
  the second-level 8x8 IDCT and its two barriers are skipped too.

It is a specialization constant rather than a push constant because the test
sits inside the innermost coefficient loop: left dynamic it cost 0.10 ms of
Pass B, which is 40 % of the whole v1 planar path.

With `colorTransform == kCtNone` on the RGB paths the three planes are written
to R, G and B unchanged: the stream is carrying display-space planes. That is
also what the CPU reference does with `NXVC_CT_NONE`.

### The 4x4 split (tool bit 19) [minor 6]

`docs/SYNTAX.md` 6.8. A coded block of a tile whose word1 bit 28 is set carries
a 1-bit flag, and when it is 1 the block is four 4x4 sub-blocks in raster
order, each in its own quadrant of the 64-value coefficient array, scanned by
the concatenated scan and quantised with the tile's 8x8 weighting matrix
subsampled by two in each frequency axis. The 4-point inverse transform's
shift chain is the 8x8's unchanged: the 4-point graph has one butterfly level
fewer AND one fewer sqrt(2) of gain, and the two cancel.

Pass A publishes the flags as one bit per block after the mode words of the
same per-tile region — one bit and not a fifth mode field, because nothing in
the syntax ties tool bit 19 to tool bit 17 and a stream may split without
coding modes at all.

**The scheduling is where the interest is.** `idct_block()` writes transposed
in both passes, so pass 2's row `r` reads `out[r]` of every pass-1 row and
produces COLUMN `r` of the sub-block — which means a thread that wants the two
columns it already owns runs four pass-1 rows keeping two of each four outputs,
then two pass-2 rows. Twelve 4-point transforms, **eight** live intermediates,
no staging through the sample store and no extra barrier, and the two paths
hand the prediction the same `res0`/`res1`. Two more natural shapes were built
first and both cost an Adreno 650 a factor of five of Pass B; `../README.md`,
"The minor-6 realignment", is the measurement.

The whole path is behind specialization constant 6, so a stream without the
tool compiles a kernel that does not contain it.

### Large transforms (tool bit 27) [minor 6]

`docs/SYNTAX.md` 6.7. The tile header names one transform edge for the whole
tile, capped per plane by its coded extent, so `bsize` is 8, 16 or 32 and `nb`
is a power of two in `[1, 8]`. The N-point inverse is the even/odd recursion
of 6.2.1 stacked on the same 8-point Loeffler core the 8x8 form uses, with
`kOdd16` / `kOdd32` and the shift chain 7/13, 7/14, 8/14 by size -- one
transform family over `{4, 8, 16, 32}`, because two would be two dequantiser
scales and the second one would be wrong silently.

**The schedule is one thread per 1D transform**, not four threads per block,
and it fits the workgroup exactly once at every size that reaches it. The
intermediate is stored *un-transposed* and read back transposed, which is the
opposite of the 8x8 path and is what keeps every shared word owned by one
thread; the column pass then lands its output in the two words of the plane
slot the thread already owns, so `res0[8]` stays eight at every transform size
and the prediction reads its residual back out of the slot. `../README.md`,
"The transform size, priced", is the derivation and the measurement.

The `n x n` intra predictors still gather 7.4's reference arrays per block,
and `NXVW_INTRA_REFS` sizes them per build variant: 17 in the 8x8 module,
which is what it always had, and 65 only where a 32x32 block can occur.
Reading each reference through `dirAt()` where it is used was built first --
it is the obvious way to avoid a sixty-five-int live set -- and it is a
*correctness* failure on an Adreno 650, because a predictor has about
twenty-five reference sites and `dirAt()` inlines the whole DC-plane bilinear
behind its fallback. `predictOne()` records the numbers.

**It is a build variant, `NXVW_XFORM_LARGE`, not a specialization constant**,
and so is the pair of scan arms in `nxvw_scan_pos()`. Both were measured as
specialization constants first and both cost an 8x8-only stream real time on
an Adreno 650; `../README.md` has the instruction counts and says why each
one was invisible to the driver's dead-code pass.

The model carries the whole path, not just the transform: `nxvc-passB-test`
emits `xform_size` tiles at both sizes, in both chroma formats, at
`res_level` 0 and over the random `res_level` mix where 6.7's plane cap gives
one tile three block sizes, with a saturating arm and on the two-plane store.
`vk.passB.ref_conformance` pins the primitive underneath it against
`ref/src/transform.cpp` `idct_block(n)`.

**On the Adreno 650 the module is miscompiled** and the streams that reach it
decode wrong, which is the one place this decoder is not correct on all three
ICDs. It is not a bitstream question -- RADV and lavapipe agree with `ref/`
bit for bit -- and `../README.md`'s open-issues list carries it.

### Chroma from luma (tool bit 24) [minor 6]

`docs/SYNTAX.md` 7.7. Mode 9 of a CHROMA block: chroma is a linear model of the
co-located reconstructed luma, `base_c + alpha * (luma - base_l)`, fitted once
per block over its sixteen reconstructed neighbours — the two with the smallest
co-located luma set `base`, the two with the largest the far end — and then
evaluated per sample. It is the only predictor that clamps, because a fitted
slope can leave the sample domain.

The tile's reconstructed luma is still in its plane slot when the chroma planes
decode, because the slots coexist; that is the whole of what the tool needs
from the schedule. It lives inside the directional wavefront, and can only:
mode 9 exists solely in the `CTX_V2` mode symbol of the replace form, which
the stream header enforces.

### Directional intra (tool bit 17)

`docs/SYNTAX.md` 7.4. Each 8x8 block carries a mode; modes 1..8 predict it from
its reconstructed left, above and above-right neighbours, which makes the plane
a **wavefront** instead of a parallel pass. Pass A hands the modes over in
binding 7, 4 bits per block.

* **References** clamp *into the tile*. A block whose neighbour has not been
  reconstructed yet — including everything above row 0 and left of column 0 —
  reads `base` instead, which is derived from this tile's own DC plane. A tile
  therefore still never reads a neighbouring tile, which is what the
  transport's per-tile loss recovery and the rate controller's per-tile ladder
  both depend on.
* **`base`** is the DC-plane prediction in the default (replace) form and the
  all-zero plane in the layered form, frame `flags` bit 2 (7.5). It is
  recomputed from the block means on demand rather than stored, which is what
  lets the shared sample store hold the running reconstruction alone.
* **The layered form** stores the reconstructed DC-plane *residual* for later
  blocks and converts it back to samples once the plane is finished, with one
  pass of `sample = pred + recon`. That is exact, not an approximation, because
  `recon` was formed as `clamp(pred + v) - pred`.
* **The residual is staged in the sample store** before the wavefront runs,
  at exactly the positions the block will occupy. That costs no memory: a
  block's own 8x8 region holds its residual until its step and its
  reconstruction afterwards, and `dirAt()` never reads the region of a block
  that is not done yet — it reads `base`. What it buys is that the wavefront
  is no longer tied to the transform's four-threads-per-block mapping.
  `dirBlockOfStep()` enumerates the blocks of a step — the arithmetic inverse
  of `dirStepOf()` — and each gets `kDirLanesPerBlock` = 16 threads owning two
  shared words each. 16 blocks x 16 threads is exactly the workgroup, which is
  what the 32x32 sub-tile schedule needs at its widest step.
* **The schedule is specialization constant 2**, `kDirSched`. 0 is the
  normative derivation of 7.4; 1 drops the above-right reference, 2 confines
  the dependency to 32x32 sub-tiles, 3 does both. The bit encoding is
  `ref/src/codec.cpp build_refs()`'s, so a stream produced by a `ref/` built
  with `-DNXVC_DIR_SCHED_EXPERIMENT` and `NXVC_DIR_SCHED=k` decodes bit-exactly
  under `kDirSched == k`. **It is a bitstream property, not a tuning knob.**
  What each one costs in time is in `../README.md`.

The residual is computed for every block in parallel, exactly as before; only
the prediction and the add are serialized. What the schedule changes is the
number of `barrier()`s between them: 22 per 8x8-block plane at `kDirSched` 0,
15 at 1, 7 at 3, plus one for the residual staging.

### Shared memory

One sample store, `kPlaneStoreWords` uints (specialization constant 1), holding
every plane's samples as int16 packed two per uint, plus 512 B for the block
means. **There is no separate transpose buffer**: the row pass writes its
transposed output into the plane's own slot, at exactly the pixel positions the
block will end up occupying, and the column pass reads it back before the
prediction-add overwrites them. The three barriers the schedule needs anyway
make that safe.

| stream | shared memory |
|---|---|
| 4:2:0, no alpha | 12.5 KB |
| 4:4:4, no alpha | 24.5 KB |
| 4:2:0 + alpha | 20.5 KB |
| 4:4:4 + alpha | 32.5 KB — over the 32 KB limit on Adreno 650 and lavapipe |

Only the last configuration does not fit a 32 KB device; the harness reports it
as a skip rather than failing. Everything the Pico 4 target actually streams
fits.

**Do not spend effort shrinking this.** 12.5 KB is two workgroups per 32 KB
Adreno 650 SP and the obvious move is to get under 10.6 KB for three, but the
pass is not occupancy-limited: padding the allocation to 17.6 KB and then to
30.8 KB — one workgroup per SP either way — cost 0.08 ms of a 22.3 ms pass at
QP 24 and nothing at QP 36. If halving residency is free, tripling it is worth
nothing. `../README.md`, "What was not the problem", has the numbers. What the
pass *is* limited by is LDS and image traffic: load count in the predictor,
store count in the output.

### Portability

No subgroup operation is used at all, so the same SPIR-V runs on any subgroup
size (PAPER 3.2.6 asks for >= 8; this kernel does not even need that). No
extensions, no 16-bit storage feature, no vendor `#ifdef`. All normative
arithmetic is int32; `>>` on a signed int compiles to `OpShiftRightArithmetic`,
which matches the C++ reference on every platform we build for.

## Where the constants came from

`syntax_constants.h` is the one file to re-diff when the spec moves. Every
block in it is marked `[SYN]` (docs/SYNTAX.md), `[REF]` (ref/) or `[PAPER]`
(docs/PAPER.md, only where the other two are silent).

The file was first written against `ref/` alone, before `docs/SYNTAX.md`
existed, and then re-aligned against it section by section: 4.1 tile header,
4.2 geometry, 4.3 sample domains, 5.1 YCoCg-R, 5.2 chroma upsampling, 6.1-6.3
transform, 6.5 quantization and weighting matrices, 6.6 transform skip, 7.1-7.3
DC-plane intra, 8 the resampling kernel, 10 the reconstruction summary. Every
value already agreed; the only edits were the mode enum names (SYNTAX 4.1 calls
them `WARP_SKIP`, `STATIC_MV`, `WARP_MV`, `INTRA`, `STEREO`) and the provenance
comments.

**The IDCT shift chain does not come from the paper.** PAPER 1.4 says "7 bits
after the first dimension, 12 after the second". That is wrong for these 9-bit
constants: it is off by a factor of two. SYNTAX 6.3 and `ref/` both use 7 then
13 (total shift 20, total gain 1), and `vk.passB.ref_conformance` pins this two
ways — bit-exactly against `ref/`, and against a float orthonormal DCT-III, so
any power-of-two gain error fails the test rather than silently halving the
picture.

## Agreement with Pass A

`vk/decoder/passA/README.md` describes the same coefficient buffer from the
producing side, and the two descriptions match: coefficients are int16 with a
fixed `coef_stride` per tile, and inside a tile they run, for each coded plane
in the order Y, Co, Cg and A (A only when `alpha_mode == 2`), as `nb*nb`
DC-plane coefficients followed by `nb*nb` blocks of 64. The strides agree too:
`nxvw_coef_stride_i16()` gives 6240 for 4:2:0 at `res_level` 0 and 16640 for the
widest 4:4:4-plus-alpha tile, which is Pass A's `kCoefStrideMax`.

**One gap remains between the two passes.** Pass A's binding 1 is a tile
*descriptor* (byte offset, byte length, coefficient index, cbf index) — what the
entropy decoder needs — not the tile *record* Pass B reads (binding 1 here:
`NxvwTileRec`, the two tile-header words plus `alpha_value`). Both are derived
from the same parsed tile header, so whichever component parses tile headers for
Pass A can fill in both; nothing needs to be re-parsed. Pass A's cbf-bit and
status buffers are not inputs to Pass B.

## Inter prediction hook [inter]

`docs/SYNTAX.md` 13 has landed. The predictor is **not** in this kernel: it is
`vk/decoder/inter/warp_pred.comp`, a third dispatch, and this file reads its
output. See `../README.md`, "The inter path", for why.

What the inter path added here, and nothing else:

* one `#include "../inter/inter_hook.glsl"`, after the shared-memory
  declarations because the ring store reads `sPlane`;
* the **prediction hook** at the `INTER HOOK` marker in the prediction-and-add
  step. `pred` is the DC plane's planar interpolation for an intra tile and
  `clamp(W + planar(M) - dc_offset, 0, maxval)` for an inter one, `W` being
  the sample Pass W wrote. The residual add, the clamp, the resample and the
  colour conversion are unchanged and shared;
* `nxvwIsInterTile`, set at the `bool intra` marker and read by `dcMean()`.
  13.3: an intra tile's block mean is a sample value and is clamped to the
  sample domain; an inter tile's is `dc_offset + a residual mean`, whose range
  is wider on both sides, and clamping it would cap the DC correction the warp
  needs;
* `unitsPerPlaneExtra` now asks whether the tile is intra, because 13.3 puts
  the mode unit of 9.6 on `INTRA` tiles only. Pass A makes the same change,
  and it is a bitstream property, not an optimisation: a decoder that kept
  counting a mode unit on an inter tile reads every later unit of the tile
  from the wrong place and Pass A refuses the payload;
* one call to `nxvwRefRingStore()` at the end of `main()`. The reference-ring
  slot is a second store of the same samples — the same shape `kOutSecond`
  already established — in the coded domain rather than the display one.

The short-circuit the old note asked for was already there and is still there:
a tile with no coded unit runs no dequantize and no IDCT, because every unit's
published length is 0, which is what a `WARP_SKIP` tile is.

`w3` of `NxvwTileRec` stays reserved and unused. The warp record turned out
not to want a per-tile index into a side buffer at all: Pass W is indexed by
the tile index directly, and Pass B addresses `WPred` the same way, so the
coefficient side of this interface did not have to change.

## Running it

```
build-passB/vk/decoder/passB/nxvc-passB-test --list
build-passB/vk/decoder/passB/nxvc-passB-test --verbose          # full sweep
build-passB/vk/decoder/passB/nxvc-passB-test --device-name RADV
build-passB/vk/decoder/passB/nxvc-passB-test --bench            # timing only
VK_DRIVER_FILES=/path/to/lvp_icd.x86_64.json build-.../nxvc-passB-test
```

Exit 0 means zero mismatching pixels, 1 means a mismatch, 77 means no usable
ICD (which is how the ctest entries skip on a machine without a GPU).

Measured on this box, 2048 tiles (2048x2048 luma), 4:2:0, `res_level` 0, via
the full decoder rather than this harness (`../README.md` has the method):

| device | `INTRA_DIR` off | `kDirSched` 0 | 1 | 3 |
|---|---|---|---|---|
| RX 7900 XTX (RADV NAVI31) | 0.24 ms | **1.18 ms** | **0.89 ms** | **0.73 ms** |
| llvmpipe (lavapipe, 4 cores) | 112 ms | 261–291 ms | 244–266 ms | 200–289 ms |

The wavefront is still the whole of the difference, but it is 30 % cheaper at
every schedule than it was before the 16-thread mapping, and Pass B's cost is
no longer fixed per tile: 890 ns → 248 ns at zero payload, with a real slope
against payload above it. `../README.md` has the before/after tables.

Coefficient SSBO traffic follows the payload now. The dense res_level-0 4:2:0
tile slot is 6240 int16 — 24.4 MB per 2048-tile frame from this harness, 25.6
from the full decoder, and that number includes chroma: luma is 64 DC + 64
blocks x 64 = 4160, and each of the two chroma planes is 16 DC + 16 blocks x
64 = 1040, so 32 chroma blocks per tile on top of the 64 luma ones. PAPER
3.2.5's 16.8 MB estimate counts the luma plane only; at 4:4:4 the slot grows
to 12480 int16 and the frame to 48.8 MB. **Sparse, the same frame is 0.87 MB
at QP 63, 0.93 MB at QP 36, 11.6 MB at QP 24 and 13.6 MB at QP 12**, of which
0.54 MB is the length words themselves.
