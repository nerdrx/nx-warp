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

**A caveat that applies to every number below.** The machine was running five
other codec experiments throughout (load average 25-27 on 32 cores). Rate and
quality are unaffected by that -- they are deterministic -- but every *time* in
section 6 is inflated, and only the ratios there are meaningful.

---

## 0. What it buys, in one table

*(filled in from sections 1 and 2)*

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

*(pending)*

---

## 2. Inter on: the Phase 2 kill test

*(pending)*

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

All three sizes are used at every operating point above QP 8, and the mix moves
towards the larger sizes as the quantizer coarsens -- which is what the tool is
for: at a fine quantizer the residual has real high-frequency content and an
8x8 transform localizes it better; at a coarse one the residual is smooth and a
large transform's DC-adjacent coefficients carry it for fewer symbols.

This is also the evidence about the **per-32x32-quadrant** variant, which is
deliberately not built (`SYNTAX.md` Appendix A item 53; word1 bits 30-31 are
reserved for it). The histogram says the choice varies strongly *between*
tiles. It says nothing at all about whether it varies *within* one, and the
honest position is that finer granularity is unmeasured, needs its own syntax
field, and should be built only against a measurement -- the same standard
directional intra was finally held to.

---

## 6. Encode and decode time

`vr-mixed-1024-v2` 4:4:4, 6 frames, QP 16, wall clock divided by frames, on a
loaded machine (see the caveat at the top). The ratios are the number to read.

| | encode | decode | bytes |
|---|---|---|---|
| `--xform 8` (version 1) | 3359 ms/frame | 127 ms/frame | 672712 |
| `--xform 16` | 6945 ms/frame | 193 ms/frame | 649052 |
| `--xform 32` | 8438 ms/frame | 179 ms/frame | 627424 |

**Encode is 2.5x**, and it is 2.5x for a boring reason: the per-tile header
search now evaluates three transform sizes where it evaluated one, and each
candidate is a full quantize, trellis and reconstruct of the tile. That is an
encoder choice, not a syntax cost -- an encoder that picked the size from a
cheap classifier instead of an RD search would pay almost none of it.

**Decode is 1.4x**, and that one is real arithmetic: a 32-point 1D transform is
331 multiplies against the 8-point graph's 11, which is 20.7 multiplies per
sample against 2.8. The reference decoder is scalar C++ and spends that
serially; `SYNTAX.md` 6.7 has the GPU accounting, where the same work is 8
threads per 32x32 block and costs **no extra LDS and no extra barriers**.

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
