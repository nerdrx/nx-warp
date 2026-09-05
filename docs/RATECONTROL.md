# Rate control, tile classification, the degradation ladder and foveation

This document specifies what `rc/` (`nxvc_rc`) and `fov/` (`nxvc_fov`) actually
do. It is the implementation counterpart of PAPER.md sections 1.5, 4.6, 4.6.1,
4.7, 5.1 and 5.2. Where the implementation departs from the paper, the
departure is stated here and argued in appendix A; measured numbers from the
simulator are in `rc/RESULTS.md`.

Everything below is per tile. A tile is 64x64 luma samples of the lens-space
image, tile id `= ty * tiles_x + tx`, and every input and output is a flat
array of that length. There is no per-pixel work anywhere in the library
except `compute_tile_stats()`, which is the CPU model of a dispatch the
encoder already runs.

---

## 1. Where the pieces live

| | |
|---|---|
| `fov/include/nxfov/foveation.hpp` | lens model, foveation map |
| `rc/include/nxrc/nxrc.hpp` | the whole rate-control API |
| `rc/include/nxrc/synth.hpp` | synthetic tiles for the tests and the simulator |
| `rc/src/classify.cpp` | tile statistics and the four-class classifier |
| `rc/src/allocate.cpp` | the ladder table, the allocator, the bit model |
| `rc/src/governor.cpp` | the decode-time governor |
| `rc/include/nxrc/tvm.hpp` | the peripheral temporal visibility model (section 8.2) |
| `rc/include/nxrc/refresh.hpp` | the per-tile refresh scheduler (section 8) |
| `rc/src/tvm.cpp` | Tursun-Didyk, reduced onto the per-tile statistics |
| `rc/src/refresh.cpp` | the gate search and the temporal invariants |
| `rc/sim/` | `nxvc-rcsim`, writes `rc/RESULTS.md` and `rc/ladder-map.svg`; `--temporal` writes `rc/RESULTS-temporal.md` and `rc/refresh-map.svg` |
| `tests/rc/` | ctest targets `rc.classify`, `rc.foveation`, `rc.allocate`, `rc.model`, `rc.governor`, `rc.tvm`, `rc.refresh` |

**Why `fov/` is its own library.** The foveation map has three consumers
(PAPER.md 5.1.1): the render pass via `VK_KHR_fragment_shading_rate`, the
encoder, and the client's defoveation shader. Only one of those is the
encoder. `nxvc_fov` therefore has no dependency on `nxvc_rc`, links into the
client as well as the server, and can be handed to the OpenXR foveation
extension without dragging the rate controller along. `nxvc_rc` depends on
`nxvc_fov`, not the other way round.

## 2. The pipeline for one frame

```
   encoder analysis dispatch            (one workgroup per tile)
        |  TileStats: mean luma, log-variance, Jxx, Jxy, Jyy
        v
   classify_tiles()                     (one thread per tile)
        |  class[]: flat | texture | edge | text
        v
   foveation_map(lens, gaze)            (one thread per tile)
        |  level[], ecc_deg[], R8 map
        v
   refresh scheduler (section 8)        (one thread per tile + a gate search)
        |  force_warp_skip[]: this frame's WARP_SKIP set
        v
   allocate(B, fov, class, complexity, motion, force_warp_skip)
        |  per tile: qp, res_level, chroma_mode, wm_id, ladder_step,
        |            dc_plane, skip
        v
   encode dispatch, packetize
        |
        v
   update_model(actual bits per tile)   -> a_t for the next frame
   governor(decode_us, frame_period)    -> knob state for the next frame
```

The two feedback loops run on different timescales and different variables
(PAPER.md 4.7): the bit model updates every frame, the governor moves at
3 frames down / 180 frames up.

---

## 3. Tile classification

### 3.1 The statistics

The encoder's analysis pass produces five numbers per tile. `compute_tile_stats()`
is the normative CPU model:

```
mean  = (1/N) sum I
var   = (1/N) sum I^2 - mean^2
log_var = log2(var + 1)
gx    = (I[x+1,y] - I[x-1,y]) / 2        (clamped at the tile edge)
gy    = (I[x,y+1] - I[x,y-1]) / 2
Jxx   = sum gx*gx   Jxy = sum gx*gy   Jyy = sum gy*gy
```

Partial tiles at the right and bottom edge are summed over the pixels that
exist and the tensor is scaled to a full 64x64 tile so the thresholds below do
not depend on the tile being complete.

Three derived quantities:

```
coherence C = sqrt((Jxx - Jyy)^2 + 4 Jxy^2) / (Jxx + Jyy)        in [0, 1]
gradient  G = (Jxx + Jyy) / 4096                  mean squared gradient
frequency R = G / var             normalised mean squared spatial frequency
```

`C` is the structure-tensor eigenvalue ratio the paper asks for: 1 when every
gradient in the tile points the same way, 0 when they are isotropic.
`R` is the interesting one. By Parseval it is the second moment of the tile's
power spectrum normalised by its energy, so it measures *how fine* the
structure is independently of *how strong* it is. For white noise it is
exactly 1.0 whatever the amplitude, which is what makes it a usable guard.

### 3.2 What the four classes actually measure

These are measured, not assumed. Statistics of 64x64 synthetic tiles
(`nxrc::synth`, reproduced by `rc.classify`):

| tile | mean | log_var | C | G | R |
|---|---|---|---|---|---|
| constant | 128 | 0.00 | 0.000 | 0.0 | 0.000 |
| smooth ramp, 40..90 | 65 | 6.76 | 0.984 | 0.4 | 0.004 |
| smooth ramp, 0..255 | 128 | 11.45 | 0.992 | 8.0 | 0.003 |
| step edge, 0 deg | 125 | 13.14 | 1.000 | 282 | 0.031 |
| step edge, 20 deg | 125 | 13.14 | 0.707 | 379 | 0.042 |
| step edge, low contrast | 130 | 8.65 | 0.707 | 17 | 0.042 |
| band-limited noise (3x3) | 128 | 7.16 | 0.077 | 95 | 0.674 |
| band-limited noise (5x5) | 128 | 5.69 | 0.066 | 21 | 0.405 |
| white noise, amp 60 | 128 | 10.22 | 0.004 | 1156 | 0.973 |
| white noise, amp 120 | 129 | 12.23 | 0.033 | 4825 | 1.008 |
| 5x7 glyph field | 179 | 13.12 | 0.033 | 7905 | 0.886 |
| glyphs at 2x | 186 | 12.98 | 0.015 | 6207 | 0.766 |
| glyphs at 3x | 191 | 12.89 | 0.103 | 3775 | 0.498 |
| stripes, period 4 | 128 | 13.50 | 1.000 | 5417 | 0.469 |
| stripes, period 16 | 128 | 13.50 | 1.000 | 1083 | 0.094 |

Two things fall out of this table, and both change the design the paper
sketched.

**A glyph field has coherence 0.03, not high coherence.** PAPER.md 4.6.1 says
text is "UI stencil or high coherence plus high contrast". The first half is
right and the second half is backwards: real text has strokes running in every
direction, so its structure tensor is nearly isotropic. High coherence is what
a *single boundary* looks like. The coherence axis is therefore used for its
real job, separating edge from texture, and text gets a different test.

**A smooth ramp has coherence 0.98 and variance 2800.** Any classifier that
looks at variance or coherence before gradient energy calls a sky gradient an
edge or a texture and then spends bits protecting it. The gradient test has to
come first.

### 3.3 The classifier

Order is normative:

```
1.  ui_stencil                                          -> TEXT
2.  G < 12  or  log_var < 3.0                           -> FLAT
3.  G >= 2500  and  log_var >= 11.0  and  R <= 0.93     -> TEXT
4.  C >= 0.45                                           -> EDGE
5.                                                      -> TEXTURE
```

- **Step 1** is the reliable route and the one PAPER.md 5.2 names: the
  compositor knows which tiles are quad-layer UI. Everything else is a
  fallback for text baked into the rendered image.
- **Step 2** catches the sky. `G < 12` means an rms gradient under 3.5 codes.
- **Step 3** is "hard, full-contrast micro-structure". `R <= 0.93` is the
  guard against white noise, which is pinned at `R = 1.0` by construction
  while glyph fields measure 0.50 to 0.89. The margin is 5% either way.
- **Step 4** is one dominant gradient orientation over the tile.

Known failure mode, stated plainly: near-full-amplitude per-pixel white noise
(sigma above about 60 codes) satisfies the gradient and variance halves of
step 3 and is separated from text only by `R`, on a 5% margin. Rendered VR
content does not contain that - real texture is band limited and sits at
`R = 0.25` to `0.67` - but film grain overlays or a deliberately dithered
gradient could get close. The consequence of the error is that a noise tile is
protected as if it were text: more bits than it deserves, never a worse
picture. It is the safe direction, and the stencil route makes it irrelevant
for actual UI.

The third statistic that would settle it - a count of distinct luma levels, or
the gradient magnitude histogram's kurtosis - is not in the agreed analysis
output. If the encoder ever adds one, step 3 should use it.

### 3.4 Hysteresis

A tile that is already in a class keeps it until it crosses the far side of a
band: gradient thresholds move by 15% relative, log-variance by 0.5, coherence
by 0.06. Without it a periphery tile sitting on a threshold flips between
texture and edge every frame, which changes its weighting matrix and its
`res_level` every frame, and shimmers. `ClassifyConfig::hysteresis` turns it
off; passing an empty `prev` span has the same effect for that call.

---

## 4. Rate control

### 4.1 Inputs

| symbol | meaning |
|---|---|
| `B` | frame bit budget: transport target bit/s / fps, minus FEC and header overhead |
| `fov` | the foveation map: per-tile `level` in {0,1,2} and `ecc_deg` |
| `class` | from `classify_tiles` |
| `cplx` | warped SAD per tile, mean absolute difference per pixel |
| `slip` | per-tile residual motion after the pose warp, deg/s |
| `head_speed` | head angular rate, deg/s |
| `intra_ratio` | fraction of tiles above the intra threshold (scene-cut detector) |

### 4.2 The bit model

```
predicted_bits(t) = a_t * s_fov(t)^2 * gain(t) * 2^(-QP_t / 6)
```

`a_t` is per-tile state normalised to a full-resolution tile, so changing a
tile's `res_level` does not invalidate its model. `s_fov` is the foveation
scale, 1, 1/2 or 1/4. `gain` is the bit-domain effect of the ladder step
(section 4.4). "Bits halve per 6 QP" is exactly the `2^(-QP/6)` factor.

After each frame:

```
a_t <- clamp(a_t * (actual_t / predicted_t)^0.6,  a_t/4, a_t*4)  clamped to [512, 2^21]
```

`a_init = 32768`, which is 0.25 bpp at QP 30 on 4096 samples. From a 3x
mismatch the per-tile error halves by frame 3, is under 5% by frame 10, and
the recovered `a_t` is within 0.2 octaves of ground truth by frame 60
(`rc.model`). PAPER.md's "converges in 2 to 3 frames" holds for the frame
total; the per-tile model takes about ten.

### 4.3 Weights

The paper writes both a bit-domain weight (`fov_t`, `percep_t`) in 4.6 and a
set of QP offsets (`dQ_ecc`, `dQ_motion`, ...) in 5.2 for the same effects.
Applying both would count every perceptual term twice. The implementation
keeps exactly one control per effect and expresses all of them in the bit
domain, where a QP offset is a factor of `2^(-dQ/6)`:

```
dQ_ecc    = 0 (level 0, e' <= 8 deg) | +2 (level 0, beyond) | +4 (level 1) | +6 (level 2)
dQ_motion = 0 below 10 deg/s slip, +2 at 30, +4 at 60, +6 above 100, linear between
            + 2 globally while head speed > 120 deg/s
dQ_lum    = -2 if mean < 16, +2 if mean > 220, else 0
dQ_act    = clamp(+1.0 * (log_var - mean log_var), -4, +4),  0 for text tiles
dQ_class  = 0 flat | 0 texture | -2 edge | -4 text

cplx_t    = clamp(cplx_t / mean cplx, 0.25, 4),  and at least 1.0 for text tiles
head_fac  = 1 / (1 + 0.5 * clamp(head_speed / 120, 0, 4))

w_t       = s_fov(t)^2 * 2^(-(dQ_ecc + dQ_motion + dQ_lum + dQ_act + dQ_class)/6)
            * head_fac * cplx_t

bits_t    = B * w_t / sum(w)
QP_t      = floor(6 * log2(a_t * s_fov^2 * gain_t / bits_t) + dither_t)
```

`s_fov^2` is in the weight because a level-2 tile codes 1/16 of the samples
and genuinely needs fewer bits; `dQ_ecc` on top of it is the perceptual part,
the contrast the periphery cannot see. The paper's "`fov_t` from 1.0 down to
0.15" conflates the two; splitting them is what makes the QP come out
comparable across levels instead of exploding in the periphery.

Two exemptions for text are deliberate and are not in the paper. `dQ_act` is
contrast masking - a busy tile hides a coarse step - and glyphs are precisely
the high-variance content where masking does not hold; without the exemption a
text tile's own sharpness takes bits away from it. The `cplx_t` floor stops a
head-locked HUD from being starved in the frames where its pose-warped
predictor happens to be perfect.

### 4.4 The degradation ladder

Per-class ladders. `res` is an **absolute floor**: the coded level is
`max(foveation level, res, governor floor)`, so a step never sharpens a
periphery tile. `wm` is the weighting matrix of PAPER.md 1.5.

| step | flat / texture | edge | text |
|---|---|---|---|
| 0 | luma matrix | luma matrix | flat matrix |
| 1 | **low-pass matrix** | QP +2 | untouched |
| 2 | **res 1/2** | QP +2 | untouched |
| 3 | **res 1/4**, QP +4 | low-pass matrix, QP +4 | untouched |
| 4 | **DC plane**, QP +4 | res 1/2, low-pass, QP +4 | QP +4 |

Bit-domain gain of a step, relative to the tile's foveation resolution:

```
gain = (s_step / s_fov)^2  *  0.72 if low-pass matrix  *  0.12 if DC plane
```

and the **severity** of a step, which is the only scalar in which steps are
comparable across classes:

```
severity = -6 * log2(gain) + qp_add        (QP-equivalent, 0 = untouched)
```

Comparing raw step indices is meaningless: step 2 is "res 1/2" for a texture
tile and "untouched" for a text tile. Severity at every step and every
foveation level satisfies

```
severity(text) <= severity(edge) <= severity(texture) <= severity(flat)
```

which is the paper's requirement, checked exhaustively by `rc.allocate`.

**Crucially, a ladder step does not reduce a tile's bit share.** `w_t` uses
the foveation resolution only. A tile that steps down keeps its bits and
spends them on fewer, lower-frequency coefficients, so its coded QP falls a
long way and the result is clean and soft rather than blocky. That is the
whole point of 4.6.1: "blur, never block" is a statement about *where the bits
go*, not about spending fewer of them. Bits are freed only when a stepped tile
runs into its class QP floor and cannot use its share - and then they are
redistributed to the fovea, which is the right place for them.

### 4.5 Per-class QP bounds

PAPER.md 4.6.1 calls these "per-class QP floors". They are quality floors,
i.e. ceilings on the coded QP, so the code names them `qp_ceiling`; there are
also genuine floors to stop overspending.

| class | QP floor | QP ceiling |
|---|---|---|
| flat | 8 | 38 |
| texture | 8 | 38 |
| edge | 4 | 32 |
| text | 0 | 26 |

The ceiling is the "never block" bound: above it an 8x8 DCT at that step size
starts to show its grid. A tile that would need a higher QP is clamped there
and the ladder is asked to make up the difference. Text can reach QP 0, which
is lossless with transform skip (PAPER.md 1.5) - that is where the surplus
goes above 400 Mbit/s.

### 4.6 The pressure search

Ladder pressure `P` is a single frame-level scalar in [0, 4].

```
step_t = min( floor(P) + [ecc_t >= (1 - frac(P)) * max ecc], 4 )
```

so the integer part applies to every tile and the fractional part engages the
outer tiles first. It is one broadcast scalar plus a per-tile compare, which
is why it maps onto the GPU: no sorting, no per-tile state.

The predicate is the **ceiling deficit**: the bits that tiles pinned at their
class QP ceiling overspend relative to what they were allocated. Zero means no
tile has to be coded above the QP at which its class starts to block. This is
deliberately not "does the frame fit": a frame can be exactly on budget while
individual periphery tiles are mosaics, and the ladder exists for those tiles.

Some ceiling clamps cannot be relieved at all, because the text ladder barely
moves by design. The target is therefore not zero but

```
target = deficit(P=4) + 0.15 * (deficit(P=0) - deficit(P=4)) + 1
```

and `P` is the smallest value on a grid of 16 quarter-steps meeting it. Two
extra evaluations bracket the range; the whole search is 19 passes over the
tile array, each a sum reduction. Chasing the last few bits instead would slam
the frame to step 4 over one unfixable tile, and step 4 is cheap enough that
whole tiles then fall under the one-header floor and drop out entirely.

`P` may rise immediately - the budget is a hard constraint - but falls at most
0.25 per frame, so the periphery does not flap between two ladder steps. Same
asymmetry, and the same reason, as the decode governor.

### 4.7 Clamp, redistribute and the rounding problem

Within one pressure evaluation the allocator iterates six times:

1. split `B` over the tiles by `w_t`;
2. compute `QP_t`, clamp it into `[qp_floor, qp_ceiling]` and mark clamped
   tiles as pinned;
3. sum what the pinned tiles cost, and rescale the unpinned tiles' targets by
   `(B - pinned bits) / (unpinned predicted bits)`.

The rescale in step 3 uses *predicted* bits, not weights, because it has to
close the integer-QP rounding gap as well as hand on the pinned tiles' bits.

That gap is not small. Tiles that share a class and a foveation level share a
bit target, so plain `round()` sends a whole group the same way and the bias
does not average out: measured 4.0% of the frame, in one direction, every
frame, on homogeneous content. The fix is to dither the rounding threshold
with a per-tile van der Corput (bit-reversed index) value:

```
QP_t = floor(6*log2(a_eff / bits_t) + vdc(t))
```

The group's *average* cost then hits its target exactly, at the price of
neighbouring tiles differing by one QP - below the visible threshold, and what
adaptive quantisation does anyway. It is deterministic, so the encoder and the
model agree, and it is a bit-reverse plus a float multiply on the GPU. With
dithering the frame error on the same homogeneous content drops from -4.00% to
-0.01%.

### 4.8 Skip and the one-header floor

Two separate reasons a tile is not coded:

- `cplx_t < 1.0` (mean absolute difference per pixel): the pose warp is good
  enough, the tile is `SKIP_WARP` and costs one bit in the row bitmap.
- allocated fewer than 96 bits: below a tile header plus a rANS flush there is
  nothing to send. Text tiles are exempt.

The second is a real constraint at the bottom of the range. At 20 Mbit/s and
72 Hz the whole stereo frame is 278 kbit over 2312 tiles: 120 bits per tile
average, of which 64 is header. The allocator therefore drops most of the
periphery to skip and spends on what is left - 77% of tiles skipped at
20 Mbit/s in the simulator - which is the mechanism behind PAPER.md 4.6's
"below about 40 Mbit/s ... static content goes to skip tiles". The refresh of
those tiles is the rolling refresh's job, not the allocator's.

The whole allocation runs three times, so that the bits of every dropped tile
are handed back to the survivors before the result is committed.

**Open issue, and the most surprising number in `rc/RESULTS.md`.** With the
paper's `dQ_ecc = +6` for level-2 tiles on top of their `s^2 = 1/16`, a far
periphery tile's weight is 0.031 of a fovea tile's. At 150 Mbit/s that works
out to about 70 bits, under the 96-bit floor, so **the entire level-2 ring is
skip-dominated even at 150 Mbit/s** - the ladder-step maps at 150 and 300
Mbit/s show a blank border. For genuinely static far-field content that is
correct and free, because the pose warp is exact for the far field. For a
periphery with real residual it means visible staleness between rolling
refreshes.

The lever is `dq_ecc_quarter`, not `min_tile_bits`: a coded tile costs 64 bits
of header whatever else happens, so requiring 32 bits of payload on top is not
the aggressive part. Whether +6 or +4 is right is a subjective-testing
question that this library cannot answer; both are one line in `RateConfig`.
Flagged rather than silently retuned.

### 4.9 Scene cuts and overrun

`intra_ratio > 0.5` (or an explicit flag) marks a cut. The allocator gets
`1.5 * B` for that frame and the excess becomes debt, repaid at `debt/30` per
frame over the next 30 frames. Over the whole episode the average is exactly
the nominal budget (checked by `rc.allocate`). The fovea recovers first
because the weights already favour it, not through a separate mechanism.

Frame overrun uses the same machinery: if the measured total exceeds `1.15 B`,
the excess is added to the debt and comes off the next frame.

### 4.10 Outputs

Per tile: `qp`, `res_level`, `chroma_mode`, `wm_id`, `ladder_step`, `dc_plane`,
`skip`, `predicted_bits`, `weight`. Per frame: `pressure`, `budget_bits`,
`predicted_total`, `scene_cut`, `cg_qp_offset` (+2, PAPER.md 5.2), and counts
of tiles pinned at a ceiling or a floor.

Chroma follows the coded resolution: 4:4:4 at level 0, 4:2:0 at level 1,
4:1:0-like at level 2, and text is always 4:4:4 whatever its level, because
4:2:0 is what breaks glyph fringes.

---

## 5. The decode-time governor

Target is 40% of the frame period (PAPER.md 4.7): 4.44 ms at 90 Hz, 5.56 ms at
72 Hz.

```
step down   if 3 consecutive frames > 110% of target, or any one frame > 150%
step up     after 180 consecutive frames < 70% with no deadline miss
```

A deadline miss cancels progress towards a step up. Any change resets both
counters. Frames in the dead band reset the down counter only.

### 5.1 The five knobs

| level | knob | single-layer effect |
|---|---|---|
| 1 | drop enhancement layers on class C | class C `res_level` floor 2 |
| 2 | class C to the half-resolution base | class C forced to DC plane |
| 3 | shrink the fovea radius by 10% | `FoveationConfig::region_scale = 0.9` |
| 4 | drop enhancement layers on class B | class B `res_level` floor 2 |
| 5 | 90 to 72 Hz | `refresh_hz = 72`, a control-channel call |

Class A / B / C are the foveation levels 0 / 1 / 2.

### 5.2 Coupling to the allocator

`RateController::set_knobs()` takes the state. A knob's `res_level` floor is
applied **in both the weight and the cost model**: the tile's share of the
budget drops by the sample ratio and so does its predicted cost, so its QP is
unchanged. The knob buys decode time at constant quality-per-sample instead of
just moving the QP around. Getting this wrong - scaling the share but not the
model - makes periphery tiles clamp at their ceiling, drives the ladder
pressure up, and degrades the fovea in response to a knob that was supposed to
touch only the periphery. `rc.allocate` tests for exactly that.

Knob 3 changes the foveation map itself, so the caller regenerates it with the
scaled `region_scale` and passes the new map on the next frame.

### 5.3 Honest note on the single-layer mapping

A v1 stream with one native layer has no enhancement layer to drop, so knobs
1, 2 and 4 are mapped onto the tools that do reduce coded samples. When
multi-layer encoding lands, knobs 1 and 4 should become literal layer drops
and this mapping should be revisited.

---

## 6. The foveation map

### 6.1 Lens model

Per-eye projection in OpenXR `XrFovf` convention: four half-angle tangents,
left and down negative. The render target is rectilinear, so pixels per unit
tangent is constant:

```
k_x = width / (tan_right - tan_left)          k_y = height / (tan_up - tan_down)
ppd_center = min(k_x, k_y) * pi/180
ppd_render(theta) = ppd_center / cos^2(theta)
```

The `cos^2` is exact, not an approximation: `d(tan theta)/d theta = 1/cos^2`.
`theta` is the full off-axis angle of the tile centre.

### 6.2 Density and the ladder

```
ppd_needed(e) = 60 / (1 + e / 2.3)
s_raw = 1.5 * ppd_needed(e') / ppd_render(theta)

s = 1    if s_raw >= 0.55
    1/2  if s_raw >= 0.17
    1/4  otherwise
```

The two thresholds are chosen to reproduce PAPER.md 5.1.2's table: 1/2 engages
at about 14 degrees and 1/4 at about 35 degrees on the Pico 4 lens. They are
more aggressive than plain rounding on the ladder would give, which is what
the paper's table asks for.

The map is written as R8, one texel per tile: 255, 128, 64 for s = 1, 1/2,
1/4. `FoveationMap` also carries `level`, `ecc_deg` (after the eye box or
gaze pad), `ecc_raw`, `theta_deg`, `ratio` and `weight`, because the encoder
needs the eccentricity for `dQ_ecc` and the client needs the level.

### 6.3 Fixed foveation, Pico 4

Without a tracker the map is centred on the lens axis and an elliptical eye
box of 20 x 15 degrees is treated as fovea (PAPER.md 5.1.3):

```
q = sqrt((ex/20)^2 + (ey/15)^2)
e' = 0                if q <= 1
     e * (1 - 1/q)    otherwise
```

which is the angular distance past the ellipse along the ray, and is smooth
across the boundary.

`pico4_eye()` uses tangents of +/-0.8568 (+/-40.6 degrees) over 2160 px, which
gives `ppd_center = 22.0` and a corner eccentricity of 50 degrees - the
numbers PAPER.md 5.1.2's table is computed with. That is the render FOV WiVRn
asks for at 1.0x scale, not the full lens FOV; `pico4_eye_wide()` gives the
+/-50 degree variant at `ppd_center = 15.8`. Real per-eye tangents should come
from `xrLocateViews` and be passed in; the presets exist so the tests and the
simulator have something fixed to check against.

Result on the default: 30% of tiles at s=1, 57% at 1/2, 13% at 1/4, for 0.45
of the full-resolution samples. PAPER.md 5.1.3 predicts 0.50 for the same
configuration.

### 6.4 Eye-tracked foveation

With a gaze point, `e' = max(0, e - pad)` and

```
pad = 0.05 deg/ms * gaze_to_photon_ms + 1.0 deg        (PAPER.md 5.1.4)
```

so 40 ms of measured latency gives 3 degrees of pad on top of the 5 degree
fovea. `region_scale` scales the pad and the eye box together; it is both the
user's "foveation strength" slider and the governor's knob 3.

Saccade landing prediction (PAPER.md 5.1.4) belongs on the client, which has
the raw samples; the server consumes it as just another gaze point, so nothing
in this library changes.

### 6.5 Rate-control weight

```
weight = clamp((1 + e'/2.3)^-0.75, 0.15, 1.0)
```

1.0 at the fovea, floors at 0.15 around 33 degrees, which is the range
PAPER.md 4.6 asks of `fov_t`. It is exposed on the map but the allocator uses
the discrete `dQ_ecc` ladder instead (section 4.3); the continuous weight is
there for the transport's FEC class assignment and for callers that want a
smooth falloff.

---

## 7. GPU mapping

Everything is arrays of tiles, and the operations are:

| stage | shape |
|---|---|
| `compute_tile_stats` | one workgroup per tile, 5 sums, already the encoder's analysis pass |
| `classify_tiles` | one thread per tile, no cross-tile reads |
| `foveation_map` | one thread per tile, pure function of (lens, gaze) |
| weights | one thread per tile + 2 workgroup sum reductions (mean log-variance, mean complexity) |
| pressure search | 19 iterations of (one thread per tile + 1 sum reduction) |
| clamp/redistribute | 6 iterations of (one thread per tile + 2 sum reductions) |
| `update_model` | one thread per tile |

The whole allocator is one workgroup of 256 lanes striding the tile array, as
PAPER.md 4.6 assumes, with `subgroupAdd` for the reductions and shared memory
for the workgroup total. There is no prefix sum in the final design - the
weight split is a division by a total, not a scan - and no data-dependent
control flow: the pressure search is a fixed 19-iteration loop with a
convergence flag, not a `while`.

Everything the allocator needs per tile fits in 48 bytes (5 floats of stats,
2 of complexity and slip, the class and level bytes, and the model's `a_t`),
so 2312 tiles is 111 KB of SSBO traffic per frame, which is nothing.

`float` is used throughout. The rate control is not part of the normative
decode path - only the resulting `qp`, `res_level`, `chroma_mode` and
`wm_id` reach the bitstream - so it does not need bit-exact reproducibility
between the CPU model and the GPU. The one place that would change is
`qp_dither`, which is integer and portable by construction.

---

---

## 8. The temporal ladder: per-tile refresh

Sections 4.4 to 4.7 decide how much detail a tile keeps. This section decides
how *often* a tile's residual is sent at all. It is the fourth consumer of the
foveation map (PAPER.md 5.1.1 names three) and the second axis of 4.6.1's
ladder, which is purely spatial as the paper writes it.

Implementation: `rc/include/nxrc/tvm.hpp` and `rc/src/tvm.cpp` for the model,
`rc/include/nxrc/refresh.hpp` and `rc/src/refresh.cpp` for the scheduler,
`tests/rc/test_tvm.cpp` and `tests/rc/test_refresh.cpp` for the invariants,
`rc/RESULTS-temporal.md` for the measurements.

Two papers, both from docs/RESEARCH-ACADEMIC.md, both marked "implement now":

* **Flöter, Geringer, Reina, Weiskopf, Ropinski**, *Evaluating Foveated Frame
  Rate Reduction in Virtual Reality for Head-Mounted Displays*, ETRA 2025,
  arXiv 2505.03682. 15 participants, 90 Hz HMD, five concentric regions,
  update rates 1/1 to 1/5 per region. Their finding: **a reduction of 63.6% of
  the pixels drawn (their FRC 11223 - full rate in the inner three regions,
  1/2 in the fourth, 1/3 in the fifth) is feasible without users feeling
  uncomfortable**; discomfort begins at FRC 12233 and even the worst
  configuration they tested, 12345, kept the discomfort upper quartile at
  "very little". Their region boundaries come from Mohanto et al. 2022 and are
  given as diameters of 6.3, 9.1, 18.1 and 31.1 degrees, so as eccentricities
  3.15, 4.55, 9.05 and 15.55.
* **Tursun and Didyk**, *Perceptual Visibility Model for Temporal Contrast
  Changes in Periphery*, ACM TOG 41(6) 2022, doi 10.1145/3564241, arXiv
  2205.00108. The cost function: given eccentricity, spatial frequency,
  temporal frequency and contrast, how likely is an observer to see a temporal
  change. This is what turns Flöter's "the periphery can go slower" into a
  per-tile decision.

### 8.1 What a temporal step actually is

A tile the scheduler steps down is emitted as **`WARP_SKIP`**, which is the
mode that already exists (SYNTAX.md, GLOSSARY.md) and which the allocator
already produces for static tiles (4.8). It is not a frozen tile:

* the decoder's pose warp still moves it with the head every frame, at full
  frame rate, because `WARP_SKIP` means "predict from the pose-warped
  reference", not "repeat the pixels";
* per PAPER.md 2.8 the client also has the residual-motion field for its own
  in-between frames, so a skipped tile keeps tracking object motion too;
* what is withheld is only the **residual correction**. The tile drifts by
  whatever the warp gets wrong, and that drift is exactly what the visibility
  model is asked to price.

So the decoder needs no new tool, the bitstream needs no new syntax, and a
skipped tile costs one bit in the row's `skip_bitmap`. The temporal ladder is
an encoder-side scheduling decision and nothing else. That is the whole reason
it is worth doing at all.

### 8.2 The visibility model, and what we had to approximate

The paper's model takes a local spatio-temporal DCT of the video, converts
each band to Weber contrast, scales it by an eccentricity-dependent
sensitivity, pools over bands and maps the result through a psychometric
function. We do not have a spatio-temporal DCT and never will on the
encoder's critical path, so `nxrc::tvm` is a **reduction** of it onto the five
per-tile statistics 3.1 already produces plus `ecc_deg` from the foveation
map.

The fitted constants are the paper's, verbatim (their Table 2):

```
S_DL(x)     = a0 + a1 x + a2 x^2 + a3 x^3
              a = (3.2714, 0.3830, 0.7669, -0.2555)
S_SP(x)     = ln(1 + exp(S_DL(x)))                       soft-plus
T(f_s, e)   = b1 - b2 f_s^b3 + b4 e^q(f_s)
              b1 = 1.0051, b2 = 0.1830, b3 = 0.9517, b4 = 0.0173
q(f_s)      = b51 f_s^2 + b52 f_s + b53
              b51 = -0.1375, b52 = 0.3753, b53 = 2.3855
U(f_t,f_s,e)= f_t - b6 + b7 f_s + b8 e,   b6 = b7 = b8 = 0
S           = T(f_s, e) * S_SP(U(f_t, f_s, e))
C_JND       = S * C                       C = Weber contrast, floor 50 cd/m^2
C_M         = ( sum |C_JND|^r )^(1/r),    r = 1.9932
P(detect)   = Weibull, p_g = 0.5, p_l = 0, beta0 = 1.7934, beta1 = 1.5
```

`f_s` is the paper's `f_h + f_v` in cycles/degree, `f_t` in Hz, `e` in
degrees. Their experiments cover eccentricities of about 10 to 40 degrees.

Our per-tile reduction, for a tile coded one frame in `k` at `fps`:

```
f_s   = 2 * asin(sqrt(R/2)) / (2 pi) * ppd_render(theta)      cycles/degree
dL    = residual_mad * k                                      luma codes
C     = dL/255 * peak * gamma * (mean/255)^(gamma-1)
        / max(peak * (mean/255)^gamma, 50 cd/m^2)
f_t,m = m * fps / k,   amplitude C / (pi m),   m = 1..3
C_M   = ( sum_m |S(f_t,m, f_s, e') * C/(pi m)|^r )^(1/r)
```

`R` is the classifier's normalised frequency ratio from 3.1, and the inversion
is exact for the operator that defines it: `R = E[sin^2(2 pi f_x)] +
E[sin^2(2 pi f_y)]` over the tile's spectrum, which is why `R = 1.0` for white
noise. `e'` is `FoveationMap::ecc_deg`, i.e. eccentricity **after** the fixed
eye box or the gaze pad, the same conservative reading the spatial ladder uses
(6.3, 6.4).

**Five approximations, all of them ours, none of them in the paper.** They are
listed here rather than buried because the absolute numbers in
`rc/RESULTS-temporal.md` are conditional on them.

| # | what | why it is a guess | how much it matters |
|---|---|---|---|
| 1 | the De Lange polynomial's argument is read as `ln(1 + f_t)`, not raw Hz | in raw Hz the published cubic peaks at 0.5 Hz and crosses zero at 4.5 Hz, so nothing above 5 Hz would ever be visible. That is not a De Lange curve and not what the paper's figures show. In log-frequency the same four coefficients give a band-pass curve peaking near 10 Hz and reaching zero near 70 Hz, which is the shape the paper says it fits | **the largest one.** Every `C_M` in RESULTS-temporal.md depends on it. The *orderings* do not: all the scheduler needs is that sensitivity falls steeply above 30 Hz. First thing to check against the authors' code |
| 2 | `f_s` clamped to 4.0 cycles/degree, `e` to 0.5..60 degrees | `T` goes negative above about 5.5 cpd and `q` changes sign at 5.75 cpd, both outside the fit. Clamping keeps `T > 0` and `q > 0`, which is what makes monotonicity in eccentricity a theorem | moderate. It compresses the flat-versus-texture gap of 8.3; uncompressed it would be larger, not smaller, so the clamp is the conservative direction |
| 3 | the psychometric function is a standard Weibull | the printed form does not evaluate to a probability - it is below `p_g` for every positive `C_M` and does not tend to 1. We use the function that family of parameters describes. At the paper's JND unit `C_M = 1` this gives `P = 0.67` rather than the stated 0.75 | small. The scheduler gates on `C_M` and never on `P`; only the reported probabilities move |
| 4 | one representative spatial frequency per tile, pooling over temporal harmonics only | the paper pools over a real DCT; we have one number per tile | moderate, and it is why the model is a *ranking* device here rather than an absolute threshold |
| 5 | luma code to cd/m^2 through a 100 nit, gamma 2.2 display | the panel's real transfer function is not in the library | none, in practice. At HMD luminances every tile in the simulated scene sits below the model's 50 cd/m^2 Weber floor, so the de Vries-Rose clamp is always active and the luminance term is inert. `dQ_lum` and the temporal cost function therefore do not interact |

### 8.3 The combined ordering: spatial step versus temporal step

**The two ladders do not order tiles the same way, and must not be collapsed
into one scalar.** This is the substantive finding of this section and it
falls straight out of the model.

The spatial ladder (4.4) gives up **flat first, then texture, then edge, then
text**, because a flat tile has no spatial detail to lose. Read the model's
`T(f_s, e)` for the *temporal* case and the order inverts at the top:

Sensitivity `S` at `f_t = 12 Hz`, i.e. what a `k = 6` step at 72 Hz excites:

| tile | `f_s` cpd | `S` at e = 1 deg | `S` at e = 30 deg | ratio |
|---|---|---|---|---|
| flat, `R = 0.004` | 0.31 | 4.80 | 415.8 | 87 |
| texture, `R = 0.97` | 5.40, clamped to 4.0 | 1.69 | 28.4 | 17 |

At the fovea a smooth tile is 2.8x more sensitive to a withheld update than a
finely textured one; at 30 degrees it is 14.6x more sensitive. Eccentricity
*multiplies* the gap by five. Note also the raw magnitudes: sensitivity to a
12 Hz change rises by a factor of 87 between 1 and 30 degrees on a smooth
tile. The mechanism is the paper's `b4 e^q`
term with `q` falling from 2.39 at DC to 1.69 at 4 cpd: peripheral vision gets
**more** sensitive to temporal change with eccentricity, not less - the
Ferry-Porter direction, the same reason critical flicker fusion rises towards
the periphery - and it gets more sensitive fastest for low spatial
frequencies.

`rc/RESULTS-temporal.md` section 2 shows this happening on real tiles: the
9.1-15.6 degree ring is stepped hardest (duty 0.69 at 20 Mbit/s) while the two
rings *outside* it are barely touched, because this scene's far field is sky
above and ground below. A scheduler that ordered by eccentricity alone would
have done exactly the wrong thing.

So the combined ladder is two axes with one arbiter, not one ordering:

```
worst-last, per tile:

  S0   spatial step 0             untouched
  S1   spatial step 1             low-pass matrix           free
  S2   spatial step 2             res 1/2                   free
  ---- the spatial ladder has now spent every step that costs no bits ----
  T1   temporal k = 2             texture and flat, e' > fovea_full_deg
  S3   spatial step 3             res 1/4, QP +4
  T2   temporal k = 3
  S4   spatial step 4             DC plane, QP +4
  T3   temporal k = 4 and k = 6
```

Three reasons the temporal ladder waits for the spatial one, in decreasing
order of how much they settle the question:

1. **A spatial step frees no bits; a temporal step frees nothing but bits.**
   4.4 is explicit: a ladder step does not reduce a tile's bit share, it
   redirects the same bits onto fewer, lower-frequency coefficients. Bits come
   free only when a stepped tile hits its class QP floor. A `WARP_SKIP` tile,
   by contrast, gives back its entire allocation. Spending a free step before
   a paid one is not a perceptual judgement, it is arithmetic.
2. **A spatial artefact is stationary; a temporal one is not.** A blurred
   peripheral tile is a fixed loss of acuity in the region of the visual field
   that has least of it. A stale peripheral tile is a *change* signal in the
   region of the visual field that is best at detecting change. The model's
   `T` above is the quantitative form of that sentence.
3. **Empirically, Flöter's own operating point sits there.** Their FRC 11223
   is full rate in the inner three regions and 1/2 and 1/3 in the outer two,
   at 63.6% fewer pixels, and it is the most aggressive configuration their
   participants tolerated. `T1` and `T2` reach that shape and stop; `T3`
   corresponds to their 12345, which is where their discomfort scores start to
   move.

The per-class caps in `RefreshConfig::max_k` encode the same argument in the
other direction: `{ 6, 6, 2, 1 }` for flat, texture, edge, text. Text is never
stepped (8.5); an edge reaches 1/2 and stops, because a stale edge is a double
image rather than a blur, which is 4.6.1's "blur, never block" requirement
restated on the time axis.

### 8.4 The gate, the budget knob, and the coupling to pressure

The scheduler mirrors the spatial pressure search (4.6) exactly, because it
has to run in the same place: one broadcast frame-level scalar plus a per-tile
compare, no sort, no per-tile control flow.

The scalar is the **gate** `G`, a visibility threshold in the model's JND
units. A tile takes the largest divisor its caps allow whose predicted
visibility is at or under the gate:

```
k_max(t, G) = max { k in {1,2,3,4,6} :
                        k <= cap(t)  and  C_M(t, k) <= G }

cap(t)      = 1                        if e'(t) <= fovea_full_deg
              k_max_frames             if the tile is already static
              min(max_k[class(t)], k_max_frames)   otherwise
```

`C_M` is monotone non-decreasing in `k` (`tests/rc/test_tvm.cpp`), so this is
a scan with an early out. `k_max` is monotone non-decreasing in `G`, so the
steady-state duty cycle `sum 1/k_t / n` is monotone non-increasing in `G`, so
the budget search is a real search: 16 log-spaced steps, smallest `G` meeting
the target, the same shape and the same number of reductions as the pressure
grid.

Two ways to set `G`, and exactly one is active:

* **Budget mode.** `target_coded_fraction` in (0, 1] is the duty cycle to hit.
  `target_bits` does the same through an EMA of the measured bits per coded
  tile, fed back by `update_cost()`. The duty cycle has a hard floor well
  above zero - the fovea, text, the mandatory refreshes and `k_max_frames` are
  not for sale - and the target cannot push past it.
* **Pressure mode**, the default when both are off. `G` interpolates
  logarithmically from `gate_lo` to `gate_hi` as `AllocResult::pressure` runs
  from `engage_pressure` (2.0) to `max_pressure` (4.0). Below `engage_pressure`
  the gate is pinned at `gate_lo` and the temporal ladder does nothing at all.
  This is 8.3's ordering expressed as a control law.

`G` may tighten immediately - a budget cut is a hard constraint - but relaxes
at most `gate_slew_up` = 1.35x per frame, so the periphery does not flap
between two refresh rates. Same asymmetry, and the same reason, as the
spatial pressure and the decode governor.

Which frame of its `k`-cycle a tile refreshes on is a fixed **van der Corput**
permutation of the tile id, `refresh_phase()`, so a `k = 2` field refreshes
half its tiles every frame instead of all of them every other frame. Without
it the temporal ladder converts a bitrate saving into a bitrate *spike* every
`k` frames and produces a visible refresh wave. Note the implementation
detail that cost a test: the bit-reversed id of an `n <= 2^m` tile array only
has its top `m` bits set, so `reverse(i) % k` is identically zero for every
power-of-two `k`. The phase is `floor(k * reverse(i) / 2^32)`, which is the
textbook use of the sequence and is exactly equidistributed.

### 8.5 The invariants, and why the reference chain survives

The scheduler is allowed to make the periphery stale. Six things it is not
allowed to do, all asserted in `tests/rc/test_refresh.cpp` and all measured in
`rc/RESULTS-temporal.md` section 6:

1. **The fovea is never below full rate.** `e'(t) <= fovea_full_deg` (8
   degrees, matching `RateConfig::mid_ring_deg`) forces `k = 1` at every
   budget and every gate. This is a policy, not a model output, so that no
   re-fit of the model can ever put the fovea below full rate.
2. **Text is never skipped unless it is static.** `max_k[Text] = 1`. The
   exemption is real and not a loophole: a text tile whose warped SAD is under
   `skip_sad` is *already* `WARP_SKIP` in the allocator (4.8) and has no
   residual to withhold. A head-locked HUD over a moving scene is the case
   this protects, and it is the same argument as the `cplx_t` floor in 4.3.
3. **Every tile is coded at least every `k_max_frames`.** Default 6. The
   scheduler counts consecutive skips per tile and forces a code at
   `k_max_frames - 1`, which also caps the ladder, so the bound holds even
   when the gate changes underneath a tile mid-cycle.
4. **The rolling intra refresh wins.** A tile in this frame's 1/180 slice
   (PAPER.md 6.6, ADR-0006) is passed in as `intra_due` and is never turned
   into a `WARP_SKIP`. The scheduler only ever *adds* skips; it never removes
   a code the encoder has already decided on.
5. **Reference eligibility wins.** A tile whose 3x3 acknowledged neighbourhood
   failed (TRANSPORT.md 9) is going intra anyway, is passed in as
   `ref_ineligible`, and is likewise never skipped.
6. **A scene cut codes everything.**

**Why a skipped tile still warps from a valid reference.** This is the
question ADR-0006 makes it easy to get wrong, and the answer is that the
temporal ladder does not touch the reference chain at all.

The reference for tile `t` of frame `N` is the newest frame `M` in
`{N-1, N-2, N-3}` whose 3x3 neighbourhood is fully acknowledged - and `M` is a
*decoded picture*, not a coded one. A `WARP_SKIP` tile is decoded output like
any other: the client reconstructs it by warping its own reference and holds
full-resolution pixels for it. Whether those pixels were reached with a
residual or without one is invisible to the eligibility rule, which asks only
whether the client's pixels are bit-exact and acknowledged. The encoder's
mirror ring replays the identical warp, so the shadow stays exact. Skipping
therefore cannot make a tile ineligible as a reference and cannot lengthen
`ref_delta`.

What skipping *does* accumulate is prediction error: the drift the residual
would have corrected. That is bounded by two things and nothing else - the
`k_max_frames` bound above, and the rolling intra refresh underneath it - and
it is priced by the model, which is the entire point of using one. The
scheduler introduces no new failure mode into the loss model; it makes the
existing "how stale can a tile get" question have a smaller answer than the
rolling refresh alone gives it (6 frames rather than 180).

### 8.6 Outputs and GPU mapping

`RefreshResult`, per tile: `divisor` (the chosen `k`), `force_skip` (the
decision for this frame), `age`, `mandatory`, `visibility` (`C_M`),
`p_detect`. Per frame: `gate`, `duty_cycle`, `coded_fraction`,
`mean_visibility`, `max_visibility` and the four counters.

`force_skip` is handed to the allocator as `FrameInputs::force_warp_skip`,
where it joins the existing static-skip path in `compute_weights()`: weight
zero, one bit in the row bitmap, and the freed bits redistributed to the
survivors by the `skip_rounds` loop that already exists (4.8). The scheduler
runs *before* `allocate()` so that the redistribution happens inside the
normal allocation, and `AllocResult::skipped_temporal` reports how many of the
frame's skips it caused.

| stage | shape |
|---|---|
| `tile_model` + `tile_visibility` | one thread per tile, pure ALU, no cross-tile reads |
| gate search | 16 iterations of (one thread per tile + 1 sum reduction) |
| `schedule` commit | one thread per tile plus a per-tile `age` byte of state |

The gate search evaluates `tile_visibility` up to 5 times per tile per
iteration (once per ladder entry, with an early out), which is the only place
this library does real transcendental work: 16 x 5 x 3 harmonics of `pow` and
`log` per tile. On the GPU that is worth restructuring into a per-tile
precomputation of `C_M(t, k)` for the five `k` once, into 5 floats of shared
state, after which the 16 gate iterations are compares. The CPU model does not
bother.

The only state is one `uint8_t` of `age` per tile. `refresh_phase` is integer
and portable by construction; the rest is `float` and, like the spatial
allocator, does not need bit-exact agreement between CPU and GPU because none
of it reaches the bitstream directly.

### 8.7 What the encoder mode decision in `ref/` must expose

**Done.** `nxvc_encoder_set_skip_map()` is the input below and
`nxvc_encoder_tiles()` reports `skipped` / `age_since_coded` / `ref_delta` as
the output. `nxrc::EncDriver` (`nxrc/encdrive.hpp`) is the wire: it runs
sections 3 to 8 of this document per frame and hands the encoder the four
per-tile arrays, and `nxv-enc --rc` is that driver on the command line.
Measured results are in `ref/RESULTS-percept.md`. The requirements the
implementation had to meet, unchanged:

**Input, required:** a per-tile `force_warp_skip` flag, or equivalently a
per-tile mode override that can pin `mode` to `WARP_SKIP`. Semantics:

* when set, the tile **must** be coded as `mode == WARP_SKIP` with no residual
  and no vector, exactly as if its rate-distortion search had chosen it;
* the flag is **advisory in one direction only**. The encoder may ignore it -
  and must - when the tile is being coded `INTRA` for the rolling refresh, or
  when reference eligibility forces `ref_delta == 3`, or on a scene cut. The
  scheduler already refuses to set it in those cases, but the encoder is the
  authority and the flag must not be able to override a correctness decision;
* it must apply *after* the mode search, not before, so that a tile whose
  search would have chosen `WARP_SKIP` anyway is indistinguishable from one
  that was told to;
* it changes no syntax. `WARP_SKIP` is an existing mode with an existing
  encoding (`skip_bitmap` bit, `dir_len == 0` in the transport directory), so
  this is a scheduling input to the encoder and not a bitstream change. It is
  the one place the temporal ladder differs from A.5's weighting-matrix
  problem, which does need syntax.

**Output, wanted:** the per-tile `age_since_coded` the encoder is already
tracking for the rolling refresh, and the per-tile `ref_delta` chosen by the
eligibility rule. The scheduler currently takes `intra_due` and
`ref_ineligible` as spans the caller assembles; if `ref/` exposes them
directly the caller stops having to shadow that state. This is a convenience,
not a correctness requirement - the invariants of 8.5 hold with both spans
empty, because the `k_max_frames` bound does not depend on them.

**Not needed, explicitly:** no new tile mode, no new header field, no decoder
change, no change to the reference ring, and no change to the eligibility rule
(8.5 argues why). If the Phase 2 agent finds itself adding syntax for this,
something has gone wrong.

---

## Appendix A. Decisions

### A.1 Departures from PAPER.md, and why

| # | Paper | Implementation | Reason |
|---|---|---|---|
| 1 | text = "high coherence plus high contrast" | text = high gradient energy + high contrast + `R <= 0.93`; coherence is used for edge | measured: glyph fields have coherence 0.03. Section 3.2 |
| 2 | `dQ_act = -strength * (log_var - avg)` | `+strength * (...)` | the paper's own next sentence says "flat tiles get finer steps, busy tiles coarser", which needs the plus. A.2 |
| 3 | `fov_t` 1.0 -> 0.15 *and* `dQ_ecc` +0/+2/+4/+6 | one eccentricity control: `s_fov^2 * 2^(-dQ_ecc/6)` | applying both double-counts eccentricity. Section 4.3 |
| 4 | ladder steps as a table of tools | plus a QP adder on texture/flat steps 3 and 4 | without it the ordering invariant fails in the far periphery. A.3 |
| 5 | `QP = 6*log2(a/bits)` | `floor(... + vdc(t))` | plain rounding costs 4% of the frame, biased. Section 4.7 |
| 6 | ladder engages "under budget pressure" | engages on the per-tile ceiling deficit | a frame can be on budget with blocky periphery tiles. Section 4.6 |
| 7 | (nothing said) | text exempt from `dQ_act`, `cplx_t` floored at 1 for text | A.2 |
| 8 | 4.6.1's ladder is purely spatial | a second, temporal axis: per-tile refresh divisor | 2.8 already decouples frame rate; RESEARCH-ACADEMIC entries 3 and 4 say implement. Section 8 |
| 9 | (nothing said) | the two ladders order tiles *differently* and are arbitrated, not merged | the temporal visibility model is more sensitive in the periphery, not less, and most so for flat tiles - the reverse of the spatial ladder. Section 8.3 |

### A.2 The sign of the activity term

PAPER.md 5.2 writes `dQ_act = -strength * (log2(sigma^2_tile) - log2(sigma^2_avg))`
and then says "Flat tiles get finer steps, busy tiles coarser; the Watson
contrast-masking exponent of about 0.7 is what the log-variance rule
approximates". Those two disagree: with the minus, a busy tile
(`log_var > avg`) gets a *negative* offset, i.e. a finer step. x264's
`aq-mode` uses the plus, and contrast masking is the plus. The prose and x264
win; the formula is a typo.

Text is exempted from the term entirely. Contrast masking is a statement about
noise-like detail hiding quantisation error, and glyph edges are the canonical
counter-example: high variance, and the eye is more sensitive there, not less.
The `cplx_t >= 1.0` floor for text is the same argument in the complexity
term: a head-locked HUD has a near-perfect pose-warp predictor, which would
otherwise starve it of bits in exactly the frames where it is most readable.

### A.3 Why texture and flat gained a QP adder

The paper's ladder has texture descending by resolution (`1/2`, `1/4`,
DC plane) and edge descending by QP (`+2`, `+4`). For a tile the foveation map
has already put at 1/4, the texture ladder's resolution steps are no-ops -
there is nowhere lower to go - so its severity stops rising at 2.84
(the low-pass matrix) while the edge row keeps climbing to 6.84. An edge tile
would then be more degraded than the texture tile beside it, which is exactly
backwards.

Texture and flat therefore carry `QP +4` at steps 3 and 4. The ordering then
holds at every step and every foveation level, which `rc.allocate` verifies
exhaustively. The added QP is bounded by the class ceiling of 38, so it cannot
reintroduce blocking.

### A.4 Numbers that are guesses

Three constants are modelled, not measured, and a real encoder run must
replace them. They are isolated in `RateConfig` for that reason.

| constant | value | what it means |
|---|---|---|
| `gain_wm_periph` | 0.72 | bits saved by the low-pass weighting matrix at equal QP |
| `gain_dc_plane` | 0.12 | bits of a DC-plane tile relative to a full one |
| `a_init` | 32768 | starting bit model, 0.25 bpp at QP 30 on 4096 samples |

The temporal ladder adds five more, of a different kind - they are readings of
a published model rather than unmeasured constants of this codec - and they are
tabulated separately in section 8.2 because the argument for each is specific.
The one to worry about is the log-frequency reading of the De Lange
polynomial.

The QP ceilings (38 / 38 / 32 / 26) are also judgement, not measurement: they
encode "where does an 8x8 DCT at this step size start to show its grid on this
class of content", which is a subjective-testing question. The simulator's
numbers are all conditional on these four.

### A.5 The per-tile weighting matrix has no syntax

**Resolved: option 2 was taken.** Syntax v1.2 spends tile-header word1 bits
26-27 on `wm_id` behind tool bit 20 (`NXVC_TOOL_WM_ID`), which is exactly the
change proposed below. `nxvc_encoder_set_wm_map()` is the encoder-side way to
drive it per tile, and `nxrc::EncDriver` feeds `AllocResult::wm_id` straight
into it. One consequence to know: `wm_id == 0` means "the frame's matrix", so
a stream that wants `nxrc::WM_FLAT` on its text tiles has to declare the flat
matrix as the frame matrix, which is what `nxv-enc --rc` does.

The original note:


PAPER.md 1.5 selects a weighting matrix **per frame**; 4.6.1's ladder needs it
**per tile**. The v1 tile header (1.2) has no field for it. Three options:

1. use the frame matrix most tiles want and express the rest as QP - loses the
   shape of the low-pass, which is the point of step 1;
2. spend 2 of the 7 reserved bits in tile header word1 on `wgt_matrix`;
3. make the matrix a function of `res_level`, which is already in the header.

The allocator emits `wm_id` per tile on the assumption that option 2 is taken.
It is a 2-bit change to a header that has the room, and option 3 would tie the
blur to the resolution step and remove the ability to blur at full resolution,
which is ladder step 1 and the gentlest thing the codec can do. Flagged for
the bitstream owner.

### A.6 What the simulator does and does not show

`nxvc-rcsim` models the encoder as `a_true * s^2 * gain * 2^(-QP/6)` plus a
64-bit header, with +/-8% noise and a per-tile ground truth the controller does
not know. That is the same functional form the controller assumes, so the
simulator exercises the **control loop** - budget tracking, ladder engagement,
convergence, governor hysteresis, scene-cut recovery - and says nothing about
whether the form itself is right for this codec. Confirming that needs the
real encoder and is the first thing to do when `ref/` can encode a frame.

`RESULTS-temporal.md` inherits all of that and adds one more layer: its
visibility numbers are a reduction of a psychophysical model (8.2), so they
rank tiles reliably and predict absolute detection probabilities only as well
as approximation 1 holds. What that scenario does establish without any
appeal to the model's calibration is structural: the scheduler is monotone,
bounded, deterministic, respects every invariant of 8.5, and lands unprompted
on the refresh pattern a 15-participant user study independently found
acceptable. The thing it cannot establish, and which needs a headset and
observers, is where the gate should actually sit.

The scene's per-material statistics are measured from real 64x64 synthetic
tiles, so the classifier is exercised on genuine numbers even though the
complexity sequences are synthetic. `Scene::load_dump()` accepts a real
per-tile stats dump if one ever exists; nothing depends on it.
