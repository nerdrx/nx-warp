# Pass B: reconstruction

One workgroup per 64x64 tile, 256 threads. Reads the int16 coefficient buffer
and the tile records that Pass A produced, dequantizes, inverse-transforms,
predicts, and writes the display image. `docs/PAPER.md` 3.2.1, 3.2.3, 3.2.4,
3.2.5, 3.2.6.

```
reconstruct.comp        the kernel (GLSL 4.60 -> SPIR-V, vulkan1.1)
syntax_constants.h      every normative constant, shared by GLSL and C++
passB_layout.h          the Pass A -> Pass B SSBO layout, shared by both
passB_model.{h,cpp}     line-for-line CPU model of the kernel
tools/nxvc-passB-test   headless harness: CPU-vs-GPU compare, timing, traffic
cmake/gen_spv.cmake     glslc -> C array
```

Tests live in `tests/vk-decoder/passB/` and are registered as `vk.passB.*`.

## What the kernel does

Per plane (Y/R, Co/G, Cg/B, and alpha when `alpha_mode == 2`):

1. **DC plane.** `nb*nb` DC coefficients are dequantized at `QP - 6` with a
   flat weight. When the plane is 8 blocks wide they pass through a
   second-level 8x8 IDCT (PAPER 3.2.4/6.4). The result plus the plane's DC
   offset, clamped, is the array of block means.
2. **Planar intra prediction.** Every pixel is a Q4 bilinear interpolation of
   the four nearest block means, sampled at `(2x - 7, 2y - 7)`, which puts the
   sample grid on the block centres. No wavefront, no barrier beyond the
   transform's.
3. **Residual.** Either transform-skip (dequantized coefficients *are* the
   residual, flat weight, raster order) or dequantize with the weighting
   matrix and run the 8x8 integer IDCT: 4 threads per block, 2 rows each in
   the row pass, 2 columns each in the column pass, transposed through local
   memory in between.
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

With `colorTransform == kCtNone` on the RGB paths the three planes are written
to R, G and B unchanged: the stream is carrying display-space planes. That is
also what the CPU reference does with `NXVC_CT_NONE`.

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

## Inter prediction hook

v1 codes INTRA tiles only — the CPU reference rejects every other mode. The
pose-warp predictor plugs into `reconstruct.comp` at the two places marked
`INTER HOOK`:

* the `bool intra` derivation near the top of `main()`, which is where
  `kModeWarpSkip` / `kModeStaticMv` should short-circuit the coefficient
  path entirely;
* the `pred0` / `pred1` computation in the prediction-and-add step, which is
  where `bilinearMeans()` is replaced by the bit-exact 4-tap bilinear of the
  reference image at the warp coordinate (PAPER 3.2.3 step 5).

The warp record itself does not belong in the 16-byte tile record. `w3` of
`NxvwTileRec` is reserved as an index into a `WarpRecs` SSBO at binding 5 of
the future layout, so adding inter prediction does not disturb the coefficient
side of the interface. Nothing else in the kernel changes: the residual add,
clamp, resample and colour conversion are already shared between the two
prediction modes.

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

Measured on this box, 2048 tiles (2048x2048 luma), 4:2:0, `res_level` 0:

| device | dispatch |
|---|---|
| RX 7900 XTX (RADV NAVI31) | 0.50 ms |
| Ryzen 9 9950X3D iGPU (RADV RAPHAEL) | 12.9 ms |
| llvmpipe (lavapipe) | 7.1 ms for 256 tiles |

Coefficient SSBO traffic is **24.4 MB per 2048-tile frame**. The dense
res_level-0 4:2:0 tile slot is 6240 int16, and that number includes chroma:
luma is 64 DC + 64 blocks x 64 = 4160, and each of the two chroma planes is
16 DC + 16 blocks x 64 = 1040, so 32 chroma blocks per tile on top of the 64
luma ones. PAPER 3.2.5's 16.8 MB estimate counts the luma plane only; at
4:4:4 the slot grows to 12480 int16 and the frame to 48.8 MB. The sparse
coefficient layout named in 3.2.5 as the first optimization would cut all of
these by roughly 4x at typical QP.
