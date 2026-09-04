# NX Warp: the pose-warped predictor

**Normative definition. Draft 1, 2026-09-04.**
Implements paper sections 2.2, 2.3 and 2.10, in the Pass B shape of 3.2.1 / 3.2.3.

This document defines the predictor exactly. Two independent implementations that
follow it must produce bit-identical output for identical inputs, on every CPU and
every Vulkan implementation. That is not a quality goal, it is a correctness
requirement: the encoder runs the decoder to build its references (paper 2.6), so a
single differing pixel drifts forever and is only cleared by the next intra refresh.

Normative implementations:

| | |
|---|---|
| CPU reference | `warp/ref/warp_ref.cpp` (`nxvc_warp_ref`) |
| GPU kernel | `warp/glsl/warp_tile.comp` |
| API | `warp/include/nxvc/warp.h` |
| Conformance | `tests/warp/`, `warp/tools/nxvc-warpdiff` |

`warp/ref/homography.cpp` (pose to matrix) and `warp/ref/warp_oracle.cpp` (float
oracle) are **not** normative. The first runs only on the encoder and its output
crosses the wire already quantised; the second exists only to bound the error of
the integer path in tests and must never be reachable from a decode.

---

## 1. What the predictor is

For a tile of the frame being decoded, the prediction is the previous decoded
frame resampled through a per-eye homography derived from the head pose delta,
plus a per-tile quarter-pel motion vector. Nothing else: no depth, no per-pixel
motion, no hole filling. The predictor is dense by construction, so there is
never a hole to fill.

```
warp_tile(ref, tile_x, tile_y, H, mv_qpel, filter, mode, out_tile, out_stride)
```

Five tile modes exist in the bitstream (paper 2.3, 6.5). This module implements
the two that touch the warp:

| Mode | `mode` argument | Predictor |
|---|---|---|
| `WARP_SKIP`, `WARP_MV` | `kModeWarp` | homography + MV |
| `STATIC_MV` | `kModeStatic` | identity + MV, `H` ignored entirely |
| `STEREO` | `kModeStatic` against the left-eye picture | identity + disparity |
| `INTRA` | not applicable | no prediction |

`STEREO` reuses the `kModeStatic` path with the decoded left eye as `ref`. See
section 11 for a v1 limitation this exposes.

### Constraints obeyed everywhere below

- Every value on the normative path is `int32`. No `float`, no `double`.
- No 64-bit integer opcode. Where a 64-bit intermediate is needed it is an
  explicit `(hi, lo)` pair of `uint32` built from 32x32->64 multiplies
  (`OpUMulExtended`, core SPIR-V; `shaderInt64` is **not** required).
- No division anywhere except the fixed 32-iteration restoring divide of
  section 5. No modulo, no reciprocal.
- Right shifts of signed values are arithmetic (sign-propagating). C++20
  mandates this; SPIR-V spells it `OpShiftRightArithmetic`. The reference
  writes it as `sar()` to make the requirement visible.
- The hardware texture sampler is never used (paper 3.2.3). Sampler weight
  precision is vendor-specific and the encoder runs on different silicon than
  the decoder, so a sampler-based predictor would drift by an LSB per frame.
  All taps are explicit integer loads.

---

## 2. Coordinate conventions

The streamed picture for one eye is `W x H` samples. A sample is identified by
its integer index `(x, y)`, `0 <= x < W`, `0 <= y < H`. The **centre** of sample
`(x, y)` sits at continuous position `(x + 0.5, y + 0.5)`.

The homography operates on **centred integer sample indices**:

```
cx = x - ox        ox = W / 2
cy = y - oy        oy = H / 2
```

Both the half-sample offset and the centring are folded into the matrix on the
encoder (section 4), so the decoder never applies either. The decoder subtracts
`ox`/`oy`, runs the matrix, and adds them back.

Centring is not cosmetic. The translation entries of an uncentred matrix carry
the absolute position of the picture origin and grow with `W`; centred, they
carry only the displacement of the picture centre, which is bounded by the head
motion. This is what makes the fixed-point format of section 3 fit.

A tile is `kTile = 64` samples square and its origin `(tile_x, tile_y)` is a
multiple of 64.

## 2.1 Pose conventions, and the `.poses.json` schema

Everything in section 4 is stated on quantities — a quaternion, a field of
view, a picture — whose meaning is a **convention**. This section names each
one, because a convention error is the failure mode this module cannot
otherwise detect: it does not crash, it does not produce an illegal bitstream,
it passes every `warp.*` test (all of which check the predictor against its own
arithmetic rather than against a picture the world produced), and it presents
as a codec that is merely bad. `docs/WARP-AUDIT.md` is the measurement that
established the list, and section 3 there prices each error in dB.

**Convention `nxv-openxr-1`.** This is what `derive_homography()` implements
and what `nxv-enc` assumes.

| | |
|---|---|
| Quaternion component order | `(x, y, z, w)`, `w` last |
| Handedness | right-handed, active rotation |
| What the quaternion rotates | **camera to world**: `R * v_camera = v_world` |
| Camera axes | x right, y **up**, **-z forward** (OpenXR) |
| Field of view | `XrFovf` half-angles in radians, **left and down negative** |
| Image origin | row 0 is the **top** of the picture; the row index runs downward, which is why `K` row 1 carries `-sy` |
| Pixel centre | sample `(i, j)` is sampled at `(i + 0.5, j + 0.5)`; the `+0.5` is folded into `K` |
| Coordinate origin | picture **centre**, `(W/2, H/2)`, per section 2 |
| Which pose | the **render** pose, the one the frame was actually drawn with — not a predicted display pose |
| Frame pairing | frame `N` is predicted from frame `N-1`; `R_rel = R_{N-1}^T R_N` |
| Position units | metres (unused by the rotation-only matrix; see section 4) |

**Head translation is not in this list because it is not in the matrix.** The
quaternion is the whole input. The pose log's `position_xyz` is read by nothing
on this path, by the design decision in section 4, and the parallax it would
have produced is the motion vector's job.

**The sidecar.** The encoder's pose log is `<sequence>.poses.json`, written by
`tools/quality/capture/gen_synthetic.py` and read by `nxv-enc --poses`:

```json
{
  "version": 2,
  "convention": { "id": "nxv-openxr-1", "quaternion": "xyzw", ... },
  "fov_deg":  { "h": 95.0, "v": 95.0 },
  "fov_rad":  { "left": -0.829, "right": 0.829, "up": 0.829, "down": -0.829 },
  "eye":      { "width": 1024, "height": 1024 },
  "fps": 90.0,
  "frames": [ { "frame": 0, "orientation_xyzw": [x, y, z, w], ... }, ... ]
}
```

`convention.id` and `fov_deg` are the two fields that exist for correctness
rather than for bookkeeping, and both were added by the audit:

- **`convention.id`** — a decoder of this file that does not recognise the id
  must **refuse** it rather than assume. `nxv-enc` does.
- **`fov_deg`** — `nxv-enc` used to assume 95x95 unconditionally, so a sequence
  generated at any other FOV was warped with the wrong `K` and nothing said so.
  Measured cost of that one assumption, on a 2-degree pair rendered at 110
  degrees: **18.70 dB against the 31.01 dB the correct FOV gives**
  (`docs/WARP-AUDIT.md` section 5). `--fov` on the command line overrides the
  sidecar; if neither is present the encoder still assumes 95x95 but now prints
  that it is doing so.

`version` 1 files carry neither field. They are still accepted, with the
assumption stated on stderr.

The conformance test for all of this is `ref.warp_convention`
(`tests/ref/test_warp_convention.cpp`), which measures the first warped frame
of a pure-rotation pair end to end against band-limited ground truth. It is
band-limited deliberately: see section 11 limitation 6.

---

## 3. Fixed-point formats

**This section deviates from paper 2.2, which specifies "nine int32 in Q8.24".
That format cannot work.** Two independent measurements agree:

- The translation entries `h02`, `h12` are displacements in pixels. Q8.24
  saturates at 128.0. A 300 deg/s turn at a 2048 px / 95 deg eye already moves
  the picture 70 px, and the worst case swept by `tests/warp` (4096 px eye, FOV
  tangent 1.4, 180 degree delta) reaches 511 px — **4x over int32 in Q8.24**.
  The stereo agent reached the same conclusion from the other direction
  (coefficients on the order of the focal length, ~940 px at a 2160 px eye,
  about 7 bits over).
- The perspective entries `h20`, `h21` are on the order of `theta / f`, about
  5e-5. In Q8.24 that is 838 counts, ~10 significant bits, and the resulting
  error in the denominator produces about 0.25 pel of coordinate error at the
  picture edge — 16x the 1/64 pel the corner divide is supposed to deliver.

A single Q format cannot hold both. The rows therefore carry different formats:

| Entries | Format | Range | Resolution |
|---|---|---|---|
| `h00 h01 h02` (row 0) | **Q10.21** | +-1024.0 | 2^-21 |
| `h10 h11 h12` (row 1) | **Q10.21** | +-1024.0 | 2^-21 |
| `h20 h21 h22` (row 2) | **Q2.29** | +-4.0 | 2^-29 |

`h22` is normalised to exactly `1.0 == 1 << 29` and is not free.

**Carriage (spec reconciliation, `spec/annex-d-inter-decisions.md` D-1).** The
nine coefficients travel in a `warp_ext()` structure of 36 bytes per eye
(`h00` first, `h22` last, each a little-endian `int32`), placed immediately
after the 40-byte frame header and gated by the frame header's `warp_present`
flag. `h22` is transmitted although it is fixed, and a decoder rejects any
value but `0x20000000`. The origin `(ox, oy)` is **not** transmitted: it is
`(W >> 1, H >> 1)`, which is why the API takes an origin and the bitstream does
not. The decoder-side rejection rules are the `kEntryMax` and `den` bounds of
this section, restated normatively in `spec/04` 4.4.3.

Entries within a row share a format because they are summed. The numerator and
the denominator do not, so the divide carries an explicit compensating shift:

```
kQNum     = 21
kQDen     = 29
kQCorner  = 6                                  // 1/64 pel
kDivShift = kQCorner + kQDen - kQNum  == 14
```

which is the only structural change from the paper's scheme, where the shift is
6 because both sides are Q24.

### Guaranteed range

`derive_homography()` **rejects** any matrix with an entry beyond
`kEntryMax = 2^30` — half the int32 range, i.e. a translation term of +-512 px.
The predictor is therefore never operated at the ragged edge of its format. At a
2160 px / 95 deg eye, 512 px of per-frame displacement is roughly **2200 deg/s**,
about seven times what paper 2.2 calls a fast turn. Measured over the sweep in
`tests/warp` (`range` suite), inside the accepted envelope:

| Entry class | Largest magnitude observed | Share of the `kEntryMax` budget |
|---|---|---|
| linear (`h00 h01 h10 h11`) | 2 263 747 | 0.2 % |
| translation (`h02 h12`) | 1 071 899 236 (511.1 px) | 99.8 % |
| perspective (`h20 h21`) | 447 245 | 0.042 % |

The denominator must additionally satisfy `kDenMin <= den < kDenMax`, i.e.
`2^28 <= den < 2^30` (0.5 to 2.0), at **every corner of every tile**.
`derive_homography()` checks the four picture corners, which bounds the whole
picture because `den` is affine in `(cx, cy)`. Observed span over the sweep:
`[268 706 816, 805 035 008]` against a legal `[268 435 456, 1 073 741 824)`.

The bound `den < 2^30` is what lets the restoring divide of section 5 use a
32-iteration loop with a `uint32` remainder (section 5).

Other constants:

| Name | Value | Meaning |
|---|---|---|
| `kQMv` | 2 | motion vectors are Q.2 (1/4 pel) |
| `kQSample` | 4 | sampling positions are Q.4 (1/16 pel) |
| `kCornerClamp` | `1 << 19` | corner coordinates saturate at +-8192 pel |
| `kCoordClamp` | `1 << 22` | sampling coordinate saturates after the MV add |
| `kTile` | 64 | tile side |

---

## 4. Deriving the matrix (encoder, informative)

Given the eye's orientation quaternions for the previous and current frame and
the eye's FOV (OpenXR `XrFovf`, half-angles in radians, left and down negative):

```
K(fov, W, H) maps a camera-space direction v = (x right, y up, -z forward)
to a homogeneous centred sample index (X, Y, Wh), with xc = X/Wh:

    tl = tan(angle_left)    tr = tan(angle_right)
    tu = tan(angle_up)      td = tan(angle_down)
    sx = W / (tr - tl)      sy = H / (tu - td)

        [ sx    0    sx*tl + 0.5 + ox ]
    K = [  0  -sy  -(sy*tu - 0.5 - oy) ]
        [  0    0          -1          ]
```

The `+0.5` is the pixel-centre convention and the `ox`/`oy` the centring of
section 2; folding both into `K` is why the decoder needs neither.

```
    R_rel = R_prev^T * R_cur                 // camera_cur -> camera_prev
    H     = K(fov_prev) * R_rel * K(fov_cur)^-1
```

`H` is then normalised so `h22 == 1` and quantised into the formats of section 3
with round-to-nearest. Per-eye FOV is allowed to differ between the two frames;
the common case `fov_prev == fov_cur` is not special-cased.

Only the rotation part of the pose is used. Translation-induced parallax is not
in this matrix by design: with a per-tile constant depth it collapses to a per-
tile 2D shift, which is exactly what the motion vector of section 8 carries
(paper 2.1).

The encoder derives `H` in double precision and then **uses the quantised matrix
itself** for its own prediction. The quantisation error lands in the residual and
never causes drift.

---

## 5. The restoring divide

The only division in the decoder.

```
uint32 nxvc_warp_div(U64 n, uint32 d)
```

Preconditions, which the caller guarantees: `d != 0`, `n.hi < d`, and `d < 2^30`.

```
rem = n.hi
q   = 0
for k = 31 down to 0:
    rem = (rem << 1) | ((n.lo >> k) & 1)
    ge  = (rem >= d) ? 1 : 0
    rem -= ge * d
    q   |= ge << k
return q
```

Exactly 32 iterations, always, with no data-dependent exit — the cost is fixed
and the control flow is uniform across a subgroup. The loop invariant `rem < d`
holds at the top of every iteration, so `(rem << 1) | 1 <= 2*d - 1 < 2^31` and
the shifted remainder never wraps; this is what the `d < 2^30` precondition buys.
The result is exactly `floor(n / d)`.

The reference implements the comparison as a branch and the shader as the
branchless `ge` form. The two are required to produce identical results and are
tested against the host divider over 500 000 random vectors plus the envelope
edges (`warp.divide`).

### Emulated 64-bit

`U64` is a `(lo, hi)` pair of `uint32`; in GLSL, a `uvec2` with `.x = lo`.

- `nxvc_umul_ext(a, b)` is the exact 64-bit unsigned product. The shader uses
  `umulExtended` (`OpUMulExtended`); the reference spells out the same
  schoolbook 16x16 decomposition rather than leaning on the host's 64-bit ALU,
  so both sides exercise the same logic.
- `nxvc_imul_ext(a, b)` is the signed product, computed as the unsigned product
  of the two's-complement patterns with the standard correction
  `hi -= (a<0 ? b : 0) + (b<0 ? a : 0)`.
- `nxvc_add64`, `nxvc_neg64`, `nxvc_shl64` (shift count 0..31, with `n == 0`
  handled explicitly so the `>> (32 - n)` is never a full-width shift),
  `nxvc_from_i32` (sign-extend).

All six are tested against native `uint64`/`int64` over 200 000 random vectors
(`warp.int64`).

---

## 6. Step 1 — the four tile corners

Computed **once per tile**, not per pixel: four corners, two divides each, eight
divides per 4096-pixel tile.

Corner `i` in `0..3` is the tile origin offset by `kTile` in x for `i & 1` and in
y for `i >> 1`. Note the corners are at `tile_x + 64`, one past the last sample:
this makes the interpolation of section 7 divide by a power of two.

For `kModeStatic` there is no arithmetic at all:

```
corner[i].x = (tile_x + ((i & 1) ? 64 : 0)) << 6      // Q.6
corner[i].y = (tile_y + ((i >> 1) ? 64 : 0)) << 6
```

For `kModeWarp`:

```
cx = tile_x + ((i & 1)  ? 64 : 0) - ox
cy = tile_y + ((i >> 1) ? 64 : 0) - oy

den = h20*cx + h21*cy + h22           // 64-bit accumulate, then narrowed
```

`den` is required to fit `int32` and to satisfy `kDenMin <= den < kDenMax`. If it
does not — a point behind the camera, or a matrix outside the validated envelope
— both corners saturate to `+kCornerClamp` and no divide is attempted.

Otherwise, for each of x and y, with `(h_a, h_b, h_c)` the matrix row and
`origin` the corresponding centring offset:

```
num  = h_a*cx + h_b*cy + h_c          // 64-bit, Q10.21
neg  = (num < 0)
mag  = |num|                          // 64-bit magnitude
mag  = mag << kDivShift               // << 14
mag  = mag + (den >> 1)               // round half away from zero
if mag.hi >= den:  v = neg ? -kCornerClamp : +kCornerClamp
else:              v = neg ? -nxvc_warp_div(mag, den) : +nxvc_warp_div(mag, den)
v = v + (origin << 6)
corner = clamp(v, -kCornerClamp, +kCornerClamp)
```

Rounding is applied to the magnitude, so it is round-half-away-from-zero and
symmetric about the origin. Working in magnitude also keeps the divide unsigned.

The result is Q.6 (1/64 pel) in absolute, uncentred reference-picture
coordinates.

Measured accuracy: over 20 000 random poses, the largest difference between the
integer corner and an exact double evaluation of the same quantised matrix is
**0.0078 pel**, inside the 1/64 = 0.0156 pel the format can represent
(`warp.corners`).

The `mag.hi >= den` branch cannot be taken by any matrix that passed
`derive_homography()`. It exists so that a corrupt or hostile frame header
produces a *defined, identical* result on every implementation rather than
undefined behaviour. Outside the validated envelope the predictor guarantees
determinism, not accuracy.

### Saturation is applied before the arithmetic that would overflow

The decoder receives `H` from a bitstream, not from `derive_homography()`, so
the out-of-envelope case is exactly the case it must survive. Three rules make
that survivable without changing any in-envelope result:

- The quotient is negated **through `uint32`** (`0u - q`), because `q` is an
  unrestricted quotient and `-(int32_t)q` is undefined when `q == 2^31`.
- The `origin` term is added with a **saturating** add, not a plain one. The
  plain add overflows before the clamp can see it, so the clamp cannot honour
  the contract. Saturating add is bit-identical whenever the plain add does not
  overflow.
- Every `<<` applied to a value that came off the wire goes through the
  unsigned pattern (`shl_i32_mod`), which is identical to `v << n` for every
  input that does not overflow and defined for the rest.

The same three rules apply in `kModeStatic`, which must saturate to
`kCornerClamp` exactly like `kModeWarp`: the interpolation of section 7 relies
on `|corner| <= 2^19` for its overflow argument, and the GLSL twin has to
compute the same value. Both were found by `warp_tile_fuzz`
(`fuzz/FINDINGS.md` F2 and F7) and are locked in by `warp.saturate`.

After the motion vector is added the coordinate is saturated again, to
`kCoordClamp`, so that the `+ 2` of the Q.6 -> Q.4 step cannot overflow for a
hostile vector. In the operational envelope the coordinate is at most
`kCornerClamp` plus a 64 px vector, about 2^20, so this never binds.

---

## 7. Step 2 — interpolating inside the tile

The source coordinate for an interior sample is bilinearly interpolated from the
four corners, in integers, with no divide. For sample `(u, v)` within the tile,
`0 <= u, v < 64`:

```
top = (X00*(64-u) + X10*u + 32) >> 6
bot = (X01*(64-u) + X11*u + 32) >> 6
X   = (top*(64-v) + bot*v   + 32) >> 6
```

and the same for Y. All shifts arithmetic, all adds `int32`.

**Two rounded steps, not one.** Multiplying out and taking a single `>> 12`
would need the corners clamped to +-4096 pel to keep the intermediate inside
`int32`, and a 4096-wide eye plus a few hundred pixels of displacement already
passes that: the envelope sweep found 170 000 saturated corners inside the
operational envelope with the single-step form. Splitting the interpolation caps
the intermediate at `2^25` and lets `kCornerClamp` be +-8192 pel, for one extra
shift and add per axis per pixel. The price is a second rounding: the
interpolation error against the exact bilinear value is at most one Q.6 step
(1/64 pel) instead of half a step.

### Interior approximation error

The homography is projective and this interpolation is bilinear, so there is a
residual approximation error in the tile interior that grows with the tile size
and with the angular rate. Paper 2.2 step 3 estimates "under 1/32 pel at 32 px"
and "under 1/16 pel below 250 deg/s at 64 px". Measured, at 2048^2 and 95 deg
FOV, worst case over 4000 random tiles per cell (`warp.interior`):

| deg/frame | deg/s at 90 Hz | 32x32 tile | 64x64 tile |
|---|---|---|---|
| 0.50 | 45 | 0.0257 | 0.0336 |
| 1.00 | 90 | 0.0262 | 0.0444 |
| 1.65 | 148 | 0.0311 | 0.0613 |
| 2.00 | 180 | 0.0331 | 0.0686 |
| 2.40 | 216 | 0.0353 | 0.0798 |
| 3.30 | **297** | 0.0412 | 0.1149 |
| 5.00 | 450 | 0.0544 | 0.1756 |
| 6.60 | 594 | 0.0780 | 0.2631 |
| 10.00 | 900 | 0.1427 | 0.5508 |

Values in pixels. Reading:

- **32x32 is comfortably inside 1/16 pel across the whole envelope** and inside
  1/32 pel up to about 180 deg/s. The paper's estimate is right.
- **64x64 crosses 1/16 pel at about 150 deg/s, not 250.** The paper is
  optimistic here by roughly 40 %. At the 297 deg/s reference rate a 64x64 tile
  needs a **1/8 pel** budget.

This is a direct argument for the paper's own position that 32x32 is the Full
profile default and 64x64 the Lite fallback (paper 2.2, 6.2), and it quantifies
the cost of the 64x64 choice: about 3x the interior coordinate error.

---

## 8. Step 3 — the motion vector

The per-tile vector is Q.2 (1/4 pel), range +-64 px (`+-256` counts), and is
added after the interpolation, promoted to Q.6:

```
X_q6 = interpolated_X + (mv_x << (kQCorner - kQMv))     // << 4
Y_q6 = interpolated_Y + (mv_y << (kQCorner - kQMv))
```

The MV is added identically in `kModeWarp` and `kModeStatic`. In `kModeStatic`
the interpolated coordinate is exactly the sample's own index (section 10), so
the predictor is a pure integer-or-quarter-pel shift of the reference — which is
the whole point of the mode for head-locked content.

The vector is a *displacement in the reference picture*, not a correction to the
matrix. Adding one pixel to `h02` and subtracting 4 from `mv_x` therefore produce
identical output; this is asserted in `warp.mv`.

---

## 9. Step 4 — sampling

The Q.6 coordinate is reduced to the 1/16-pel sampling grid:

```
X_q4 = (X_q6 + 2) >> (kQCorner - kQSample)      // (c + 2) >> 2, round half up
Y_q4 = (Y_q6 + 2) >> 2

ix = X_q4 >> 4      fx = X_q4 & 15
iy = Y_q4 >> 4      fy = Y_q4 & 15
```

Both shifts arithmetic, so `ix` is `floor` and `fx` is in `0..15` for negative
coordinates too. This is the `c += mv << 4; c = (c + 2) >> 2` of paper 2.10,
unchanged.

### Border policy

Clamp-to-edge, applied **per tap on the integer sample index**, after `ix`/`iy`
are formed and before the load:

```
fetch(x, y) = ref[ clamp(y, 0, H-1) ][ clamp(x, 0, W-1) ]
```

Never a clamp of the coordinate before interpolation: that would change the
fractional position and therefore the filter weights. The interpolation runs at
the true fractional offset and only the *taps* are clamped, so a tile that
straddles the picture edge is bit-identical to one that does not for the samples
that fall inside (`warp.border`).

**Filter selection (spec reconciliation, `spec/annex-d-inter-decisions.md`
D-5).** The filter is selected by the bitstream's tool bit 20
`FILTER_CATMULL_ROM`, not by `profile`, which is informative and selects
nothing. **Tool bit 20 is not defined for version 1 and a version 1 decoder
rejects a stream that sets it, so every conforming version 1 stream is
bilinear** — in every profile. The evidence is `docs/ERRATA.md`'s measurement
that Catmull-Rom is within 0.05 dB of bilinear on a single step and buys about
2 dB only on chains, against 16 taps per sample rather than 4. The table below
stays normative for the version 2 bit; the "Lite"/"Full" labels are historical.

### Bilinear (Lite profile, `kFilterBilinear`)

Four taps, four loads, weights over 16:

```
p = ( (16-fx)*(16-fy)*P00 + fx*(16-fy)*P10
    + (16-fx)*fy    *P01 + fx*fy    *P11 + 128 ) >> 8
```

Weights sum to 256; `+128 >> 8` is round-half-up. This is the four-load gather of
paper 3.2.3.

### Catmull-Rom (Full profile, `kFilterCatmullRom`)

Separable 4x4, sixteen taps, sixteen loads. The taps are the Catmull-Rom basis
(`a = -1/2`) scaled by 64 and rounded, then adjusted so **each row sums to
exactly 64** and the table is **symmetric** (`row[f]` reversed equals
`row[16-f]`). Largest tap error against the exact kernel: 0.0107, inside 1/64.

| `f` | tap -1 | tap 0 | tap +1 | tap +2 |
|---|---|---|---|---|
| 0 | 0 | 64 | 0 | 0 |
| 1 | -2 | 64 | 2 | 0 |
| 2 | -3 | 61 | 6 | 0 |
| 3 | -4 | 59 | 10 | -1 |
| 4 | -5 | 56 | 15 | -2 |
| 5 | -5 | 51 | 20 | -2 |
| 6 | -5 | 47 | 25 | -3 |
| 7 | -4 | 41 | 30 | -3 |
| 8 | -4 | 36 | 36 | -4 |
| 9 | -3 | 30 | 41 | -4 |
| 10 | -3 | 25 | 47 | -5 |
| 11 | -2 | 20 | 51 | -5 |
| 12 | -2 | 15 | 56 | -5 |
| 13 | -1 | 10 | 59 | -4 |
| 14 | 0 | 6 | 61 | -3 |
| 15 | 0 | 2 | 64 | -2 |

Row `f = 0` is the identity tap, so an integer sampling position reproduces the
reference sample exactly.

```
for j in 0..3:
    row_j = sum over i of  CR[fx][i] * fetch(ix-1+i, iy-1+j)
acc   = sum over j of  CR[fy][j] * row_j
p     = (acc + 2048) >> 12                       // 64*64 == 4096
```

The horizontal pass is **not** rounded or clamped: it is carried at full `int32`
precision into the vertical pass, and there is exactly one rounding and one clamp
at the end. Intermediate magnitude is bounded by the tap sums (largest positive
row sum 72, largest negative -8), so for 10-bit input `|acc| < 2^23` and `int32`
is comfortable.

### Output

```
out = clamp(p, 0, max_value)          // max_value = (1 << bit_depth) - 1
```

The residual add of paper 2.10 happens in the reconstruct stage, not here;
`warp_tile()` returns the predictor.

### Accuracy against the float oracle

`warp/ref/warp_oracle.cpp` evaluates the *exact* double homography per pixel with
the *exact* Catmull-Rom kernel in double precision. Measured over 3000 random
poses at the 297 deg/s reference rate with 64x64 tiles (`warp.oracle`):

| Quantity | Measured | Budget |
|---|---|---|
| Coordinate error vs the exact matrix | 0.1389 pel | 5/32 = 0.15625 pel |
| Coordinate error vs the quantised matrix | 0.1391 pel | — |
| Pixel error, band-limited picture, max | 1.58 LSB (8-bit) | 4 LSB |
| Pixel error, band-limited picture, rms | 0.355 LSB | 1 LSB |

The coordinate budget decomposes as the section 7 interior approximation
(<= 1/8 pel at this rate and tile size) plus half a step of the 1/16-pel
sampling grid (1/32 pel). The two error columns being equal to three decimals is
the useful result: **quantising the matrix contributes essentially nothing**, and
the entire error is the corner-then-interpolate approximation. Tightening the
formats would buy nothing; shrinking the tile would buy everything.

The pixel bound is stated on a *band-limited* picture on purpose. On white noise
a 1/32-pel coordinate difference is a full-scale sample difference and bounds
nothing at all.

---

## 10. Exact cases

Three cases must be exact, and are tested (`warp.identity`):

1. **Identity pose.** `R_prev == R_cur` and equal FOV give `h00 = h11 = 2^21`,
   `h22 = 2^29`, everything else 0. Then `num = 2^21 * cx`, `den = 2^29`, and
   `X_q6 = (2^21*cx << 14 + 2^28) / 2^29 = cx << 6`. The interpolation of a
   linear ramp with `top == bot` reproduces `(tile_x + u) << 6` exactly, the
   fraction is 0, tap row 0 is the identity, and the output is a **bit-exact copy
   of the reference**. Both filters, all tiles.

2. **`STATIC_MV`.** By construction the corners are the tile's own indices, so
   the same argument gives an exact copy shifted by the MV. The homography is
   not read at all: passing an arbitrary aggressive matrix must not change one
   bit of the output.

3. **Integer motion vectors.** `mv = (4*px, 4*py)` under an identity matrix is
   bit-identical to warping the tile at `(tile_x + px, tile_y + py)` with a zero
   vector.

---

## 11. Conformance and known limitations

### Conformance

| Test | What it fixes |
|---|---|
| `warp.int64` | emulated `(hi,lo)` == native 64-bit, 200 k vectors |
| `warp.divide` | 32-step restoring divide == exact floor, 500 k vectors + edges |
| `warp.identity` | the three exact cases of section 10 |
| `warp.border` | clamp-to-edge on all four sides; no Catmull-Rom overshoot on flat input |
| `warp.mv` | MV semantics and MV/matrix commutation |
| `warp.corners` | corner divide within 1/64 pel of the exact quantised matrix |
| `warp.interior` | the section 7 table |
| `warp.oracle` | the section 9 table |
| `warp.range` | the section 3 envelope; no `int32` overflow, no saturation inside the envelope |
| `warp.saturate` | corners stay inside `kCornerClamp` for arbitrary `int32` input, both modes (F2/F7 regression) |
| `warp.filters` | tap table normalised to 64, symmetric, within 1/64 of the exact kernel |
| `warp.determinism` | one identical output hash from every compiler and optimisation level |
| `ref.warp_convention` | the section 2.1 conventions, end to end: first-frame warp prediction PSNR on a pure-rotation pair against band-limited ground truth |
| `warp.gpu_diff` | zero mismatching pixels, GPU against CPU, on every installed ICD |

`warp.determinism` compiles the same corpus at `-O0`, `-O1`, `-O2`, `-O3`, `-Os`,
`-O2 -ffast-math` and `-O3 -march=native`, under every C++ compiler present, and
requires one hash from all of them. `-ffast-math` is in the list deliberately:
there is no floating point on the normative path, so it must be inert, and if it
ever moves a bit that is the bug the test exists to catch. The corpus itself is
generated without any floating point — the nine matrix entries are synthesised
directly inside their fixed-point formats — so a hash mismatch can only come from
the integer path and never from libm.

`warp.gpu_diff` returns exit code 77 (ctest skip) when no Vulkan ICD is present.

The module is additionally covered by `fuzz/warp_tile_fuzz`, whose checked-in
reproducers replay on every build through `warp_tile_fuzz_replay`. The
normative path is UBSan-clean under `-fsanitize=undefined,integer` on hostile
input, which for integer-only code is most of what there is to prove.

### Limitations

1. **`STEREO` disparity does not fit the shared MV field.** The stereo agent
   measures 37.6 % of stereo tiles with disparity beyond the +-16 px coarse
   search range and 2.7 % beyond the +-64 px range of the Q.2 motion vector.
   `STEREO` reuses this module's `kModeStatic` path and therefore inherits the
   +-64 px limit of the `mv_qpel` argument. Not implemented here.

   *Resolved in the bitstream (spec reconciliation,
   `spec/annex-d-inter-decisions.md` D-4):* the disparity is a **separate**
   16-bit unsigned field, 12 bits used, reaching 1023.75 samples, carried in the
   tile's optional area in place of `mv_x`/`mv_y`. It is not the Q.2 signed
   vector and it does not share its range. There is no vertical component. When
   `STEREO` is implemented, this module's `kModeStatic` path takes the disparity
   through a widened argument; the +-64 px limit above applies only to the
   temporal vector.

2. **Paper 3.2.3 has Pass A transmit four corner displacements per tile in Q4
   `int16`, while paper 2.10 has Pass B derive the corners from the frame
   header's matrix.** This module implements the latter: `warp_tile()` takes the
   matrix.

   *Resolved (spec reconciliation, `spec/annex-d-inter-decisions.md` D-7):*
   **derived, and no corner record is transmitted.** Q4 `int16` cannot hold what
   section 6 produces (Q.6 up to +-8192 pel needs 20 bits), and widening it to
   Q6 `int32` would put 32 bytes per tile — 74 kB per stereo frame — in the
   bitstream to save eight divides per tile amortised over 4096 pixels. A
   decoder MAY cache the derived corners; that is an implementation matter and
   is not observable in the bitstream.

3. **No foveation.** Paper 6.8 puts the predictor at the coded resolution against
   a full-resolution reference, and Phase 2 runs unfoveated. Nothing here is
   aware of a foveation map.

4. **64x64 only.** `kTile` is 64 and the GPU kernel's 256-thread, 16-pixels-per-
   invocation shape assumes it. Section 7 shows 32x32 would be materially more
   accurate; supporting it is a parameterisation this module has not done.

5. **The `mag.hi >= den` and illegal-`den` paths are defined but not
   characterised.** They guarantee identical output across implementations, not a
   useful prediction. An encoder must never emit a matrix that reaches them.

6. **No absolute PSNR figure measured on `tools/quality/capture` material
   bounds this predictor.** That generator point-samples its equirectangular
   panorama once per output sample, at about 2.1x oversampling. The ground
   truth it produces is therefore aliased, and the aliasing is *not* a
   geometric function of the pose, so no warp — integer, float or exact — can
   predict it. Measured on the first warped frame of `vr-mixed-1024`, an
   0.063-degree rotation: the exact float homography reaches **24.43 dB** and
   this module's integer path reaches **24.40 dB**, a gap of 0.02 dB. The
   predictor is at the material's ceiling, and the ceiling is the harness.
   On band-limited ground truth the same path measures **50 to 58 dB** at 22
   to 450 deg/s (`ref.warp_convention`). `docs/WARP-AUDIT.md` decomposes the
   difference. Any quality gate stated on this predictor must say which of
   those two materials it is stated on.

---

## 12. GPU kernel notes

`warp/glsl/warp_tile.comp` is the Pass B shape of paper 3.2.1: **one workgroup
per 64x64 tile, 256 invocations, 16 output pixels each**.

Portability rules (paper 3.2.6) as applied here:

- **No subgroup operations at all.** The only cross-invocation exchange is the
  four corner coordinates, which invocations 0..3 write to `shared` and everyone
  reads after one `barrier()`. Nothing assumes the workgroup is one subgroup and
  nothing assumes a subgroup width.
- **No `shaderInt64`.** 64-bit intermediates are `uvec2` pairs built with
  `umulExtended`, which is core GLSL 4.00 and lowers to `OpUMulExtended`.
- **No float.** The kernel contains no floating-point type.
- **No sampler.** The reference is an SSBO of packed samples read with explicit
  integer indices and `clamp`.
- **One SPIR-V binary for every vendor.** No `#ifdef`, no vendor paths, no
  specialisation constants on the normative path.
- Signed right shift is `OpShiftRightArithmetic`, matching the C++ reference.

The reference picture is bound as an SSBO in this module because that keeps the
harness minimal; paper 3.2.3 specifies a storage image in the real decoder. The
sampling arithmetic is identical either way — the binding does not touch the
normative path.

The divide loop is branchless (`ge` multiply-subtract) so its control flow is
uniform; the reference writes the same loop as a branch for readability and the
two are required to agree.

### Verified

`nxvc-warpdiff` ran 10 000 random tiles (40 960 000 pixels) per device against
the CPU reference, on random references and random matrices spanning the full
envelope, both filters, both modes:

| ICD | Device | Mismatching pixels |
|---|---|---|
| RADV (Mesa 26.2.1) | AMD Radeon RX 7900 XTX (Navi 31) | **0** |
| RADV (Mesa 26.2.1) | AMD Ryzen 9 9950X3D iGPU (Raphael) | **0** |
| lavapipe (Mesa 25.2.4) | llvmpipe, LLVM 21 | **0** |
| SwiftShader 5.0.0 | Subzero | **0** |

Three independent driver stacks, two of them pure software and one of them not
Mesa at all. Nothing about this is a claim about Adreno, which is the target and
is not represented on this machine.
