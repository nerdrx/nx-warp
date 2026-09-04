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

### 3.1 The same weight at 4:2:0, and why it is not the same number

The 6:1:1 convention weighs plane MEANS. At 4:4:4 that makes one chroma
SAMPLE worth 1/6 of a luma sample; at 4:2:0 a chroma plane holds a quarter of
the samples, so one chroma sample stands for four luma samples and is worth
4/6 of one. Stating the weight per sample and using the same 0.25 in both
formats is therefore not the same statement, and it shows:

| `--chroma-weight`, 4:2:0 | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) |
|---|---|---|
| 0.25 | -8.00 % | **+4.62 %** |
| 0.5 | -4.79 % | +0.39 % |
| **1.0** | -0.96 % | **-0.81 %** |
| 1.5 | +0.97 % | -0.45 % |

`vr-mixed-512-v2` 4:2:0, 6 frames. 0.25 buys 8 % of PSNR-Y by making the
picture worse, which is exactly the failure mode section 3 warns about, and
the weighted metric catches it. The optimum is at 1.0 — which is 0.25 times
the four luma samples a 4:2:0 chroma sample covers, the same fitted constant
seen through the same convention. So the encoder states the weight per sample
at 4:4:4 density and scales it by that area
(`TileCoder::chroma_dist_weight`). Every 4:4:4 stream is byte-identical
either way; only subsampled chroma moves.

The honest reading of the two tables together: **the chroma weight is worth
about 4 to 5 points on the weighted metric at 4:4:4 and about 1 point at
4:2:0**, and the large PSNR-Y numbers at 4:4:4 are mostly the metric.

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

## 7. What each item is worth on the inter path

`vr-mixed-1024-v2` 4:4:4, 12 frames, `--eyes 2 --inter on`, QP 0/4/8/12 (band
A), BD-rate of the medium preset against the shipped encoder, then the same
with one item turned off. A row that is *better* than the "all of it" row is an
item that is costing quality on this material.

| | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) | encode time |
|---|---|---|---|
| **all of it (medium)** | **-13.03 %** | **-7.76 %** | 1.11x |
| `--mode-lambda 0.25` (the old mode lambda) | -10.19 % | -4.95 % | 1.24x |
| `--trellis-dc off` | -13.95 % | -8.55 % | 1.09x |
| `--chroma-weight 1.0` (the old chroma weight) | +1.83 % | +0.68 % | 1.13x |
| `--mv-rd-qpel off` | -11.08 % | -5.88 % | 0.91x |

Three readings, and the third is the uncomfortable one.

* **The chroma weight is the package.** Turn it off and the rest of the work is
  worth +1.8 % — i.e. nothing, and slightly worse than nothing. Every other
  item is measured against a baseline that already has it.
* **Unifying the mode lambda is worth 2.8 points** and *saves* time, and the
  quarter-pel RD refinement is worth 2 points for 20 % more time. Both are what
  they were meant to be.
* **The DC-plane trellis is worth -0.9 points here**, i.e. it is a small loss on
  the inter path while being a small gain on the intra path (section 3.2). It
  is left on because the intra path is the one the Phase 1 gate is stated on
  and because the two effects are the same size; a future revision that gates
  it on tile mode has a measurement to beat.

---

## 8. The Phase 1 gate

`vr-mixed-1024-v2`, 6 frames, against `x264 --keyint 1 --tune zerolatency` over
the 100-400 Mbit band. "before" is the shipped encoder built from
`e4e85af`; "after" is this branch at its default (`--preset medium`). Both
columns are the same harness, the same sequence and the same anchor run.

| | 4:4:4 before | 4:4:4 after | 4:2:0 before | 4:2:0 after |
|---|---|---|---|---|
| BD-rate vs x264-intra, PSNR-Y | +63.94 % | **+42.39 %** | +40.03 % | **+38.92 %** |
| BD-rate vs x264-intra, SSIM-Y | +105.17 % | **+71.22 %** | +72.96 % | **+65.41 %** |
| worst delta in band | -6.545 dB | **-5.994 dB** | -6.133 dB | -6.233 dB |
| mean delta in band | -5.552 dB | **-4.564 dB** | -4.981 dB | **-4.881 dB** |
| gate | FAIL | FAIL | FAIL | FAIL |

**BD-rate of this branch against the shipped encoder**, same material, same
rate axis:

| | PSNR-Y | PSNR-YCbCr (6:1:1) | SSIM-Y |
|---|---|---|---|
| `vr-mixed-1024-v2` 4:4:4 | **-12.89 %** | **-5.27 %** | **-12.51 %** |
| `vr-mixed-1024-v2` 4:2:0 | **-0.96 %** | **-0.83 %** | **-1.50 %** |

The verdict lines, verbatim:

```
  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -6.545 dB at 266.4 Mbit/s, mean -5.552 dB over 266.4-400.0 Mbit/s   (4:4:4, before)
    FAIL: worst -5.994 dB at 233.8 Mbit/s, mean -4.564 dB over 233.8-400.0 Mbit/s   (4:4:4, after)
    FAIL: worst -6.133 dB at 257.5 Mbit/s, mean -4.981 dB over 257.5-400.0 Mbit/s   (4:2:0, before)
    FAIL: worst -6.233 dB at 254.9 Mbit/s, mean -4.881 dB over 254.9-400.0 Mbit/s   (4:2:0, after)
```

**The gate is still not met, and this package does not come close to meeting
it.** It takes 21.5 BD-rate points off a 64-point deficit at 4:4:4 and one
point off a 40-point deficit at 4:2:0. `RESULTS-intra.md`'s conclusion stands
unchanged: the remaining gap has no single dominant term and no encoder-side
tool closes it.

**The -10 % target is met at 4:4:4 and missed at 4:2:0.** The 4:2:0 number is
small for a reason that is now understood rather than mysterious: the largest
item in the package is the chroma distortion weight, and at 4:2:0 the fitted
weight is 1.0 — which is what the encoder was already doing (section 3.1). At
4:2:0 this package is the *other* six items, and they are worth about a point.

---

## 9. The Phase 2 kill test

`vr-mixed-1024-v2`, 12 frames, `--eyes 2 --inter on --poses ...`, against
`x265-p` (zerolatency, P-only, one reference, one IDR). Band A is the literal
100-300 Mbit band; band B is the paper's own bits per pixel
(`RESULTS-inter.md` section 1).

| | before | after |
|---|---|---|
| band A, 4:4:4, BD-rate vs x265-p | +383.41 % | **+311.60 %** |
| band A, 4:2:0, BD-rate vs x265-p | +306.78 % | **+295.42 %** |
| band B, 4:2:0, BD-rate vs x265-p | +518.73 % | **+417.27 %** |

**BD-rate of this branch against the shipped encoder**, same material:

| | PSNR-Y | PSNR-YCbCr | SSIM-Y |
|---|---|---|---|
| band A, 4:4:4 | **-15.67 %** | **-10.32 %** | **-28.61 %** |
| band A, 4:2:0 | **-4.32 %** | **-6.84 %** | **-18.74 %** |
| band B, 4:2:0 | **-15.92 %** | **-16.38 %** | **-16.57 %** |

The verdict, verbatim, band A 4:4:4 before and after:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +393.35 % (allowed up to +10 %)  FAIL     <- before
    on motion : BD-rate +354.41 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL

    at rest   : BD-rate +314.14 % (allowed up to +10 %)  FAIL     <- after
    on motion : BD-rate +303.89 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

and band B 4:2:0:

```
    at rest   : BD-rate +528.66 % (allowed up to +10 %)  FAIL     <- before
    on motion : BD-rate +488.96 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL

    at rest   : BD-rate +423.03 % (allowed up to +10 %)  FAIL     <- after
    on motion : BD-rate +399.58 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

**FAIL before and FAIL after, in every band and both formats.** The -10 %
target is met on band A 4:4:4 and band B 4:2:0 and missed on band A 4:2:0,
where the chroma weight has nothing to give and the frame is majority INTRA
(`RESULTS-inter.md` section 4).

One thing worth recording because it is larger than the headline: **SSIM-Y
improves by 17 to 29 % of BD-rate on the inter path**, two to six times the
PSNR-Y figure. The trellis and the mode decision are spending their bits on
structure rather than on the last fraction of a decibel, which is what a
Lagrangian on squared error is *not* supposed to reward. It is reported
because it was measured, not because it was aimed at.

---

## 10. Encode and decode time

**Every number in this document was produced on a machine running eight to
twelve other encodes.** Rate and quality do not care; wall-clock time does, so
the times below are stated as ratios measured back-to-back against the shipped
encoder on the same loaded machine, and the absolute milliseconds are an upper
bound rather than a measurement of this code.

`vr-mixed-1024-v2` 4:4:4, 12 frames, `--eyes 2 --inter on`, one core:

| QP | encode, before | encode, after | decode, before | decode, after |
|---|---|---|---|---|
| 0 | 2268 ms/frame | 2584 ms/frame | 78.8 ms/frame | 84.0 ms/frame |
| 4 | 1978 ms/frame | 2158 ms/frame | 76.3 ms/frame | 87.1 ms/frame |
| 8 | 1489 ms/frame | 1586 ms/frame | 74.1 ms/frame | 71.9 ms/frame |
| 12 | 1214 ms/frame | 1388 ms/frame | 63.3 ms/frame | 72.2 ms/frame |

**Encode is 1.11x at the default preset on the inter path and 0.88x on the
intra path** — the intra path is *faster* than the encoder it replaces,
because the trellis's exact scan truncation (section 4) and the INTRA
early-out (`kIntraGate`) save more than the DC-plane trellis and the extra
directional candidate cost. **Decode is unchanged**: nothing in this package
is visible to the decoder, and the small movements above are the machine.

The ladder, on `vr-turn-256-v2` 4:4:4 intra, BD-rate against the shipped
encoder and encode time relative to it:

| preset | BD-rate (PSNR-Y) | BD-rate (PSNR-YCbCr) | encode time |
|---|---|---|---|
| `fast` | -11.96 % | -1.42 % | 0.52x |
| `medium` | -15.78 % | -4.98 % | 0.88x |
| `slow` | -17.37 % | -6.71 % | 7.30x |

Those three rows were measured before `kDcPropagation` was raised from 16 to
32 (section 3.2), which costs the medium row 0.7 points; the ladder's shape
and its times are unaffected. **The budget was "under 3x today's": `fast` and
`medium` are under 1x. `slow` is 7.3x and is not offered as a default.**

---

## 11. What was NOT measured

Stated because leaving it out would misrepresent the coverage:

* `vr-turn-256-v2` and `vr-mixed-512-v2` were used for the fits in sections 2,
  3, 5 and 6 but **not** run through `compare.py` against an anchor. The gate
  and kill-test tables are `vr-mixed-1024-v2` only.
* Band B at **4:4:4** was not run.
* VMAF was disabled (`--no-vmaf`) on every run in sections 8 and 9.
* The `--preset` ladder was measured on the intra path only; the inter path
  has `medium` and its components (section 7) but not `fast` and `slow`.
* No timing was taken on a quiet machine.

---

## 12. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF -DNXWARP_BUILD_WARP=ON
cmake --build build-ref -j4

# the whole before/after set; TAG names the run, EXTRA is appended to nxv-enc
NXQ_BIN=$PWD/build-base/bin ./tools/quality/tourney-rdo.sh base   # the shipped encoder
./tools/quality/tourney-rdo.sh rdoa                               # this branch
$NXQ_SCRATCH/venv/bin/python tools/quality/tourney-table.py

# the tuning loop: one sequence, one QP ladder, BD-rate against a previous run
$NXQ_SCRATCH/venv/bin/python tools/quality/rdsweep.py \
    --seq $NXQ_SCRATCH/seq/vr-turn-256-v2.yuv444p.json --frames 6 --qp 0,8,16,24 \
    --enc "build-ref/bin/nxv-enc --quiet" --dec "build-ref/bin/nxv-dec --quiet" \
    --out $NXQ_SCRATCH/results/tourney/x.json \
    --vs $NXQ_SCRATCH/results/tourney/sw-base-turn.json

$NXQ_SCRATCH/venv/bin/python ref/phase2_verdict.py \
    --results $NXQ_SCRATCH/results/tourney/kA-*.json
```

Everything under `chrt -i 0 taskset -c 8-11 nice -n 19`. Result files are in
`$NXQ_SCRATCH/results/tourney/`. **Build the encoder once and freeze it before
measuring**: an encoder rebuilt while `compare.py` is running produces a curve
whose points came from two different binaries, which is how the first pass of
section 9 produced a 24-point regression that did not exist.

Conformance: `ctest --test-dir build-ref -R 'ref\.'`, and the same suite under
`--preset asan-ubsan`, both green; `ctest -R 'fuzz\.'` green;
`tests/vectors/vectors.md5` and `rejects.md5` regenerated, because an
encoder-side change moves every vector's bitstream while changing no syntax.
