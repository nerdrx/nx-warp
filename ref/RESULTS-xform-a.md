# Large transforms: 16x16 and 32x32, per tile (tool bit 24)

What this package is, why it exists, what it measured, and what it costs a GPU
decoder. Everything here was produced by `tools/quality/compare.py` and by
`ref/`'s own tools, every process under
`chrt -i 0 taskset -c 4-7 nice -n 19`, with result files under
`$NXQ_SCRATCH/results/tourney-xform-a-*.json`. Section 6 reproduces every
number.

**The premise.** `RESULTS-intra.md` ends with the intra core about 4 dB behind
x264 intra, and says the deficit is dominated by smooth content: a DC plane of
8x8 block means plus an 8x8 DCT has no way to spend few bits on a large flat
gradient. x264 has a 16x16 plane mode and H.264/HEVC have 16x16 and 32x32
transforms for exactly that. This package adds the transform half of that
answer.

---

## 1. The tool, in one page

`xform_size` is two bits of the tile header (word1 bits 28-29), gated on new
tool bit 24 `XFORM_LARGE`, naming a transform edge of 8, 16 or 32 for **every
plane of the tile**. The edge a plane actually uses is capped by the plane's
own coded extent, so no combination of `res_level`, chroma format and
`xform_size` is illegal and none needs a constraint.

The transforms are the even/odd recursion on the existing Loeffler 8-point
core: a length-`2M` DCT-III is the length-`M` one on the even-indexed
coefficients plus a dense `M x M` rotation on the odd ones. Writing the two
rotation matrices on the **same 512 scale** as the seven v1 constants makes the
even half need no rescaling at all, which is the whole reason the construction
is this one:

| edge | gain per dimension | 2D gain | inverse shifts | forward shifts |
|---|---|---|---|---|
| 8 | `2^10` | `2^20` | 7, 13 | 6, 14 |
| 16 | `2^10 * sqrt(2)` | `2^21` | 7, 14 | 7, 14 |
| 32 | `2^11` | `2^22` | 8, 14 | 8, 14 |

The 16-point gain is irrational; the two-dimensional gain is not, so every size
is exactly unit gain after its shift chain and a coefficient of `n * 128` at
position 0 reconstructs a flat 128 at all three. The first-pass shift grows by
one per size because the value entering it doubles per size, so all three sizes
leave the same margin under the `int16` clamp of the transpose buffer — a
full-amplitude residual lands on 25 650 at every size — and a GPU may keep that
buffer 16-bit however large the block is.

Worst-case magnitudes, each the largest absolute row sum of the exact 1D
transform times the largest legal input, so each is attainable:

| stage | 8x8 | 16x16 | 32x32 | int32 headroom at 32x32 |
|---|---|---|---|---|
| before either inverse pass | `8.9e7` | `1.7e8` | `3.5e8` | 6.2x |
| before forward pass 1 | `1.6e6` | `3.3e6` | `6.6e6` | 325x |
| before forward pass 2 | `1.1e8` | `2.1e8` | `4.2e8` | 5.1x |

**Nothing else is signalled.** The DC plane is `nb x nb` with
`nb = coded_extent / bsize` and keeps its second-level 8x8 transform exactly
where it had it (`nb == 8`); the planar interpolation grid is a rounding shift
that reduces to the v1 `2x - 7` at `bsize == 8`; the nine directional
predictors are the same formulas with the block edge left as `n`; the
weighting matrix is the 8x8 one replicated by `u >> k`, `v >> k`; the scan is
one zigzag rule evaluated at the block edge; the LAST classes and the LEVEL
bands scale by scan group. Two bits per tile buy all of it.

`tskip` and `xform_size` are mutually exclusive, so lossless is untouched.

---

## 2. The Phase 1 gate, before and after

`tools/quality/compare.py` on `vr-mixed-1024-v2` (the band-limited v2
sequence), 36 frames, against `x264 --keyint 1 --tune zerolatency` over the
100-400 Mbit band, QP ladder `0,4,8,12,16,20,24` against anchor
`8,12,16,20,24,28`. "before" is the shipped default (`--xform 8`, which is
byte-identical to a build without the tool); "after" is `--xform auto`.

<!--RESULTS-INTRA-TABLE-->

---

## 3. Inter, with the same tool

Residuals of warped tiles are smooth, so the tool should help there too. The
Phase 2 kill test of `RESULTS-inter.md`, band A and band B, `--inter on
--poses`:

<!--RESULTS-INTER-TABLE-->

---

## 4. Two variants that were measured and rejected

### 4.1 A per-32x32-quadrant transform size

Not built. The reason is structural rather than numeric and it is worth
stating plainly: **the DC plane's resolution is the transform grid.** It is a
per-plane structure — `nb x nb` block means for the whole plane, bilinearly
interpolated across the whole plane — and a per-quadrant size would split it
into four independently interpolated pieces with a seam between them, which is
the one artefact the DC-plane predictor exists to avoid. The tile is the
smallest unit at which the transform size can change without changing what the
DC plane *is*, and it is also the unit of the GPU workgroup, so a uniform size
per tile is what keeps section 5's schedule uniform.

### 4.2 A per-size entropy context family

Measured, and it does not pay. Build with `-DNXVC_XFORM_CTX_EXPERIMENT` and the
encoder splits the frame's symbol histogram by transform size and reports what
coding each size with its own perfectly trained table — an oracle, with no
signalling cost at all — would save against the one merged table the format
transmits. On `vr-mixed-1024-v2` 4:4:4 at QP 16, `--xform auto
--custom-tables`:

```
[xform-ctx] merged 589268 bits, per-size 570573 bits, a per-size family would save 3.173 %
[xform-ctx]    8x8  208865 symbols
[xform-ctx]   16x16 55115 symbols
[xform-ctx]   32x32 64150 symbols
```

3.173 % of 589 268 bits is **2 337 bytes**. Paying for it means transmitting
three table families instead of one: up to 16 more 160-byte sets, **2 560
bytes**, on the same frame. The oracle loses before the split tables are
penalised for being trained on a third of the data. No context was added, and
the LAST classes and LEVEL bands are scaled by scan group instead
(`SYNTAX.md` 9.3), so one trained set of frequencies serves all three sizes.

### 4.3 Restricting the intra mode set at the larger sizes

Also measured, and it is the wrong way round. The same experiment build reports
which of the nine modes the encoder chooses at each size:

| block | `DC_PLANE` | `DC` | `PLANAR` | `H` | `V` | `DDL` | `DDR` | `VR` | `HD` |
|---|---|---|---|---|---|---|---|---|---|
| 8x8 | 76.1 % | 7.6 % | 2.9 % | 5.8 % | 5.0 % | 0.8 % | 0.6 % | 0.6 % | 0.7 % |
| 16x16 | 42.8 % | 19.6 % | 7.2 % | 15.7 % | 10.3 % | 1.3 % | 0.9 % | 0.8 % | 1.3 % |
| 32x32 | 54.7 % | 15.2 % | 5.7 % | 10.8 % | 8.7 % | 1.8 % | 0.8 % | 0.7 % | 1.6 % |

(294 912 / 56 256 / 16 656 blocks respectively, `vr-mixed-1024-v2` 4:4:4 QP 16,
`--xform auto`.) The neighbour-based predictors are used **more** as the block
grows, not less: a 32x32 grid gives the DC plane four block means for a whole
tile, so it has almost no spatial detail left to give and the directional modes
take over. All nine modes are defined at all three sizes.

---

## 5. What it costs a GPU decoder

The constraint is PAPER design principle 2 and `docs/03-vulkan.md` 3.2.3: **one
workgroup of 256 threads per 64x64 tile, no cross-tile state.** Nothing here
changes that. `SYNTAX.md` 6.8 is the normative note; the summary is:

**Thread mapping.** One thread per 1D transform. The odd half of a 32-point
transform is a dense 16 x 16 product over all sixteen odd coefficients, so
splitting one transform across threads means either duplicating the 16-point
even half or an unbalanced 3.4-to-1 split; one row per thread is balanced,
needs no cross-lane exchange inside a transform, and reads its 32 coefficients
as one coalesced 64-byte load.

A `res_level` 0 4:2:0 tile at `xform_size == 2`: 4 luma blocks of 32x32 (128
transforms), one Co and one Cg block of 32x32 (32 each) — **192 of 256 threads,
75 % occupancy**, all three planes through both passes together.

**LDS.** The transpose buffer is a whole *plane*, not a block, so it is the
size it already was: `64 x 64 x 2 B = 8 192 B` for luma and `32 x 32 x 2 B =
2 048 B` per 4:2:0 chroma plane, **12 288 B for the tile**. The `clamp16` after
pass 1 is what keeps it `int16` at every size.

**Dependent steps.** Two per round — pass 1, barrier, pass 2. A 4:2:0 tile is
one round at 32x32 (**2 barriers**) against three at 8x8 (6 barriers), because
256 threads at 4 per 8x8 block cover 64 of the tile's 96 blocks. 4:4:4 is two
rounds at 32x32 (4 barriers) against six at 8x8 (12).

**Registers.** About **48 int32** live at the peak of a 32-point transform (the
sixteen odd coefficients, sixteen accumulating outputs, sixteen even-half
results) against 8 + 8 for the 8x8 form. On hardware with a 64-VGPR
full-occupancy budget that is the binding constraint rather than LDS; the
fallback is a second LDS buffer for the coefficient vector.

**Arithmetic, which is where it is expensive.**

| edge | multiplies per 1D transform | per sample | per `res_level` 0 4:2:0 tile |
|---|---|---|---|
| 8 | 11 | 2.75 | 16 896 |
| 16 | 11 + 64 = 75 | 9.4 | 57 792 |
| 32 | 75 + 256 = 331 | 20.7 | 127 104 |

7.5x the multiply count at 32x32. Neither rotation matrix is factorised, and
factorising them would change no bit — the *result* is normative, the spelling
is not, exactly as for `mulC4`. Against that cost the tool removes barriers,
removes rounds, and cuts the directional-intra wavefront:

| `xform_size` | blocks per luma edge | wavefront steps | threads per block | occupancy |
|---|---|---|---|---|
| 0 (8x8) | 8 | 22 | 4 | 4.5 % |
| 1 (16x16) | 4 | **10** | 16 | **10.0 %** |
| 2 (32x32) | 2 | **4** | 64 | **25.0 %** |

Those are the numbers `SYNTAX.md` 7.6's restriction table prices: `xform_size
1` lands exactly on restriction **B** and `xform_size 2` beats **B + C** —
except that the restrictions cost 1.6 to 1.8 % of rate to buy and `xform_size`
saves rate on the tiles where the encoder chooses it. Barriers per 4:4:4 tile
fall from 3 + 66 to 3 + 30 at 16x16 and 3 + 12 at 32x32.

**The reference Vulkan decoder does not implement tool bit 24** and refuses a
stream that sets it at the handshake, which is the same forward-compatibility
gate every other unimplemented tool goes through.

### Encode and decode time

`vr-mixed-1024-v2` 4:4:4, 4 frames, QP 16, one core of a contended machine, so
read the ratios and not the absolutes:

| `--xform` | encode s/frame | decode s/frame | bytes (4 frames) |
|---|---|---|---|
| `8` (the default, unchanged) | 2.841 | 0.195 | 447 484 |
| `16` | 2.450 | 0.115 | 495 326 |
| `32` | 3.094 | 0.152 | 494 916 |
| `auto` | 12.397 | 0.251 | 405 900 |

Decode is **faster** at a fixed 16x16 or 32x32 than at 8x8 despite 3.4x and
7.5x the multiplies, because the reference decoder is dominated by the entropy
stage and a 32x32 tile has a sixteenth of the coding units. `auto` costs about
4.4x the encode time: the per-tile search runs the whole quantize-and-
reconstruct pass three times, exactly as `--qp-search` and `--wm auto` already
do, and it is pure encoder work that changes no decoding path.

---

## 6. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-ref -j4
export PATH=$PWD/build-ref/bin:$PATH

# section 2, the Phase 1 gate (and --xform auto for the after column)
MODE=intra tools/quality/run-xform-a.sh base --xform 8
MODE=intra tools/quality/run-xform-a.sh auto --xform auto

# section 3, inter
MODE=interA tools/quality/run-xform-a.sh base --xform 8
MODE=interA tools/quality/run-xform-a.sh auto --xform auto
MODE=interB tools/quality/run-xform-a.sh base --xform 8
MODE=interB tools/quality/run-xform-a.sh auto --xform auto

# section 4.2 and 4.3, the two experiments
cmake -S . -B build-xexp -G Ninja -DNXWARP_BUILD_VK=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DNXVC_XFORM_CTX_EXPERIMENT
cmake --build build-xexp -j4 --target nxv-enc
build-xexp/bin/nxv-enc --in $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.yuv \
    --w 2048 --h 1024 --pix yuv444p --qp 16 --frames 1 --xform auto \
    --custom-tables --quiet --out /dev/null
```

`tools/quality/run-xform-a.sh` is the driver; it pins `NXQ_CPUS=4-7` and writes
`$NXQ_SCRATCH/results/tourney-xform-a-*.json`.

Conformance: `ctest --test-dir build-ref -R 'ref\.'`, and under the sanitizers
`cmake --preset asan-ubsan && ctest --preset asan-ubsan -R 'ref\.'`, where
`ref.transform` and `ref.saturate` sweep both large transforms at the bounds
of section 1's table.
