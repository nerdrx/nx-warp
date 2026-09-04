# Large transforms: 16x16 and 32x32, per tile (tool bit 24)

What this package is, why it exists, what it measured, and what it costs a GPU
decoder. Everything here was produced by `tools/quality/compare.py` and by
`ref/`'s own tools, every process under
`chrt -i 0 taskset -c 4-7 nice -n 19`, with result files under
`$NXQ_SCRATCH/results/tourney-xform-a-*.json`. Section 6 reproduces every
number.

**The four numbers.** On 8 frames of `vr-mixed-1024-v2`, before is the shipped
default and after is `--xform auto`:

| measurement | before | after | change |
|---|---|---|---|
| Phase 1 gate, 4:4:4, BD-rate vs x264 intra | +64.84 % | **+45.95 %** | **-18.9 points** |
| Phase 1 gate, 4:2:0, BD-rate vs x264 intra | +40.62 % | **+19.50 %** | **-21.1 points** |
| Phase 2 kill test, band A, BD-rate vs x265-p | +333.00 % | **+282.73 %** | -50.3 points |
| Phase 2 kill test, band B, BD-rate vs x265-p | +454.96 % | **+398.86 %** | -56.1 points |

Both gates still **FAIL**; the verdicts are quoted verbatim in sections 2 and
3. The 4:2:0 gate's mean deficit is now -1.198 dB against a -1.0 dB criterion.
Every operating point of every run is both smaller and better than its before,
which is the shape a prediction-neutral coding tool should have.

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

`tools/quality/compare.py` on `vr-mixed-1024-v2-8f` -- the first **8 frames**
of the band-limited v2 sequence, truncated into its own sidecar because ten of
these measurements were running on the machine at once -- against
`x264 --keyint 1 --tune zerolatency` over the 100-400 Mbit band, QP ladder
`0,4,8,12,16,20,24` against anchor `8,12,16,20,24,28`. "before" is the shipped
default (`--xform 8`, byte-identical to a build without the tool); "after" is
`--xform auto`. Both columns are the same 8 frames, the same ladder and the
same anchor run, so the pair is comparable by construction; the absolute
numbers are not comparable with `RESULTS-intra.md`, which used 36 frames of the
v1 sequence.

### 4:4:4

| | before (`--xform 8`) | after (`--xform auto`) | change |
|---|---|---|---|
| **BD-rate vs x264-intra, PSNR-Y** | **+64.84 %** | **+45.95 %** | **-18.9 points** |
| BD-PSNR | -4.465 dB | -3.439 dB | **+1.03 dB** |
| BD-rate on SSIM-Y | +106.57 % | +80.99 % | -25.6 points |
| gate: worst deficit | -4.694 dB | **-3.487 dB** | **+1.21 dB** |
| gate: mean deficit | -3.835 dB | **-2.844 dB** | +0.99 dB |

Per operating point, and this is the shape a tool that helps should have --
every point is both **smaller and better**:

| QP | before Mbit/s | before PSNR-Y | after Mbit/s | after PSNR-Y | rate | quality |
|---|---|---|---|---|---|---|
| 0 | 251.4 | 57.28 | 237.8 | 57.74 | -5.4 % | +0.46 dB |
| 4 | 189.9 | 55.35 | 174.2 | 55.68 | -8.3 % | +0.33 dB |
| 8 | 141.7 | 53.31 | 130.2 | 53.59 | -8.1 % | +0.28 dB |
| 12 | 106.7 | 50.62 | 98.4 | 51.02 | -7.8 % | +0.40 dB |
| 16 | 80.6 | 47.62 | 73.3 | 47.95 | -9.1 % | +0.33 dB |
| 20 | 59.6 | 44.52 | 54.1 | 44.86 | -9.2 % | +0.34 dB |
| 24 | 44.7 | 41.45 | 39.3 | 41.63 | -12.1 % | +0.18 dB |

The gate verdicts, verbatim:

```
  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -4.694 dB at 102.4 Mbit/s, mean -3.835 dB over 100.0-212.6 Mbit/s     <- before

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.487 dB at 102.4 Mbit/s, mean -2.844 dB over 100.0-212.6 Mbit/s     <- after
```

**The gate is still not met.** It needs -1.0 dB and the tool buys 1.2 of the
3.7 that are missing. What it is, is the second-largest single tool in the
codec: directional intra was -22.5 BD-rate points on 4:4:4 and this is -18.9,
against a decoder cost that is arithmetic rather than serialisation (section 5)
and a syntax cost of two bits per tile.

### 4:2:0

| | before (`--xform 8`) | after (`--xform auto`) | change |
|---|---|---|---|
| **BD-rate vs x264-intra, PSNR-Y** | **+40.62 %** | **+19.50 %** | **-21.1 points** |
| BD-PSNR | -3.437 dB | -1.909 dB | **+1.53 dB** |
| BD-rate on SSIM-Y | +73.76 % | +46.68 % | -27.1 points |
| gate: worst deficit | -3.459 dB | **-1.749 dB** | **+1.71 dB** |
| gate: mean deficit | -2.449 dB | **-1.198 dB** | +1.25 dB |

| QP | before Mbit/s | before PSNR-Y | after Mbit/s | after PSNR-Y | rate | quality |
|---|---|---|---|---|---|---|
| 0 | 200.0 | 57.29 | 182.6 | 57.74 | -8.7 % | +0.45 dB |
| 4 | 153.4 | 55.35 | 136.5 | 55.66 | -11.0 % | +0.31 dB |
| 8 | 119.3 | 53.31 | 105.1 | 53.57 | -11.9 % | +0.26 dB |
| 12 | 94.0 | 50.63 | 82.5 | 51.03 | -12.2 % | +0.40 dB |
| 16 | 74.2 | 47.63 | 64.3 | 48.07 | -13.3 % | +0.44 dB |
| 20 | 56.2 | 44.49 | 48.2 | 44.93 | -14.2 % | +0.44 dB |
| 24 | 42.9 | 41.45 | 36.0 | 41.69 | -16.1 % | +0.24 dB |

```
  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.459 dB at 101.1 Mbit/s, mean -2.449 dB over 100.0-200.0 Mbit/s     <- before

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -1.749 dB at 101.0 Mbit/s, mean -1.198 dB over 100.0-182.6 Mbit/s     <- after
```

4:2:0 gains more than 4:4:4 does, and it is the configuration the gate is
closest to meeting: **-1.749 dB worst and -1.198 dB mean against a -1.0 dB
criterion.** The chroma plane of a 4:2:0 tile is 32x32, so `xform_size == 2`
codes each chroma plane as a *single* 32x32 block with a one-value DC plane --
the cheapest possible description of a smooth chroma plane, and smooth chroma
is most of what a 4:2:0 tile's chroma is.

### Summary

| | 4:4:4 before | 4:4:4 after | 4:2:0 before | 4:2:0 after |
|---|---|---|---|---|
| BD-rate vs x264 intra | +64.84 % | **+45.95 %** | +40.62 % | **+19.50 %** |
| gate mean deficit | -3.835 dB | **-2.844 dB** | -2.449 dB | **-1.198 dB** |
| gate verdict | FAIL | FAIL | FAIL | FAIL |

---

## 3. Inter, with the same tool

Residuals of warped tiles are smooth, so the tool should help there too. The
Phase 2 kill test of `RESULTS-inter.md`, band A (the literal 100-300 Mbit band
of PAPER 2.11), on the same 8-frame 4:4:4 sequence with `--eyes 2 --inter on
--poses`, `--qp 0,4,8,12` against `x265-p` at `2,8,14,20`:

| | before (`--xform 8`) | after (`--xform auto`) | change |
|---|---|---|---|
| **BD-rate vs x265-p, overall** | **+333.00 %** | **+282.73 %** | **-50.3 points** |
| BD-PSNR | -7.450 dB | -6.743 dB | +0.71 dB |
| fastest 20 % of frames | +311.57 % | +263.94 % | -47.6 points |
| the remaining frames | +339.81 % | +288.67 % | -51.1 points |

| QP | before Mbit/s | before PSNR-Y | after Mbit/s | after PSNR-Y | rate | quality |
|---|---|---|---|---|---|---|
| 0 | 206.5 | 57.19 | 192.8 | 57.47 | -6.6 % | +0.28 dB |
| 4 | 140.1 | 55.21 | 128.6 | 55.42 | -8.2 % | +0.21 dB |
| 8 | 95.0 | 53.04 | 87.5 | 53.21 | -7.9 % | +0.17 dB |
| 12 | 66.7 | 50.15 | 61.2 | 50.45 | -8.2 % | +0.30 dB |

The tool helps inter about as much as it helps intra, and for the same reason:
a warped tile's residual is smooth, and a 32x32 transform is what codes a
smooth residual cheaply. It helps the motion frames and the still frames
almost equally, which says it is acting on the residual's spectrum rather than
on the predictor's failure mode.

The kill test still fails, by a margin the tool does not touch. Verbatim:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +339.81 % (allowed up to +10 %)  FAIL      <- before
    on motion : BD-rate +311.57 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL

  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +288.67 % (allowed up to +10 %)  FAIL      <- after
    on motion : BD-rate +263.94 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

The absolute figures are worse than `RESULTS-inter.md`'s +160.70 % because
that measurement had 36 frames to amortise its first intra frame over and this
one has 8; the before/after pair here is internally consistent and that is
what it is for.

### Band B, the paper's own bits per pixel

`--qp 18,24,30,36` against `x265-p` at `26,32,38,44`, the band
`RESULTS-inter.md` argues is where the codec is actually designed to run:

| | before (`--xform 8`) | after (`--xform auto`) | change |
|---|---|---|---|
| **BD-rate vs x265-p** | **+454.96 %** | **+398.86 %** | **-56.1 points** |
| BD-PSNR | -11.770 dB | -10.528 dB | +1.24 dB |

| QP | before Mbit/s | before PSNR-Y | after Mbit/s | after PSNR-Y | rate | quality |
|---|---|---|---|---|---|---|
| 18 | 35.17 | 45.12 | 31.73 | 45.30 | -9.8 % | +0.18 dB |
| 24 | 16.56 | 40.48 | 15.40 | 40.57 | -7.0 % | +0.09 dB |
| 30 | 8.69 | 36.17 | 7.56 | 36.25 | -13.0 % | +0.08 dB |
| 36 | 4.95 | 31.74 | 4.16 | 31.99 | -16.0 % | +0.25 dB |

The tool is worth slightly more at the paper's own operating point than at the
literal band, which is the opposite of what section 3.1's per-tile histogram
predicts for *intra* and is worth saying plainly: on an inter frame the coded
residual is small and smooth at every QP, so the transform size matters for
the same reason at both ends of the ladder.

### 3.1 What the encoder actually chooses

`nxv-info --tiles` on one 2048x1024 4:4:4 frame (512 tiles) encoded with
`--xform auto`:

| QP | 8x8 | 16x16 | 32x32 |
|---|---|---|---|
| 4 | 238 | 38 | 236 |
| 12 | 225 | 54 | 233 |
| 20 | 256 | 88 | 168 |
| 28 | 331 | 57 | 124 |

The large transforms win **more** at low QP than at high QP, which is the
opposite of the naive expectation and is the same fact section 4.3's mode
histogram shows from the other side. At a coarse quantiser a smooth tile is
nearly free at any block size -- the DC plane alone already describes it -- so
the transform size barely matters and the 8x8 form wins on the detailed tiles.
At a fine quantiser the smooth tiles are where the bits actually are, and that
is exactly where a 32x32 transform's energy compaction pays. The tool is a
high-rate tool on this material, and the Phase 1 gate band is a high-rate band.

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

### 4.4 What was not tried

The DC plane of a 16x16-transform tile is a 4x4 image of block means and of a
32x32-transform tile a 2x2 one, and both are coded **flat** -- the second-level
8x8 DCT fires only at `nb == 8`, by the rule that was already there for
`res_level` tiles. A 4x4 second-level transform would be a new transform in the
format for 16 values per plane per tile. It is the obvious next thing to
measure and it is deliberately not in this package: the syntax is untouched by
leaving it out, so it stays available.

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
