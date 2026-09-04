# Encoder rate-distortion: measurements

What one lambda, a full trellis, true bit counts, a better motion search and a
cheap per-tile QP offset are worth, measured on `tools/quality`. Everything
here is **encoder-side**: no tool bit is set, no syntax changes, and a stream
produced by any configuration in this document decodes through exactly the
same path as one produced by any other. The decoder is not touched, so its
GPU cost is unchanged by construction.

Every process ran under `chrt -i 0 taskset -c 8-11 nice -n 19`, on the **v2
band-limited** sequences in `$NXQ_SCRATCH/seq` (`*-v2`, the ones
`docs/WARP-AUDIT.md` says are the honest ground truth). Result files are under
`$NXQ_SCRATCH/results/tourney/`. Section 9 reproduces every number.

**Two metrics are reported for every row, and both matter.** `PSNR-Y` is what
the Phase 1 gate is stated on. `PSNR-YCbCr` is the JVET 6:1:1 weighted figure.
Where they disagree, the disagreement is the point: section 3 changes how much
a chroma error is worth, and a change like that moves PSNR-Y more than it moves
the picture. Reporting only the flattering one would be a choice about which
answer to give.

---

## 1. What changed

| # | change | where |
|---|---|---|
| 1 | one lambda for every decision, `lambda(QP, class) = scale * class * qstep^2` | `codec.cpp` `Lambda`, `lambda_for` |
| 2 | a chroma squared error is worth a quarter of a luma one, in every decision | `kChromaDistWeight`, `tile_distortion` |
| 3 | the DC plane is trellised, at `lambda / kDcPropagation` | `analyze_dc_plane`, `dc_rdoq` |
| 4 | a wider rdoq candidate set, and an exact scan truncation | `rdoq_unit` |
| 5 | transform skip by RD instead of a gradient rule | `choose_tskip` |
| 6 | the mode decision runs at the trellis's lambda, not a quarter of it | `decide_tile` |
| 7 | INTRA is not scored when it cannot win | `kIntraGate` |
| 8 | hierarchical motion search, temporal + spatial seeds | `decide_tile` |
| 9 | quarter-pel refinement by real RD | `eff.mv_rd_qpel` |
| 10 | per-tile QP offset by bounded descent over reused analysis | `run_pass` |
| 11 | `--preset fast\|medium\|slow` | `Effort`, `resolve_effort` |

None of these is new syntax. Items 1, 2, 6 and 10 are the ones that move the
number; 3, 4, 5, 8 and 9 are worth between 0.3 and 1.5 points each; 7 and the
truncation in 4 are why the medium preset is **faster** than the encoder it
replaces rather than slower.

---

## 2. The lambda model

### 2.1 The shape and the scale

`lambda = scale * class_weight * qstep(QP)^2`. The `qstep^2` shape is the
standard high-rate result and is not fitted; `scale` is. Swept on
`vr-turn-256-v2` 4:4:4, 6 frames, QP 0/8/16/24, BD-rate against the shipped
encoder at its own default:

| `--rdo-lambda` | BD-rate (PSNR-Y) |
|---|---|
| 0.10 | +3.74 % |
| 0.18 | +0.33 % |
| **0.30** | **-0.28 %** |
| 0.45 | +2.45 % |
| 0.70 | +5.13 % |
| 1.00 | +7.94 % |

The shipped value was already at the optimum of its own curve, and the sweep
says so. **This is the first result of this package and it is a negative one:**
the lambda scale was not where the bits were. What was wrong with the lambda
was not its value but that there were four of them.

### 2.2 The class weights

`docs/RATECONTROL.md` 3.3's four classes, computed in `ref/` on the tile's
luma (`classify_tile`, the same statistics and the same normative order,
without the UI stencil and without hysteresis). The multipliers are mild by
design: the classifier is a 64x64 statistic and a large per-tile lambda swing
shows up as a visible tile boundary.

| class | weight | why |
|---|---|---|
| flat | 0.70 | banding is visible at any rate |
| texture | 1.15 | masks its own quantisation noise |
| edge | 1.00 | — |
| text | 0.70 | a broken glyph is not a small error |

### 2.3 One lambda, not four

Before this package the encoder used its lambda four different ways: the
trellis at `0.30 * qstep^2`, the directional intra mode at the same, the mode
decision at **a quarter** of it, and the motion search at a hand-written
`2 * qstep * 8` per vector with no lambda at all. The mode decision's quarter
was justified by "a frame is a reference for the next three" — an argument the
`kSkipPersist` term already carries, and carries correctly, by charging the
*excess* distortion a skip leaves in the reference. Charging it twice biased
every coded mode.

On `vr-mixed-512-v2` 4:2:0, 8 frames, inter, against the shipped encoder:

| mode lambda | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) |
|---|---|---|
| 0.25 (the old default) | -10.15 % | +1.16 % |
| **1.00 (the trellis's own)** | **-11.53 %** | **-2.14 %** |

---

## 3. What a chroma error is worth

The encoder weighed a squared error in a Co or Cg sample exactly as much as one
in a Y sample. That is the one weighting nobody uses: the JVET reporting
convention is 6:1:1, every perceptual model puts chroma lower still, and both
anchors in this document code chroma with a quantiser of its own.

Swept on `vr-turn-256-v2` 4:4:4, everything else at the medium preset:

| `--chroma-weight` | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) |
|---|---|---|
| 1.0 (the old behaviour) | -0.29 % | -0.01 % |
| 0.5 | -7.40 % | -3.27 % |
| **0.25** | **-14.85 %** | **-4.10 %** |
| 0.125 | -20.50 % | -1.52 % |

**Read the two columns together.** PSNR-Y keeps improving as chroma is starved,
because chroma bits do not appear in it at all; that column is not a measure of
the picture and 0.125 proves it. The 6:1:1 column turns over at 0.25, which is
where the default is set. The value the JVET weighting itself implies is 1/6;
the fitted optimum landing at 0.25 rather than 0.167 is the AC blocks and the
DC plane sharing a quantiser, and the agreement is close enough to say the
number is a measurement of the metric rather than an accident.

This is the single largest item in the package on PSNR-Y and it is the one to
be most careful about quoting. **Roughly a third of its PSNR-Y gain is real
and the rest is the metric.**

## 3.2 The DC plane in the trellis

`RESULTS-intra.md` left the DC plane on the dead-zone quantizer deliberately:
it is the intra predictor, so a level chosen there moves the prediction of all
64 blocks, and the trellis's distortion is stated in block-MEAN units rather
than sample units. That is a scale problem, not a reason: a mean error `e`
raises the block's sample SSE by `64 e^2` before the AC blocks correct part of
it. Trellising the DC plane at `lambda / kDcPropagation` puts it in the same
currency as everything else.

| `kDcPropagation` | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) | `ref.codec` quality floors |
|---|---|---|---|
| 16 | -15.78 % | -4.98 % | **FAIL** (QP 48 at 21.34 dB, floor 22.0) |
| **32** | **-15.09 %** | **-4.42 %** | pass |
| 64 (the geometric value) | -14.59 % | -3.96 % | pass |

16 is a better rate-distortion trade and a worse picture: it takes 43 % off a
QP 48 frame for 2.4 dB. `ref.codec`'s absolute quality floors exist to catch
exactly that, and they did. 32 is the fitted value and the floor of what is
safe.

---

## 4. The trellis

The existing `rdoq_unit` was already a Viterbi over the entropy coder's three
level-class states, not a zero/one decision, and its rate model was already the
real per-context table. Two things were added.

**A wider candidate set** (`--trellis-full`). `{0, floor, floor+1}` brackets
the unquantized value but cannot see two levels a Markov rate model can prefer
over a closer one: `floor-1`, which may sit in a cheaper magnitude class, and
14, the largest level that is not an Exp-Golomb escape, where the rate jumps by
several bits. Worth about 0.1 points on this material, which is why `fast`
gives it up first.

**An exact scan truncation**, which is what pays for the rest. A scan position
whose magnitude is below half its step can only be zero: `(a - st)^2 > a^2`
there, so level 1 costs more distortion than level 0, and — when
`RateCost::zero_cheapest` says every context prices a zero at or below a one,
which is checked off the table rather than assumed — it costs at least as much
rate too. No lambda makes it win. So `last` can never reach past the highest
position that clears the bar, and the trellis does not visit anything above it.
This is an identity, not a heuristic, and it is why the medium preset is
faster than the encoder it replaces.

---

## 5. Transform skip

`tskip_decision()` asked whether the residual looked like blocky graphics
(mean gradient step over changed positions above 24). Measured against never
skipping at all, on `vr-turn-256-v2` 4:4:4 at the medium preset:

| | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) | encode time |
|---|---|---|---|
| `--tskip auto`, the gradient rule | +0.59 % | +1.48 % | 1.00x |
| `--tskip auto --tskip-rd on` | **-1.51 %** | **-1.23 %** | 2.68x |

The rule was worse than not having the tool. The RD form quantises the tile
both ways and compares `D + lambda*R`, which is the only honest answer and
costs one extra quantisation of every tile. It is therefore on at `--preset
slow` only, and `--preset slow` is the one effort level where `--tskip` defaults
to `auto`, because turning `auto` on anywhere else would be a regression.

---

## 6. The motion search and the per-tile QP offset

The coarse stage is hierarchical: one sweep of the whole `+-mv_range` window at
`--mv-step`, then a step-halving refinement around the winner down to one
sample. Its seeds are PAPER 2.3 step 1's — zero and the tile's own vector from
the previous frame — plus the vector the tile to its left just chose. The
vector's rate is *not* searched over, and deliberately: `mv_x` and `mv_y` are
fixed-length fields in the tile header, so every vector in a mode costs the
same two bytes and only the choice against `WARP_SKIP` has a rate term. That
term is now `L.sad * 16 bits` instead of a hand-written `2 * qstep * 8`.

Quarter-pel refinement by real RD (`--mv-rd-qpel on`, on at `slow`) re-scores
the winner's four quarter-pel neighbours by encoding them.

**The per-tile QP offset** was off by default because it cost 3 to 10x encode
time for under 0.5 %. Two things make it affordable: a candidate is now a
*copy* of the tile with `requant_params()` re-run rather than a reload and a
second colour transform, because the samples, the prediction and the residual
do not depend on the QP; and the offset is searched by a bounded descent —
0, then the better of -1 and +1, then keep walking that way while the cost
falls — instead of exhaustively. `D + lambda*R` is convex in the offset over
the range that matters.

| | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) | encode time |
|---|---|---|---|
| `--preset slow` | -17.37 % | -6.71 % | 7.30x |
| `--preset slow --qp-search off` | -17.09 % | -6.18 % | 4.01x |

It is now worth 0.5 points for 1.8x rather than 0.5 points for 3-10x, which
is why it is on at `slow` and still off below it.

---
