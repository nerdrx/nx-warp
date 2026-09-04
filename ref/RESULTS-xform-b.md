# Larger transforms (tool bit 24 `XFORM_LARGE`): measurements

Everything here was produced by `tools/quality/compare.py` through ffmpeg
n9.0.1, every process under `chrt -i 0 taskset -c 8-11 nice -n 19`, on the
**v2 (band-limited) sequences**, `vr-mixed-1024-v2` at 2048x1024 side by side,
all 36 frames, 90 fps. Result files are under
`$NXQ_SCRATCH/results/tourney-xform-b/`. Section 8 reproduces every number.

The tool is `docs/SYNTAX.md` 6.7 and 7.7: a per-tile 16x16 or 32x32 integer
DCT, chosen by the encoder's rate-distortion search, behind tool bit 24. It is
off by default; `nxv-enc --xform 32` turns it on. Every stream a v1.4 build
wrote is byte-identical, and `v01`-`v56` of the conformance set regenerate
unchanged.

**A note on the machine.** The comparison runs shared the box with five other
codec experiments (load average 25-34 on 32 cores). Rate and quality are
unaffected -- they are deterministic and every run used the same 4-core slice
-- and the timings in section 6 were **re-measured afterwards on an idle
machine** (load 6), because the first attempt at them under load was wrong by a
factor of two in the decoder's disfavour.

---

## 0. What it buys, in one table

`before` is the shipped default. `after` is the same encoder with
`--xform 32`, which sets tool bit 24 and adds 16x16 and 32x32 to the per-tile
RD search. Nothing else differs, in either direction.

| measurement | anchor | before | after | change |
|---|---|---|---|---|
| Phase 1 gate, 4:4:4 | x264-intra | +61.43 % | **+48.24 %** | **-13.19 points** |
| Phase 1 gate, 4:2:0 | x264-intra | +38.17 % | **+26.69 %** | **-11.48 points** |
| Phase 2 kill test, band A | x265-p | +342.67 % | **+309.91 %** | **-32.76 points** |
| Phase 2 kill test, band B | x265-p | +548.23 % | **+488.19 %** | **-60.04 points** |

| gate deficit (PSNR-Y, worst point in band) | before | after | change |
|---|---|---|---|
| 4:4:4 | -4.534 dB | **-3.545 dB** | **+0.99 dB** |
| 4:2:0 | -3.296 dB | **-2.313 dB** | **+0.98 dB** |

Both gates still **FAIL** and both kill tests still **FAIL**; the verdicts are
quoted verbatim in sections 1 and 2. What the tool does is move the intra gate
by **a hair under a decibel** on both chroma formats, which against
`INTRA_DIR`'s 1.89 / 1.69 dB (`ref/RESULTS-intra.md` section 0) makes it the
second largest single tool in this codec, and it is worth **more** with inter
on than off -- which is what the residual of a warped tile being smooth
predicts.

Cost: **no extra LDS and no extra dependent steps** for a GPU decoder
(section 7), a reference decode 0 to 24 % slower depending on how many tiles
take the large sizes, and a reference encode about 2.2x slower because the
per-tile search evaluates three transform sizes where it evaluated one
(section 6).

---

## 1. The Phase 1 gate, intra

`before` is the shipped default (`nxv-enc`, tools 17, 21 and 22 on, no tool bit
24). `after` is the same encoder with `--xform 32`, which sets tool bit 24 and
adds 16x16 and 32x32 to the per-tile RD search. Nothing else differs.

### 4:4:4

| | BD-rate vs x264 intra | BD-PSNR | worst deficit | mean deficit | verdict |
|---|---|---|---|---|---|
| before | +61.43 % | -4.281 dB | -4.534 dB at 103.7 Mbit/s | -3.717 dB | FAIL |
| **after** | **+48.24 %** | **-3.480 dB** | **-3.545 dB at 103.7 Mbit/s** | **-3.115 dB** | FAIL |
| change | **-13.19 points** | **+0.80 dB** | **+0.99 dB** | **+0.60 dB** | -- |

Verbatim, `before`:

```
  BD-rate of nxv-before on PSNR-Y (negative is better):
    vs x264-intra     +61.43 %   BD-PSNR -4.281 dB   (overlap 47.04-57.21 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -4.534 dB at 103.7 Mbit/s, mean -3.717 dB over 100.0-215.0 Mbit/s
```

and `after`:

```
  BD-rate of nxv-after on PSNR-Y (negative is better):
    vs x264-intra     +48.24 %   BD-PSNR -3.480 dB   (overlap 47.04-57.16 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.545 dB at 103.7 Mbit/s, mean -3.115 dB over 100.0-215.0 Mbit/s
```

**The gate is still not met** -- it needs 1.0 dB and the deficit is 3.5 -- but
it moved by **0.99 dB at the worst point**, which against `INTRA_DIR`'s own
1.89 dB (`ref/RESULTS-intra.md` section 0) is the second largest single tool
this codec has. On SSIM-Y the BD-rate goes from +103.25 % to +91.38 %, so the
metrics agree about the direction and the size.

### Operating points, 4:4:4

| QP | before Mbit/s | before PSNR-Y | before SSIM-Y | | after Mbit/s | after PSNR-Y | after SSIM-Y |
|---|---|---|---|---|---|---|---|
| 0 | 251.6 | 57.209 | 0.99887 | | **241.5** (-4.0 %) | 57.164 | 0.99886 |
| 4 | 190.0 | 55.287 | 0.99837 | | **181.5** (-4.5 %) | **55.359** | 0.99836 |
| 8 | 141.6 | 53.292 | 0.99776 | | **132.0** (-6.8 %) | **53.381** | 0.99769 |
| 12 | 106.7 | 50.594 | 0.99664 | | **99.5** (-6.7 %) | **50.925** | **0.99666** |
| 16 | 80.3 | 47.594 | 0.99464 | | **74.9** (-6.7 %) | **47.984** | **0.99477** |
| 20 | 59.2 | 44.507 | 0.99156 | | **55.4** (-6.5 %) | **44.965** | **0.99180** |
| 24 | 44.4 | 41.438 | 0.98664 | | **41.3** (-7.0 %) | **41.825** | **0.98687** |

Every point is **both smaller and better**, at every QP but 0, where it is 4 %
smaller for 0.045 dB. That is the shape a transform tool should have: it is not
trading quality for rate anywhere, it is coding the same residual with fewer
symbols. The gain grows from 4 % at QP 0 to 7 % from QP 8 down, which is the
same trend as the size histogram in section 5 -- the coarser the quantizer, the
more tiles choose a large transform and the more each one saves.

VMAF was not measured: the machine was running five other codec experiments and
`libvmaf` at 2048x1024 dominated the wall clock, so `--no-vmaf` is on every run
here (as it is on `ref/RESULTS-inter.md`'s own runs). PSNR-Y and SSIM-Y agree
on both the sign and the magnitude, and the gate is defined on PSNR-Y.

### 4:2:0

| | BD-rate vs x264 intra | BD-PSNR | worst deficit | mean deficit | verdict |
|---|---|---|---|---|---|
| before | +38.17 % | -3.290 dB | -3.296 dB at 101.1 Mbit/s | -2.356 dB | FAIL |
| **after** | **+26.69 %** | **-2.484 dB** | **-2.313 dB at 102.1 Mbit/s** | **-1.804 dB** | FAIL |
| change | **-11.48 points** | **+0.81 dB** | **+0.98 dB** | **+0.55 dB** | -- |

Verbatim, `before`:

```
  BD-rate of nxv-before on PSNR-Y (negative is better):
    vs x264-intra     +38.17 %   BD-PSNR -3.290 dB   (overlap 46.99-57.22 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.296 dB at 101.1 Mbit/s, mean -2.356 dB over 100.0-200.7 Mbit/s
```

and `after`:

```
  BD-rate of nxv-after on PSNR-Y (negative is better):
    vs x264-intra     +26.69 %   BD-PSNR -2.484 dB   (overlap 46.99-57.16 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -2.313 dB at 102.1 Mbit/s, mean -1.804 dB over 100.0-190.6 Mbit/s
```

### Operating points, 4:2:0

| QP | before Mbit/s | before PSNR-Y | before SSIM-Y | | after Mbit/s | after PSNR-Y | after SSIM-Y |
|---|---|---|---|---|---|---|---|
| 0 | 200.7 | 57.218 | 0.99887 | | **190.6** (-5.0 %) | 57.162 | 0.99886 |
| 4 | 153.8 | 55.291 | 0.99837 | | **144.3** (-6.2 %) | **55.293** | 0.99832 |
| 8 | 119.4 | 53.286 | 0.99776 | | **110.6** (-7.4 %) | **53.324** | 0.99764 |
| 12 | 94.2 | 50.614 | 0.99665 | | **87.5** (-7.1 %) | **50.954** | 0.99665 |
| 16 | 74.3 | 47.611 | 0.99464 | | **68.8** (-7.4 %) | **47.992** | **0.99476** |
| 20 | 56.3 | 44.481 | 0.99155 | | **52.6** (-6.6 %) | **44.961** | **0.99177** |
| 24 | 42.9 | 41.444 | 0.98674 | | **40.1** (-6.6 %) | **41.828** | **0.98694** |

The same shape as 4:4:4: 5 to 7.4 % of the rate for the same or better PSNR at
every point. The tool helps 4:2:0 slightly less in BD-rate points than 4:4:4
because half the chroma samples are gone before the transform sees them, and
because a 4:2:0 tile's 32-sample chroma plane is a *single* 32x32 block whose
choice the RD then has to make for the whole plane at once.

---

## 2. Inter on: the Phase 2 kill test

`nxv-enc --eyes 2 --inter on --poses ...`, against `x265-p`, in the two rate
bands `ref/RESULTS-inter.md` defines, `--no-vmaf` as that document's own runs
use. `after` adds `--xform 32` and nothing else.

| band | | BD-rate vs x265-p | BD-PSNR | at rest | on motion | verdict |
|---|---|---|---|---|---|---|
| A | before | +342.67 % | -7.053 dB | +344.77 % | +334.59 % | FAIL |
| A | **after** | **+309.91 %** | **-6.525 dB** | **+312.12 %** | **+301.54 %** | FAIL |
| A | change | **-32.76 points** | **+0.53 dB** | -32.65 | -33.05 | -- |
| B | before | +548.23 % | -11.472 dB | +558.25 % | +513.93 % | FAIL |
| B | **after** | **+488.19 %** | **-10.908 dB** | **+496.52 %** | **+459.37 %** | FAIL |
| B | change | **-60.04 points** | **+0.56 dB** | -61.73 | -54.56 | -- |

Verbatim, band A `after` (`ref/phase2_verdict.py`):

```
=== vr-mixed-1024-v2.yuv444p  (kill-mixed1024-444-A-after.json)
  codec nxv-inter-after against x265-p, PSNR-Y
  velocity split at the 20th percentile = 43.4 deg/s (8 of 36 frames)
    overall (all frames)          BD-rate +309.91 %  BD-PSNR -6.525 dB
    fastest 20 % of frames        BD-rate +301.54 %  BD-PSNR -6.307 dB
    the remaining frames          BD-rate +312.12 %  BD-PSNR -6.587 dB
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +312.12 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +301.54 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

and band B `after`:

```
=== vr-mixed-1024-v2.yuv444p  (kill-mixed1024-444-B-after.json)
  codec nxv-inter-after against x265-p, PSNR-Y
  velocity split at the 20th percentile = 43.4 deg/s (8 of 36 frames)
    overall (all frames)          BD-rate +488.19 %  BD-PSNR -10.908 dB
    fastest 20 % of frames        BD-rate +459.37 %  BD-PSNR -10.664 dB
    the remaining frames          BD-rate +496.52 %  BD-PSNR -10.977 dB
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +496.52 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +459.37 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

The `before` verdicts are the same two FAILs at +344.77 / +334.59 and
+558.25 / +513.93; the kill test is nowhere near passing in either
configuration and this tool was never going to change that -- section 3 of
`ref/RESULTS-inter.md` already establishes that most of the gap is not the
inter path. What the numbers say is the thing this package was asked to check:
**the tool is worth more with inter on than with it off**, 33 and 60 BD-rate
points against 13 and 11, on the same sequence and the same encoder.

That is the prediction that motivated measuring it. A warped tile's residual is
the difference between the picture and a resampled, homography-warped copy of
itself: smooth, low-frequency, and spread over the whole tile rather than
localised at edges -- exactly the signal a 32x32 DCT compacts and an 8x8 one
does not.

### Operating points, inter, band A

| QP | before Mbit/s | before PSNR-Y | | after Mbit/s | after PSNR-Y |
|---|---|---|---|---|---|
| 0 | 204.0 | 57.032 | | **193.7** (-5.0 %) | 56.981 |
| 4 | 136.3 | 55.049 | | **128.1** (-6.0 %) | **55.105** |
| 8 | 89.6 | 52.771 | | **83.6** (-6.7 %) | **52.870** |
| 12 | 61.1 | 49.835 | | **56.1** (-8.1 %) | **50.040** |

### Operating points, inter, band B

| QP | before Mbit/s | before PSNR-Y | | after Mbit/s | after PSNR-Y |
|---|---|---|---|---|---|
| 18 | 30.3 | 44.766 | | **28.3** (-6.7 %) | **44.927** |
| 24 | 13.1 | 39.803 | | **12.2** (-6.7 %) | **39.995** |
| 30 | 6.2 | 35.225 | | **5.9** (-6.2 %) | **35.335** |
| 36 | 3.4 | 30.608 | | **3.1** (-6.6 %) | **30.843** |

Every inter point is smaller and better too, down to 3.1 Mbit/s.

---

## 3. The LEVEL band floor

`docs/SYNTAX.md` 9.3 gives a coefficient group other than the one holding the
block's DC a **band floor** of 3: its LEVEL band is `max(band(pos), 3)`. This
is the whole of the larger transforms' entropy mapping, and the question it
answers is whether reusing the four existing bands unchanged (floor 0) is good
enough.

Measured by rebuilding with `group_band_min()` returning 0, 2 and 3, on
`vr-mixed-512-v2` 4:2:0, 3 frames, `--xform 32`, bytes of the whole stream:

| floor | QP 8 | QP 16 | QP 24 |
|---|---|---|---|
| 0 (reuse the bands as they are) | 165468 | 101288 | 57500 |
| 2 | 165464 | **101070** | 57504 |
| 3 (**shipped**) | 165464 | **101070** | 57504 |

Floors 2 and 3 are **identical**, and not by accident: `kLevelCtx` maps bands 2
and 3 to the same three LEVEL contexts, so any floor at or above 2 selects the
same statistics. Against floor 0 the floor is worth **0.22 %** at QP 16 and
nothing at either end -- four bytes better at QP 8, four worse at QP 24.

That is a small number, and it is reported as one. The floor stays in because
it is one `max()`, it is inert (`band_min == 0`) on every version 1 unit so it
adds no arithmetic to an existing stream, and it never loses. Floor 3 rather
than 2 is written because it says what it means: those coefficients are the
high-frequency part of the block whatever their position inside their group.
**No context was added**, which is what the alternative would have cost.

---

## 4. What the DC plane costs at 32x32

`docs/SYNTAX.md` 7.7 keeps the DC plane on the **8x8 grid at every transform
size**: a 32x32 tile still codes 64 block means, not 4, so 7.1-7.3, the
resampling kernel and every conformance vector's predictor are bit-identical to
version 1. The obvious objection is that the tile then codes its low
frequencies twice -- once in the DC plane and again in the transform's low
coefficients.

`nxv-enc --stats`, one frame of `vr-mixed-512-v2` 4:2:0, 128 tiles:

| | QP 8 | QP 16 | QP 24 | QP 32 |
|---|---|---|---|---|
| total, `--xform 8` | 56414 B | 34718 B | 19470 B | 11352 B |
| total, `--xform 32` | **55102 B** | **33756 B** | **19120 B** | **11062 B** |
| DC planes, `--xform 8` | 7731 B | 6551 B | 4834 B | 3681 B |
| DC planes, `--xform 32` | **6906 B** | **6070 B** | **4448 B** | **3294 B** |
| DC share of the stream at `--xform 32` | 12.5 % | 18.0 % | 23.3 % | 29.8 % |

The objection does not survive the measurement. The DC plane does not cost
*more* alongside a large transform; at every QP it costs **7-11 % less**, and
the residual costs less as well. Nothing about the DC plane's own coding
changed, so the difference is the entropy model: a large-transform tile's
symbol histogram picks a different probability table set, and the DC plane
rides on that.

So the two channels are not fighting. The DC plane at 1/8 resolution and half
the QP index (`qp >> 1`) is a cheap, well-quantized low-frequency layer, and
the transform is left to code what is above it -- which is exactly the regime a
32x32 DCT is good at.

Re-gridding the DC plane onto the transform (4 means for a 32x32 tile) was
**not** built. It is not a transform size, it is a different intra predictor:
it changes the block-mean grid, the planar interpolation's sample positions,
the DC plane's own second-level 8x8 DCT and the resampling kernel's input
extent, and it would need its own tool bit, its own vectors and its own
measurement. What is measured here is the price of not doing it, and the price
is negative.

---

## 5. Which size the tiles choose

Per-tile `xform` histogram from `nxv-info --tiles`, one frame of
`vr-mixed-512-v2` 4:2:0, 128 tiles, `--xform 32`:

| QP | 8x8 | 16x16 | 32x32 |
|---|---|---|---|
| 8 | 99 | 0 | 29 |
| 16 | 80 | 7 | 41 |
| 24 | 68 | 14 | 46 |
| 32 | 37 | 54 | 37 |

The same sweep on the comparison sequence, `vr-mixed-1024-v2` 4:4:4, 6 frames,
3072 tiles:

| QP | 8x8 | 16x16 | 32x32 |
|---|---|---|---|
| 8 | 1930 | 98 | 1044 |
| 16 | 1402 | 717 | 953 |
| 24 | 1164 | 1328 | 580 |

All three sizes are used at every operating point but one, and the **share of
8x8 falls monotonically as the quantizer coarsens** on both sequences -- 77 %
to 29 % on the 512 clip, 63 % to 38 % on the 1024 one. That is what the tool is
for: at a fine quantizer the residual has real high-frequency content and an
8x8 transform localizes it better; at a coarse one the residual is smooth and a
large transform's DC-adjacent coefficients carry it for fewer symbols.

Which of the two *large* sizes wins is content-dependent and the two sequences
disagree about it -- 32x32 keeps growing on the 512 clip while 16x16 overtakes
it on the 1024 one -- which is the argument for having both sizes and letting
RD choose, rather than picking one and calling it the large transform.

This is also the evidence about the **per-32x32-quadrant** variant, which is
deliberately not built (`SYNTAX.md` Appendix A item 53; word1 bits 30-31 are
reserved for it). The histogram says the choice varies strongly *between*
tiles. It says nothing at all about whether it varies *within* one, and the
honest position is that finer granularity is unmeasured, needs its own syntax
field, and should be built only against a measurement -- the same standard
directional intra was finally held to.

---

## 6. Encode and decode time

`vr-mixed-1024-v2` 4:4:4, 2048x1024, 6 frames, QP 16, wall clock divided by
frames, on an **idle** machine, `chrt -i 0 taskset -c 8-11 nice -n 19`:

| | encode | decode | bytes |
|---|---|---|---|
| `--xform 8` (version 1) | 2102 ms/frame | 84.4 ms/frame | 672712 |
| `--xform 16` | 4091 ms/frame | 80.1 ms/frame | 649052 |
| `--xform 32` | 4655 ms/frame | 87.2 ms/frame | 627424 |

**Encode is 2.2x**, for a boring reason: the per-tile header search now
evaluates three transform sizes where it evaluated one, and each candidate is a
full quantize, trellis and reconstruct of the tile. That is an encoder choice,
not a syntax cost -- an encoder that picked the size from a cheap classifier,
or that only tried the larger sizes on tiles whose 8x8 residual is already
nearly empty, would pay almost none of it. The comparison runs saw the same
ratio end to end: 105.0 s against 199.5 s per operating point on 4:4:4, 44.4
against 93.6 on 4:2:0.

**Decode is between 2 % faster and 24 % slower, and which one depends on the
rate.** Best of three, same conditions, with the per-tile size mix `nxv-info`
reports for each stream:

| QP | `--xform 8` | `--xform 32` | change | tiles at 8 / 16 / 32 |
|---|---|---|---|---|
| 8 | 88.7 ms/frame | 110.1 ms/frame | **+24 %** | 1930 / 98 / 1044 |
| 16 | 80.0 ms/frame | 87.3 ms/frame | **+9 %** | 1402 / 717 / 953 |
| 24 | 80.0 ms/frame | 78.4 ms/frame | **-2 %** | 1164 / 1328 / 580 |

The arithmetic really is 7.4x per sample at 32x32 (331 multiplies per 32-point
1D transform against the 8-point graph's 11), but the inverse transform is not
what a decoder spends its time on: at QP 8 a third of the tiles take 32x32 and
the whole decode grows by a quarter, and by QP 24 the coefficients the entropy
coder no longer has to decode pay for the transform outright.

The first attempt at this table was taken while the machine was loaded and
reported 127 -> 179 ms/frame, a 40 % decode regression. That number was
contention, not arithmetic, and it is recorded here because it is exactly the
kind of measurement that would have gone into a decision unchallenged.

`SYNTAX.md` 6.7 has the GPU accounting, where the same work costs **no extra
LDS and no extra barriers** and only the multiply count moves.

---

## 7. GPU cost accounting

The full table is `docs/SYNTAX.md` 6.7. The three claims that matter for
PAPER design principle 2:

1. **One workgroup per tile, unchanged.** A transform block never crosses a
   tile edge -- `N` divides every coded extent, and the per-plane cap of 6.7
   guarantees it -- and the size is a tile-header field, so no tile's decode
   depends on any other tile's choice.
2. **No extra dependent steps.** Both passes of every block are independent of
   every other block, so the schedule stays *load, row pass, barrier, column
   pass* per plane: 3 dependent steps at every transform size, against the 22
   the directional wavefront of 7.6 costs. A large-transform tile carries no
   mode unit, so it has no wavefront at all.
3. **No extra LDS.** The int16 transpose buffer holds the plane, not the block:
   four resident 32x32 blocks are `4 * 32 * 32 * 2 = 8192` bytes, and the same
   plane as sixty-four 8x8 blocks is `64 * 8 * 8 * 2 = 8192` bytes. Identical,
   because the `clamp16` after pass 1 is size-independent (6.3).

What it does cost is multiplies: 2.8 per sample at 8x8, 9.4 at 16x16, 20.7 at
32x32 -- 85k per 64x64 luma plane against 11k. With 256 threads per tile a
32x32 plane is 128 one-dimensional transforms per pass, so two lanes per row,
each producing 16 of the 32 outputs from the same 32 inputs, fills the
workgroup.

---

## 8. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export PATH=$PWD/build-ref/bin:$PATH
PY=$NXQ_SCRATCH/venv/bin/python
cd tools/quality

# --- section 1: the Phase 1 gate, before and after, 4:4:4 and 4:2:0
for PIX in yuv444p yuv420p; do
  for V in before after; do
    [ $V = before ] && ENC="nxv-enc --quiet" || ENC="nxv-enc --quiet --xform 32"
    chrt -i 0 taskset -c 8-11 nice -n 19 $PY compare.py \
      --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.$PIX.json \
      --codec-enc "$ENC" --codec-dec "nxv-dec --quiet" --codec-name nxv-$V \
      --anchors x264-intra \
      --qp 0,4,8,12,16,20,24 --anchor-qp 8,12,16,20,24,28 \
      --phase1-anchor x264-intra --phase1-band 100,400 \
      --phase1-tolerance 1.0 --no-vmaf \
      --out $NXQ_SCRATCH/results/tourney-xform-b/intra-$V-$PIX.json
  done
done

# --- section 2: the Phase 2 kill test, both rate bands
P=$NXQ_SCRATCH/seq/vr-mixed-1024-v2.poses.json
for V in before after; do
  [ $V = before ] && X="" || X="--xform 32"
  for BAND in A B; do
    [ $BAND = A ] && { QP=0,4,8,12; AQP=2,8,14,20; } \
                  || { QP=18,24,30,36; AQP=26,32,38,44; }
    chrt -i 0 taskset -c 8-11 nice -n 19 $PY compare.py \
      --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json \
      --codec-enc "nxv-enc --quiet --eyes 2 --inter on --poses $P $X" \
      --codec-dec "nxv-dec --quiet" --codec-name nxv-inter-$V \
      --anchors x265-p --qp $QP --anchor-qp $AQP --no-vmaf \
      --out $NXQ_SCRATCH/results/tourney-xform-b/kill-mixed1024-444-$BAND-$V.json
  done
done
$PY ../../ref/phase2_verdict.py \
    --results $NXQ_SCRATCH/results/tourney-xform-b/kill-*.json
```

**Do not add `--frames` to those commands.** `compare.py --frames N` truncates
the metric window and the bitrate divisor but *not* the anchor encode --
`nxq/ffmpeg.py encode_anchor()` uses `nframes` only to size the GOP and never
emits `-frames:v` -- so the anchor's reported rate comes out inflated by
`seq.frames / N`. On this sequence `--frames 6` reports x264-intra at qp 8 as
1289.8 Mbit/s against the true 215.0. Every number here is from an untruncated
run.

Sections 3 to 6 are direct `nxv-enc` runs on `vr-mixed-512-v2`:

```sh
# 4 and 5: the bit split and the size histogram
nxv-enc --in $NXQ_SCRATCH/seq/vr-mixed-512-v2.yuv420p.yuv --w 1024 --h 512 \
        --pix yuv420p --qp 16 --frames 1 --xform 32 --stats --out /dev/null
nxv-info --in out.nxv --tiles | grep -o ' x[0-9]* ' | sort | uniq -c

# 3: the band floor.  Edit group_band_min() in ref/src/common.h and rebuild.
```

The transform itself is pinned by `ctest -R 'ref\.(transform|saturate|vectors)'`,
which checks the 16- and 32-point transforms against a float DCT-II, their
round-trip error, unit gain, the coefficient-group bijection, the per-plane cap
and the weighting-matrix derivation, and decodes both saturation vectors
(`v35` at 8x8 and `v62` at 32x32) through the real decoder. Run the suite under
`cmake --preset asan-ubsan`, which is where a signed overflow in the new
arithmetic would abort.
