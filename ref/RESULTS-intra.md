# Phase 1 intra: measurements

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
