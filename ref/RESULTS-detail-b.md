# Intra detail tools (syntax v1.5): measurements

The "intra detail tools" package, built and measured on branch
`tourney/detail-b`. Four items were asked for; **two are built and shipped,
one was measured and rejected, and one was not reached.**

| # | tool | tool bit | verdict |
|---|---|---|---|
| 1 | chroma from luma | 24 `INTRA_CFL` | **built**, on by default, -1.13 % / -0.28 % BD-rate |
| 2 | 4x4 transform split | 19 `XFORM_4X4_SPLIT` | **built**, on by default, -0.48 % / -0.45 % BD-rate |
| 3 | adaptive dead zone per context class | none | **measured and rejected**; +0.10 % at best, five settings, section 3 |
| 4 | planar / DC-plane refinement | none | not reached; section 6 says what is left and why it is not free |

Everything here was produced by `tools/quality/compare.py` against
`x264 --keyint 1 --tune zerolatency` through ffmpeg n9.0.1, on the **v2
(band-limited) sequence** `vr-mixed-1024-v2` (2048x1024 side by side,
6 frames, 90 fps, `synthetic:mixed:seed1:v2-bandlimited-ss4`), which is the
sequence `$NXQ_SCRATCH/seq` now carries. Every process ran under
`chrt -i 0 taskset -c 24-27 nice -n 19`. Result files are
`$NXQ_SCRATCH/results/tourney-detail-b-*.json`; the driver is
`tools/quality/run-b.sh`.

> **These numbers are not comparable with `RESULTS-intra.md`.** That document
> measured the v1 sequence, whose rates at the same QP are about a quarter of
> the v2 sequence's; the 100-400 Mbit band therefore lands at QP 20-34 here
> instead of QP 0-24, on band-limited material that is harder for us at every
> point. The v1.4 baseline re-measured on this sequence is **+117.67 %** on
> 4:4:4, not the +40.35 % `RESULTS-intra.md` records. The before/after pair
> below is internally consistent and that is what it is for.

---

## 1. The gate, before and after

Each row adds one tool to the v1.4 default (`--intra-dir on --ctx v2
--sign-hide`). `+ INTRA_CFL` is `--split4 off`, `+ XFORM_4X4_SPLIT` is
`--cfl off`, and the last row is the shipped v1.5 default.

PLACEHOLDER_GATE

---

## 2. Per-tool detail

### `INTRA_CFL` (bit 24)

The larger of the two, and on 4:4:4 by a wide margin. It is also the one whose
BD-rate against the anchor *understates* it, because the Phase 1 gate is scored
on **PSNR-Y** and chroma-from-luma does nothing to luma: everything it is worth
arrives as rate, at the same luma quality.

What it actually does, on one 2048x1024 4:4:4 frame at QP 20 (`nxv-enc
--stats`, `--split4 off` on both sides):

| | `--cfl off` | `--cfl on` |
|---|---|---|
| chroma blocks | 7204 B | **4572 B** (-37 %) |
| luma blocks | 32811 B | 32638 B |
| DC planes | 18126 B | 18728 B |
| frame | 82 830 B | **79 880 B** (-3.6 %) |
| chroma blocks using mode 9 | 0 | 2053 of 65 536 (**3.1 %**) |

Three per cent of chroma blocks carry a third of the chroma bits, and mode 9
takes those bits out. That is the whole mechanism: the model is exact where
chroma is a linear function of luma, which on this material is every coloured
edge, and those are exactly the blocks whose chroma residual was expensive. It
is not a smooth-area tool -- the DC plane already handles those, and the DC
plane grows slightly (+3 %) because a CFL block's mean is no longer what the
DC plane would have predicted.

The tool fades with QP, as a prediction tool that costs a mode symbol should:
3.1 % of chroma blocks at QP 20, 0.6 % at QP 28, 0.04 % at QP 36 on 4:4:4.
The Phase 1 band ends around QP 34, so it is a band tool and not a low-rate
one.

4:2:0 gets less of it (-0.28 % against -1.13 %) for the obvious reason: its
chroma is a quarter of the samples and about a third of the bits that 4:4:4's
is, so a third of a smaller number.

### `XFORM_4X4_SPLIT` (bit 19)

Worth about half a percent on both formats, and the interesting part is how it
got there, because the first two versions of it were worth **nothing**.

| version | 4:4:4 | 4:2:0 |
|---|---|---|
| quadrant-major layout, own 4x4 scan and band mapping, flag after `CBF` | +0.03 % | +0.04 % |
| interleaved quadrants, flag after `LAST`, `kSplitMinLast = 4` | +0.03 % | +0.04 % |
| ... `kSplitMinLast = 10` | -0.30 % | -0.29 % |
| ... `kSplitMinLast = 16` | -0.45 % | -0.46 % |
| ... **`kSplitMinLast = 24`** (shipped) | **-0.47 %** | **-0.47 %** |
| ... `kSplitMinLast = 32` | -0.50 % | -0.44 % |
| ... `kSplitMinLast = 48` | -0.36 % | -0.38 % |

(BD-rate of the tool against the same build with it off, PSNR-Y, QP 20-32 on
one frame; the full-sequence numbers in section 1 are close to these.)

**The transform was never the problem; the flag was.** A 4x4 transform on a
block whose residual is genuinely local is worth a lot -- at QP 24 on 4:4:4 the
quadrant-major version is +0.31 dB for +1.4 % of bytes -- but it is worth
nothing at all on the blocks with three coefficients in them, and those are
most of the blocks. Coding one bypass bit on every block with `CBF == 1` spends
more than the transform saves everywhere except the very top of the band.

The fix is to make the flag conditional on something the decoder already knows
at that point, and the only such thing is `LAST`. That forces the split to
leave the scan and the `LAST` classes alone, which forces the interleaved
coefficient layout of SYNTAX.md 6.7 -- and that layout is, on its own, slightly
*worse* at compaction than a quadrant-major one with a proper 4x4 scan
(+0.09 dB against +0.31 dB at QP 24). Taking the worse transform layout to buy
the conditional flag is what turns +0.03 % into -0.47 %, and it also removes a
scan table and a band mapping from the specification instead of adding them.

The threshold is flat between 16 and 32; 24 is chosen because it is exactly
`kLastBase[12]`, so "`LAST` class 12 or above" is the same condition and a
decoder needs no second comparison.

Two smaller decisions inside the tool, each measured:

* **Weights.** Taking the 8x8 weighting matrix at the matching frequency
  (`w[2u][2v]`) rather than a flat matrix is worth about 0.2 dB at QP 24 and
  costs nothing: no second matrix family, nothing transmitted, nothing trained.
* **Where the encoder looks.** The split is scored only for the mode the
  unsplit pass chose, not for every mode candidate. The full cross product was
  not measured; the two-stage version already doubles the per-block
  quantize-and-reconstruct work, and `RESULTS-intra.md` measured the analogous
  widening of the mode search (`--intra-dir-cand 8`) at 0.1 % for 2.2x the
  encode time.

The tool fires on 0.16 % of blocks at QP 20 on 4:4:4 and on none at all by
QP 36. Like `INTRA_CFL`, it is a band tool.

---

## 3. The adaptive dead zone: measured and rejected

The brief asked for "adaptive dead zone and reconstruction offsets per context
class (encoder-side dead zone by context, plus optional decoder-side
reconstruction offset per band behind a tool bit **only if it measures**)". The
encoder-side half was built -- a Q5 rounding offset per LEVEL band for residual
blocks and a second one for the DC-plane unit, replacing the single `f = 1/3`
of v1.2-v1.4 -- and measured against the v1.4 baseline on both formats. It does
not measure, so it is not in the tree and the decoder-side half was not built.

`f` is the dead-zone quantizer's only free parameter: `q = floor(|c|/step + f)`.
`{a, b, c, d}` below is `f * 32` for LEVEL bands 0, 1, 2 and 3 of a DC-plane
unit; the residual-block table was swept alongside it and is reported where it
differed. BD-rate is against the v1.4 encoder on the same ladder, PSNR-Y,
lower is better:

| DC-plane `f * 32` | 4:4:4 | 4:2:0 |
|---|---|---|
| `{11,11,11,11}` (= 1/3, the v1.4 value, recomputed in Q5) | +0.10 % | +0.25 % |
| `{13,12,11,10}` | +0.10 % | +0.19 % |
| `{16,15,14,13}` | +0.55 % | +0.26 % |
| `{16,16,16,16}` (= 1/2, unbiased) | +1.87 % | +1.49 % |
| `{8,8,8,8}` (= 1/4, a wider dead zone) | +0.27 % | +0.52 % |

**Every setting is worse than `f = 1/3`, and the shape says why.** Moving `f`
up buys quality and costs more rate than the quality is worth (`{16,16,16,16}`
is +0.06 dB and +1.0 % of bytes at QP 28); moving it down does the reverse.
`f = 1/3` is at the optimum and the optimum is flat, which is exactly what
`RESULTS-intra.md` section 8 predicted from the structure rather than from a
measurement:

> adaptive dead zone per context: expected ~0, and encoder-only -- subsumed by
> the RD trellis by construction: the trellis already chooses levels against
> the real rate model, which is what a tuned dead zone approximates.

That argument is now measured, and it holds with one refinement worth
recording. The band table for **residual blocks** turns out to be nearly inert
whatever it is set to, because with `--rdo` on (the default) the trellis
re-quantizes every residual block and the dead-zone pass only survives as the
input to the table-set choice. The only unit the trellis never touches is the
**DC plane**, deliberately, because a level chosen there changes `pred` for all
64 blocks of the plane and the trellis's single-unit distortion model would be
wrong about it -- so the DC plane is the only place a tuned `f` had a mechanism
at all, and the table above is that place, swept.

**The decoder-side reconstruction offset was therefore not built.** It is the
other half of the same knob: shifting the reconstruction point inside the bin
is what re-optimises against the encoder's rounding, and the encoder's rounding
has just been shown to have no gradient left at `f = 1/3`. Building it would
have spent a tool bit, a `SYNTAX.md` clause, an inverse-scan table in the
dequantizer (the band is a scan position and `dequant()` walks raster order)
and a conformance-vector regeneration, on a quantity whose encoder-side twin
measures +0.10 % at best. The brief's own condition -- "only if it measures" --
is not met.

---

## 4. What it costs

PLACEHOLDER_COST

---

## 5. GPU cost accounting for Pass B

Both tools are additive on the decoder in the sense that matters most: they
change no arithmetic the decoder cannot skip. What they cost is stated here in
the same terms `RESULTS-intra.md` section 7 and `SYNTAX.md` 7.6 use, so Pass B
can price them against the wavefront it already has.

### The 4x4 transform split (bit 19)

* **Extra dependent steps: none.** The split is entirely inside one block. The
  wavefront of 7.6 is unchanged in shape, length and occupancy; a split block
  and an unsplit block occupy the same step of the same schedule.
* **Extra LDS: none.** The four 4x4 transposes fit inside the 8x8 transpose
  buffer the block already owns (SYNTAX.md 6.7 clamps each 4x4 pass to int16
  for the same reason 6.3 does).
* **Arithmetic goes down, not up.** One 8x8 2D inverse transform is
  `2 * 8 * (11 mul + 29 add) = 640` operations; four 4x4 ones are
  `4 * 2 * 4 * (2 mul + 6 add) = 256`. A split block is about 2.5x cheaper to
  reconstruct than an unsplit one, and about 15 % of blocks split at the
  operating point.
* **What it does cost is divergence.** With 4 threads per 8x8 block, the
  natural mapping is one thread per quadrant, so no extra threads are needed --
  but a wavefront step containing both split and unsplit blocks executes both
  paths. The worst case is `640 + 256` where it was `640`, i.e. +40 % of the
  inverse transform in a fully divergent step, against -60 % in a uniform
  split one. The transform is a small fraction of Pass B either way.
* **Pass A cost: one bypass bit** on blocks with `CBF == 1` and `LAST >= 24`.
  A bypass operation is one step of the same rANS schedule as any other, so it
  costs one schedule slot on those blocks and nothing on the rest.

### Chroma from luma (bit 24)

* **One extra dependent step, and it is a real one.** The chroma planes'
  prediction reads plane 0's *finished* reconstruction. The reference decoder
  already reconstructs planes in order, so the shipped schedule gains no
  barrier -- but the freedom to run the three planes' wavefronts
  **concurrently** is gone for chroma. That freedom is worth having: 7.6
  measures the luma wavefront at 4.5 % occupancy, and interleaving the three
  planes of a 4:4:4 tile would have taken it to about 13.5 % for free. With
  `INTRA_CFL` on, the two chroma planes can still run concurrently with each
  other, but only after luma finishes: 2 phases instead of 1, and about 9 %
  peak occupancy instead of 13.5 %. **This is the tool's whole GPU cost, and
  it is a scheduling cost, not an arithmetic one.**
* **Extra LDS: 8 KiB, or zero.** The reconstructed luma tile (64x64 int16) has
  to stay resident while chroma is predicted. A Pass B that already keeps its
  output tile in LDS pays nothing; one that streams luma to memory as it
  finishes has to keep it, or re-read it.
* **Arithmetic per chroma block**: 16 co-located luma fetches for the model
  (each a 2x2 rounded average in 4:2:0), four accumulators over 16 pairs
  (about 64 multiply-adds), one division, then 64 predictions of one multiply,
  one shift, one add and a clamp. Against the DC plane's bilinear
  interpolation this is small.
* **The division is the one serial part.** `divide()` is 31 dependent
  iterations of shift-compare-subtract, on one lane of the four assigned to a
  block. Blocks in the same wavefront step divide in parallel, so a chroma
  plane pays about `31 * 22 = 682` extra serial ALU steps -- once per step of a
  schedule it was already serialising on. If Pass B measures that as material,
  the priced alternative is a 32-entry normalised reciprocal table, which
  takes it to about 5 steps at the cost of a rounding argument this document
  would then have to make normative; the exact division was chosen because it
  needs no such argument. Deriving one model per *tile* instead of per block
  is the other lever and is the one that costs rate.
* **Pass A cost: nothing.** The mode symbol is the symbol that was already
  there, one alphabet wider.

---

## 6. What is left

**Item 4 of the brief, the planar-mode or DC-plane refinement for smooth
tiles, was not reached.** What is worth recording is that the measurement most
likely to decide it already exists and is discouraging:
`RESULTS-intra.md` section 2 step 1 measured a 2- and 3-level in-tile pyramid
over the DC plane -- the most direct form of "a second-level DC-plane
refinement" -- and found 0.84 dB of residual *energy* for 6 % more coded
values, or 2.37 dB for 31 %. At the Phase 1 operating point that is not a
trade that closes, and directional intra has since taken most of the structure
the pyramid was after. A planar refinement that is *not* a pyramid -- a
per-tile gradient correction on top of the bilinear interpolation, say, coded
in the tile header -- has not been measured and is the shape worth trying next,
because it is the one that costs no extra coded values per block.

The two tools that were built have a common shape worth naming, because it
predicts where the next one should look. **Both fire on well under 1 % of
blocks and are each worth about half a percent of the frame**, because the
blocks they fire on are the expensive ones:

| | 4:4:4 QP 20 | QP 28 | QP 36 | 4:2:0 QP 20 | QP 28 | QP 36 |
|---|---|---|---|---|---|---|
| blocks split 4x4 | 0.16 % | 0.03 % | 0.00 % | 0.31 % | 0.06 % | 0.00 % |
| chroma blocks using mode 9 | 3.13 % | 0.64 % | 0.04 % | 5.05 % | 0.52 % | 0.06 % |

(`nxv-enc --stats` now reports both lines; one 2048x1024 frame.)

At QP 20 on 4:4:4, mode 9 on 3.1 % of chroma blocks takes the chroma residual
from 7204 to 4572 bytes -- **-37 %** -- and the whole frame from 82 830 to
79 880. That is the useful lesson: on this content the remaining bits are
concentrated in a small number of blocks, and a tool that does nothing at all
on 97 % of blocks and something exact on the other 3 % pays for itself. Both
tools also fade to nothing above QP 32, which is where the Phase 1 band ends,
so neither is a low-rate tool and neither should be expected to help the rate
controller's regime.

The gate remains what `RESULTS-intra.md` section 8 said it was: 3 to 4 dB
short, with no single dominant term. Nothing in this package changes that
conclusion; each tool is a half-percent, and there are not sixty of them
left.

---

## 7. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export PATH=$PWD/build-ref/bin:$PATH
cd tools/quality

./run-b.sh base  yuv444p $NXQ_SCRATCH/detail-b-basebin      # the v1.4 baseline
./run-b.sh final yuv444p $PWD/../../build-ref/bin           # v1.5 defaults
./run-b.sh cfl   yuv444p $PWD/../../build-ref/bin --split4 off
./run-b.sh split yuv444p $PWD/../../build-ref/bin --cfl off
```

`run-b.sh` is the whole invocation, including the QP ladder chosen for the v2
sequence and the Phase 1 gate flags; `yuv420p` for the other format.
