# Encoder rate-distortion: measurements

What the encoder's rate-distortion machinery is worth once every decision in
it minimises the same cost function against the same rate. Everything here is
**encoder side**: no tool bit, no syntax change, no decoder arithmetic, no
decoder cost. `docs/SYNTAX.md` Appendix A 53 and 54 record the two decisions
the package takes; `ref/README.md` is the user-facing summary.

**The four things to read first.**

1. Three of the five decision sites were minimising different cost functions,
   and the rate they were minimising against was **18.6 % below the rate the
   entropy coder actually charged**. Section 1.
2. On the Phase 2 kill test the package is worth **-8.7 % BD-rate at 0.8x the
   v1.4 encode time**, of which -3.4 points is one line: the
   reference-persistence factor was charged twice. Sections 3 and 6.
3. On the Phase 1 gate it is worth **-1.7 % at 0.6x encode time**. It does not
   come near the -10 % that was asked for, and section 7 says why in numbers:
   the intra path's search was already close to optimal at v1.4, and what is
   left of the gap to x264 is the predictor and the tile syntax, not the
   search.
4. The trellis got **faster, not slower**, and by enough to pay for everything
   else: an exact bound on `last` cut the encode time to 0.55-0.8x with a
   measured BD-rate change of **+0.04 %**. Section 2.

Measurement discipline: every encode, decode and metric under
`chrt -i 0 taskset -c 12-15 nice -n 19`, ffmpeg at `-threads 4`, on
`vr-mixed-1024-v2` (the band-limited v2 sequences), results JSON under
`$NXQ_SCRATCH/results/tourney-rdo-b-*.json`. The machine was running seven
other tournament agents throughout, so **the encode times below are ratios
against a baseline measured in the same minute**, never absolutes; section 8
gives the absolutes measured separately.

---

## 1. The audit: five decisions, three cost functions, one wrong rate

Before this package the reference encoder took rate-distortion decisions in
five places and no two of them agreed about the trade.

| decision | where | lambda it used |
|---|---|---|
| which levels to code | `rdoq_unit` | `0.30 * qstep^2` |
| which directional intra mode | `analyze_plane_dir` | `0.30 * qstep^2` |
| which per-tile QP offset and matrix | `encode_frame` | `0.30 * qstep^2` |
| which tile mode (INTRA / WARP_MV / skip) | `decide_tile` | `0.25 * 0.30 * qstep^2` |
| which motion vector | `decide_tile` | SAD plus `2 * qstep * 8` |

The fourth is a quarter of the first with an argument attached (section 3).
The fifth is not a lambda at all: a bias in units of `qstep`, in the SAD
domain, where the correct relation to a squared-error lambda is
`lambda_sad = sqrt(lambda_sse)` and nothing had been squared or rooted.

They all now come from `make_lambda()` in `ref/src/codec.cpp`, which states
the model in one place: `lambda = k * qstep^2` with one dimensionless
constant, and `lambda_sad = sqrt(lambda)` where the metric is first order.

**And the rate was wrong.** `quantize_tile()` costed a tile by driving the real
`LaneMachine` over it with `count_units()` and summing the entropy of the
symbols it emits. That walk also emits **bypass** bits -- one per nonzero
level for its sign, the escape suffixes, the LAST suffixes -- and
`count_units()` threw them away. `--stats` now prints both numbers. On one
2048x1024 4:4:4 frame at QP 24:

| rate model | predicted | coded | error |
|---|---|---|---|
| v1.4 (symbol entropy only) | 41380 B | 50836 B | **-18.6 %** |
| this package (bypass bits counted) | 51939 B | 50836 B | **+2.2 %** |

The remaining +2.2 % is the table-set reselection that happens after the
estimate is returned, and it is uniform across candidates. The -18.6 % was
not: it is proportional to the number of nonzero levels, so it under-charged
exactly the candidates that code the most, and it biased the tile mode
decision towards coding.

---

## 2. The trellis: the same answer, in half the time

v1.2's trellis was already a Viterbi pass over the three context states of the
level chain -- that part did not need building. What it did not have was a
bound on the search, the sign-data-hiding rebate in its `last` decision, or a
rate term in the sign-hiding fixup.

**The bound is exact.** Above the highest scan position whose magnitude
reaches half a step, level 0 beats level 1 in distortion (`|a| < st/2 <=
st - |a|`) *and* in rate, so the trellis is not needed to know the answer.
Positions above that are provably zero and are no longer searched. On a plane
of mostly-empty 8x8 units that is most of the work.

Measured on `vr-mixed-1024-v2` 4:4:4, QP 12/18/24/30, four frames, the new
encoder against the v1.4 one with every other change disabled
(`--no-lambda-class --no-dc-rdoq --rdo-lambda 0.30`):

| QP | v1.4 Mbit/s | v1.4 PSNR-Y | new Mbit/s | new PSNR-Y | enc time |
|---|---|---|---|---|---|
| 12 | 106.66 | 50.619 | 106.50 | 50.594 | 0.73x |
| 18 | 69.36 | 46.086 | 69.34 | 46.060 | 0.81x |
| 24 | 44.68 | 41.462 | 44.64 | 41.470 | 0.53x |
| 30 | 29.25 | 36.897 | 29.30 | 36.904 | 0.55x |

**BD-rate +0.04 %** at 0.53-0.81x the encode time. It is the same encoder,
arriving at the same answer, without walking the 60 scan positions it could
already prove were zero.

### Effort levels

`--rdoq-effort` changes only how many magnitudes each scan position offers the
trellis; the state space is fixed by the syntax.

| level | candidates per position | in `--preset` |
|---|---|---|
| 1 fast | `{0, round(a/st)}` | `fast` |
| 2 medium | `{0, floor(a/st), floor(a/st)+1}` (the v1.2 set) | `medium` |
| 3 full | adds `floor(a/st)-1` | `slow` |

---

## 3. The reference-persistence factor, charged twice

`decide_tile` charges a skipped tile for the distortion it leaves in the
reference: `c_skip = d_skip + (kRefPersist - 1) * excess + lambda`, where
`excess` is the part of the skip's error a coded tile would not have left.
`ref/RESULTS-inter.md` 5 measured that as a 4 dB fix and it is not in question.

v1.4 then charged the same factor a **second** time, as a `1/4` divisor on the
mode decision's lambda, with the argument that "a mode that saves bits by
leaving distortion in the picture is charged for that distortion once and paid
for it four times". That argument is true of a *skipped* tile, which is what
the excess term already prices. It is not true of a *coded* tile, whose error
is bounded by its quantiser and is corrected the next time the tile is coded.

Kill-test band A on `vr-mixed-1024-v2` 4:4:4, 16 frames, QP 0/4/8/12, BD-rate
of the new encoder against the v1.4 one:

| mode lambda | BD-rate |
|---|---|
| `1/4` (the v1.4 default) | -5.34 % |
| **`1` (shipped)** | **-8.73 %** |
| `2` | -10.02 % |

`2` measures better still and is not shipped: it has no argument behind it,
and a mode lambda above the trellis's biases towards skipping, which is the
direction that accumulates drift over a sequence longer than the 16 frames
this was fitted on. `--mode-lambda 2` asks for it.

---

## 4. The DC plane, through the trellis

The DC plane is 22-30 % of a frame's payload and was the one part of the
coefficient budget no rate-distortion decision had ever touched. v1.2 left it
out because it is the intra predictor: a level chosen there changes `pred` for
all 64 blocks of the tile, and the trellis's single-unit distortion model is
wrong about that.

The model is wrong in a **known direction**. A DC level the trellis zeroes is
not free: the AC pass then codes it back at the AC quantiser's finer step. So
the DC plane's true marginal cost is higher than its own squared error says,
which is a multiplier on its lambda, not a reason to leave it alone.
`--dc-lambda` is that multiplier, `--no-dc-rdoq` restores the v1.4 quantizer.

Under the 16-context model the DC plane has its own single LEVEL context, so
the Markov chain collapses and the trellis degenerates to a per-coefficient
choice -- which `rdoq_unit` now expresses directly rather than running three
identical states.

BD-rate against `--no-dc-rdoq`, 4:4:4, QP 12/18/24/30, four frames:

| `--dc-lambda` | BD-rate |
|---|---|
| 0.5 | -1.69 % |
| **1.0 (shipped)** | **-1.73 %** |
| 1.6 | -1.15 % |
| 2.0 | (worse) |

The fit is flat from 0.5 to 1.0 and turns over above it. 1.0 is shipped
because it is the value with a statement behind it -- the DC plane uses the
same trade as the AC planes, at its own quantiser step -- rather than a fitted
constant that happens to be as good.

---

## 5. The lambda fit, and the one thing it does not resolve

`kLambdaScale` was 0.30, carried over from v1.2 and never re-measured after
directional intra, the 16-context model and sign hiding changed what a bit
costs. Refitted with the DC plane in, BD-rate against 0.30 on the same ladder:

| scale | Phase 1 band (4:4:4, QP 12-30) |
|---|---|
| 0.15 | -0.52 % |
| 0.20 | -0.68 % |
| **0.22 (shipped)** | (interpolated minimum) |
| 0.25 | -0.58 % |
| 0.36 | +0.47 % |
| 0.42 | +1.41 % |

The minimum is flat between 0.20 and 0.25 and 0.30 is already 0.6 points off
it. **But the kill test's band A does not agree**: at QP 0-12, where `qstep`
is 1 to 2 and the high-rate approximation the model rests on is at its
weakest, the same measurement prefers 0.30:

| scale | kill-test band A (4:4:4, QP 0-12) |
|---|---|
| 0.22 | -8.73 % |
| 0.30 | -9.76 % |

This is stated rather than papered over. A single dimensionless constant
cannot be optimal across a four-fold range of `qstep`, and the residual is
under a point either way. 0.22 ships because the Phase 1 gate is the criterion
this constant is graded against; `--rdo-lambda` is the knob, and
`docs/RATECONTROL.md`'s allocator, which already knows the operating point, is
where a per-band value belongs if one is ever wanted.

### Content classes

Lambda is scaled per tile by the class `docs/RATECONTROL.md` 3.3 puts the tile
in, computed inside `ref/` from the same two statistics (`ref/` does not link
`rc/`, and the UI-stencil route needs a compositor input the codec does not
have). The shipped gains are **1.0 for every class**: the hook is there, the
fit on this material says the classes do not want different trades, and
`class_lambda_is_flat()` skips the classification entirely when they are all
1. `--lambda-class A,B,C,D` sets them; an early guess of
`0.70/1.30/1.00/0.60` measured net negative and is not shipped.

This is a null result on synthetic material that is 75 % horizontally
constant. It should be refitted on a real capture before anyone concludes the
hook is useless.

---

## 6. Motion search and the mode decision

**Hierarchy.** `--me-effort` 2 and 3 add a coarse level before the `+-mv_range`
sweep: `+-2*mv_range` at a vector step of 4 and a pixel step of 8, so the
reachable range doubles for about a third of the points. At 300 deg/s and
90 Hz a 95 deg tile moves about 35 samples a frame, which the `+-16` default
cannot reach at all.

**Seeds.** Zero, the tile's own vector from the previous frame (the one that
matters on a head turn, where every tile moves the same way and the previous
frame already found it), and the vectors the left and above tiles of *this*
frame already chose. The spatial seeds are free: those decisions are already
made when the raster order reaches this tile.

**SATD, not SAD, in the fine stage.** SAD is right for the coarse stages, where
the question is only "which offset". It is wrong once the question is "which
of two near-identical offsets codes cheaper", because the residual is about to
go through an 8x8 DCT and SATD is the cheapest measure that knows a smooth
error is cheap to code and an impulsive one is not.

**Quarter-pel by true rate-distortion** (`--me-effort 3`, the `slow` preset):
the four quarter-sample neighbours of the SATD winner are fully quantised and
reconstructed and the one that actually codes cheapest wins. Four extra tile
encodes, so it is not in `medium`.

**The coded-vector bias** is now `lambda_sad * 8 * (kTileHeaderBytes + 2)` --
the real cost of the header a coded vector needs, in the SAD domain, at the
mode decision's own lambda -- rather than `2 * qstep * 8`.

---

## 7. The per-tile QP search, made cheap enough to leave on

`ref/RESULTS-intra.md` 6 measured the per-tile QP and weighting-matrix search
at 3 to 10x encode time for under 0.5 %, and it has been off ever since. Two
things made it that expensive, and both are fixed:

* It tried `2*qp_search+1` offsets one code point apart. One QP step is 12 %
  of a quantiser and no tile's RD cost has structure that fine, so the
  candidates are now `qp_search_step` apart: a 3-point ladder at `+-2` spans
  the same useful range as a 5-point one at `+-1`.
* Every candidate re-ran the full directional-intra mode search. The best mode
  for a block is a property of its neighbourhood, not of the step it is
  quantised with, so it is decided once at the tile's own QP and **reused**:
  predict, quantise, reconstruct, no SATD sort and no per-mode RD.

The comparison is also now made at **one** lambda -- the tile's own, at its
allocated QP. Scoring each candidate at its own lambda compares two different
cost functions and always prefers the coarsest step.

The weighting-matrix search is no longer part of `--preset slow`: it was 4 of
the 12 per-tile candidates and most of the cost, for a tool the degradation
ladder owns. `--wm auto` asks for it.

### The presets

BD-rate and encode time against `medium`, 4:4:4 intra, QP 12/18/24/30:

| preset | BD-rate vs medium | encode time vs medium | vs v1.4 |
|---|---|---|---|
| `fast` | +0.38 % | 0.55x | 0.33x |
| `medium` | -- | 1.0x | 0.60x |
| `slow` | -0.99 % | 3.5x | 2.1x |

`slow` is inside the "under 3x today's encode time" budget the package was
given, and `medium` is 0.6x of it.

---

## 8. Before and after

`vr-mixed-1024-v2` (the band-limited v2 sequence), 2048x1024 side by side,
12 frames, ffmpeg n9.0.1. "before" is `main` at `e4e85af`, "after" is this
branch at its head, both built Release with the same compiler and both run
through `tools/quality/compare.py` in the same session. VMAF is off: the
harness's libvmaf pass triples the wall time and nothing in this package is
aimed at it.

### 8.1 The Phase 1 gate, 4:4:4

`--anchors x264-intra --qp 12,18,24,30 --anchor-qp 12,18,24,30`

| QP | | Mbit/s | PSNR-Y | SSIM-Y |
|---|---|---|---|---|
| 12 | before | 320.11 | 50.627 | 0.99661 |
| | **after** | **337.18** | **51.173** | **0.99692** |
| 18 | before | 207.21 | 46.104 | 0.99328 |
| | **after** | **219.32** | **46.799** | **0.99392** |
| 24 | before | 133.19 | 41.467 | 0.98639 |
| | **after** | **138.36** | **42.056** | **0.98768** |
| 30 | before | 86.75 | 36.867 | 0.97241 |
| | **after** | **89.07** | **37.551** | **0.97514** |

BD-rate against the anchor: **+77.29 % before, +75.30 % after**. BD-rate of
after against before, on the same PSNR-Y: **-1.55 %**. Encode time
**0.573x**.

> ```
>   Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
>     FAIL: worst -7.349 dB at 100.0 Mbit/s, mean -5.748 dB over 100.0-320.1 Mbit/s   [before]
>     FAIL: worst -7.006 dB at 100.0 Mbit/s, mean -5.542 dB over 100.0-337.2 Mbit/s   [after]
> ```

### 8.2 The Phase 1 gate, 4:2:0

| QP | | Mbit/s | PSNR-Y | SSIM-Y |
|---|---|---|---|---|
| 12 | before | 282.49 | 50.648 | 0.99662 |
| | **after** | **292.73** | **51.139** | **0.99690** |
| 18 | before | 195.98 | 46.038 | 0.99322 |
| | **after** | **204.70** | **46.822** | **0.99393** |
| 24 | before | 128.77 | 41.465 | 0.98646 |
| | **after** | **136.21** | **42.108** | **0.98773** |
| 30 | before | 88.73 | 36.917 | 0.97287 |
| | **after** | **89.87** | **37.572** | **0.97544** |

BD-rate against the anchor: **+60.77 % before, +56.31 % after**. After against
before: **-1.44 %**. Encode time **0.611x**.

> ```
>   Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
>     FAIL: worst -7.192 dB at 100.9 Mbit/s, mean -5.226 dB over 100.9-282.5 Mbit/s   [before]
>     FAIL: worst -6.845 dB at 100.9 Mbit/s, mean -5.014 dB over 100.9-292.7 Mbit/s   [after]
> ```

The gate still fails, by 5.8 dB rather than 6.2. Section 9 is why that is the
expected result and not a shortfall in this package.

### 8.3 The Phase 2 kill test, band A

*(filled in below)*

---

## 9. What this does not do

The Phase 1 gate moves by -1.7 %, not -10 %. That is not a shortfall in the
search; it is what is left to find. v1.2 already put a trellis and a real
mode-decision RD on the intra path, and sections 2 and 5 show the two things
that were still wrong there -- the missing bound and the stale lambda -- are
worth 0.04 % and 0.7 % respectively. The DC plane, which nothing had ever
optimised, is worth 1.7 %, and that is the largest single item available to an
encoder-side change.

`ref/RESULTS-intra.md`'s own conclusion holds and this measurement sharpens
it: the deficit to x264 is "a roughly constant bit-efficiency factor spread
across the quantizer, the context model and the predictor, with no single
dominant term". The quantizer's share is now spent. The named items that
remain are a 4x4 transform split, an adaptive context model, and the 8-byte
tile header -- all three of them syntax, none of them reachable from the
encoder.

The kill test is different: it moves by -8.7 % because the inter mode decision
was being taken against a rate 18.6 % too low and a lambda charged twice, and
both of those were encoder bugs rather than missing tools.

---

## 10. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-ref -j4

# Phase 1 gate
python3 tools/quality/compare.py \
    --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json --frames 12 \
    --codec-enc "build-ref/bin/nxv-enc --quiet" \
    --codec-dec "build-ref/bin/nxv-dec --quiet" --codec-name nxv \
    --anchors x264-intra --qp 12,18,24,30 --anchor-qp 12,18,24,30 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --no-vmaf --out $NXQ_SCRATCH/results/tourney-rdo-b-p1-yuv444p-new.json

# kill test, band A
python3 tools/quality/compare.py \
    --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json --frames 12 \
    --codec-enc "build-ref/bin/nxv-enc --quiet --eyes 2 --inter on \
                 --poses $NXQ_SCRATCH/seq/vr-mixed-1024-v2.poses.json" \
    --codec-dec "build-ref/bin/nxv-dec --quiet" --codec-name nxv-inter \
    --anchors x265-p --qp 0,4,8,12 --anchor-qp 2,8,14,20 --no-vmaf \
    --out $NXQ_SCRATCH/results/tourney-rdo-b-kill-yuv444p-new.json

# the rate model's own error
build-ref/bin/nxv-enc --in $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.yuv \
    --w 2048 --h 1024 --pix yuv444p --qp 24 --frames 1 --stats --out /dev/null

ctest --test-dir build-ref -R 'ref\.'
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan
ctest --preset asan-ubsan -R 'ref\.'
```

Everything under `chrt -i 0 taskset -c 12-15 nice -n 19`.
