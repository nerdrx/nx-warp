# Hybrid mode: results

What a hardware HEVC base layer plus our pose-warped enhancement layer costs
and buys, measured. Generated from `nxvc-hybridsim` (see
[README.md](README.md) for the model and its limits); design consequences are
in [../docs/HYBRID.md](../docs/HYBRID.md).

Run: 68-point sweep, 1024^2 x 90 frames, synthetic panorama with a pose log
reaching 300 deg/s plus screen-space movers, x265 (libx265) base under
zerolatency P-only settings, 2-bit blend weights, +-6 px per-tile MV.
Bitrates are quoted throughout as their **2 x 2048^2 x 90 Hz equivalent**.

---

## The answer, up front

1. **The best base resolution is 1x. Reduced-resolution bases lose badly** --
   3.0 to 4.0 dB at 0.75x and 4.0 to 4.6 dB at 0.5x against HEVC alone, and
   they do not recover at any split or any bitrate tested.
2. **The best base share is the largest one tested, at every bitrate and every
   base resolution above 0.5x** -- and pushing it further keeps helping. The
   optimum is at the edge of the grid, and the thing it converges to is plain
   HEVC.
3. **No hybrid configuration beats HEVC alone at a matched bitrate.** The best
   is 0.27 to 0.35 dB short of it. Hybrid mode does not buy quality.
4. **Hybrid does beat our pure codec** by 3.4 to 4.1 dB at a 1x base -- but so
   does plain HEVC, by slightly more.
5. **The enhancement layer is doing real work** (up to +4.4 dB over the base it
   sits on) -- it is just a less efficient use of marginal bits than giving
   those bits to x265.
6. **The two-hypothesis design is validated on its own terms.** Which
   hypothesis wins flips cleanly with base strength, and dropping either one
   costs 0.3 to 12 dB depending on the operating point.
7. **The fifth blend weight is worth nothing.** 2 bits, not 3.

Recommendation and the case against these numbers are at the end.

---

## Anchors

`hevc` is x265 alone at the full bitrate and full resolution, same
zerolatency P-only settings as the base layer. `pure` is the intra/inter DCT
model of our codec with no base layer: same transform, quantiser, bit model,
tile grid, pose warp and per-tile MV as the enhancement layer, so
hybrid-vs-pure is an internally consistent comparison.

| Target Mbit | HEVC Mbit | HEVC PSNR-Y | HEVC SSIM | Pure Mbit | Pure PSNR-Y | Pure SSIM |
|---|---|---|---|---|---|---|
| 50 | 50.1 | 35.94 | 0.9617 | 50.0 | 31.58 | 0.8908 |
| 100 | 100.2 | 39.76 | 0.9789 | 100.0 | 35.33 | 0.9384 |
| 150 | 150.9 | 41.83 | 0.9846 | 150.0 | 37.78 | 0.9587 |
| 200 | 201.5 | 43.38 | 0.9879 | 200.0 | 39.64 | 0.9698 |

The pure-codec anchor sits 4.0 to 4.4 dB below x265. That gap is the
simulator's own inefficiency, not a prediction about the real codec: the model
has no rate-distortion optimisation, no trellis quantisation, no adaptive
contexts, no deblocking, no directional intra and one motion vector per 64x64
tile. PAPER.md 1.10 estimates the real codec at roughly parity with HEVC.
**Every number in this document should be read with that 4 dB in mind**; the
last section works through exactly which conclusions it threatens.

---

## The sweep

### Base at 1x resolution

| Mbit | base 25% | base 40% | base 55% | base 70% | base 85% | best | vs HEVC | vs pure |
|---|---|---|---|---|---|---|---|---|
| 50 | 32.12 | 33.16 | 34.06 | 34.94 | 35.58 | **35.58** @ 85% | -0.35 | +4.00 |
| 100 | 36.24 | 37.25 | 38.08 | 38.81 | 39.42 | **39.42** @ 85% | -0.34 | +4.09 |
| 150 | 38.62 | 39.54 | 40.33 | 40.99 | 41.56 | **41.56** @ 85% | -0.27 | +3.78 |
| 200 | 40.35 | 41.17 | 41.90 | 42.54 | 43.07 | **43.07** @ 85% | -0.30 | +3.43 |

### Base at 0.75x resolution

| Mbit | base 25% | base 40% | base 55% | base 70% | base 85% | best | vs HEVC | vs pure |
|---|---|---|---|---|---|---|---|---|
| 50 | 31.93 | 32.37 | 32.76 | 32.90 | 32.67 | **32.90** @ 70% | -3.04 | +1.31 |
| 100 | 35.51 | 35.80 | 35.90 | 35.67 | 34.89 | **35.90** @ 55% | -3.86 | +0.57 |
| 150 | 37.70 | 37.83 | 37.77 | 37.30 | 36.16 | **37.83** @ 40% | -4.00 | +0.05 |
| 200 | 39.33 | 39.36 | 39.14 | 38.48 | 37.10 | **39.36** @ 40% | -4.02 | -0.28 |

### Base at 0.5x resolution

| Mbit | base 25% | base 40% | base 55% | base 70% | base 85% | best | vs HEVC | vs pure |
|---|---|---|---|---|---|---|---|---|
| 50 | 31.75 | 31.92 | 31.86 | 31.46 | 30.44 | **31.92** @ 40% | -4.02 | +0.34 |
| 100 | 35.11 | 35.04 | 34.72 | 33.94 | 32.46 | **35.11** @ 25% | -4.64 | -0.22 |
| 150 | 37.24 | 36.99 | 36.45 | 35.48 | 33.70 | **37.24** @ 25% | -4.59 | -0.54 |
| 200 | 38.87 | 38.47 | 37.77 | 36.62 | 34.65 | **38.87** @ 25% | -4.51 | -0.77 |

Note the shape difference. At a 1x base, quality rises monotonically with base
share: more base is always better. At 0.75x and 0.5x there is an interior
optimum that moves *left* (toward a smaller base share) as the bitrate rises,
because a soft base saturates -- past some point extra bits spent on a
half-resolution picture buy nothing the upsample can express, and the
enhancement layer is the better home for them even at our codec's efficiency.
That interior optimum is the one genuinely encouraging structure in the sweep.

### SSIM (Y) at the best split of each row

| Mbit | base 1x | base 0.75x | base 0.5x | HEVC | pure |
|---|---|---|---|---|---|
| 50 | 0.9588 | 0.9488 | 0.9241 | 0.9617 | 0.8908 |
| 100 | 0.9774 | 0.9653 | 0.9488 | 0.9789 | 0.9384 |
| 150 | 0.9837 | 0.9704 | 0.9627 | 0.9846 | 0.9587 |
| 200 | 0.9870 | 0.9764 | 0.9708 | 0.9879 | 0.9698 |

SSIM agrees with PSNR on every ordering, which is worth stating because the
enhancement layer's failure mode is blur rather than blocking (4.6.1) and blur
is where the two metrics most often disagree. They do not disagree here.

---

## Is the enhancement layer earning its bits?

The comparison that matters is not "hybrid versus HEVC at the same total". It
is: given that the base consumed `f x T` bits, did spending the remaining
`(1-f) x T` on our enhancement layer beat spending them on x265? The x265
anchor curve is interpolated between the four measured anchor points.

Base at 1x, all four totals:

| Total | Base share | Base Mbit | x265 at base rate | Hybrid | **Enh. gain** | x265 at full rate | Hybrid - x265 |
|---|---|---|---|---|---|---|---|
| 50 | 25% | 12.7 | 35.94 | 32.12 | **-3.82** | 35.94 | -3.82 |
| 50 | 55% | 27.6 | 35.94 | 34.06 | **-1.88** | 35.94 | -1.88 |
| 50 | 85% | 42.6 | 35.94 | 35.58 | **-0.35** | 35.94 | -0.35 |
| 100 | 25% | 25.1 | 35.94 | 36.24 | **+0.30** | 39.76 | -3.52 |
| 100 | 55% | 55.1 | 36.32 | 38.08 | **+1.75** | 39.76 | -1.68 |
| 100 | 85% | 85.0 | 38.61 | 39.42 | **+0.81** | 39.76 | -0.34 |
| 150 | 25% | 37.6 | 35.94 | 38.62 | **+2.68** | 41.83 | -3.21 |
| 150 | 40% | 60.0 | 36.71 | 39.54 | **+2.84** | 41.83 | -2.29 |
| 150 | 55% | 82.5 | 38.42 | 40.33 | **+1.91** | 41.83 | -1.50 |
| 150 | 85% | 127.8 | 40.91 | 41.56 | **+0.65** | 41.83 | -0.27 |
| 200 | 25% | 50.1 | 35.94 | 40.35 | **+4.41** | 43.38 | -3.02 |
| 200 | 55% | 110.1 | 40.17 | 41.90 | **+1.72** | 43.38 | -1.48 |
| 200 | 85% | 170.8 | 42.47 | 43.07 | **+0.60** | 43.38 | -0.30 |

Two things at once, and they must be read together:

* **The enhancement layer works.** At 200 Mbit with a 25% base it adds
  **4.41 dB** over what that base alone delivers. The pose-warped hypothesis
  plus a coded residual is genuinely reconstructing detail. The gain peaks
  around a 25-40% base share, which is where the enhancement layer has enough
  bits to matter and the base is still good enough to anchor it.
* **It still loses.** The final column is negative in every row. x265 converts
  the same marginal bits into more dB than we do, at every split and every
  rate. The deficit shrinks monotonically as the base share grows -- because a
  larger base share means less of the total passing through our less-efficient
  layer.

### The optimum is at the boundary

If the sweep's optimum is "spend as much as possible on the base", the obvious
question is what happens past 85%. It keeps helping, and converges on plain
HEVC from below:

| Mbit | base 70% | base 85% | base 92.5% | HEVC alone |
|---|---|---|---|---|
| 50 | 34.94 | 35.58 | 35.91 | 35.94 |
| 100 | 38.81 | 39.42 | 39.71 | 39.76 |
| 150 | 40.99 | 41.56 | 41.79 | 41.83 |
| 200 | 42.54 | 43.07 | 43.32 | 43.38 |

This is the sharpest statement the experiment makes. Within this model, the
rate-distortion-optimal amount of NX Warp enhancement on top of a
full-resolution HEVC base is **as little as possible**. The optimisation is
not finding a good hybrid operating point; it is minimising the fraction of
the stream that goes through our codec.

---

## Which hypothesis wins

`temporal win` is the share of enhancement tiles where the pure pose-warped
hypothesis (w=0) has lower residual energy than the pure upsampled-base
hypothesis (w=1) -- the LCEVC-style layer -- before any blending. `mean w` is
the mean signalled weight (0 = all temporal, 1 = all base). `blend gain` is
the residual-energy reduction of the chosen blend over the better of the two
single hypotheses.

| base res | Mbit | base share | temporal win | mean w | blend gain | intra |
|---|---|---|---|---|---|---|
| 1x | 50 | 85% | 53.3% | 0.505 | 3.4% | 0.0% |
| 1x | 100 | 85% | 41.9% | 0.585 | 2.9% | 0.0% |
| 1x | 150 | 85% | 36.7% | 0.628 | 2.7% | 0.0% |
| 1x | 200 | 85% | 33.2% | 0.654 | 2.7% | 0.0% |
| 0.75x | 50 | 70% | 66.2% | 0.415 | 2.2% | 0.0% |
| 0.75x | 100 | 55% | 70.3% | 0.390 | 1.6% | 0.0% |
| 0.75x | 150 | 40% | 79.6% | 0.307 | 1.3% | 0.0% |
| 0.75x | 200 | 40% | 80.0% | 0.308 | 1.3% | 0.0% |
| 0.5x | 50 | 40% | 81.4% | 0.235 | 1.4% | 0.0% |
| 0.5x | 100 | 25% | 88.1% | 0.163 | 2.1% | 0.0% |
| 0.5x | 150 | 25% | 88.0% | 0.162 | 2.4% | 0.0% |
| 0.5x | 200 | 25% | 88.1% | 0.160 | 2.6% | 0.0% |

**The temporal hypothesis wins exactly when the base is weak.** At a
half-resolution base it wins 88% of tiles; at a full-resolution base carrying
85% of the bits it wins 33%. This is the behaviour PAPER.md 1.7 predicts, and
it is the reason the design carries two hypotheses instead of LCEVC's one.

`intra` is 0.0% everywhere in hybrid mode: with two hypotheses available, a
flat-DC intra tile is never the best predictor. Frame 0 is the only exception
and it is below the rounding of this column.

### Per tile class, at 150 Mbit

`temporal win %` and mean weight `w` per class, across base resolutions and
shares:

| config | PSNR-Y | flat | texture | edge | text |
|---|---|---|---|---|---|
| 1x, 25% | 38.62 | 92.5% w=0.15 | 70.2% w=0.31 | 83.8% w=0.24 | 60.9% w=0.50 |
| 1x, 55% | 40.33 | 73.6% w=0.38 | 53.4% w=0.54 | 51.3% w=0.54 | 9.6% w=0.80 |
| 1x, 85% | 41.56 | 60.1% w=0.48 | 30.7% w=0.68 | 14.6% w=0.74 | **0.0% w=0.94** |
| 0.75x, 25% | 37.70 | 91.7% w=0.18 | 82.1% w=0.20 | 88.5% w=0.16 | 78.4% w=0.22 |
| 0.75x, 55% | 37.77 | 68.8% w=0.41 | 71.3% w=0.39 | 68.6% w=0.38 | 75.9% w=0.31 |
| 0.5x, 25% | 37.24 | 91.2% w=0.18 | 83.1% w=0.18 | 90.1% w=0.11 | **82.1% w=0.18** |
| 0.5x, 55% | 36.45 | 73.9% w=0.35 | 78.0% w=0.27 | 85.7% w=0.22 | 81.8% w=0.18 |
| 0.5x, 85% | 33.70 | 68.1% w=0.41 | 70.7% w=0.37 | 72.1% w=0.33 | 79.9% w=0.20 |

**Text is the class that flips hardest, and it flips both ways.** Against a
strong full-resolution base it goes to the base completely (0.0% temporal win,
mean weight 0.94): HEVC reproduces high-contrast glyphs well, and 90 Hz of
repeated Catmull-Rom resampling does not -- this is the "resampling blur"
PAPER.md 2.2 warns about, visible in a measurement. Against a
half-resolution base it goes the other way just as hard (82.1% temporal, mean
weight 0.18), because an upsampled half-resolution glyph has lost detail that
the warped previous output still carries.

Edge behaves the same way, more moderately. Flat prefers the temporal
hypothesis in nearly every configuration, which is unsurprising and cheap
either way.

The practical consequence: **a per-stream or per-frame weight cannot serve
this content.** The right weight for text and the right weight for flat differ
by 0.4 to 0.8 in the same frame, and the right weight for text inverts between
base resolutions. The per-tile weight of PAPER.md 1.7 is load-bearing.

---

## A/B: the weight alphabet

150 Mbit total, 2-bit alphabet {0, 1/4, 1/2, 1} against the paper's 3-bit
{0, 1/4, 1/2, 3/4, 1}:

| base | share | 2-bit | 3-bit | delta |
|---|---|---|---|---|
| 1x | 25% | 38.62 | 38.63 | +0.01 |
| 1x | 55% | 40.33 | 40.35 | +0.01 |
| 1x | 85% | 41.56 | 41.56 | +0.00 |
| 0.5x | 25% | 37.24 | 37.24 | +0.00 |
| 0.5x | 55% | 36.45 | 36.46 | +0.01 |
| 0.5x | 85% | 33.70 | 33.72 | +0.02 |

**The fifth weight is worth at most 0.02 dB.** This is not because it goes
unused -- at 1x/85% the 3-bit encoder spreads tiles across all five weights
(w=0: 3.1%, 1/4: 20.1%, 1/2: 26.1%, 3/4: 23.9%, 1: 26.7%) -- but because the
neighbouring weights substitute for it almost perfectly.

**Recommendation: ship the 2-bit alphabet.** Keep the per-frame
`wgt_alphabet` flag so the decision is reversible, but v1 encoders should not
use the 3-bit form. It costs a bit per tile for nothing.

## A/B: dropping a hypothesis

Same operating points. `spatial only` forces w=1 (plain spatial scalability,
the H.263 Annex O fallback of PAPER.md 1.7 and the LCEVC-shaped configuration).
`temporal only` forces w=0.

| base | share | both | spatial only | delta | temporal only | delta |
|---|---|---|---|---|---|---|
| 1x | 25% | 38.62 | 36.45 | **-2.17** | 36.36 | -2.26 |
| 1x | 55% | 40.33 | 39.65 | **-0.68** | 33.98 | -6.35 |
| 1x | 85% | 41.56 | 41.24 | **-0.32** | 29.51 | -12.05 |
| 0.5x | 25% | 37.24 | 33.62 | **-3.62** | 36.37 | -0.87 |
| 0.5x | 55% | 36.45 | 33.73 | **-2.72** | 33.90 | -2.54 |
| 0.5x | 85% | 33.70 | 31.69 | **-2.01** | 29.39 | -4.31 |

This is the cleanest validation of the two-hypothesis structure in the whole
study. The two columns are near-mirror images: where the base is strong
(1x, 85%) dropping the temporal hypothesis costs 0.32 dB while dropping the
base costs 12.05 dB; where the base is weak (0.5x, 25%) dropping the base
costs 0.87 dB while dropping the temporal hypothesis costs 3.62 dB. Neither
hypothesis is redundant across the operating range, and no single-hypothesis
design covers it.

**For the FTO review** (docs/HYBRID.md section 5, item 6): the price of the
spatial-only fallback is the `spatial only` delta column. At the recommended
operating point (1x base, large base share) it is **0.32 dB** -- cheap. At a
reduced-resolution base with a thin base share it is **3.62 dB** -- expensive.
If the fallback is ever forced, it forces the 1x base with it.

---

## Bit-model cross-check

`model` is the estimator of `codec.py` (coded-block flag, per-frequency
significance entropy, sign, `log2(1+|q|)`); `order-0` is the entropy of the
same quantised symbols under a context-free static model, at the best split of
each row.

| base res | Mbit | model, Mbit total | order-0, Mbit total | ratio |
|---|---|---|---|---|
| 1x | 50 | 0.70 | 0.60 | 0.85 |
| 1x | 100 | 1.65 | 1.63 | 0.99 |
| 1x | 150 | 2.55 | 2.60 | 1.02 |
| 1x | 200 | 3.42 | 3.53 | 1.03 |
| 0.75x | 100 | 5.39 | 6.92 | 1.28 |
| 0.75x | 200 | 14.77 | 19.39 | 1.31 |
| 0.5x | 100 | 9.13 | 11.76 | 1.29 |
| 0.5x | 150 | 13.82 | 18.07 | 1.31 |
| 0.5x | 200 | 18.51 | 24.24 | 1.31 |

The two estimators agree within 3% where the enhancement layer is thin and
diverge to 31% where it carries most of the stream, with the context-free
entropy always the *larger* of the two -- i.e. our estimator is the optimistic
one, by up to 31%, exactly in the configurations (reduced-resolution base,
large enhancement share) that already lose. Correcting for it would push those
rows further down, not up. It does not rescue any conclusion.

---

## Recommendation

**For the Pico 4, if hybrid mode ships: a 1x (full-resolution) HEVC base
carrying 85% or more of the total bitrate, with the enhancement layer taking
the remainder.** At 100 Mbit that is 85 Mbit of base and 15 Mbit of
enhancement, landing 0.34 dB below plain HEVC at the same total. Do not ship a
half- or three-quarter-resolution base: it costs 3 to 4.6 dB and no split
recovers it.

**But the honest recommendation is that hybrid mode should not ship as a
quality feature at all, because on these numbers it has no quality case.**

Set the latency question up properly, because it is easy to get backwards.
Hybrid mode's 8 to 12 ms of MediaCodec latency is *not* an extra cost relative
to HEVC -- plain HEVC pays exactly the same 8 to 12 ms, since it is the same
hardware decoder. The 8 to 12 ms is what hybrid mode costs relative to the
**pure compute path** (4 to 6 ms, PAPER.md 6.10). So the three-way comparison
at 150 Mbit is:

| Path | Decode latency | PSNR-Y (model) | What it needs |
|---|---|---|---|
| Pure compute | 4-6 ms | 37.78 | the full compute decoder to fit the Adreno 650 |
| Hybrid, 1x base @ 85% | 8-12 ms + ~0.5 ms | 41.56 | MediaCodec + Pass C |
| HEVC alone (WiVRn today) | 8-12 ms | 41.83 | nothing new |

Read that table honestly and hybrid mode is dominated by the row below it: it
is slower than pure compute, worse than plain HEVC, and more complex than
either. Its rate-distortion case is that it is 3.78 dB better than pure
compute -- but if you are willing to pay MediaCodec latency to get that, plain
HEVC pays the same latency and gives you another 0.27 dB for free.

**Therefore: hybrid mode's justification has to be something other than
rate-distortion, or it should be dropped.** The candidates, none of which this
experiment measured:

* **Per-tile loss behaviour on the enhancement layer.** The base degrades
  through HEVC's own reference invalidation, but the enhancement layer keeps
  the codec's tile-granular concealment (2.7). On a lossy link the comparison
  against plain HEVC could look very different. This is the strongest
  remaining case and it is directly testable.
* **Frame-rate decoupling** (2.8): the enhancement layer's MV field drives
  motion smoothing for free, retiring the server-side block matcher.
* **Foveation** (5.1), held flat here. A foveated enhancement layer should
  need fewer bits for the same perceived quality, which would move every
  hybrid row up and the reduced-resolution-base rows up most.
* **It is a migration path, not an endpoint.** Hybrid keeps the tiling,
  transport, shadow model and enhancement syntax shared with the pure codec,
  so it is one `layer_desc` field rather than a second codec to maintain
  (2.9). That is an engineering argument, and a real one, but it is not a
  quality argument and should not be presented as one.

**Concrete proposal.** Do not gate Phase 3 on hybrid mode. Keep it implemented
as the compatibility floor PAPER.md 1.7 and 3.5 already call it, configured at
1x base / 85% share, and re-run this sweep with (a) loss modelled and (b)
foveation switched on before deciding whether it is a shipping default.
Meanwhile the Phase 0 compute benchmark (3.4) decides the question that
actually matters, which is whether the pure path fits the Adreno 650 -- because
if it does, hybrid mode's only remaining constituency is devices that cannot
run it at all.

---

## What would change this answer

In rough order of how much they threaten the conclusions.

1. **The pure-codec model is 4.0 to 4.4 dB behind x265, and PAPER.md 1.10
   estimates the real codec at parity.** This is the big one. Every hybrid row
   inherits that deficit in proportion to how much of the stream passes
   through the enhancement layer: the 1x/85% row inherits about 15% of it, the
   0.5x/25% row about 75%. That is *why* the sweep's optimum sits at the
   largest base share and the highest base resolution -- the optimiser is
   minimising exposure to the model's own weakness, not discovering something
   about hybrid decoding. If the real codec closed even half the gap, the
   reduced-resolution-base rows would move up by roughly 1.5 dB and the
   interior optima at 0.75x and 0.5x would shift further toward the
   enhancement layer. **The recommendation to use a 1x base is therefore much
   softer than the numbers make it look, and should be revisited the moment
   `ref/` can be measured in place of this model.** The conclusion that no
   configuration beats HEVC alone would survive a 2 dB improvement; it would
   not survive parity.
2. **Foveation is off.** It is the paper's main claimed advantage over HEVC
   (5.1) and it is precisely the tool that would let a thinner enhancement
   layer carry the same perceived quality.
3. **Loss is not modelled.** The one place hybrid mode might beat plain HEVC
   outright.
4. **x265 is a better base than a Pico-class hardware encoder.** Every hybrid
   row is slightly optimistic about its base, so the *hybrid-versus-HEVC* gap
   is if anything understated -- but so is the base quality in absolute terms.
5. **The warp is float here, not the Q8.24 integer warp of PAPER.md 2.2.** The
   homography quantisation error would land in the residual and is not
   charged. Small, and it penalises the temporal hypothesis.
6. **One synthetic sequence.** The content mix (roughly a quarter flat, and
   3.3% of frame area in screen-space movers) sets how often each hypothesis
   wins. A sequence with more moving content would favour the base hypothesis;
   a static scene with fine texture would favour the temporal one. The
   direction of the class-level findings is robust; the exact percentages are
   not.
7. **90 frames is one second, and the closed loop spends its first ~12 frames
   converging from an intra start.** That drags the pure-codec anchor's mean
   down more than it drags the hybrid rows, which are anchored by their base
   every frame. Longer sequences would narrow the hybrid-versus-pure gap.

## Reproducing

```sh
cd hybrid/sim
./nxvc-hybridsim sweep --size 1024 --frames 90 --workers 4 \
    --out $NXVCH_SCRATCH/results/sweep-main.json
./nxvc-hybridsim report $NXVCH_SCRATCH/results/sweep-main.json

# the A/B variants
for v in 3bit base-only temporal-only; do
  ./nxvc-hybridsim sweep --no-anchors --weights $v --totals 150 \
      --scales 1.0,0.5 --fracs 0.25,0.55,0.85 \
      --out $NXVCH_SCRATCH/results/ab-$v.json
done
```

The main sweep is 68 points and took 38 minutes on a 4-core slice.
