# STEREO experiment: what inter-view prediction is worth

CPU experiment for PAPER 2.5 and `docs/STEREO.md`. Everything below comes from
`stereo/sim/nxvc-stereosim` on synthetic stereo pairs. **No real VR content was used.** The
numbers are a design decision aid, not a measurement of the codec — the codec does not exist yet.

## How to run

```
cmake -S stereo -B build-stereo -DCMAKE_BUILD_TYPE=Release   # or the root build with -DNXWARP_BUILD_STEREO=ON
cmake --build build-stereo -j4
./build-stereo/nxvc-stereosim --w 1024 --h 1024 --q 4,8,16 --json stereo/results.json --csv stereo/tiles.csv
ctest --test-dir build-stereo            # stereo.raster, stereo.disparity, stereo.determinism, stereo.sim_smoke
```

Default run: 5 scenes x 1024x1024 per eye x 256 tiles of 64x64 = 1280 right-eye tiles per
quantiser, about 80 seconds single-threaded for three quantisers. `stereo/results.json` and
`stereo/tiles.csv` in this directory are the run the tables below quote (q = 4, 8, 16;
`--motion 1.0`, the default).

## What is simulated

**Scenes.** Axis-aligned textured boxes rendered by primary-ray casting from a parallel stereo pair
(64 mm apart, 95 degree FOV, 2x2 supersampled, exact per-pixel eye-space depth). Textures are
procedural: value-noise fbm, hard checkers, stripes, brick with mortar lines, low-contrast "skin",
and a 5x7 bitmap font panel. Five scenes: `room` (mid-depth world), `panel` (a text quad at 1.5 m),
`hand` (near-field object at 30 to 42 cm, disparity far outside the coarse search range), `far`
(everything beyond 8 m, sub-pixel to few-pixel disparity), `clutter` (14 boxes from 0.45 to 8 m,
the depth-discontinuity stress case).

**Frames.** Two instants per scene, both eyes: `N-1` and `N`, separated by one frame at 90 Hz of a
brisk head turn — 3.3 degrees of yaw (300 deg/s, the figure PAPER 2.2 uses), 0.8 of pitch, 0.6 of
roll, and 11 mm of lateral translation. `--motion S` scales all of it; `--motion 0` freezes the
head.

**Modes measured, per right-eye tile.** All predictions use the codec's integer sampler (16-phase
4-tap Catmull-Rom, each row summing to 64, clamp-to-edge) and quarter-pel vectors.

| Mode | Reference | Vector |
|---|---|---|
| `INTRA` | none | DC-plane fit (PAPER 6.4) |
| `STEREO` | pristine `left(N)` | per-tile disparity from the depth buffer |
| `STEREO_MV` | pristine `left(N)` | ... plus ±4 px integer and quarter-pel refinement |
| `STEREO_EST` | pristine `left(N)` | disparity from an encoder coarse search, no depth |
| `STEREO_REC` / `_MV` | **reconstructed** `left(N)` | as above |
| `WARP_SKIP` | `right(N-1)` | none (pose homography only) |
| `WARP_MV` | `right(N-1)` | ... plus refinement |

The pose homography is the real one: `K R_{N-1}^T R_N K^-1`, quantised to nine int32, evaluated at
the four tile corners by integer division and bilinearly interpolated inside the tile, exactly as
PAPER 2.2 specifies. (Implementing it turned up a fixed-point defect in the paper; see
`docs/STEREO.md` section 5.)

**The reconstructed reference matters and is the headline number.** The real STEREO reference is
the *decoded* left eye, which has itself been through a fractional resample and a quantiser.
`STEREO_REC` predicts from `warp(left(N-1)) + dequantised residual` at the same quantiser — what a
decoder would actually hold. `STEREO` predicts from the rendered left eye and is therefore
optimistic. Both are reported; **the reconstructed number is the one to quote.**

**Bit model.** Not an entropy coder. Per tile: 8x8 orthonormal DCT-II of the residual,
`level = round(coeff / q)`, cost = signed Exp-Golomb length for non-zero levels, 0.08 bits per zero
level (run-length amortisation), 2 bits of flags per 8x8 block, plus per-mode side information (3
bits of mode plus 1 to 21 bits: 24 for `INTRA`'s plane, 9 for `STEREO`, 13 for `STEREO_MV`, 11 for
`WARP_MV`, 4 for `WARP_SKIP`). Modes are chosen per tile by minimum modelled bits — a pure rate
decision at fixed quantiser, not a full RD search, so it slightly *understates* every predictor's
value relative to an encoder that can trade distortion. The model has a floor of about 456 bits per
tile from the zero-level term, which is why a perfectly predicted tile in the `--motion 0` run
costs 460 bits rather than nothing; that floor is common to all modes and cancels in the
comparisons. Residual SAD and SSE are recorded per tile in the CSV for anyone who distrusts the
bit model.

**Baseline.** "Base" is the codec as specified today, without STEREO: the best of `INTRA`,
`WARP_SKIP`, `WARP_MV` per tile. Savings are against that.

---

## Headline result

Right-eye bits, 1280 tiles, `--motion 1.0`:

| q | base (kbit) | with STEREO, reconstructed ref | with STEREO, pristine ref | with STEREO, no depth |
|---|---|---|---|---|
| 4 | 6920.9 | 6084.4 (**−12.1 %**) | 5874.4 (−15.1 %) | 5909.6 (−14.6 %) |
| 8 | 4431.0 | 3944.8 (**−11.0 %**) | 3774.6 (−14.8 %) | 3810.7 (−14.0 %) |
| 16 | 2605.6 | 2357.1 (**−9.5 %**) | 2241.8 (−14.0 %) | 2269.6 (−12.9 %) |

**Whole-frame saving, left eye unchanged: 4.8 to 6.0 percent.** PAPER 2.5 predicted 5 to 10 percent
overall and 20 to 30 percent on the dependent view from the MVC/MV-HEVC literature. The dependent-
view figure here (9.5 to 12.1 percent) is **below** the literature range, and the overall figure
lands at the bottom of the predicted band. Two reasons, both worth stating plainly: the literature
compares inter-view prediction against *simulcast* (no temporal prediction of the dependent view at
all), whereas the baseline here already has a pose-warped temporal predictor; and the reconstructed
reference costs about 3.5 points of the gain.

Per scene at q = 8, reconstructed reference:

| scene | base (kbit) | with STEREO | saving | STEREO chosen |
|---|---|---|---|---|
| room | 702.2 | 626.7 | 10.8 % | 35.5 % of tiles |
| panel | 819.0 | 713.8 | 12.9 % | 31.6 % |
| hand | 770.1 | 680.1 | 11.7 % | 37.9 % |
| far | 1095.0 | 974.1 | 11.0 % | 33.6 % |
| clutter | 1044.8 | 950.2 | 9.1 % | 30.9 % |

STEREO beats DC-plane `INTRA` on **97.8 percent** of right-eye tiles. That number is nearly
meaningless on its own — DC-plane intra is a weak predictor and almost anything beats it — and it
is reported only because the task asked for it. The number that matters is the 30.2 percent of
tiles where STEREO beats the *best available alternative*, which is what the savings column
reflects.

## The gain is proportional to how much the head moved

q = 8, reconstructed reference, all five scenes:

| head motion | base (kbit) | saving | STEREO chosen |
|---|---|---|---|
| still (`--motion 0`) | 588.4 | **0.00 %** | 0.0 % of tiles |
| 75 deg/s | 3698.0 | 6.7 % | 25.8 % |
| 150 deg/s | 4076.1 | 8.9 % | 29.5 % |
| 300 deg/s (default) | 4431.0 | 11.0 % | 33.9 % |
| 600 deg/s | 5488.2 | 18.1 % | 58.8 % |

With a perfectly still head the temporal predictor is exact, every tile is `WARP_SKIP`, and STEREO
is chosen **zero** times and saves **nothing**. This is the single most important shape in the
experiment: **STEREO is not a compression tool, it is a head-motion tool**, and it earns its
complexity in exactly the frames where PAPER 4.6's rate controller is already under pressure. That
is the same argument the paper makes for the pose warp itself.

## Why STEREO wins where it wins — and it is not the reason the paper gives

PAPER 2.5 predicts that inter-view mode "mostly replaces INTRA tiles: content that is new to both
eyes at once". The experiment says that is a small part of it:

- Tiles the baseline codes `INTRA` are only **6.3 percent** of the frame, and STEREO takes **33.1
  percent** off those — a large relative win on a small population, worth about 2 points of the
  11.0.
- The other 9 points come from tiles where `WARP_MV` was already available and STEREO beat it
  anyway. Comparing winners against losers: tiles where STEREO wins have mean detail (|Laplacian|)
  of 23.0 against 10.2, and mean depth 3.8 m against 2.5 m. **STEREO wins on far, high-detail
  content.**

The mechanism is resampling blur, which PAPER 2.2 already names as a risk. `WARP_MV` samples a
reference at a fractional position, and every frame of head motion puts one more Catmull-Rom
resample between the content and the prediction. The inter-view reference has been resampled once
(it is the decoded left eye) instead of N times, and for far content the disparity is small enough
that the inter-view prediction is nearly a copy. So STEREO acts partly as a **detail-refresh path**
that costs far less than intra. That is a better story than the paper's, but it is also a fragile
one: it depends on the temporal reference being degraded, and the experiment only chains one frame
(see Threats to validity).

## Disocclusion behaves exactly as designed

Right-eye tiles binned by the fraction of pixels the left eye geometrically cannot see (q = 8,
reconstructed reference):

| disocclusion | tiles | STEREO wins | mean STEREO bits / best-alternative bits |
|---|---|---|---|
| < 0.1 % | 1156 | 29.4 % | 0.975 |
| 0.1 to 1 % | 41 | 29.3 % | 1.141 |
| 1 to 5 % | 30 | 33.3 % | 1.103 |
| 5 to 20 % | 35 | 14.3 % | 1.223 |
| > 20 % | 18 | **0.0 %** | **1.430** |

Above 20 percent disocclusion STEREO never wins and costs 43 percent more bits than the
alternative. This is the entire justification for the encoder's depth-driven disocclusion guard
(`docs/STEREO.md` 7.3) and it puts the threshold at about 5 percent. Note that RD alone already
rejects these tiles — the guard's value is saving the encoder the search, and giving a
depth-supplied encoder a decision it can make without one.

## Disparity magnitudes break the specified search range

Per-tile disparity seed `f * IPD / z`, 1280 tiles at 1024 px per eye (double all of these at 2048):

| p50 | p90 | p99 | max |
|---|---|---|---|
| 13.6 px | 22.7 px | 93.2 px | 101.7 px |

- **37.6 percent of tiles have a disparity larger than the ±16 px coarse search of PAPER 2.3.**
- 2.7 percent exceed 64 px, which is the specified MV range. At 2048 px per eye that becomes
  routine.

This is a specification defect, not a tuning issue: without a depth seed or a dedicated coarse
disparity search, STEREO cannot find its reference for a third of the frame, and the near-field
"hand" case is unreachable at any search range the encoder can afford as a 2D search.
`docs/STEREO.md` 2.3 takes both consequences.

## Depth helps, but the encoder can mostly manage without it

`STEREO_EST` (no application depth; coarse SAD search over 0 to 192 px in 2 px steps on a
4x-downsampled tile, then normal refinement) recovers **14.0 percent** of 14.8 against the pristine
reference — 95 percent of the depth-seeded gain. Its disparity agrees with the depth-derived value
within 2 px on 87.4 percent of tiles, but is off by more than 16 px on **5.3 percent**, all on
repetitive texture (brick courses, stripes) where SAD locks onto the wrong period. Those tiles lose
to `WARP_MV` on RD and cost nothing but search time.

Reading: application depth via `XR_NX_render_hints` (`docs/XR_EXT_NXWARP.md`) is a
**latency-and-power optimisation for the encoder, not a correctness requirement for STEREO**. It
buys the disocclusion guard (which the search cannot provide at all) and about 0.2 ms of encoder
time per eye-frame. That is a weaker argument for the extension than 2.3's velocity buffer, and the
extension proposal says so.

## Refinement on top of the depth seed is nearly free to skip

The ±4 px integer plus quarter-pel refinement is worth **1.8 percent** of the tile's bits on
average over the depth seed alone, and more than 5 percent on 12.2 percent of tiles. It is worth
keeping (it is the same code path as `WARP_MV`'s refinement) but it is not where the gain is.

Its vertical component was chosen non-zero on 22.1 percent of tiles — true vertical disparity is
zero, so those are the search fitting aliasing noise — and 69.3 percent of those were *upward*.
Constraining `dy = 0` for row-ordered dispatch therefore costs a fraction of 1.8 percent. See
`docs/STEREO.md` 6.1.

## Text

The 24 highest-detail tiles of the `panel` scene (the text quad) give up **13.1 percent** with
STEREO, slightly above the frame average. A text panel is a fronto-parallel plane at a single
depth, which is the best possible case for a single per-tile disparity, and its content is exactly
what resampling blur damages most. If the panel were head-locked it would have zero disparity and
`STATIC_MV` would win instead — the sim's panel is world-locked.

---

## Threats to validity

Read this section before quoting any number above.

1. **Synthetic content.** Flat-shaded axis-aligned boxes with procedural textures, no specular
   highlights, no transparency, no particles, no animated characters, no mirrors. Specular
   highlights in particular are *view-dependent*: they sit at different surface points in the two
   eyes, and they are the classic failure case for inter-view prediction that this scene set cannot
   exhibit at all. VRChat mirrors are worse still. The measured gain is therefore an upper bound
   for content with significant view-dependent shading.
2. **The temporal reference is pristine and the inter-view one is not.** `WARP_*` predicts from the
   rendered `right(N-1)`, while `STEREO_REC` predicts from a reconstructed `left(N)`. That
   asymmetry favours the baseline, so the reconstructed-reference savings are a **conservative
   bound** — but only for a one-frame chain. A real encoder's `right(N-1)` has been through many
   warp-and-quantise cycles and would be blurrier than this experiment's, which would push STEREO's
   share *up*. The true value is bracketed by the 11.0 and 14.8 percent columns and the experiment
   cannot narrow it further without a full encode loop over a sequence.
3. **Luma only.** Chroma is asserted to follow (halved disparity in 4:2:0) and not measured.
4. **The bit model is not an entropy coder.** Exp-Golomb over DCT levels with a zero-run
   approximation, no context modelling, no rANS, no rate-distortion optimisation of the residual.
   It ranks modes consistently, which is all the mode-decision conclusions need, but the absolute
   kbit figures mean nothing.
5. **Mode decision by rate at fixed q, not by RD.** An encoder that can spend distortion would
   choose STEREO slightly more often.
6. **One frame pair per scene.** No temporal accumulation, no drift, no rolling intra refresh, no
   loss. The loss-amplification cost in `docs/STEREO.md` 6.3 is reasoned, not measured.
7. **Perfect rectification.** Symmetric frusta, no cant, no per-eye foveation grid. The foveation
   interaction (`docs/STEREO.md` 10.1) is the most likely reason a real system would see less than
   this.
8. **64x64 tiles at 1024 px per eye**, so the frame is 16x16 tiles where the real one is 32x32.
   Disparities in pixels roughly double at 2048; the *ratio* results are scale-free but the
   disocclusion fractions per tile would change somewhat.

## What would change the conclusion

- A run against real captured VR frame pairs with depth (`tools/quality/capture` can import media;
  the missing piece is a stereo-plus-depth capture path from Monado). This is the Phase 2 item.
- A full encode loop over 60+ frames so the temporal reference degrades realistically. Item 2 above
  is the largest uncertainty in the experiment and this is the only way to settle it.
- Any content with strong view-dependent shading.

## Verdict

STEREO is worth building, at Phase 4 priority as the paper says, on the strength of a **5 to 6
percent whole-frame saving concentrated entirely in high-head-motion frames** — where it is worth
10 to 18 percent of the right eye and where the rate controller is already choosing what to
degrade. It is not worth building for its average bitrate. The design's cost is one extra encoder
candidate, an ordering constraint the pipeline already has, and a loss-amplification factor of up
to 3 on the right eye that the mode decision must price. The disocclusion and border guards are not
optional polish: without them the mode loses bits on exactly the tiles it is supposed to help.
