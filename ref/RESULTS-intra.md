# Phase 1 intra: measurements

> **Read section 0 first.** Sections 1-6 are the v1.2 record: the Phase 1 gate
> as it stood with the DC-plane predictor and the 12-context entropy model,
> and the measurement that said directional intra was the largest tool left.
> Sections 0 and 5b-9 are the v1.3 record: that tool built and measured, plus
> the 16-context model and sign data hiding. Neither record was rewritten to
> agree with the other; where they differ, section 0 is the current number.

Everything here was produced by `tools/quality/compare.py` against
`x264 --keyint 1 --tune zerolatency` through ffmpeg n9.0.1, on
`vr-mixed-1024` (2048x1024 side-by-side, 6 frames, 90 fps, `synthetic:mixed:seed1`)
and on the `corpus/` entries. Every process ran under
`chrt -i 0 taskset -c 28-31 nice -n 19`. Result files are under
`$NXQ_SCRATCH/results/intra/`.

The Phase 1 exit criterion is **within 1.0 dB of x264 intra over 100-400 Mbit**.
On this sequence at 90 fps that band is 0.53-2.12 bits per pixel, which is a
*high quality* operating point, not the low-rate regime `docs/SYNTAX.md`
Appendix B was measured in. That difference turns out to matter, and it is why
the conclusions below differ from the ones the gap analysis in `README.md`
predicted.

---

## 0. The v2 intra tools (v1.3)

Three tool bits, each measured on its own with `tools/quality/compare.py`
against the same anchor and the same command line as section 6, on the same
`vr-mixed-1024` sequence. Result files are under
`$NXQ_SCRATCH/results/intra-v2/`.

| tool | bit | what it is |
|---|---|---|
| `INTRA_DIR` | 17 | nine intra modes per 8x8 block, MPM coded, from reconstructed neighbours inside the tile (SYNTAX.md 7.4) |
| `CTX_V2` | 21 | 16 entropy contexts: dedicated CBF/LAST/LEVEL for the DC plane, plus a mode context (SYNTAX.md 9.3) |
| `SIGN_HIDE` | 22 | the sign at scan position `LAST` is the parity of the unit's absolute levels (SYNTAX.md 9.7) |

### The gate, cumulative

Each row adds one tool to the row above. `v1.2` is the previous state of this
document, re-measured on this build to confirm it reproduces exactly (it does:
+65.79 % / -5.937 dB, byte for byte the section 1 numbers).

**4:4:4**

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| v1.2 (`--intra-dir off --ctx v1 --no-sign-hide`) | +65.79 % | -5.937 dB | -6.588 dB at 181.9 Mbit/s | FAIL |
| `+ INTRA_DIR` | +43.27 % | -4.430 dB | -5.297 dB at 100.0 Mbit/s | FAIL |
| `+ CTX_V2` | +40.96 % | -4.110 dB | -4.613 dB at 181.9 Mbit/s | FAIL |
| `+ SIGN_HIDE` (**shipped default**) | **+40.35 %** | **-4.047 dB** | -4.546 dB at 181.9 Mbit/s | FAIL |

**4:2:0**

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| v1.2 | +43.69 % | -4.678 dB | -5.335 dB at 100.0 Mbit/s | FAIL |
| `+ INTRA_DIR` | +27.07 % | -3.144 dB | -4.239 dB at 100.0 Mbit/s | FAIL |
| `+ CTX_V2` | +26.42 % | -3.055 dB | -3.915 dB at 100.0 Mbit/s | FAIL |
| `+ SIGN_HIDE` (**shipped default**) | **+25.86 %** | **-2.988 dB** | -3.823 dB at 100.0 Mbit/s | FAIL |

Verbatim, the final gate lines. 4:4:4:

```
  BD-rate of final on PSNR-Y (negative is better):
    vs x264-intra     +40.35 %   BD-PSNR -4.111 dB   (overlap 45.14-56.59 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -4.546 dB at 181.9 Mbit/s, mean -4.047 dB over 100.0-303.1 Mbit/s
```

4:2:0:

```
  BD-rate of final on PSNR-Y (negative is better):
    vs x264-intra     +25.86 %   BD-PSNR -3.123 dB   (overlap 45.14-56.59 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -3.823 dB at 100.0 Mbit/s, mean -2.988 dB over 100.0-290.3 Mbit/s
```

**The gate is still not met.** It moved by **1.89 dB** on 4:4:4 and **1.69 dB**
on 4:2:0 -- against the 1.0 dB section 4 predicted for directional intra, and
against the 5 dB the gate needs. What is left is 3.0-4.0 dB, and section 8 says
where it might come from.

### Operating points, 4:4:4, shipped default

| QP | Mbit/s | PSNR-Y | SSIM-Y | VMAF | | v1.2 Mbit/s | v1.2 PSNR-Y |
|---|---|---|---|---|---|---|---|
| 0 | 356.4 | 56.59 | 0.9988 | 99.4 | | 399.8 | 55.70 |
| 4 | 282.1 | 54.68 | 0.9983 | 99.4 | | 318.1 | 53.92 |
| 8 | 220.4 | 52.30 | 0.9975 | 99.3 | | 247.5 | 51.80 |
| 12 | 174.8 | 49.30 | 0.9961 | 99.0 | | 195.4 | 48.87 |
| 16 | 135.9 | 45.78 | 0.9932 | 98.0 | | 151.4 | 45.70 |
| 20 | 105.4 | 42.42 | 0.9887 | 95.5 | | 115.3 | 42.34 |
| 24 | 81.9 | 39.12 | 0.9824 | 92.1 | | 85.4 | 39.01 |

(The exact per-QP figures are in `$NXQ_SCRATCH/results/intra-v2/final-yuv444p.json`;
the table is rounded from that file.)

Every point is both smaller **and** better than the v1.2 point at the same QP,
which is what a prediction tool that leaves the quantizer alone should look
like.

### corpus/

BD-rate against x264 intra on the corpus clips, both configurations measured on
this build over the same QP ladder (`--qp 4,10,16,22,28,34`, anchors
`10,16,22,28,34,40`). These are 256 px clips that never reach the 100-400 Mbit
band, so the gate declines to give a verdict on them by design; the BD-rate is
the point. **The ladder differs from section 1's, so these numbers are not
comparable with the section 1 corpus table** -- the before/after pair here is
internally consistent and that is what it is for.

| sequence | v1.2 | shipped default | change |
|---|---|---|---|
| `mono-mixed-256.yuv444p` | +92.25 % | **+65.28 %** | -27.0 points |
| `panel-static-256.yuv444p` | +119.96 % | **+70.80 %** | -49.2 points |
| `panel-static-256.yuv420p` | +85.15 % | **+55.72 %** | -29.4 points |

`panel-static-256` gains most, which is the predicted shape: it is flat panels
and bitmap text, the content whose residual a directional predictor drives to
exactly zero and whose block means the DC plane could only ever smooth over.

### Per-tool detail

**`INTRA_DIR`** is by far the largest of the three: -22.5 points of BD-rate on
4:4:4 and -16.6 on 4:2:0, about **1.5 dB**. That is half again the "-12 to
-15 % BD-rate, about 1 dB" section 2 predicted from the oracle-neighbour rate
proxy. The prediction was low for a reason worth recording: the proxy scored
the best of 8 modes per block *against* the DC plane and charged mode
signalling at 3 bits/block, but it could not see two things that turn out to
matter more than the mode choice itself. First, mode 0 **is** the DC plane, so
the tool is a per-block superset and never pays for a bad mode; the proxy
assumed a forced choice. Second, once a block is predicted well its neighbours
are predicted from a *better reconstruction*, which compounds down the tile --
an effect no single-block oracle can measure.

The mode decision is SATD over all nine modes, then a real `D + lambda*R`
comparison over the best two plus mode 0. Widening that to all nine
(`--intra-dir-cand 8`) is worth 0.1 % of rate for 2.2x the encode time, so the
default is 2.

**`CTX_V2`** is worth -2.3 points on 4:4:4 and -0.65 on 4:2:0 *on top of*
directional intra. Measured on its own, without `INTRA_DIR`, it is worth about
-1 % -- the DC-plane contexts alone are a small win, and most of the 2.3 points
is the **mode context**, which replaces 1 or 4 bypass bits per block with a
trained symbol. That is why the two tools are worth more together than apart,
and why the bootstrap v2 tables (a copy of the v1 family with four rows bolted
on) were slightly *worse* than v1 until `nxv-gentables` retrained the family
with the DC plane and the mode decision actually in the corpus.

**`SIGN_HIDE`** is worth -0.6 points on both. It is the smallest of the three
and it is the only one whose byte count goes the *wrong* way: at QP 16 the
frame is 0.13 % **larger** and 0.125 dB better, because the parity adjustment
usually raises a level rather than dropping one. Net of the rate-quality slope
(about 7 dB per rate octave here) that is a gain, but a small one.

### What it costs

One 2048x2048 4:4:4 frame, single threaded, under the standard CPU discipline.

| | encode | decode | bytes |
|---|---|---|---|
| QP 12, v1.2 default | 0.86 s | 0.11 s | 536 640 |
| QP 12, v2 default | 2.46 s | 0.10 s | 466 780 |
| QP 24, v1.2 default | 0.68 s | 0.08 s | 235 044 |
| QP 24, v2 default | 2.30 s | 0.09 s | 207 136 |
| QP 24, v2, `--intra-dir-cand 8` | 5.17 s | 0.09 s | 206 932 |

**Encode is 2.9-3.4x**, on top of the 2.7x the RD trellis already cost, and
that is inherent rather than sloppy: the tile is analysed twice (once to choose
a table set, once for the real RD decision) and each block runs nine SATDs and
three full quantize-plus-reconstruct candidates, in a loop that cannot be
vectorized across blocks because each block's references are the previous
block's output.

**Decode is unchanged.** That number deserves the emphasis: on a CPU,
directional intra is free on the decoder. Nine predictors of adds and shifts
are less arithmetic than the bilinear interpolation the DC plane already does.
The cost is not arithmetic, it is **scheduling**, and it lands entirely on the
GPU -- see section 5b.

---

## 1. The gate

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| before, yuv444p | +81.71 % | -6.858 dB | -7.496 dB at 181.9 Mbit/s | FAIL |
| after, yuv444p | **+65.79 %** | **-5.937 dB** | -6.588 dB at 181.9 Mbit/s | FAIL |
| before, yuv420p | +53.26 % | -5.323 dB | -5.985 dB at 176.0 Mbit/s | FAIL |
| after, yuv420p | **+43.69 %** | **-4.678 dB** | -5.335 dB at 100.0 Mbit/s | FAIL |

Verbatim, the final gate line on the 4:4:4 sequence:

```
  BD-rate of nxv on PSNR-Y (negative is better):
    vs x264-intra     +65.79 %   BD-PSNR -5.976 dB   (overlap 45.14-55.70 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -6.588 dB at 181.9 Mbit/s, mean -5.937 dB over 100.0-303.1 Mbit/s
```

and on 4:2:0:

```
  BD-rate of nxv on PSNR-Y (negative is better):
    vs x264-intra     +43.69 %   BD-PSNR -4.784 dB   (overlap 45.14-55.74 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -5.335 dB at 100.0 Mbit/s, mean -4.678 dB over 100.0-294.2 Mbit/s
```

The gate is **not met**. It moved by 0.92 dB (4:4:4) and 0.65 dB (4:2:0); it
needs about 5 more, which is roughly a further -40 % BD-rate. Section 4 says
where that would have to come from and what each candidate is actually worth,
measured rather than assumed.

### Operating points, 4:4:4, after

| QP | Mbit/s | PSNR-Y | SSIM-Y | VMAF | | x264 crf | Mbit/s | PSNR-Y |
|---|---|---|---|---|---|---|---|---|
| 0 | 399.8 | 55.70 | 0.9984 | 99.4 | | 8 | 303.1 | 59.56 |
| 4 | 318.1 | 53.92 | 0.9978 | 99.4 | | 12 | 228.5 | 56.75 |
| 8 | 247.5 | 51.80 | 0.9970 | 99.3 | | 16 | 180.2 | 54.48 |
| 12 | 195.4 | 48.87 | 0.9955 | 98.8 | | 20 | 139.2 | 49.94 |
| 16 | 151.4 | 45.70 | 0.9926 | 98.2 | | 24 | 109.4 | 47.69 |
| 20 | 115.3 | 42.34 | 0.9878 | 95.5 | | 28 | 84.2 | 45.14 |
| 24 | 85.4 | 39.01 | 0.9807 | 91.8 | | | | |

The two curves are close to parallel — about 7 dB per rate octave each — so the
deficit is a roughly constant bit-efficiency factor of ~1.8x, not a saturation
or a ceiling. In particular there is **no quality ceiling inside the band**:
QP 0 with the flat matrix reaches 60.74 dB at 644 Mbit/s, so the transform's
round-trip precision is not what limits us here.

### corpus/

BD-rate against x264 intra, before and after the encoder work:

| sequence | before | after |
|---|---|---|
| `mono-mixed-256.yuv444p` | +94.79 % | +83.24 % |
| `panel-static-256.yuv444p` | +114.88 % | +105.97 % |
| `panel-static-256.yuv420p` | +77.01 % | +71.51 % |

These clips are 256 px, so they never reach the 100-400 Mbit band and the gate
declines to give a verdict on them, by design. They are here for the BD-rate
only. Note that all of this material — the harness's synthetic panorama and the
corpus alike — is the *same content class*; see the honesty note in section 5.

---

## 2. What changed, step by step

### Step 0 (priority fix): signed overflow in the inverse transform

Not a rate or quality item, but it came first: `cmake --preset asan-ubsan`
reported signed integer overflow at `ref/src/transform.cpp:23`. The inverse
flow graph's odd-part rotation computes `(P ± Q) * C4`, and on legal int16
coefficients `|P ± Q|` reaches 8.6e7, so the product is 3.1e10 — outside int32,
i.e. undefined behaviour in the **normative** path, where a different compiler
or a GPU could legitimately produce different pixels.

The value is well defined; only the way to compute it was missing. It is now an
exact two-word product (`s = 512*hi + lo`, and the `>> 9` distributes over
`512*hi*C4`), so it is bit-identical to the mathematical result for every
reachable input. **No conformance vector changed as a result of this fix**,
which is the proof that it was a specification gap rather than a bitstream
change. `docs/SYNTAX.md` 6.3 now states the guaranteed range at every stage and
Appendix A item 34 records the decision.

Regression cover: `ctest -R ref.saturate` sweeps the arithmetic at its bounds
(the worst-case odd-part sign pattern, every single-coefficient extreme, all
256 sign patterns of a row, 20 000 random int16 blocks, the dequantizer at
QP 63 with the largest legal weight and level) and decodes the new
`v35_saturate420` conformance vector, whose every dequantized coefficient
saturates the int16 clamp in exactly the pattern that maximises `|P + Q|`. The
whole `ref.` suite is clean under `-fsanitize=address,undefined`.

### Step 1: pyramid intra — measured, not implemented

The plan was to extend the DC plane into a 2- or 3-level in-tile pyramid. It
was measured first, on the actual harness sequence, as the residual energy left
by a predictor built from block means at three granularities:

| predictor | residual MSE | gain over current | extra coded values per tile |
|---|---|---|---|
| 8x8 means (current DC plane) | 530.97 | — | 0 |
| + a 16x16 level | 437.15 | 0.84 dB | 256 (+6 %) |
| + a 32x32 level | 307.38 | 2.37 dB | 1280 (+31 %) |

A 0.84 dB reduction in *residual energy* is worth at most ~0.14 bits/sample at
high rate, and the level that buys it costs 256 extra coded values per tile per
plane. The 2.37 dB variant costs 31 % more coefficients. Neither is a plausible
route to the 5 dB that is missing, and both add a dependent step inside the
tile. **Not implemented.** The syntax is untouched, so this stays available.

### Step 2: directional intra — measured, not implemented

Measured twice, because the first measurement was wrong in an instructive way.

*First measurement (residual energy).* Best-of-6 H.264-style 8x8 directional
prediction from **source** neighbours (an oracle: the real thing predicts from
reconstructed neighbours and is worse) gave residual MSE 620.4 against the DC
plane's 591.1 — i.e. directional prediction was **0.21 dB worse**. Taken at
face value this says the paper's bet in 6.4 was right and directional intra is
not worth an in-tile wavefront.

*Why that was the wrong metric.* 75 % of this content's pixels are horizontally
constant: it is a synthetic panorama of flat panels, text, checkerboards and
star fields. On piecewise-constant content the value of a directional predictor
is not that mean residual energy drops, it is that the residual becomes
**exactly zero** in many blocks, which energy averaged over a tile cannot see.

*Second measurement (rate proxy).* Same predictors, but scoring each 8x8 block
by the bits its quantized coefficients would cost at QP 16 (`6 + Σ 2 + 2·log2(1+|q|)`
over the nonzeros), best of 8 modes and best of DCT/transform-skip per block,
still from source neighbours:

| predictor | bits/block | luma bpp |
|---|---|---|
| DC plane (current) | 41.45 | 0.6477 |
| directional, 8 modes, oracle neighbours | 32.71 | 0.5110 |

**-21 % on the luma coefficients**, before paying for the mode signalling
(~3 bits/block ≈ +7 %) and before the loss from using reconstructed rather than
source neighbours. A realistic figure is **-12 to -15 % BD-rate, about 1 dB.**

That is the largest single tool left, and it is more than the 0.5 dB threshold
in the brief — but it is not implemented here, for reasons worth stating
plainly:

* It is more than 1 dB, so it does not fail the "leave it off" test; but it is
  also not 5 dB, so it does not by itself change the Phase 1 verdict.
* It is a change to the normative core: a mode symbol per 8x8 block, new
  contexts, a new position in the lane schedule, and a decoder that can no
  longer reconstruct blocks independently.
* **GPU cost.** Directional prediction over the 8x8 blocks of a 64x64 tile is a
  15-step diagonal wavefront. Each step has at most 8 active blocks and the
  wavefront average is 64/15 ≈ 4.3, so on a 64-lane Adreno wave with one lane
  per block column the workgroup runs at roughly **4.3/64 ≈ 7 % occupancy
  during prediction**, against 100 % for the DC plane, plus 15 barriers per
  tile per plane where the DC plane needs one. PAPER 3.2.4 estimated the DC
  plane at ~12 ops/pixel fully parallel; a wavefront predictor is not more
  arithmetic, it is 15 serialized rounds of it. For a 4:4:4 tile that is 45
  barriers. This is exactly the cost the paper's design principle 2 exists to
  refuse, and it should be adopted only against a measurement of Pass B's
  actual barrier cost on the target part, which is a Phase 0/3 number nobody
  has yet.

Recommendation: keep `INTRA_DIR` as tool bit 17, carry the -21 %-oracle number
forward, and decide it against a real Pass B barrier measurement rather than
against this estimate.

Related negative result: **transform skip does not substitute for it.** With
the DC-plane predictor, `--tskip on` is both larger and worse at every QP
(e.g. QP 16: 236.9 Mbit/s at 45.24 dB, against 150.6 at 45.76 with the
transform), and `--tskip auto` is also a net loss. Transform skip only pays
once the prediction is good enough for the residual to be sparse in the sample
domain, which on this content means directional prediction. The two are one
tool, not two.

### Step 3: RD quantization — implemented, on by default

This is the one that shipped. The level syntax is a three-state Markov chain —
each level's context depends on the magnitude class of the previously decoded
level, in reverse scan order — so the rate-optimal assignment is a trellis, not
a per-coefficient threshold. `rdoq_unit()` in `ref/src/codec.cpp` computes

```
f[p][s] = min over m of ( rate(m | band(p), s) + D(p, m) + f[p-1][cls(m)] )
```

in one ascending pass over scan positions, which gives every prefix cost; every
candidate `last` is then evaluated in O(1) against the all-zero unit
(`CBF = 0`). Candidates per position are `{0, floor(|c|/step), floor+1}`, cost
is `D + λR` with `D` in squared coefficient units (the transform is
orthonormal, so that is squared sample units) and `R` in real bits from the
tile's own probability table.

λ was tuned on the harness, as a multiple of the tile's squared quantizer step:

| λ scale | BD-rate vs the dead-zone quantizer |
|---|---|
| 0.05 | -1.43 % |
| 0.10 | -4.56 % |
| 0.15 | -6.16 % |
| 0.22 | -7.25 % |
| **0.30** | **-7.83 %** |
| 0.40 | -7.37 % |
| 0.55 | -5.95 % |

0.30 is the built-in default (`--rdo-lambda` overrides it, `--no-rdo` disables
the trellis). Measured on the full 6-frame sequence through `compare.py` it is
worth **-8.8 % BD-rate and +0.92 dB** (4:4:4) and **-6.2 % / +0.65 dB**
(4:2:0), and 5-12 % on the corpus clips.

Two deliberate limits:

* The **DC plane is not trellised.** It is the intra predictor: a level chosen
  there changes `pred` for all 64 blocks of the plane, and the trellis's
  single-unit distortion model would be wrong about it. It stays on the plain
  dead-zone quantizer.
* Table-set selection runs before the trellis (the trellis needs a rate model)
  and again after (the trellis changes the statistics), and the per-frame
  probability tables are trained on the post-trellis histograms. That is why
  the encoder's cost went up more than the trellis alone would explain.

**No syntax change.** The decoder is untouched, `nxv-enc`/`nxv-dec` flags are
stable apart from the new optional ones, and a stream produced with `--no-rdo`
and one produced with the default decode through exactly the same path.

### Step 4: per-frame transmitted probability tables

Already implemented and on by default before this work (`--custom-tables`,
`--no-custom-tables`). Re-measured here so the number is on the record: at
2048x2048 4:2:0 QP 28, transmitting trained tables costs 0.28 s of encode time
and saves 7.4 % of the frame (185 084 B -> 171 392 B). The 8 x 120 bytes of
transmitted tables are 0.4 % of that frame. Kept on.

### Step 5: per-tile `wm_id` — implemented

Two of the tile header's reserved bits (word1 bits 26-27) now carry `wm_id`,
gated on new tool bit 20 `WM_ID`. `rc/` needs it for step 1 of the degradation
ladder (PAPER 4.6.1), which has to drop a *single* tile onto a low-pass
weighting matrix without touching the frame.

`wm_id == 0` means "the frame's matrix", so **every stream written before this
field existed is byte-identical**, and the common case costs nothing. Values
1-3 select built-in matrix `wm_id` for that tile alone. A frame carrying custom
matrices refuses `wm_id != 0` rather than defining a precedence rule.

The ladder's step-1 matrix is **`wm_id = 2`**, `w[i] = min(32, 16 + 2s)` with
`s = u + v`. It is the strongest roll-off the normative `[1, 32]` weight range
can express: it reaches the cap at `s = 8`, so half the coefficient positions
are quantized twice as coarsely as the DC. Coverage: vectors `v33_wm_id444`
(`wm_id = 2`) and `v34_wm_id420_tables` (`wm_id = 3` with transmitted tables),
and rejection vector `r11_wm_id_no_tool`.

**Per-tile search, measured.** `--wm auto` and `--qp-search N` do a real
rate-distortion decision per tile: every candidate is a full quantization of the
tile scored as `D + λR`, with `D` the exact squared error of the reconstruction
the decoder will produce (the encoder runs `reconstruct_plane` to get it) and
`R` the coefficient bits plus the tile's fixed cost. They work, and they are not
worth their price:

| | BD-rate vs default | encode time, one 2048x1024 frame |
|---|---|---|
| default | — | 0.65 s |
| `--wm auto` | -0.02 % | 1.89 s |
| `--qp-search 1` | -0.35 % | 1.67 s |
| `--qp-search 1 --wm auto` | -0.27 % | 4.53 s |
| `--qp-search 2 --wm auto` | -0.46 % | 6.94 s |

Both are **off by default** for that reason. At a fixed λ a uniform QP is
already close to RD-optimal on this content, and every built-in matrix other
than the frame's is worse for PSNR, so the search mostly confirms the default.
`--wm auto` does exercise the field — on a QP 18 frame it picks `wm0` for 354
tiles, `wm2` for 101 and `wm3` for 57 — which is what makes it a usable
reference for `rc/` to check its ladder against.

For reference, the frame-level matrix choice measured on this content: matrix 0
(flat) is worth about +0.15 to +0.27 dB at fixed rate over the default matrix 1,
because PSNR does not care about the perceptual roll-off. The default was left
at matrix 1 — the ladder and the perceptual model want it, and 0.2 dB is not
what decides this gate.

### Step 6: the tile header floor — not a factor in this band

Appendix B measured the 8-byte tile header at 13.7 % of a QP 36 frame, and the
brief asked whether a compact 4-byte form is worth a tool bit. In the gate's
band it is not close:

| | QP 8 | QP 16 | QP 24 |
|---|---|---|---|
| tile headers | 1.08 % | 1.78 % | 3.01 % |
| rANS init/flush | 2.67 % | 3.25 % | 4.45 % |
| DC planes | 3.36 % | 5.01 % | 6.93 % |
| luma blocks | 43.86 % | 51.17 % | 55.03 % |
| chroma blocks | 21.38 % | 16.43 % | 11.81 % |

(2048x1024 4:4:4, 512 tiles, one frame, `nxv-enc --stats`.)

At 100 Mbit/s the header is 3 % of the frame and a 4-byte form would save at
most 1.5 %, well under the 2 % bar in the brief. **Not implemented, and it
should not be**: the 13.7 % figure is a QP 36 / low-rate fact and does not
transfer to the Phase 1 band. It remains a real lever for the *rate-control*
regime, where tiles are small, and that is where it should be re-measured.

One accounting bug worth recording for whoever reads `--stats`: the category
lines sum to about 77 % of the payload, not 100 %. The missing ~23 % is the
bypass bits — sign, `LAST` raw bits and escape suffixes — which are not in any
context histogram and so are attributed to no category. They are not lost bits,
they are unlabelled ones, and they are close to incompressible (sign is one bit
per nonzero coefficient by construction).

---

---

## 3. Encode and decode time

One 2048x2048 frame, single-threaded, under the standard CPU discipline.

| | encode | decode | bytes |
|---|---|---|---|
| 4:4:4 QP 12, dead-zone | 0.42 s | 0.14 s | 596 114 |
| 4:4:4 QP 12, RD trellis | 1.24 s | 0.15 s | 536 658 |
| 4:4:4 QP 24, dead-zone | 0.36 s | 0.13 s | 271 948 |
| 4:4:4 QP 24, RD trellis | 1.11 s | 0.13 s | 235 062 |
| 4:2:0 QP 28, dead-zone | 0.21 s | 0.07 s | 194 334 |
| 4:2:0 QP 28, RD trellis | 0.57 s | 0.07 s | 171 392 |
| 4:2:0 QP 28, RD, `--no-custom-tables` | 0.29 s | 0.07 s | 185 084 |

The RD trellis costs **2.7x encode time** (0.21 s -> 0.57 s on the 4:2:0 frame
that `README.md` quotes) and **nothing at all on the decoder**, which is the
whole point of an encoder-only tool. Most of the extra is not the trellis
itself: the tile is quantized twice (once to pick a table set, once through the
trellis) and the per-frame table training pass repeats both.

---

## 4. Where the remaining ~5 dB is

The gap is not one thing, which is why nothing in the brief's list closes it.
Measured or bounded, in descending order:

| candidate | measured value | status |
|---|---|---|
| directional intra, 8 modes | -21 % oracle, ~-15 % realistic (~1 dB) | measured, not implemented, GPU cost noted above |
| RD quantization | -8.8 % BD-rate (0.92 dB) | **done** |
| per-frame trained tables | -7.4 % | done, was already on |
| dedicated DC-plane contexts | unmeasured; Appendix B calls it the cheapest remaining win | needs 16 contexts, a syntax change |
| adaptive vs static probabilities | 5-8 % per PAPER 1.6 | rejected by design (rANS encodes backwards) |
| flat frame matrix | +0.15 to +0.27 dB | available, deliberately not the default |
| per-tile QP / matrix RD search | -0.02 to -0.46 % | implemented, off by default: 3-10x encode time for nothing |
| 2- or 3-level intra pyramid | 0.84-2.37 dB of residual *energy* at +6 to +31 % coefficients | measured, not worth it |
| 4-byte tile header | ≤1.5 % in this band | measured, not worth it |
| bigger tile-header savings | 3 % of the frame in total | not worth it |

Even taking every implementable item at its optimistic value, that is roughly
-30 % BD-rate against the -40 % needed. **Closing this gate on this material
needs a coding tool the v1 syntax does not have**, and the honest candidates
are directional intra (with the wavefront cost the paper wanted to avoid) plus a
context model with room for the DC plane.

> **How this held up.** Both candidates were built and measured; see section 0.
> The conclusion was right about *which* tools and wrong about *how much*:
> directional intra is worth 1.5 dB rather than 1, and the context model 0.2
> rather than nothing. Section 8 is the post-v1.3 version of this table.

---

---

## 5. An honesty note about the material

`tools/quality/README.md` says the Phase 1 criterion is to be met "on VR
captures rather than on synthetic material", and every number above is on
synthetic material — the harness's generated panorama and the `corpus/` clips
built the same way. That content is 75 % horizontally constant: flat panels,
bitmap text, checkerboards, a star field. It is close to the best case for
x264's directional intra and CABAC and close to the worst case for an 8x8 DCT
under a smoothly interpolated block-mean predictor, so it very likely overstates
the deficit against what a real WiVRn capture would show. `README.md`'s own
older tables say the same thing from the other side: on dense natural-looking
detail the deficit was 0.3-1.6 dB, and on smooth synthetic renders 3.5-5.8 dB.

That is an explanation, not an excuse — the gate is the gate, and it fails. But
it does mean the next measurement that matters is a real capture per
`tools/quality/README.md` section 1c, before anyone spends a wavefront on
directional intra to fix a number that may be partly an artefact of the test
material.

---

## 6. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export PATH=$PWD/build-ref/bin:$PATH
cd tools/quality

chrt -i 0 taskset -c 28-31 nice -n 19 \
  python3 compare.py --seq $NXQ_SCRATCH/seq/vr-mixed-1024.yuv444p.json \
    --codec-cmd nxv --anchors x264-intra \
    --qp 0,4,8,12,16,20,24 --anchor-qp 8,12,16,20,24,28 \
    --phase1-anchor x264-intra --phase1-band 100,400 --phase1-tolerance 1.0 \
    --out $NXQ_SCRATCH/results/intra/final-yuv444p.json
```

Add `--codec-enc "nxv-enc --no-rdo" --codec-dec nxv-dec --codec-name nxv-nordo`
for the before column.

**Section 0 (v1.3)** was produced the same way, with `--codec-enc` selecting
the tools and `--codec-name` naming the row, into
`$NXQ_SCRATCH/results/intra-v2/`:

```sh
#   v1.2 baseline
--codec-enc "nxv-enc --intra-dir off --ctx v1 --no-sign-hide" --codec-name v1
#   + INTRA_DIR
--codec-enc "nxv-enc --intra-dir on  --ctx v1 --no-sign-hide" --codec-name dir
#   + CTX_V2
--codec-enc "nxv-enc --intra-dir on  --ctx v2 --no-sign-hide" --codec-name nosdh
#   + SIGN_HIDE  (the shipped default; `nxv-enc` alone is the same thing)
--codec-enc "nxv-enc --intra-dir on  --ctx v2"                --codec-name final
```

and the corpus rows with `--qp 4,10,16,22,28,34 --anchor-qp 10,16,22,28,34,40`
against `$NXQ_SCRATCH/corpus/<name>.<pix>.json`.

The section 7 schedule variants need a build with the development hook:

```sh
cmake -S . -B build-sched -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS=-DNXVC_DIR_SCHED_EXPERIMENT
NXVC_DIR_SCHED=3 nxv-enc ...      # and the same value on nxv-dec
```

---

## 7. GPU cost accounting for Pass B (v1.3)

The full schedule, the barrier counts and three measured mitigations are in
**`docs/SYNTAX.md` 7.6**, which is where the Pass B agent should read them
because that is the normative document. The summary:

* Blocks depend on left, above and **above-right** (mode `DDL` reaches
  `A[15]`), so the independent set is `2*by + bx`, not the anti-diagonal.
  The luma plane of a `res_level` 0 tile is a **22-step** wavefront.
* At 256 threads per tile and 4 threads per 8x8 block, mean occupancy during
  prediction is **2.91 blocks = 11.6 / 256 = 4.5 %**, peak 6.2 %.
* Barriers per tile: **69** for 4:4:4 and **45** for 4:2:0, against 3 either
  way for the DC plane alone.
* The arithmetic does not grow. Section 0's decode timings show it: on a CPU
  the tool is free. All of the cost is serialization.

Three restrictions were implemented behind `-DNXVC_DIR_SCHED_EXPERIMENT` and
measured on 2048x1024 4:4:4, one frame, default configuration:

| `NXVC_DIR_SCHED` | restriction | QP 8 | QP 16 | QP 24 | steps | occupancy |
|---|---|---|---|---|---|---|
| 0 | as specified | 299 048 B / 52.248 dB | 181 288 B / 45.798 dB | 103 940 B / 39.077 dB | 22 | 4.5 % |
| 1 | no above-right reference | 299 762 / 52.238 | 181 762 / 45.814 | 103 894 / 39.053 | 15 | 6.7 % |
| 2 | 32x32 sub-tile independence | 303 692 / 52.272 | 184 506 / 45.829 | 105 764 / 39.116 | 10 | 10.0 % |
| 3 | both | 304 198 / 52.269 | 184 956 / 45.835 | 105 890 / 39.116 | 7 | 14.3 % |

**Dropping the above-right reference costs 0.24 % of rate and removes a third
of the barriers.** Adding 32x32 sub-tile independence takes it to 7 steps and
14.3 % occupancy -- 3.1x fewer barriers, 3.2x the occupancy -- for 1.8 %.
Against directional intra's own 22.5 points, 1.8 % is cheap.

**The shipped syntax is `NXVC_DIR_SCHED = 0`, the best-rate variant, and the
hook is compiled out of a normal build** so a conformant encoder cannot emit
anything else. That is deliberate: the previous version of this document
recommended deciding the wavefront "against a real Pass B barrier measurement
rather than against this estimate", and that measurement still does not exist.
What has changed is that the menu is now priced. When Pass B measures the
barrier cost on the target part, restriction 1 and restriction 1+2 are the two
candidates, and adopting either narrows what `INTRA_DIR` means rather than
adding a tool bit -- a `SYNTAX.md` edit and a vector regeneration.

A fourth option the brief raised, **16x16 super-blocks whose four blocks are
predicted in parallel from the super-block's border**, was modelled but not
implemented: it gives 10 steps on its own and 4 steps combined with 32x32
sub-tiles (25 % occupancy), but it is the only one of the four that degrades
the *prediction distance* -- the bottom-right block of a super-block would
reference samples 16 px away instead of 8 -- so its rate cost is the one that
cannot be guessed from the others and it should be measured before it is
believed.

---

## 8. What is left after v1.3

3.0 dB on 4:2:0 and 4.0 dB on 4:4:4, or about -30 % more BD-rate. Measured or
bounded, in descending order:

| candidate | measured value | status |
|---|---|---|
| directional intra, 9 modes | **-22.5 / -16.6 BD-rate points (1.5 / 1.7 dB)** | **done**, tool bit 17 |
| 16-context model | **-2.3 / -0.65 points** | **done**, tool bit 21 |
| RD quantization | -8.8 % BD-rate (0.92 dB) | done (v1.2) |
| per-frame trained tables | -7.4 % | done, was already on |
| sign data hiding | **-0.6 points** | **done**, tool bit 22 |
| 4x4 transform split | unmeasured; the residual after directional prediction is sharper and more local, which is the regime a 4x4 transform is for | the largest untried item |
| adaptive dead zone per context | expected ~0, and encoder-only | subsumed by the RD trellis by construction: the trellis already chooses levels against the real rate model, which is what a tuned dead zone approximates |
| adaptive vs static probabilities | 5-8 % per PAPER 1.6 | rejected by design (rANS encodes backwards) |
| flat frame matrix | +0.15 to +0.27 dB | available, deliberately not the default |
| 2- or 3-level intra pyramid | 0.84-2.37 dB of residual *energy* at +6 to +31 % coefficients | measured (section 2), not worth it -- and directional intra now takes the structure it was after |
| 4-byte tile header | <=1.5 % in this band | measured, not worth it |

Of the three the brief listed for the "still failing" case, **sign data hiding
was the cheapest and is done**; the **adaptive dead zone** is argued away above
rather than measured, because with the trellis on it has no mechanism left to
exploit; and the **4x4 transform split** is the one worth building next. It is
also the most expensive: a per-block split flag, a second scan and LAST class
family, four sub-units where there was one, and a decoder change. It should be
measured the way directional intra finally was -- built, not proxied -- because
this document's record is now two for two on rate proxies being wrong about
magnitude in both directions.

Note also that **transform skip is worth re-measuring**. Section 2 recorded
that `--tskip` was a net loss at every QP and explained it: transform skip only
pays once the prediction is good enough for the residual to be sparse in the
sample domain, "which on this content means directional prediction". That
prediction now exists. `v42_dir_res_tskip420` exercises the combination, but
nobody has run the rate comparison.
