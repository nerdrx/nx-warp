# 6. Decoding process

This clause specifies how a conforming decoder turns a conforming bitstream
into decoded samples. It is written in execution order. Every step is
bit-exact; an implementation may reorder or parallelise only where the result
is identical for every legal input, which is the whole reason the format
forbids floating point, division and cross-tile state (clause 3.4).

Normative source: `docs/SYNTAX.md` [R-18] clauses 5 to 10, at commit `9083dd1`,
except where noted.

## 6.1 Order of operations

For each frame, for each tile:

| Step | Clause | Output |
|---|---|---|
| 1 | 6.2 | Parsed headers; tile mode, QP, geometry |
| 2 | 6.6 | Entropy decode: quantised coefficients per coding unit |
| 3 | 6.4.1 | Dequantisation |
| 4 | 6.4.2 | Inverse transform (or residual passthrough for `tskip`) |
| 5 | 6.7 / 6.8 / 6.9 | Prediction: intra DC plane, inter warp, stereo, or layer blend |
| 6 | 6.4.5 | Reconstruction: prediction plus residual, clamped |
| 7 | 6.5.3 | `res_level` and chroma upsampling into the picture |
| 8 | 6.3 | Colour conversion, if `color_transform == 1` |
| 9 | 6.10 | Reference and per-tile state update |
| 10 | 6.11 | Concealment of tiles that did not arrive |

Steps 2 and 3–8 correspond to the two GPU dispatches of [PAPER 3.2] — Pass A
and Pass B. That split is an implementation strategy, not a normative
requirement; a decoder may fuse them.

Nothing in steps 3 to 8 depends on any other tile of the same frame, except
`STEREO`, which depends on the co-located region of the already-decoded first
eye (clause 6.8). There is **no deblocking filter and no loop filter** in
version 1 [SYNTAX 10].

## 6.2 Tile geometry and sample domains

Derived from the tile header [SYNTAX 4.2]:

```
coded_size   = 64 >> res_level                       // luma and alpha
chroma_size  = (chroma444 ? 64 : 32) >> res_level
nb_luma      = coded_size / 8
nb_chroma    = chroma_size / 8
```

| Plane | Full extent in the picture | Coded extent |
|---|---|---|
| Y, A | 64 | `coded_size` (64, 32, 16) |
| Co, Cg in a 4:2:0 stream | 32 | `chroma_size` (32, 16, 8) |
| Co, Cg in a 4:4:4 stream | 64 | `chroma_size` (64, 32, 16 with `chroma444`; else 32, 16, 8) |

The per-plane **upsampling factor** is `full_extent / coded_extent`, one of 1,
2, 4 or 8 — 8 only for a 4:2:0 tile at `res_level == 2` inside a 4:4:4 stream.
Reconstruction upsamples in **one** bilinear step by that factor and never
cascades two steps [SYNTAX 4.2, decision 26].

Tiles at the right and bottom edges of a picture whose dimensions are not
multiples of 64 are coded as full tiles; samples outside the picture are
reconstructed and then discarded [SYNTAX decision 28].

**Sample domains** [SYNTAX 4.3]:

| | Plane 0 (Y) | Planes 1, 2 (Co, Cg) | Plane 3 (A) |
|---|---|---|---|
| `color_transform == 0` | `[0, 255]`, offset 128 | `[0, 255]`, offset 128 | `[0, 255]`, offset 128 |
| `color_transform == 1` | `[0, 255]`, offset 128 | `[0, 511]`, offset 256 | `[0, 255]`, offset 128 |

`maxval` is the top of the range; `dc_offset` is the offset. The table is
stated for 8-bit samples only. `bit_depth == 10` is signalled by a tool bit but
no 10-bit sample domain, no 10-bit `qstep` scaling and no 10-bit clamp is
specified anywhere. Recorded as Annex C issue C-14. [pending SYNTAX.md]

## 6.3 Colour conversion

Applied only when `color_transform == 1`, **after** upsampling, to planes 0, 1,
2 [SYNTAX 5.1]. The transform is the reversible YCoCg-R lifting of [R-7]:

```
inverse:  Co = plane1 - 256
          Cg = plane2 - 256
          t  = Y - (Cg >> 1)
          G  = Cg + t
          B  = t - (Co >> 1)
          R  = B + Co
```

`>>` is arithmetic (clause 3.3). The chroma planes are 9-bit, biased by 256.
The forward direction, for reference, is
`Co = R - B; t = B + (Co >> 1); Cg = G - t; Y = t + (Cg >> 1)`.

Because the transform is applied before subsampling on the encoder and after
upsampling on the decoder, a 4:2:0 tile in a YCoCg-R stream subsamples in the
YCoCg-R domain.

When `color_transform == 0` the planes are output exactly as reconstructed.

The interaction with the forthcoming `color_space` element is unresolved; see
Annex C issue C-1. [pending SYNTAX.md]

## 6.4 Dequantisation, inverse transform, reconstruction

### 6.4.1 Dequantisation

Per-plane quantisation parameters [SYNTAX 6.5]:

```
qp_tile   = clamp(base_qp + qp_delta, 0, 63)
qp(Y)     = qp_tile
qp(Co)    = qp(Cg) = clamp(qp_tile + chroma_qp_off, 0, 63)
qp(A)     = clamp(qp_tile + alpha_qp_off,  0, 63)
qp(DC plane of plane p) = max(0, qp(p) - 6)
```

The DC plane is quantised six QP steps — one halving of the step — finer than
its plane, with a flat weighting matrix, because coarse means produce visible
banding through the planar interpolation and the DC plane is only 1/64 of the
coefficients [SYNTAX decision 9].

For each coefficient at raster position `i` with quantised level `q`:

```
t = (qstep[qp] * w[i] + 8) >> 4          // Q4 step, at most 46340
c = clamp16((q * t + 8) >> 4)
```

`qstep` is Annex A.2. The weight `w[i]` is:

* `16` (flat) for DC-plane coding units;
* `16` (flat) for transform-skip blocks;
* the frame's weighting matrix otherwise — the luma matrix for planes 0 and 3,
  the chroma matrix for planes 1 and 2.

**Bound.** Weights are in `[1, 32]` and levels in `[-32767, 32767]`, so
`q * t` never exceeds `1.52e9` and the whole path is `int32`-safe with no
division and no saturation logic beyond `clamp16`
[SYNTAX 6.5, decision 12].

### 6.4.2 Inverse 1D transform

Input `x[0..7]`, output `y[0..7]`, both `int32`. Gain is exactly `2^10`
relative to the orthonormal DCT-III. Constants are Annex A.1; the flow graph is
the Loeffler-Ligtenberg-Moschytz factorisation [R-5].

```
// even part
t0 = (x0 + x4) * C4
t1 = (x0 - x4) * C4
t2 =  x2 * S2 - x6 * C2
t3 =  x2 * C2 + x6 * S2
e0 = t0 + t3 ;  e3 = t0 - t3
e1 = t1 + t2 ;  e2 = t1 - t2

// odd part
A = x1 * A1 + x7 * A7
B = x1 * A7 - x7 * A1
C = x3 * A3 + x5 * A5
D = x3 * A5 - x5 * A3
O0 = A + C
O3 = B - D
P  = A - C
Q  = B + D
O1 = ((P + Q) * C4 + 256) >> 9
O2 = ((P - Q) * C4 + 256) >> 9

y0 = e0 + O0 ;  y7 = e0 - O0
y1 = e1 + O1 ;  y6 = e1 - O1
y2 = e2 + O2 ;  y5 = e2 - O2
y3 = e3 + O3 ;  y4 = e3 - O3
```

### 6.4.3 Inverse 2D transform

`src[64]` are the dequantised coefficients in raster order within the block,
index `u * 8 + v` with `u` vertical and `v` horizontal frequency.

```
pass 1 (rows):     for each row r:
                       idct8_1d(src[r*8 .. r*8+7]) -> out[0..7]
                       tmp[c*8 + r] = clamp16((out[c] + 64) >> 7)

pass 2 (columns):  for each row r of tmp:
                       idct8_1d(tmp[r*8 .. r*8+7]) -> out[0..7]
                       dst[c*8 + r] = clamp16((out[c] + 4096) >> 13)
```

Both passes write transposed, so `dst` emerges in spatial raster order
`y * 8 + x`. Total shift is 20 and total gain 1: a coefficient of 1024 at
position 0 reconstructs a flat 128.

The `clamp16` after pass 1 is **normative**. It bounds the intermediate to 16
bits so a GPU may hold the transpose buffer in `int16` local memory, and it is
reachable with legal if pathological coefficient values. Because dequantised
coefficients are themselves clamped to `int16`, every product inside the
transform is bounded by about `8.9e7`, comfortably inside `int32`
[SYNTAX 6.3, decisions 10, 11].

The shifts are 7 and 13, not the 7 and 12 of [PAPER 1.4]: with this flow graph
the per-pass gain is `2^10`, so 7 + 12 would leave a residual gain of 2
[SYNTAX decision 10].

**Warning.** `docs/ERRATA.md` [R-24] corrects the same paper error to **8 and
12**. Both splits total 20 and both give unity gain, but they place the pass-1
intermediate one bit apart, so they round differently and produce **different
decoded samples**. The errata declares `docs/SYNTAX.md` authoritative while
itself stating the other numbers. This clause follows `docs/SYNTAX.md` (7 and
13), which is the document the conformance vectors were generated against.
This is a bit-exactness-level contradiction between two normative documents and
is Annex C issue **C-20**.

The forward transform is **not normative**. Any encoder may produce any
coefficients it likes [SYNTAX 6.4].

### 6.4.4 Transform skip

When `tskip == 1` no transform is applied. The 64 coded values of a block are
the residual samples in raster order within the block, quantised with a flat
weight, and the scan order is raster rather than zigzag
[SYNTAX 6.6, decision 15].

At QP 0 the dequantiser is the identity (`t = 16`, `c = q`), so `tskip` with a
resolved QP of 0 on every plane and `res_level == 0` is mathematically
lossless: the residual is `source - prediction` and reconstruction is
`clamp(prediction + residual)`. A 4:2:0 tile is lossless only with respect to
its own subsampled chroma.

### 6.4.5 Reconstruction

Per block, with the block's residual `res[8][8]` and the plane's prediction:

```
recon[y][x] = clamp(pred[y][x] + res[y - by*8][x - bx*8], 0, maxval)
```

For a plane whose `alpha_mode` is 0 or 1 (alpha only) no coefficients are coded
and the whole tile area is filled with 255 or `alpha_value`
[SYNTAX 7.3].

## 6.5 Resampling

### 6.5.1 The kernel

One kernel serves chroma upsampling, `res_level` upsampling and the DC-plane
planar prediction. `sx`, `sy` are Q4 source coordinates [SYNTAX 8]:

```
x0 = sx >> 4 ; fx = sx & 15 ; x1 = x0 + 1
y0 = sy >> 4 ; fy = sy & 15 ; y1 = y0 + 1
clamp x0, x1 to [0, w-1] and y0, y1 to [0, h-1]
out = ( p[y0][x0]*(16-fx)*(16-fy) + p[y0][x1]*fx*(16-fy)
      + p[y1][x0]*(16-fx)*fy      + p[y1][x1]*fx*fy      + 128 ) >> 8
```

The shift on `sx` is arithmetic, so negative coordinates floor correctly. **The
whole 2D interpolation is one rounding step**: a decoder MUST NOT implement it
as two separable passes with an intermediate rounding.

### 6.5.2 Border policy

Source coordinates are clamped to the plane extent — clamp-to-edge — before the
fetch, per tap. This is what makes the interpolation flat in the outer
half-block border of a DC plane, and it is also the border policy of the inter
predictor (clause 6.7).

### 6.5.3 Upsampling into the picture

For an upsample by `factor` in `{1, 2, 4, 8}`, output sample `x` maps to the Q4
source coordinate:

```
mul = 16 / factor
off = mul / 2 - 8
sx  = mul * x + off
```

which is the half-phase alignment `source = (x + 0.5)/factor - 0.5`. At factor
2 the fractions are 12 and 4, the 3/4 and 1/4 weights [PAPER 1.3] requires. At
factor 1 the sample is copied. (`mul` and `off` are compile-time constants of
the four legal factors; the divisions in this formula are not evaluated at
run time.)

Encoder-side chroma downsampling is the rounded 2x2 average
`(a + b + c + d + 2) >> 2` and is informative [SYNTAX 5.2].

## 6.6 Entropy decoding

### 6.6.1 rANS parameters

Fixed, with exactly one state width and one probability precision, which is
what keeps the construction inside Duda's published rANS [R-8] and outside the
2022 claims of [I-15] [SYNTAX 9.5, PAPER 1.9].

| Parameter | Value |
|---|---|
| State | 32-bit unsigned |
| `L` (lower bound) | `2^16` |
| Renormalisation | 16 bits at a time |
| Probability precision `M` | `2^10` |
| Lanes per tile | `2^nsub_log2`, 8 in version 1 |

### 6.6.2 Building the frequency tables

For each of the eight table sets, for each of the 12 contexts, the 16
frequencies are 10-bit, each at least 1, summing to exactly 1024, so that every
symbol is always decodable and a hostile stream cannot produce an undefined
symbol [SYNTAX decision 21].

If bit `k` of `tables_present` is clear, set `k` is the built-in default
(Annex A.6). If it is set, each transmitted 5-bit `table_delta` selects a
multiplier from `kDeltaMul` (Annex A.3) applied to the built-in default of the
*same* set index:

```
f[s] = clamp((default[s] * kDeltaMul[d] + 128) >> 8, 1, 32767)
```

and the row is then normalised to sum exactly 1024 by this deterministic
procedure — the one place a decoder divides, running 12 x 8 times per frame,
not per symbol (clause 3.4):

```
sum = sum(f)
if sum == 1024: done
for each s:  g[s] = clamp((f[s] * 1024) / sum, 1, 1009)     // truncating divide
total = sum(g)
while total < 1024:  add 1 to the largest g[s] (ties: lowest index);  total++
while total > 1024:  subtract 1 from the largest g[s] with g[s] > 1;  total--
```

The decoder then forms the prefix sums `cum[s]` and the 1024-entry
slot-to-symbol table `slot2sym` that the decode step reads
[SYNTAX 9.4, decision 22].

### 6.6.3 Coding units and lane assignment

A tile's payload is a list of coding units in this order [SYNTAX 9.1]:

```
for each coded plane p in (Y, Co, Cg [, A if alpha_mode == 2]):
    unit: the DC plane of p            (nb*nb coefficients)
    for by in 0 .. nb-1, bx in 0 .. nb-1:      // raster
        unit: block (bx, by) of p      (64 coefficients)
```

`N = 2^nsub_log2` lanes exist and `active = min(N, unit_count)` are used. Lane
`l` owns units `l, l+N, l+2N, ...` and decodes them in that order. A lane with
no units is not initialised and consumes no bytes.

### 6.6.4 The schedule

At each step of the schedule, lanes `0, 1, ..., active-1` in that order each
consume **one** operation, skipping lanes that have finished. Because the
number of operations a unit needs is determined causally by the values already
decoded, every lane knows when it is done with no side information. This is
exactly the loop a GPU runs with the eight lanes of a tile in one subgroup
cluster [SYNTAX 9.1].

The payload begins with `4 * active` bytes of `lane_init_state`, lane 0 first,
each little endian and each at least `L`. The remaining bytes are the
interleaved renormalisation pairs, consumed in schedule order.

### 6.6.5 Decoding one symbol

For a symbol in context `t`:

```
slot = x & 1023
s    = t.slot2sym[slot]
x    = t.freq[s] * (x >> 10) + slot - t.cum[s]
if x < 2^16:  x = (x << 16) | (buf[p] << 8 | buf[p+1]) ;  p += 2
```

The renormalisation pair is read `hi, lo` — big-endian inside an otherwise
little-endian format — because it was produced by an encoder writing backwards
(clause 3.1).

For `k` bypass bits, `1 <= k <= 8`, the same step with an implicit uniform
context:

```
slot = x & 1023
v    = slot >> (10 - k)
x    = (1 << (10 - k)) * (x >> 10) + slot - (v << (10 - k))
renormalise as above
```

A bypass value wider than 8 bits is split into chunks of at most 8 bits, most
significant chunk first; the first chunk carries `nbits - 8 * (chunks - 1)`
bits and every later chunk 8. Only the escape suffix, up to 19 bits, is wider
than 8 [SYNTAX 9.5, decision 19].

**Contexts** [SYNTAX 9.3]. Twelve contexts of 16 symbols:

| Index | Use |
|---|---|
| 0 | `cbf`, luma and alpha planes |
| 1 | `cbf`, chroma planes |
| 2 | `last_class`, luma and alpha planes |
| 3 | `last_class`, chroma planes |
| 4–11 | `level`, band x previous-level class |

with

```
band = 0 if pos == 0, 1 if pos in 1..3, 2 if pos in 4..9, 3 if pos >= 10
prev = 0 if the previously decoded level was 0,
       1 if its magnitude was 1, else 2      (prev = 0 at the unit's first position)

               prev=0  prev=1  prev=2
   band 0        0       1       2
   band 1        3       4       2
   band 2        5       6       7
   band 3        5       6       7

ctx_level = 4 + that value
```

A DC-plane unit uses the same contexts as its plane.

### 6.6.6 Scan order and the escape code

| Unit | Scan |
|---|---|
| 64-coefficient block, `tskip == 0` | 8x8 zigzag (Annex A.5) |
| 64-coefficient block, `tskip == 1` | Raster, `scan[i] = i` |
| DC plane, 64 values | 8x8 zigzag |
| DC plane, 16 values | 4x4 zigzag (Annex A.5) |
| DC plane, 4 values | `0, 1, 2, 3` |
| DC plane, 1 value | `0` |

The escape suffix of a `level` symbol of 15 codes `v = abs(q) - 15` as
order-3 Exp-Golomb in bypass bits:

```
encode: n = v + 8 ; b = floor(log2(n)) ; j = b - 3
        emit j one-bits, then a zero bit, then the low b bits of n

decode: count one-bits until a zero -> j   (j > 16 MUST be rejected)
        read j + 3 bits -> r
        v = (2^(j+3) - 8) + r              (v > 32752 MUST be rejected)
```

[SYNTAX 9.3, decision 20]

## 6.7 Inter prediction: the pose warp

**Status: provisional.** No normative document specifies this process yet. The
description below is transcribed from `warp/include/nxvc/warp.h` and
`warp/ref/warp_ref.cpp` at commit `9083dd1`, cross-checked against
[PAPER 2.2], and is subject to change when `docs/WARP.md` lands. It is not
sufficient to implement from, for the reason given in clause 6.7.6.
[pending WARP.md]

### 6.7.1 What the predictor is

For every tile, the prediction is the reference picture resampled through a
global per-eye homography derived from the head-pose delta, plus the tile's own
2D vector. Nothing else: no depth, no per-pixel warp, no hole filling. Because
a homography plus a shift never produces holes, the predictor is always dense
and the decoder needs no inpainting pass [PAPER 2.1, 2.2].

### 6.7.2 Corner evaluation

Source coordinates are computed for the four tile corners only — four divisions
per tile, not per pixel:

```
num_x = h00*x + h01*y + h02
num_y = h10*x + h11*y + h12
den   = h20*x + h21*y + h22
x_src = (num_x << kDivShift) / den                 // Q.6
```

with each product formed as a 64-bit `(hi, lo)` pair via extended multiply, and
the division performed by a **fixed 32-iteration restoring division** built from
shifts, comparisons and subtractions — never a division opcode (clause 3.4).

Fixed-point formats and bounds, from `warp/include/nxvc/warp.h @ 9083dd1`:

| Quantity | Format | Bound |
|---|---|---|
| `h00 .. h12` | Q10.21 | `+-1024.0` |
| `h20 .. h22` | Q2.29, `h22 == 1 << 29` | — |
| `den` | Q2.29 | MUST be in `[2^28, 2^30)`, i.e. `[0.5, 2.0)`; the encoder guarantees it, the decoder saturates |
| Corner coordinates | Q.6 | Saturated to `+-2^18`, i.e. `+-4096` samples, so the in-tile interpolation cannot overflow `int32` |
| `kDivShift` | — | `6 + 29 - 21 = 14` |

The matrix maps *centred* integer sample indices of the target frame to centred
indices of the reference: `(x - ox, y - oy) -> (x_src - ox, y_src - oy)`, with
`(ox, oy)` normally the picture centre. Corners are returned in the order
`(tx, ty)`, `(tx+64, ty)`, `(tx, ty+64)`, `(tx+64, ty+64)`.

For `mode == STATIC_MV` the homography is ignored and the corners are the tile's
own, i.e. the identity predictor, which is exactly right for head-locked content
[PAPER 2.3].

### 6.7.3 In-tile interpolation

Inside the tile the source coordinate is bilinearly interpolated from the four
corners with integer arithmetic only:

```
top = c00 * (64 - u) + c10 * u
bot = c01 * (64 - u) + c11 * u
acc = top * (64 - v) + bot * v
coord = (acc + 2048) >> 12                        // 12 == 2*log2(64)
```

evaluated separately for x and y, `u` and `v` being the sample's position within
the tile.

### 6.7.4 Motion vector and rounding

The tile's quarter-pel vector is promoted to Q.6 by a shift of 4 and added, then
the sum is taken to the 1/16-pel sampling grid:

```
xq6 = interp_x + (mv_x << 4)
yq6 = interp_y + (mv_y << 4)
```

with the integer sample index and the 1/16 fraction derived from the Q.6 value.
The exact reduction from Q.6 to Q.4 is in `warp/ref/warp_ref.cpp`; the paper
states it as "the sum is rounded to 1/16 pel" with the rounding written once as
add-half-and-shift [PAPER 2.2 step 4].

### 6.7.5 Sampling filters

Two integer filters, selected per stream — see clause 6.7.6.

**Bilinear** (Lite): weights on 16, product on 256.

```
gx = 16 - fx ; gy = 16 - fy
acc = gx*gy*p(ix,iy) + fx*gy*p(ix+1,iy) + gx*fy*p(ix,iy+1) + fx*fy*p(ix+1,iy+1)
out = (acc + 128) >> 8
```

**Catmull-Rom** (Full): 4x4 taps from the 16-entry table of Annex A.4, each row
summing to exactly 64, with the horizontal pass carried at full precision and a
single rounding at the end.

```
for j in 0..3:
    row = sum over i in 0..3 of  wx[i] * p(ix-1+i, iy-1+j)
    acc += wy[j] * row
out = (acc + 2048) >> 12                          // 64*64 == 4096
```

The bound `|acc| <= 72*72*maxval < 2^23` at 10 bits keeps this inside `int32`.

Every tap is fetched with clamp-to-edge clamping on integer sample indices
(clause 6.5.2). **The hardware sampler MUST NOT be used**: sampler weight
precision is vendor-specific, and encoder and decoder run on different vendors,
so a sampler-based predictor would drift by `+-1` LSB per frame until the next
refresh [PAPER 3.2.3].

### 6.7.6 Why this clause is not yet implementable

1. **No syntax element carries the homography** (clause 4.4, Annex C issue
   C-4). Without it the process above has no input.
2. **The filter is unsignalled.** Bilinear versus Catmull-Rom changes every
   predicted sample, so it is a normative, bit-exactness-critical choice, yet
   the only thing that selects it is `profile`, which `docs/SYNTAX.md` marks
   *informative*, and no tool bit distinguishes them (Annex C issue C-7).
3. **The vector's coding is unstated.** Whether `mv_x`, `mv_y` are absolute or
   a delta from the tile's stored vector is asserted only by the paper
   (Annex C issue C-11).
4. **The per-tile state** that `WARP_SKIP` and concealment read — the stored
   last vector — is described only in [PAPER 2.6] and has no normative
   definition (clause 6.10).

## 6.8 Stereo prediction

Normative source: `docs/STEREO.md` [R-22].

`mode == STEREO` predicts a tile of the right eye from the **decoded left eye of
the same frame**. It requires `stereo_enable` in the frame header and the
`STEREO` tool bit. Left-eye tiles never carry mode `STEREO`; a stream in which
one does MUST be rejected [STEREO 9].

### 6.8.1 The predictor

**The pose homography is NOT applied.** The warp compensates the change of view
between two instants; the two eyes of frame `N` are the same instant, so there
is nothing to compensate, and applying the warp here would be a defect rather
than a refinement [STEREO 4]. The sampling chain is therefore the shorter one:

```
x_src_q6 = ((x + D_int) << 6) + D_frac_q6      // no homography, no corner interpolation
x_q4     = (x_src_q6 + 2) >> 2
pred     = filter16(left_recon, x_q4, y << 4)
```

with `D` split into integer and quarter-pel parts by the vector decode. The
rounding is the same add-half-and-shift as the warp path and the filter is the
same 16-phase table (clause 6.7.5, Annex A.4), so a decoder implements STEREO
by selecting a different reference image and skipping the corner-division step.
Clamp-to-edge applies as elsewhere.

**The disparity is horizontal-only: the vertical component is zero in version
1** [STEREO 6.1]. This is what bounds a right-eye tile's source region to at
most three left-eye tiles of the *same* row, which is what makes the
interleaved dispatch sufficient. A downward vertical component would make a
right tile depend on a left tile row that has not been dispatched and is
forbidden.

### 6.8.2 Ordering and loss

* A `STEREO` tile MUST be decoded after the left-eye tiles its source region
  covers. Dispatch order is L row `r`, R row `r`, interleaved; the right eye
  lags by one row's decode time and total decode time is unchanged
  [PAPER 2.5].
* A `STEREO` tile whose left-eye reference has not arrived by the deadline is
  treated as lost — concealed and reported missing — like any other loss. A
  right tile may not reference left tiles it cannot prove are present
  [STEREO 6.3].
* This is the **only** intra-frame tile dependency in the format.

### 6.8.3 Consequences for state and for downstream consumers

* `last_mv` holds the **disparity** for a `STEREO` tile. Because a tile may
  alternate between `STEREO` and `WARP_MV` across frames and the two vectors
  mean different things, the temporal delta predictor MUST be per-mode-class:
  a disparity is predicted from the last disparity, a motion vector from the
  last motion vector. This is an extra vector field in the per-tile state
  [STEREO 9]. It is a further reason the per-tile state needs a normative
  definition (clause 6.10, Annex C issue C-15).
* `STEREO` tiles MUST be excluded from the motion field handed to client-side
  motion smoothing, exactly as `STATIC_MV` tiles are: a 60-sample disparity
  extrapolated as if it were object motion tears the right eye apart on a
  synthesised frame [STEREO 4.1].

### 6.8.4 Two unresolved conflicts with the syntax

1. **The vector coding does not match the syntax.** [STEREO 2.3] decides that
   the STEREO vector is coded as an **unsigned disparity in quarter samples
   with an Exp-Golomb code and no fixed upper bound** (practically `+-512`
   samples), because `f * IPD / z` reaches 60 samples at 1 m and about 200 at
   30 cm, and 37.6% of tiles in the experiment exceeded the coarse search
   range. Clause 4.7 codes the vector as two signed bytes `mv_x`, `mv_y` in the
   tile header, which reaches `+-31.75` samples — **not enough for near-field
   content**, and not an Exp-Golomb code, and not in the payload. Annex C issue
   **C-21**.
2. **`ref_sel` is not coded for a STEREO tile** [STEREO 9], because the mode
   itself names the reference. Clause 4.7 has `ref_sel` as two fixed bits of
   `word1` that are always present. Annex C issue C-22.

## 6.9 Layered and hybrid prediction

For `layer > 0`, each tile has two predictor hypotheses — the upsampled
reconstruction of the layer below (spatial) and the warped previous
reconstruction of the same layer (temporal) — blended by `wgt`
[PAPER 1.7]. With `layer_type` 1 or 2 the base layer is an HEVC or AVC stream
decoded externally and converted into this format's sample domain.

Neither the blend formula, the rounding of the blend, the upsampling of the
lower layer, nor the external decoder's colour conversion is specified. A
decoder cannot implement this clause. [pending HYBRID.md]

## 6.10 Reference and state update

After a tile is reconstructed:

* the reconstructed samples are written into the reference slot the frame
  overwrites, in display format (RGBA8 or RGB10A2), one image per slot, four
  slots [PAPER 1.3, 6.6];
* the tile's per-tile state is updated. [PAPER 2.6] specifies 16 bytes —
  `held_frame_id`, `last_mv`, `age_since_intra`, `concealed_count`, and mode,
  QP and flags — and [TRANSPORT 7.3] specifies a *different* 4-byte per-tile
  record — `pose_seq`, `age`, `state`, `late`, `recovered`. The two overlap
  without agreeing, and only the transport one is normative. The decoder-side
  state that prediction reads is therefore not normatively defined. Recorded as
  Annex C issue C-15. [pending WARP.md]

The paper's rule that `WARP_SKIP` and `STATIC_MV` update the stored vector
differently — `STATIC_MV` and `INTRA` do not update it — is stated only in the
pseudo-code of [PAPER 2.10].

## 6.11 Concealment

At the presentation deadline the decoder reconstructs with whatever arrived. A
tile position with no bitstream is reconstructed by running the prediction
process in `WARP_SKIP` with the stored `last_mv` and no residual, and is marked
extrapolated [PAPER 2.7].

Three properties make this normative rather than a quality-of-implementation
matter:

1. **Concealment is identical to a legitimately skipped tile**, so there is no
   separate concealment code path to specify or to test.
2. **It is deterministic**, so the encoder can replay it exactly on its shadow
   of the receiver and keep predicting from the true receiver state.
3. **Concealed samples are legal reference samples**, but only under the
   recursive exactness rule of clause 7.3: a concealed tile is exact only if
   the source samples its warp read were themselves exact
   [TRANSPORT D10].

A decoder MUST NOT inpaint, blur, or otherwise invent samples for a missing
tile: anything other than the deterministic warp diverges from the encoder's
shadow and the error becomes permanent until the next intra refresh
[PAPER 2.7]. A client-side blend of an extrapolated tile toward the previous
*output* is permitted, because it happens after reconstruction and does not
touch the reference.

**Malformed tiles are rejected, not concealed** [SYNTAX decision 29].
Concealment is a transport-layer decision and needs a reference frame; a Phase 1
decoder has neither.
