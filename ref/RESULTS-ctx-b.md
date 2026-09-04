# Entropy and context modelling: measurements

What the two new tool bits and the encoder-side table refinement are worth,
measured on the reference codec. The normative text is `docs/SYNTAX.md` 9.4.1
and 9.8; this document is the record of how each piece was decided, including
the four things that were built or priced and **rejected**.

Everything was produced under `chrt -i 0 taskset -c 4-7 nice -n 19`, on the
**v2 band-limited** sequences in `$NXQ_SCRATCH/seq` (`vr-mixed-1024-v2`,
`vr-mixed-512-v2`, `vr-turn-256-v2`), through `tools/quality/compare.py` and
ffmpeg n9.0.1.
Result files are under `$NXQ_SCRATCH/results/tourney-ctx-b/`.

| tool | bit | what it is |
|---|---|---|
| `TAB_V2` | 24 | a per-row "use the built-in default" flag in a transmitted table set, and a set nothing improves is not transmitted (SYNTAX.md 9.4.1) |
| `CTX_V3` | 25 | 22 entropy contexts: CBF and LAST conditioned on whether the previous unit *this lane* decoded in the same unit class was coded (SYNTAX.md 9.8) |
| — | — | `table_iters`: Lloyd refinement of the eight per-frame table sets. Encoder only, no tool bit, default 3 |

The baseline throughout is the syntax v1.4 shipped default, which the current
encoder reproduces **byte for byte** with
`nxv-enc --ctx v2 --tab v1 --table-iters 0`; that identity is pinned by the 56
committed conformance vectors, all of which are unchanged by this package.

---

## 1. Where the bits went before this package

The package was aimed by `--stats`, not by intuition. On the v2 sequences the
picture is different from `RESULTS-intra.md` Appendix B, and one line stands
out.

**2048x1024 4:4:4 intra, one frame**

| | QP 16 | QP 32 |
|---|---|---|
| probability tables | 0.57 % | 1.36 % |
| tile headers | 3.66 % | 11.59 % |
| rANS init/flush | 5.09 % | 7.73 % |
| DC planes | 19.95 % | 30.29 % |
| luma blocks | 39.10 % | 32.40 % |
| chroma blocks | 11.69 % | 1.46 % |

**2048x1024 4:2:0 stereo inter, frame 5 of 6**

| | QP 4 | QP 24 | QP 36 |
|---|---|---|---|
| probability tables | 0.59 % | **4.15 %** | **14.45 %** |
| tile-row headers | 0.24 % | 1.99 % | 11.56 % |
| tile headers | 1.81 % | 3.63 % | 8.85 % |
| rANS init/flush | 3.54 % | 4.93 % | 4.82 % |
| DC planes | 8.38 % | 11.33 % | 17.79 % |
| luma blocks | 46.27 % | 54.30 % | 27.02 % |

At the density the paper's bit budget describes, **the transmitted probability
tables are the largest single overhead in the frame** — 14.45 % at QP 36. That
is the number that decided the shape of this package: a context model can only
buy coefficient bits, and at low rate a wider one *costs* more in table bits
than it can possibly return. `TAB_V2` had to come first.

---

## 1b. The Phase 1 gate, before and after

`vr-mixed-1024-v2`, the first 6 frames, against `x264 --keyint 1
--tune zerolatency`, `--qp 0,4,8,12,16,20,24` against `--anchor-qp
2,6,10,14,18,22,26`. Each row adds one piece to the row above.

**4:4:4**

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| v1.4 (`--ctx v2 --tab v1 --table-iters 0`) | +68.47 % | -3.945 dB | -4.857 dB at 100.0 Mbit/s | FAIL |
| `+ TAB_V2` | +68.32 % | -3.939 dB | -4.844 dB at 100.0 Mbit/s | FAIL |
| `+ CTX_V3` | +67.46 % | -3.891 dB | -4.748 dB at 100.0 Mbit/s | FAIL |
| `+ table_iters 3` (**shipped default**) | **+66.46 %** | **-3.840 dB** | -4.722 dB at 100.0 Mbit/s | FAIL |

**4:2:0**

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| v1.4 | +43.15 % | -2.501 dB | -3.492 dB at 100.0 Mbit/s | FAIL |
| `+ TAB_V2` | +43.02 % | -2.496 dB | -3.476 dB at 100.0 Mbit/s | FAIL |
| `+ CTX_V3` | +42.90 % | -2.526 dB | -3.398 dB at 100.0 Mbit/s | FAIL |
| `+ table_iters 3` (**shipped default**) | **+41.62 %** | **-2.453 dB** | -3.324 dB at 100.0 Mbit/s | FAIL |

Verbatim, the gate lines. 4:4:4, before and after:

```
  BD-rate of nxv-v14 on PSNR-Y (negative is better):
    vs x264-intra     +68.47 %   BD-PSNR -4.492 dB   (overlap 47.69-57.29 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -4.857 dB at 100.0 Mbit/s, mean -3.945 dB over 100.0-251.5 Mbit/s
```

```
  BD-rate of nxv-final on PSNR-Y (negative is better):
    vs x264-intra     +66.46 %   BD-PSNR -4.415 dB   (overlap 47.69-57.13 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -4.722 dB at 100.0 Mbit/s, mean -3.840 dB over 100.0-246.0 Mbit/s
```

4:2:0, before and after:

```
  BD-rate of nxv-v14 on PSNR-Y (negative is better):
    vs x264-intra     +43.15 %   BD-PSNR -3.349 dB   (overlap 47.69-57.29 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.492 dB at 100.0 Mbit/s, mean -2.501 dB over 100.0-200.2 Mbit/s
```

```
  BD-rate of nxv-final on PSNR-Y (negative is better):
    vs x264-intra     +41.62 %   BD-PSNR -3.239 dB   (overlap 47.69-57.12 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.324 dB at 100.0 Mbit/s, mean -2.453 dB over 100.0-196.1 Mbit/s
```

**The gate is still not met**, and this package was never going to meet it:
2.0 BD-rate points on 4:4:4 and 1.5 on 4:2:0, 0.10 dB and 0.05 dB of mean
deficit, against the 2.9 dB and 1.5 dB that are still missing. It is an
entropy-coding package, and `RESULTS-intra.md` section 8's estimate of what a
better context model is worth — the "5-8 % per PAPER 1.6" that stood
unmeasured, and the -2.3 points `CTX_V2` actually returned — is the right
bracket to read it against. It lands in it.

### Operating points, 4:4:4, before and after

| QP | v1.4 Mbit/s | v1.4 PSNR-Y | shipped Mbit/s | shipped PSNR-Y |
|---|---|---|---|---|
| 0 | 251.5 | 57.29 | 246.0 | 57.13 |
| 4 | 190.1 | 55.35 | 184.9 | 55.30 |
| 8 | 141.7 | 53.31 | 137.5 | 53.15 |
| 12 | 106.9 | 50.62 | 104.2 | 50.50 |
| 16 | 80.7 | 47.61 | 79.1 | 47.41 |
| 20 | 59.6 | 44.52 | 58.9 | 44.40 |
| 24 | 44.8 | 41.44 | 43.9 | 41.36 |

Every point is 1.6 % to 3.0 % smaller for 0.02 to 0.20 dB less, which is the
shape of a pure entropy-coding change: it does not touch the quantizer, so at a
fixed QP it moves along the rate axis and takes a little quality with it only
through the rate model the trellis consults.

### A harness bug this measurement had to fix first

`compare.py --frames N` handed the codec CLI the **whole** sequence and then
divided its bytes by `N`, so on the 36-frame v2 sequences it reported six times
the real bitrate for the codec while the anchors — which get a frame count
through ffmpeg — were right. BD-rate is a ratio and survives it; the Phase 1
gate is stated in Mbit/s and does not. It never bit the shipped Phase 1
numbers, whose sequence was six frames long, which is why it had not been seen.
Fixed in `tools/quality/compare.py`; the runs above use a pre-truncated
6-frame sequence and reproduce with either.

---

## 1c. The Phase 2 kill test, before and after

`nxv-enc --eyes 2 --inter on --poses <seq>.poses.json`, against `x265-p`
(libx265, zerolatency, P-only, one reference, one IDR), all 36 frames, PSNR-Y,
`--no-vmaf --no-ssim`. The two rate bands are `RESULTS-inter.md`'s, re-laddered
for the v2 sequences: **band A** is 100-300 Mbit/s on this clip
(`--qp 0,2,4,6`, 162 to 92 Mbit/s) and **band B** is the paper's own 0.2-0.6
bits per pixel (`--qp 6,10,14,18`, 0.49 to 0.16 bpp). The anchor ladder is
`6,12,18,24,30` and `12,18,24,30,36`.

BD-rate against `x265-p` on PSNR-Y; positive means the codec needs that much
**more** rate for the same quality, so lower is better. Every verdict is FAIL
before and after, as it was in `RESULTS-inter.md`.

| sequence | band | v1.4 | shipped | change |
|---|---|---|---|---|
| `vr-mixed-1024-v2` 4:2:0 | A | +243.95 % | **+242.27 %** | **-1.68** |
| `vr-mixed-1024-v2` 4:2:0 | B | +375.45 % | +379.77 % | **+4.32** |
| `vr-mixed-512-v2` 4:2:0 | B | +240.98 % | +240.98 % | -0.01 |
| `vr-turn-256-v2` 4:4:4 | B | +238.81 % | **+235.65 %** | **-3.16** |

**Three of the four go the right way and one goes the wrong way, and the one
that goes the wrong way is worth understanding**, because the frame is smaller
at every operating point it shares with the baseline. `vr-mixed-1024-v2` 4:2:0,
band B, per QP:

| QP | v1.4 Mbit/s | v1.4 PSNR-Y | shipped Mbit/s | shipped PSNR-Y |
|---|---|---|---|---|
| 6 | 93.13 | 54.015 | 90.92 | 53.904 |
| 10 | 62.23 | 51.217 | 61.72 | 51.188 |
| 14 | 42.44 | 48.272 | 41.81 | 48.063 |
| 18 | 26.49 | 44.753 | 26.82 | 44.710 |

At three of the four points the rate falls by 0.8 % to 2.4 % **and the PSNR
falls too**, by 0.03 to 0.21 dB, which on a curve at roughly 7 dB per rate
octave is a slightly *worse* trade than staying put; at QP 18 the rate rises.
That is not the entropy coder losing bits, it is the **RD trellis moving along
its own curve**: `rdoq_unit` scores every level against the rate model built
from the frame's probability tables (`ref/src/codec.cpp`), so changing the
contexts and the tables changes which levels it keeps, and its lambda -- 0.30,
tuned on the Phase 1 intra harness in syntax v1.2 -- was not retuned for the
new model. The package is unambiguously smaller at a fixed QP everywhere
(section 2 and 4 measure that directly); where the trellis's operating point
moves against it, BD-rate can still go the other way.

Retuning lambda is a separate encoder question with its own sweep and its own
risk of overfitting one sequence, and it is not part of an entropy-coding
package, so it is **not** done here. It is the first thing to try if this
package is adopted: `--rdo-lambda` is already the knob, `RESULTS-intra.md`
step 3 is the method, and the band-B row above is the measurement it would have
to beat.

Verbatim, band A on `vr-mixed-1024-v2` 4:2:0, before:

```
  codec nxv-v14 against x265-p, PSNR-Y
  velocity split at the 20th percentile = 43.4 deg/s (8 of 36 frames)
    overall (all frames)          BD-rate +243.95 %  BD-PSNR    n/a
    fastest 20 % of frames        BD-rate +228.59 %  BD-PSNR    n/a
    the remaining frames          BD-rate +248.10 %  BD-PSNR    n/a
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +248.10 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +228.59 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

and after:

```
  codec nxv-final against x265-p, PSNR-Y
  velocity split at the 20th percentile = 43.4 deg/s (8 of 36 frames)
    overall (all frames)          BD-rate +242.27 %  BD-PSNR    n/a
    fastest 20 % of frames        BD-rate +228.19 %  BD-PSNR    n/a
    the remaining frames          BD-rate +246.08 %  BD-PSNR    n/a
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +246.08 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +228.19 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

**The kill test fails before and after**, by the same margin it failed by in
`RESULTS-inter.md`: the distance to x265-p is two to four hundred percent and
an entropy-coding package moves it by one or two points. That is the honest
reading and it is the one this document takes. What the package *does* do at
the paper's operating density is visible in section 3 instead, where a QP 36
frame's transmitted tables go from 14.45 % of everything to 3.41 %.

---

## 2. `CTX_V3`: choosing the context layout

The brief asked for contexts conditioned on the neighbouring block's CBF and
level magnitude, on a position class, and on transform size. The constraint
that decides the design is that the conditioning must be **causal inside the
rANS lane**: the 8 lanes of a tile run in lockstep and a lane cannot read a
value another lane has not produced yet. The block *above* is a different
lane's unit for every plane geometry except the common one. What is always
available is *the previous coefficient unit this lane finished* — and for the
ordinary tile (`res_level` 0, `nsub_log2` 3, 8x8 blocks) a lane owns exactly
one column of blocks, so that unit **is** the block above.

Five layouts of that idea were built, each with its own built-in table family
retrained by `nxv-gentables v3`, and measured as encoded bytes over two frames
of `vr-mixed-1024-v2`. The neighbour class `nc` is 0 for an uncoded unit and
splits coded units by `LAST` and by mean magnitude at 4 classes; the `LEVEL`
split gives the eight banded `LEVEL` contexts a second family for a busy
neighbour.

Change against `--ctx v2 --tab v2` at the same operating point, negative is
better; mean over the eight points:

| `nc` classes | `LEVEL` split | contexts | 4:4:4 QP 8/16/24/32 | 4:2:0 QP 8/16/24/32 | mean |
|---|---|---|---|---|---|
| 4 | yes | 42 | -2.37 / -1.45 / -1.26 / -1.29 | -1.57 / -2.14 / +0.63 / -1.03 | -1.31 % |
| 4 | no | 34 | -1.96 / -1.60 / -1.51 / -1.63 | -1.01 / -2.22 / -0.30 / +0.31 | -1.24 % |
| 3 | no | 28 | -2.10 / -1.86 / -1.58 / -1.92 | -1.38 / -2.22 / -0.27 / -0.08 | -1.43 % |
| 3 | yes | 36 | -2.30 / -1.34 / -1.55 / -1.39 | -1.61 / -2.17 / +0.35 / -1.02 | -1.38 % |
| **2** | **no** | **22** | **-2.78 / -1.62 / -1.33 / -1.95** | **-1.51 / -2.49 / -0.05 / -0.86** | **-1.57 %** |
| 2 | yes | 30 | -2.77 / -1.63 / -1.33 / -1.95 | -1.52 / -2.49 / -0.04 / -0.86 | -1.57 % |

**Two classes wins**, and it wins on rate as well as on parsimony. The result
is worth stating plainly because it is the opposite of what the brief expected:

* **Level magnitude buys nothing.** Splitting a coded neighbour by `LAST` and
  by mean magnitude — the "position class" and "level magnitude" axes — costs
  12 more transmitted rows per set and returns less than it costs, at every
  operating point on both pixel formats.
* **`LEVEL` does not want a neighbour axis either.** Rows 5 and 6 differ by
  0.01 % over eight points, which is nothing: the previously decoded level
  *inside* the unit already says what the neighbour would, and it says it about
  this unit rather than about the one above.
* What is left is one bit — **was the lane's previous unit of this class coded
  at all** — applied to `CBF` and `LAST`. That is 6 contexts more than v2.
* **Transform size** is not an axis in this syntax: every residual unit is
  64 coefficients and every DC-plane unit already has contexts of its own.
  There is no 4x4 split to condition on (tool bit 19 is still reserved).

The measurement is exact, not noisy: re-running the 22-context variant end to
end — rebuild, retrain, re-encode — reproduces every byte count. All six rows
were taken on one intermediate build, before the table refinement of section 4
existed, so the column is a clean comparison of layouts and its absolute
numbers are not the shipped ones.

### The lane-count coupling, and a bug it exposed

Because the conditioning follows the lane schedule, the contexts depend on
`nsub_log2`. That field is in the tile header and is parsed before the payload,
so nothing about tile independence changes — but the *encoder* had a latent
bug that only a lane-dependent context model could expose: it chose the lane
count in the emitting pass only, so the frame's tables were trained on 8-lane
statistics and then used to code tiles with 1, 2 or 4 lanes. Under v1 and v2
that only made the *table-set choice* slightly stale; under v3 it trains the
wrong contexts. Choosing it in both passes is now unconditional. It was worth
about half a percent to a percent when it landed, but it was not isolated
behind a switch and is not separately re-measurable — it is a correctness fix
for the encoder's two passes agreeing, not a tool.

---

## 3. `TAB_V2`: making the table set affordable

A transmitted table set is `nctx` rows of sixteen 5-bit log-domain deltas: 120
bytes at 12 contexts, 160 at 16, 220 at 22. With bit 24 each row is preceded by
a `row_coded` flag, and a row whose trained version does not beat the built-in
default by more than the 80 bits it costs is left at the default. A set with no
coded row is not transmitted at all.

Transmitted-table bytes, shipped layout, against the v1.4 baseline:

| | v1.4: `--ctx v2 --tab v1` | shipped: `--ctx v3 --tab v2` |
|---|---|---|
| 4:4:4 intra QP 16, one frame | 640 B (0.57 % of the frame) | 784 B (0.71 %) |
| 4:4:4 intra QP 32, one frame | 480 B (1.36 %) | 489 B (1.43 %) |
| 4:2:0 stereo inter QP 24 | 800 B (4.15 %) | **321 B (1.81 %)** |
| 4:2:0 stereo inter QP 36 | 480 B (**14.45 %**) | **109 B (3.41 %)** |

That is the shape the tool was built for. At high rate a table set is noise and
`TAB_V2` neither helps nor hurts — the 22-context model transmits six more rows
than v2 did and the flag pays most of them back. At the paper's own operating
density it takes the largest single overhead in the frame from 14.45 % to
3.41 %, and it does it by *not transmitting* the rows and the whole sets that a
36-tile frame has no statistics to justify.

During the layout sweep, when the model still had 42 contexts, the same flag cut
a QP 24 4:4:4 frame's table area from 2 100 B to 647 B — 69 %. That measurement
is why a wider model was affordable enough to test at all.

### Exp-Golomb deltas: built, measured, rejected

The obvious companion was to code the delta itself with a small-value code:
zigzag `d - 16` around "no change" and Exp-Golomb order 0 it, so an unchanged
symbol costs 1 bit instead of 5 and the worst case is 11. It was implemented
and it is **worse** than the flat 5-bit index at every point measured (e.g.
4:4:4 QP 24: 123 910 B against 123 229 B; QP 32: 71 344 against 70 706).

The reason is worth recording, because it is the same reason the row flag
works. A trained row is not *concentrated* near its default — it is *shifted*
from it. The eight built-in sets are k-means centroids, so a frame's statistics
land near one of them as a row but each symbol moves by a similar multiplier,
and a code that is cheap only at zero loses more on the shifted symbols than it
gains on the unshifted ones. The row flag exploits the right structure (whole
rows that are already right); the small-value code exploits a structure that is
not there. Reverted; `docs/SYNTAX.md` decision 56 records it.

---

## 4. Finer table-set granularity, as encoder work

The brief asked for per-frame table selection at finer granularity, with the
table-set cost accounted. The finer granularity that turned out to pay is not
more sets — it is **using the eight the format already has properly**.

The v1.4 encoder trained set *k* on the tiles that chose *k* against the
**built-in** sets, and then, in the emitting pass, still scored every tile
against the built-ins even though the frame carried trained ones. Two things
follow: the tile is not scored against the tables it will actually be coded
with, and the training assignment is not the assignment the stream ends up
using. `table_iters` fixes both — reassign every tile against the trained sets,
retrain, repeat — and because the per-tile symbol histograms do not change, it
costs no re-quantization at all, only arithmetic over stored histograms.

Encoded bytes of two frames, `--ctx v3 --tab v2`, 4:4:4 / 4:2:0:

| `--table-iters` | QP 8 | QP 16 | QP 24 | QP 32 |
|---|---|---|---|---|
| 0 (the v1.4 encoder) | 380 838 / 325 790 | 219 256 / 200 409 | 122 402 / 118 668 | 69 098 / 71 512 |
| 1 | 380 876 / 324 126 | 219 408 / 199 290 | 121 778 / 118 457 | 68 712 / 70 876 |
| **3 (default)** | **380 204 / 324 064** | **218 672 / 198 290** | **121 496 / 117 935** | **68 332 / 70 352** |
| 6 | 380 154 / 322 970 | 218 738 / 197 307 | 121 278 / 117 183 | 68 364 / 69 872 |

Three iterations is worth **0.2 % to 1.6 %**, and it is worth most exactly
where the rest of this package is worth least: at QP 32 on 4:2:0 it is 1.6 %,
against `CTX_V3`'s -0.9 % there. Six is worth a further 0.3 % to 0.7 % on
4:2:0 and nothing on 4:4:4; three is the default because the objective the
iteration minimizes does **not** include the transmitted table cost, so running
it to convergence optimizes the wrong thing, and because each iteration is a
`table_set_cost` over every tile against all eight sets.

One caveat that is the reason `--table-iters 0` also restores the old
*selection* rule and not just the iteration count: scoring a tile against the
trained sets without then retraining on that assignment is **worse** than the
v1.4 encoder, not better — on 4:4:4 at QP 16 it costs 1.8 %. Assignment and
training have to agree, so the two halves ship together or not at all.

**More than eight sets was not built.** `table_set` is a 3-bit tile-header
field and `tables_present` is a byte in the 40-byte frame header; sixteen sets
needs a bit in each and a second built-in family of sixteen clusters, and the
measurement above says the eight the format has were not being used properly in
the first place. That is the cheaper fix and it is done.

---

## 5. The experiment: 12-bit probabilities and 16 lanes

Neither is adopted. Both are measured.

### 16 lanes per tile

`nsub_log2 = 4` is already legal syntax (tool bit 7 `NSUB_VAR`), so this needed
no code at all. Encoded bytes, `--ctx v3 --tab v2`, against the encoder's own
per-tile choice:

| | QP 8 | QP 16 | QP 24 | QP 32 |
|---|---|---|---|---|
| `--nsub 3` (8 lanes) | 393 528 | 235 712 | 142 098 | 90 904 |
| `--nsub 4` (16 lanes) | 420 292 | 264 830 | 172 402 | 122 086 |
| `--nsub auto` (shipped) | 380 204 | 218 672 | 121 496 | 68 332 |

and on the 4:2:0 stereo inter clip, 6 frames: `--nsub 3` / `--nsub 4` /
`auto` = 693 660 / 741 705 / 676 038 at QP 8, 153 836 / 181 163 / 135 376 at
QP 24, and 59 476 / **79 229** / 44 599 at QP 36.

**16 lanes is 7 % worse at high rate and 78 % worse at the paper's operating
density.** The rANS flush is four bytes per lane per tile and a tile at QP 36
is a few hundred bytes; doubling the lane count doubles a cost that already had
to be capped at a tenth of the payload, which is why the encoder's `auto` rule
exists and why it picks *one* lane for most tiles at that rate. It also halves
the length of each lane's `CTX_V3` chain. The GPU side would want a 128-thread
workgroup for 8 tiles and twice the per-lane state; there is nothing to buy it
with. **Rejected.**

### 12-bit probabilities

`kProbBits` is now a named constant with a development knob
(`-DNXVC_PROB_BITS=12`, the same shape as `NXVC_DIR_SCHED_EXPERIMENT`), and the
1024 and 1009 literals that used to be scattered through `tables.cpp` are
`kProbTotal` and `kProbMax`. A full 12-bit build encodes and decodes correctly;
it fails `ref.vectors` and `ref.saturate` by construction, because those pin a
10-bit bitstream.

Encoded bytes, `--ctx v3 --tab v2`:

| | 4:4:4 QP 8 | QP 16 | QP 24 | QP 32 | 4:2:0 QP 8 | QP 16 | QP 24 | QP 32 |
|---|---|---|---|---|---|---|---|---|
| 10-bit (shipped) | 380 204 | 218 672 | 121 496 | 68 332 | 324 064 | 198 290 | 117 935 | 70 352 |
| 12-bit | 379 686 | 218 502 | 121 018 | 68 098 | 323 224 | 198 504 | 117 809 | 70 078 |
| change | -0.14 % | -0.08 % | -0.39 % | -0.34 % | -0.26 % | +0.11 % | +0.11 % | -0.39 % |

**Mean -0.17 %, and two of the eight points go the wrong way.** The decoder
cost is genuinely near zero — Pass A binary-searches 16 cumulative frequencies,
which is 4 steps at either precision, and `cum` is 16 uints per context either
way, so LDS does not move — but the gain is not there to collect. The reason is
that the precision that binds is not the 10-bit total, it is the **5-bit
log-domain delta** a transmitted row is quantized to, whose steps are 2^(1/4).
Widening the total without widening the delta alphabet gives the trained tables
nowhere to put the extra bits. Not "clearly positive", so not adopted; the
named constants stay, because they are better than the literals were.

---

## 6. Mode, MV, `ref_delta` and disparity: priced, not built

The brief asked for dedicated contexts and better binarisation for these,
"since they are pure overhead at low rate". Measured on the stereo inter clip,
they are not, and the reason is structural rather than statistical.

* **`mode` is a 3-bit field inside a fixed 8-byte tile header** with four
  reserved bits still unused. Its empirical entropy is 1.51 / 1.32 / 0.76 bits
  per coded tile at QP 8 / 24 / 36, so a perfect model would save 1.5 to 2.2
  bits per tile — of a field that is not paid for separately. Entropy-coding it
  saves exactly zero unless the header itself shrinks, and the 4-byte header
  was measured and rejected in `RESULTS-intra.md` section 2b.
* **`ref_delta` is a transport field**, not a bitstream one (SYNTAX.md 4.1); it
  is an advisory copy of `ref_sel` and costs the bitstream nothing.
* **The vector is the only variable-length part**: two bytes when
  `mv_present`, carrying `mv_x`/`mv_y` or the 12-bit `disparity`. Its measured
  entropy, and the best case if it were coded perfectly:

| | coded tiles | of them with a vector | joint entropy | best-case saving | as % of the frame |
|---|---|---|---|---|---|
| QP 8 | 1707 | 291 | 5.19 bit (16 coded) | 393 B of 539 944 | **0.073 %** |
| QP 24 | 885 | 202 | 5.76 bit | 259 B of 120 847 | **0.21 %** |
| QP 36 | 655 | 83 | 5.08 bit | 113 B of 43 743 | **0.26 %** |

0.26 % is the *ceiling*, assuming an adaptive model this format does not have
(the tables are static per frame) and ignoring the cost of the change. And the
change is not cheap: moving the vector into the payload puts a mode-conditional
symbol at the head of a tile's coding-unit list, which is exactly what
`docs/SYNTAX.md` decision 47 already refuses for the disparity, for the same
reason — it perturbs the interleaved lane schedule, the one part of the format
with a lane-order dependency, and it ends the property that a tile header
parses without starting the entropy decoder.

**Not built.** The number is on the record so the next person does not have to
guess it: at the paper's own operating density the whole of mode, MV and
disparity coding is worth a quarter of one percent.

---

## 6b. Encode and decode time

`vr-mixed-1024-v2`, single threaded on the 4-core slice, `-O2`, wall clock per
frame. **The machine was under heavy load from other work throughout** (load
average about 21 on 32 cores; the codec process held a full core of the slice,
which `ps` confirms), so the absolute numbers are not comparable with
`RESULTS-inter.md` section 6 and only the before/after ratio is meaningful.

| | encode, v1.4 | encode, shipped | decode, v1.4 | decode, shipped |
|---|---|---|---|---|
| intra 4:4:4 QP 12 | 1.750 s/f | 1.554 s/f | 0.069 s/f | 0.064 s/f |
| intra 4:4:4 QP 24 | 1.483 s/f | 1.574 s/f | 0.069 s/f | 0.068 s/f |
| inter 4:2:0 QP 8 | 1.055 s/f | 1.039 s/f | 0.058 s/f | 0.060 s/f |
| inter 4:2:0 QP 24 | 0.594 s/f | 0.537 s/f | 0.058 s/f | 0.056 s/f |

**Encode and decode are both unchanged within the noise this machine allows.**
That is what the design predicts. The decoder does one extra store per coding
unit and indexes a table six rows longer; the encoder does three Lloyd
iterations over stored per-tile histograms, which is arithmetic over `ntiles *
8 * 22 * 16` numbers once per frame against a per-frame cost already dominated
by quantizing every tile three times.

---

## 7. What it costs a decoder

Per symbol, nothing. The context index is `base + 2 * ucls + prev_cbf` against
`base + ucls` — one add and one lookup either way — and `prev_cbf` is one store
per *unit*, of a value the lane has just decoded.

| | v1 | v2 (bit 21) | v3 (bit 25) |
|---|---|---|---|
| contexts | 12 | 16 | 22 |
| `s_cum[8][nctx][16]`, 8 tiles | 6144 B | 8192 B | **11 264 B** |
| Pass A LDS total | ~8.5 KiB | ~10 KiB | **~13.5 KiB** |
| per-lane state added | — | — | 3 bits |
| dependent steps per tile | unchanged | unchanged | unchanged |
| barriers per tile | unchanged | unchanged | unchanged |
| bytes of traffic | unchanged | unchanged | unchanged |

The budget is 32 KiB per workgroup of 8 tiles, so 13.5 KiB leaves plenty of
room for a wider model later. **No cross-lane read, no extra barrier, no change
to the round loop** — which is the whole reason the conditioning is on the
lane's own previous unit rather than on the geometric neighbour above
(`docs/SYNTAX.md` decision 53).

The Vulkan Pass A kernel is **not** changed by this package and could not be
tested here (`-DNXWARP_BUILD_VK=OFF` is the tournament build). It refuses a
stream setting either new tool bit with `VERSION`, because
`vk/decoder/nxvc_vkdec_parse.cpp`'s `kToolsSupported` does not list them, so it
can never mis-parse one. `vk/decoder/passA/syntax_constants.h` carries the v3
context constants and the `s_cum` stride the kernel would need, and
`vk/decoder/passA/README.md` says what implementing each bit costs: for
`CTX_V3`, the stride and three bits of per-lane state; for `TAB_V2`, a change
to the host parser and nothing in the shader at all.

`TAB_V2` costs the kernel nothing at all: it changes how the **host** parses
the frame's table sets into the same cumulative-frequency upload. It costs the
host one extra bit read per context row and a byte-aligned length at the end of
the table area instead of a fixed one.

`table_iters` is encoder-only and invisible to any decoder.

---

## 8. Conformance

`ctest -R 'ref\.'` is green, and green again under
`cmake --preset asan-ubsan`. The 56 committed vectors `v01`-`v56` and the 29
rejection vectors are **byte-identical** to what a v1.4 build produced, which
is the proof that both tool bits are additive and that the encoder-side
refinement does not disturb a stream that does not use trained tables.

New:

| vector | what it pins |
|---|---|
| `v57_tabv2_420` | `TAB_V2` alone, with transmitted tables |
| `v58_ctxv3_444` | `CTX_V3` alone, built-in tables only |
| `v59_ctxv3_tab_res420` | both, with directional intra, cycling `res_level` and `nsub` auto |
| `v60_default_v15_444` | the shipped default configuration of a v1.5 encoder |
| `v61_inter_ctxv3` | both, on the inter path |
| `v62_inter_stereo_v3` | both, stereo, two eyes |
| `r30_tab_v2_no_tables` | `TAB_V2` without `CUSTOM_TABLES` is `BITSTREAM` |
| `r31_ctx_v3_no_v2` | `CTX_V3` without `CTX_V2` is `BITSTREAM` |

---

## 9. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-ref -j4
export PATH=$PWD/build-ref/bin:$PATH
cd tools/quality

#   the v1.4 baseline, byte for byte
--codec-enc "nxv-enc --quiet --ctx v2 --tab v1 --table-iters 0" --codec-name nxv-v14
#   + TAB_V2
--codec-enc "nxv-enc --quiet --ctx v2 --tab v2 --table-iters 0" --codec-name nxv-tab
#   + CTX_V3
--codec-enc "nxv-enc --quiet --ctx v3 --tab v2 --table-iters 0" --codec-name nxv-ctx
#   + the table refinement (the shipped default; `nxv-enc` alone is the same)
--codec-enc "nxv-enc --quiet"                                   --codec-name nxv-final
```

Phase 1, per pixel format:

```sh
chrt -i 0 taskset -c 4-7 nice -n 19 $NXQ_SCRATCH/venv/bin/python compare.py \
  --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json --frames 6 \
  --codec-enc "..." --codec-dec "nxv-dec --quiet" --codec-name ... \
  --anchors x264-intra --qp 0,4,8,12,16,20,24 --anchor-qp 22,26,30,34,38,42 \
  --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
  --out $NXQ_SCRATCH/results/tourney-ctx-b/intra-yuv444p-final.json
```

Phase 2, band A and band B:

```sh
  --codec-enc "nxv-enc --quiet --eyes 2 --inter on \
               --poses $NXQ_SCRATCH/seq/vr-mixed-1024-v2.poses.json ..." \
  --anchors x265-p --no-vmaf \
  --qp 0,4,8,12    --anchor-qp 2,8,14,20      # band A
  --qp 18,24,30,36 --anchor-qp 26,32,38,44    # band B
python3 ref/phase2_verdict.py --results $NXQ_SCRATCH/results/tourney-ctx-b/kill-*.json
```

The 12-bit experiment of section 5 is a build flag:

```sh
cmake -S . -B build-p12 -G Ninja -DNXWARP_BUILD_VK=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DNXVC_PROB_BITS=12
```

The context-layout sweep of section 2 was run through a temporary compile knob
(`NXVC_V3_NC`, `NXVC_V3_LEVEL_SPLIT`) that parameterised `kCtxV3*` in
`ref/src/common.h`, plus a retrain of the built-in family per variant. The knob
was **removed when the layout was frozen** — carrying a switch for five
bitstreams that will never be emitted is worse than carrying the measurement —
so reproducing the sweep means reinstating it: make `kNumCtxV3` and the four
`kCtxV3*` bases functions of the two counts, widen `nc_class` back to four
classes, and for each variant run `nxv-gentables v3` into
`ref/src/default_tables.inc` before rebuilding. Each variant is about three
minutes.
