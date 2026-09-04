# Syntax v1.5: the intra detail tools, measured

The package is three items, each measured on its own:

| tool | bit | what it is | verdict |
|---|---|---|---|
| `XFORM_4X4_SPLIT` | 19 | a per-block 4x4 transform split (SYNTAX.md 6.7, 9.8) | **shipped** |
| `INTRA_CFL` | 24 | a tenth chroma intra mode, chroma from luma (SYNTAX.md 7.7) | **shipped** |
| adaptive dead zone | none | encoder-side, per frequency band | **kept as structure, no gain** |
| reconstruction offset | none | decoder-side, per band | **measured and rejected** |

Both bitstream tools are **on in `nxvc_config_default()`**, for the same
reason the v1.3 tools are: they win, and each sets a tool bit, so a decoder
without them refuses the stream at the handshake rather than misparsing it.
`nxv-enc --split4x4 off --cfl off` gets a v1.4 stream back, byte for byte.

`v01`-`v56` of the conformance set are **byte-identical** to what a v1.4 build
produced -- `ctest -R ref.vectors` proves it on every commit. That is the
whole point of the tool-bit discipline: a stream without the bits decodes
exactly as it did. The Phase 2 set `v45`-`v56` is deliberately pinned to the
tool set Phase 2 defined it with (`tests/ref/vectors.cpp`, `build_inter`):
these tools would apply to an inter stream's INTRA refresh tiles, but nothing
here measured that, so it is not shipped on that path.

Everything below was produced by `tools/quality/compare.py` against
`x264 --keyint 1` through ffmpeg n9.0.1, on the **v2 band-limited** sequence
`vr-mixed-1024-v2` (2048x1024 side-by-side, first 6 frames, 90 fps,
`synthetic:mixed:seed1:v2-bandlimited-ss4`) in both 4:4:4 and 4:2:0. Every
process ran under `chrt -i 0 taskset -c 20-23 nice -n 19` with ffmpeg capped
at `-threads 4`, alongside other tournament agents on other cores. Result
files are under `$NXQ_SCRATCH/results/tourney-detail-a/`, and each row
measures a **snapshot** of the binaries taken when the row started
(`tools/quality/run-detail-a.sh`), so a rebuild during a run cannot change what
was compared.

Reproduce the whole table with

```sh
tools/quality/all-detail-a.sh
```

---

## 0. The gate, cumulative

Each row adds one tool to the row above. `base` is `--split4x4 off --cfl off`,
which is the shipped v1.4 default and reproduces a v1.4 build byte for byte.

**4:4:4**

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| `base` = `--split4x4 off --cfl off` (reproduces v1.4 byte for byte) | +112.38 % | -7.086 dB | -7.928 dB at 150.4 Mbit/s | FAIL |
| `+ XFORM_4X4_SPLIT` | **+98.01 %** | -6.632 dB | -7.917 dB at 149.7 Mbit/s | FAIL |
| `+ INTRA_CFL` (**the v1.5 default**) | **+93.62 %** | **-6.522 dB** | -7.892 dB at 149.5 Mbit/s | FAIL |
| | | | | |
| `INTRA_CFL` alone, without the split | +107.87 % | -7.008 dB | -7.923 dB at 150.2 Mbit/s | FAIL |

**4:2:0**

| | BD-rate vs x264 intra | mean deficit | worst deficit | verdict |
|---|---|---|---|---|
| `base` = `--split4x4 off --cfl off` (reproduces v1.4 byte for byte) | +100.49 % | -6.705 dB | -8.178 dB at 156.4 Mbit/s | FAIL |
| `+ XFORM_4X4_SPLIT` | **+85.47 %** | -6.137 dB | -8.141 dB at 156.2 Mbit/s | FAIL |
| `+ INTRA_CFL` (**the v1.5 default**) | **+84.53 %** | **-6.110 dB** | -8.138 dB at 156.2 Mbit/s | FAIL |
| | | | | |
| `INTRA_CFL` alone, without the split | +99.53 % | -6.693 dB | -8.175 dB at 156.4 Mbit/s | FAIL |

**The gate is still not met**, and neither tool was ever a candidate to meet
it. It moved by **18.76 BD-rate points on 4:4:4** and **15.96 on 4:2:0**, or
0.56 dB and 0.60 dB of mean deficit. What is left is about 6 dB.

The two tools are almost exactly **additive**: -14.37 and -4.51 points
separately on 4:4:4 against -18.76 together (a 0.12-point interaction), and
-15.02 and -0.96 against -15.96 on 4:2:0 (0.02). That is the expected shape
and worth stating, because it is not what happened with `INTRA_DIR` and
`CTX_V2` in v1.3, which were worth more together than apart. These two touch
different things -- one is the transform of a residual, the other is where a
chroma prediction comes from -- and the measurement says so.

Verbatim, the final gate lines. 4:4:4:

```
  BD-rate of final on PSNR-Y (negative is better):
    vs x264-intra     +93.62 %   BD-PSNR -6.598 dB   (overlap 38.56-47.85 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -7.892 dB at 149.5 Mbit/s, mean -6.522 dB over 149.5-319.1 Mbit/s
```

4:2:0:

```
  BD-rate of final on PSNR-Y (negative is better):
    vs x264-intra     +84.53 %   BD-PSNR -6.163 dB   (overlap 38.56-47.90 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -8.138 dB at 156.2 Mbit/s, mean -6.110 dB over 156.2-326.7 Mbit/s
```

**A note on comparing these numbers with `RESULTS-intra.md`.** They are not
comparable, and the difference is the material, not a regression. That
document measured `vr-mixed-1024` (v1) over QP 0-24; this one measures
`vr-mixed-1024-v2`, the **band-limited** regeneration, over QP 16-36, which is
what puts both curves inside the 100-400 Mbit band on this sequence. Band
limiting removes exactly the aliasing an 8x8 DCT under a block-mean predictor
handles least badly and x264's directional intra handles best, so the deficit
against the anchor is larger throughout. The before/after pair here was
measured on one sequence with one ladder and one build, and that is what the
rows are for.

---

## 1. Operating points

**4:4:4**, base against the shipped v1.5 default:

| QP | base Mbit/s | base PSNR-Y | final Mbit/s | final PSNR-Y | final SSIM-Y | final VMAF | rate |
|---|---|---|---|---|---|---|---|
| 16 | 482.1 | 47.61 | 431.9 | **47.85** | 0.9949 | 97.7 | **-10.4 %** |
| 20 | 355.2 | 44.52 | 328.8 | **44.83** | 0.9921 | 96.0 | -7.4 % |
| 24 | 266.4 | 41.44 | 256.3 | **41.89** | 0.9872 | 94.1 | -3.8 % |
| 28 | 199.1 | 38.38 | 197.2 | **38.66** | 0.9787 | 89.7 | -0.9 % |
| 32 | 150.4 | 35.42 | 149.5 | 35.40 | 0.9659 | 80.9 | -0.6 % |

**4:2:0**:

| QP | base Mbit/s | base PSNR-Y | final Mbit/s | final PSNR-Y | final SSIM-Y | final VMAF | rate |
|---|---|---|---|---|---|---|---|
| 16 | 446.0 | 47.63 | 409.1 | **47.90** | 0.9950 | 97.7 | **-8.3 %** |
| 20 | 337.9 | 44.49 | 317.5 | **44.87** | 0.9921 | 96.2 | -6.0 % |
| 24 | 257.5 | 41.44 | 249.9 | **41.90** | 0.9874 | 94.1 | -3.0 % |
| 28 | 199.6 | 38.37 | 199.2 | **38.72** | 0.9791 | 89.8 | -0.2 % |
| 32 | 156.4 | 35.45 | 156.2 | 35.47 | 0.9668 | 81.0 | -0.2 % |

Every point but one is both **smaller and better** than the base point at the
same QP, which is the shape a tool that leaves the quantizer alone should
have. The exception is QP 32 on 4:4:4 (-0.6 % of rate for -0.02 dB), which is
the low-rate end where both tools have least to work with: at QP 32 most
blocks have no coefficients at all, so the split flag is never coded and the
chroma residual CFL would improve has already been quantized away.

The gain is **strongly rate-dependent** -- 10.4 % at QP 16 falling to 0.6 % at
QP 32 -- and that is the honest headline. Both tools are detail tools. The 4x4
split pays where a block has a residual sharp enough that concentrating it in
one quadrant beats spreading it over an 8x8 basis, and CFL pays where chroma
still has structure left to predict. At the top of the Phase 1 band both are
true; at the bottom neither is.

---

## 2. Per-tool detail

### `XFORM_4X4_SPLIT` (bit 19)

The larger of the two, and the one `RESULTS-intra.md` section 8 named as "the
largest untried item": **-14.4 BD-rate points on 4:4:4 and -15.0 on 4:2:0**.
It is also the only tool in this codec so far that is worth *more* on 4:2:0
than on 4:4:4, which follows from what it does -- it improves luma, and 4:2:0
is proportionally more luma.

Two things about the shape of the win are worth recording.

First, it is **exactly the regime directional intra created**, as predicted. A
well-predicted 8x8 block's residual is not smooth; it is whatever the
predictor missed, which on this content is concentrated near an edge that runs
through one part of the block. An 8x8 DCT spreads that across a full basis; a
4x4 transform keeps it in one sub-block and lets `LAST` truncate the other
three. The split scan (9.2) is what makes that truncation possible, and it is
why the four sub-blocks are concatenated rather than interleaved.

Second, the **per-tile arming flag earns its bit**. The encoder sets tile
header bit 28 on every tile, runs the per-block decision, and then clears the
bit again on any tile where no block chose to split -- so such a tile pays
nothing at all, not even one flag per coded block. On the conformance vector
`v57_split444_qp16` two of six tiles end up with the bit cleared. Without that
step the tool would pay a bit per coded block on every tile including the ones
it cannot help.

### `INTRA_CFL` (bit 24)

**-4.5 BD-rate points on 4:4:4 and -1.0 on 4:2:0.** The asymmetry is the whole
story and it is not a defect: chroma is half the samples of a 4:4:4 picture
and an eighth of a 4:2:0 one, so a chroma tool has four times less to work
with in 4:2:0. It is worth building anyway because 4:4:4 is the format the
foveated path wants.

**The gate understates it, by construction.** The Phase 1 criterion is scored
on PSNR-**Y**, and CFL does not touch luma. Look at the operating points: at
QP 16 on 4:4:4 the `cflonly` row is 459.2 Mbit/s at 47.60 dB against the
base's 482.1 at 47.61 -- **4.7 % fewer bits at the same luma quality**, which
is the only way a chroma tool can show up in a luma metric. What it also does,
and what the gate cannot see, is improve chroma: measured on one 2048x1024
frame, PSNR over all three planes rises 0.32 dB at QP 8, 0.44 dB at QP 16 and
0.26 dB at QP 24 while the frame gets 8.3 %, 5.0 % and 1.5 % smaller. A
perceptual or all-plane criterion would score this tool roughly twice as well
as the Phase 1 gate does.

The mode is chosen where it is genuinely better: it competes in the same
`D + lambda*R` decision as the other nine modes, against mode 0 which is
always in the candidate list, so a chroma block can never be worse for having
the tool available.

---

## 3. The dead zone and the reconstruction offset

Both halves of the third item measured at or below the noise floor, and the
honest record of that is the point of this section.

### 3a. Encoder-side dead zone, per band

The dead-zone quantizer is `q = floor(|c| / step + f)`. `f` was a flat `1/3`
written as `t / 3` in three places; it is now `kDeadZoneDc` / `kDeadZoneAc`
in `ref/src/tables.cpp`, indexed by `band_of(scan position)` -- the same
banding the LEVEL contexts use -- and expressed in **forty-eighths of a
step**, so that the value `16` reproduces the old `t / 3` **bit-exactly**.
That exactness
is deliberate: it is what lets the refactor land with zero conformance-vector
churn, and it is why `v01`-`v56` still pass unchanged.

Only two paths still use this quantizer at all. The RD trellis replaces it on
every residual block, so what is left is (a) the **DC plane**, which is the
intra predictor and is deliberately not trellised (a level chosen there moves
`pred` for all 64 blocks, and the trellis's single-unit distortion model would
be wrong about it), and (b) every unit under `--no-rdo`.

Swept on one 2048x1024 4:4:4 frame (the first frame of the harness sequence)
with the development hook `NXVC_DZ_DC`, which takes the same four
forty-eighths the shipped table does and is encoder-only:

| `kDeadZoneDc`, 48ths | QP 8 | QP 16 | QP 24 |
|---|---|---|---|
| **16,16,16,16 (= 1/3, shipped)** | **195 800 B / 53.336 dB** | **111 980 B / 47.654 dB** | **62 190 B / 41.516 dB** |
| 10,10,10,10 | 195 600 / 53.320 | 111 980 / 47.654 | 61 630 / 41.399 |
| 12,12,12,12 (= 1/4) | 195 438 / 53.298 | 111 980 / 47.654 | 62 190 / 41.516 |
| 20,20,20,20 | 203 656 / 53.313 | 111 870 / 47.676 | 62 190 / 41.516 |
| 24,24,24,24 (= 1/2) | 203 734 / 53.300 | 111 870 / 47.676 | 63 928 / 41.561 |
| 24,20,16,12 (fine at DC, coarse at HF) | 196 446 / 53.323 | 111 800 / 47.668 | 62 186 / 41.520 |
| 12,16,20,24 (the reverse shape) | 203 736 / 53.299 | 111 916 / 47.651 | 63 554 / 41.547 |
| 20,16,12,10 | 195 600 / 53.330 | 111 882 / 47.664 | 61 554 / 41.487 |

Nothing here moves rate by more than **0.5 %** or quality by more than
**0.06 dB** at any QP, in either direction, and no shape beats flat. The two
rows that buy bits (12,12,12,12 at QP 8; 20,16,12,10 at QP 24) pay for them in
dB at a worse slope than the QP ladder itself does, so they are losses.

`RESULTS-intra.md` section 8 predicted exactly this -- "subsumed by the RD
trellis by construction: the trellis already chooses levels against the real
rate model, which is what a tuned dead zone approximates" -- and it was right.
The only surface the tool has left is the DC plane, and the DC plane is
insensitive to it: at QP 16 four of the eight rows produce the *identical*
byte count, because the DC plane's coefficients are large enough that a
third-of-a-step change in the rounding offset moves almost none of them across
a bin boundary.

**The value therefore stays at 1/3.** What the package keeps is the
*structure*: one named table with a spec reference in place of three copies of
a magic `t / 3` and one of `t / 2`, banded by the same rule the LEVEL contexts
use, so that a retune on real capture material has somewhere to go. The
forty-eighths denominator is chosen so that 16 is exactly `t / 3` and 24
exactly `t / 2` -- which is what let the refactor land **without changing one
conformance vector**, and is the evidence that it is a refactor and not a
silent bitstream change.

### 3b. Decoder-side reconstruction offset -- rejected

The brief conditioned this on measuring, and it does not. A level `q`
reconstructs at `q * step`; the offset moves that to `(q -+ delta) * step`,
with `delta` in sixty-fourths of a step, applied toward zero. Both sides ran
the same value (build with `-DNXVC_RECON_OFFSET_EXPERIMENT` and set
`NXVC_RECON_OFF`; a normal build compiles the hook out, so no conforming
stream can depend on it). Same frame, 4:4:4:

| `delta`, 64ths of a step | QP 8 | QP 16 | QP 24 |
|---|---|---|---|
| **0 (shipped)** | **195 800 B / 53.336 dB** | **111 980 B / 47.654 dB** | **62 190 B / 41.516 dB** |
| -8 (toward the origin) | 195 564 / 53.278 | 112 016 / 47.595 | 62 102 / 41.431 |
| -4 | 195 800 / 53.336 | 112 000 / 47.612 | 62 168 / 41.488 |
| +4 | 195 800 / 53.336 | 112 668 / 47.571 | 62 334 / 41.483 |
| +8 | 195 828 / 53.262 | 112 956 / 47.499 | 62 342 / 41.388 |

Every nonzero offset is worse **in both rate and quality** at every point, in
both directions. The mechanism is clear once stated: with the RD trellis on,
levels are not the output of a dead zone at all -- `rdoq_unit()` already picks
each level by `D + lambda*R` against the *actual* reconstruction point, so
moving that point only introduces error the trellis then has to spend bits
undoing. A reconstruction offset is the right tool for a codec whose encoder
is a fixed quantizer. This one's is not.

**It therefore gets no tool bit and no syntax.** Tool bit 25 is still free.

---

## 4. What it costs a GPU decoder

The Pass B shader's cost model is in `docs/SYNTAX.md` 6.7 and 7.7, which is
where the Pass B agent should read it because that is the normative document.
Against the 22-step wavefront and 69 barriers per 4:4:4 tile that directional
intra already spends (7.6), the two tools here cost:

| tool | extra dependent steps per tile | extra barriers per tile | extra LDS | extra bytes of traffic |
|---|---|---|---|---|
| `XFORM_4X4_SPLIT` | **0** | **0** | **0** | 1 bit per coded block |
| `INTRA_CFL` | **1** (luma plane before chroma) | **1** (4:4:4 and 4:2:0 alike) | **0** | 0 |

**The 4x4 split is structurally free.** It changes neither the prediction nor
its dependency graph: a block is still predicted as one 8x8 unit from one
intra mode, and the four sub-blocks of its residual are independent of each
other and of every other block. It adds no coding unit, so Pass A's schedule
is unchanged in shape; it adds one bypass bit per unit whose `CBF` is 1, which
is one more rANS operation on a path that already runs dozens. Pass B gains a
branch on that bit and, on the taken side, **less** arithmetic than the
alternative: eight 4-point transforms against four 8-point ones is 32
multiplies against 88. The transpose buffer is the same int16 8x8.

**Chroma from luma costs exactly one barrier.** A Pass B tile shader that
predicted Y, Co and Cg concurrently now has to finish Y first. Within the
chroma planes the 7.4 schedule is unchanged -- CFL is a per-block mode like
any other, with the same left/above/above-right dependency -- so the wavefront
step count and occupancy of 7.6 do not move. The luma plane it reads is the
tile's own reconstruction, which the shader already holds in LDS (64x64 int16
= 8 KB), so there is no new allocation and no new global traffic. Per chroma
block the model fit is a 16-element two-pass min/max selection plus one
reciprocal-table lookup: about 60 scalar ops, once, against the 64 samples it
then predicts, and it parallelises across the 4 threads of a block only
partially (the selection is a small serial reduction). That is the honest
worst case, and it is small next to the barrier.

**Neither tool touches tile independence.** The split is entirely inside a
block. CFL reads a plane of its *own* tile, decoded earlier in the same tile's
own unit list, and its chroma neighbours come from the same `at()` derivation
7.4 uses, which falls back to `base` -- this tile's own DC plane -- wherever a
neighbour is not yet reconstructed. No tile reads another tile, so the
transport's per-tile loss recovery and the rate controller's per-tile ladder
are unaffected.

**Neither tool adds a division to the per-sample path.** CFL's model fit
divides once per block, and it does so through a 256-entry `u16` reciprocal
table rather than a division opcode, which keeps it inside
`spec/03-conventions.md` 3.4's integer-only rule. It is the third such
exception in the format, alongside probability-table normalisation and the
warp's fixed-iteration corner divide, and it is the cheapest of the three.

---

## 5. Encode and decode time

One 2048x1024 frame of the harness sequence itself -- the same content the
gate was measured on -- single threaded under the standard CPU discipline,
best of five runs, on a quiet machine.

**4:4:4**

| QP | tools | encode | decode | bytes |
|---|---|---|---|---|
| 16 | none | 1540 ms | 96 ms | 111 980 |
| 16 | split | 2618 ms | 93 ms | 105 700 |
| 16 | CFL | 1583 ms | 83 ms | 106 422 |
| 16 | **both** | **2615 ms** | **92 ms** | **99 908** |
| 24 | none | 1456 ms | 89 ms | 62 190 |
| 24 | **both** | 2548 ms | 107 ms | 59 588 |
| 32 | none | 1447 ms | 83 ms | 35 420 |
| 32 | **both** | 2721 ms | 96 ms | 35 266 |

**4:2:0**

| QP | tools | encode | decode | bytes |
|---|---|---|---|---|
| 16 | none | 813 ms | 53 ms | 103 080 |
| 16 | split | 1425 ms | 54 ms | 96 302 |
| 16 | CFL | 881 ms | 64 ms | 101 378 |
| 16 | **both** | **1433 ms** | **55 ms** | **94 418** |
| 24 | none | 809 ms | 51 ms | 59 706 |
| 24 | **both** | 1423 ms | 55 ms | 57 880 |
| 32 | none | 788 ms | 46 ms | 36 528 |
| 32 | **both** | 1258 ms | 45 ms | 36 488 |

**Encode is 1.7x, and it is entirely the split.** The 4x4 split doubles the
per-block candidate work in `analyze_plane_dir()`: every mode candidate is now
quantized, reconstructed and rate-scored twice, once per transform size, in a
loop that already ran three candidates. CFL costs about 3 % of encode time on
its own -- it adds one predictor to a SATD sort of nine and one 16-pair fit
per chroma block, against the nine full quantize-and-reconstruct passes the
block already does -- and at QP 24 and 32 on 4:2:0 it is *faster* than
without, because a chroma plane it predicts well leaves the RD trellis and the
table-set search less to do.

For scale, the tools this sits on top of cost 2.7x (the RD trellis) and
2.9-3.4x (directional intra) before it. 1.7x on top of that is real and is the
main price of the package.

**Decode is unchanged.** Across all 24 rows the decode times move by at most
the run-to-run spread of the measurement (±10 ms on 4:4:4, ±9 on 4:2:0) and in
both directions. That is what section 4 predicts: the split is *less*
arithmetic than the 8x8 transform it replaces and adds one bypass bit per
coded block, and CFL replaces one predictor with another of comparable cost.
Neither tool is something a CPU decoder can measure, and the GPU cost is the
one barrier in section 4, not arithmetic.

**A caveat on the encode ratio.** The per-row encode times recorded inside the
gate result JSONs give a smaller ratio -- 1.34x on 4:4:4 and 1.35x on 4:2:0 --
because those runs shared the machine with seven other agents and contention
compresses the ratio between a fast and a slow workload. The table above is
the quiet-machine number and is the one to quote.

---

## 6. What is left

The gate is still not met, and neither of these tools was ever going to meet
it: `RESULTS-intra.md` section 8 put the 4x4 split at the head of the untried
list and said plainly that the remaining 3-4 dB "still has no single dominant
term". That is unchanged. What has changed is that the largest untried item is
now tried, and the list it came from is now short.

Updated from `RESULTS-intra.md` section 8, in descending order:

| candidate | measured value | status |
|---|---|---|
| directional intra, 9 modes | -22.5 / -16.6 BD-rate points | done (v1.3), bit 17 |
| RD quantization | -8.8 % BD-rate | done (v1.2) |
| per-frame trained tables | -7.4 % | done, on by default |
| **4x4 transform split** | **see section 0** | **done (v1.5), bit 19** |
| **chroma from luma** | **see section 0** | **done (v1.5), bit 24** |
| 16-context model | -2.3 / -0.65 points | done (v1.3), bit 21 |
| sign data hiding | -0.6 points | done (v1.3), bit 22 |
| adaptive dead zone per context | **~0, measured** (section 3a) | argued away in v1.3, now measured away |
| decoder reconstruction offset | **negative, measured** (section 3b) | rejected, no tool bit |
| transform skip re-measured after directional intra | still unmeasured | `RESULTS-intra.md` section 8 asked for it and nobody has run it |
| adaptive vs static probabilities | 5-8 % per PAPER 1.6 | rejected by design (rANS encodes backwards) |
| 2- or 3-level intra pyramid | 0.84-2.37 dB of residual *energy* at +6 to +31 % coefficients | measured, not worth it |
| 4-byte tile header | <=1.5 % in this band | measured, not worth it |

**Three things this package did not do, and why.**

1. **The built-in v2 probability tables were not retrained.** `INTRA_CFL` adds
   a tenth value to the `MODE` symbol and the split scan changes the LEVEL
   statistics of a split block; both are coded against tables trained without
   them, so the *default* table family is now slightly mis-fitted for the new
   tools. The per-frame transmitted tables (on by default, `--custom-tables`)
   absorb most of that, which is why the tools still measure as they do.
   Retraining with `nxv-gentables` would change every v2 default and therefore
   every v1.3-onward conformance vector, which is a bitstream change with its
   own before/after and belongs in its own package. It is the cheapest
   remaining win on this list.

2. **A per-block CFL alpha refinement was not built.** VVC signals a small
   correction to the fitted slope; here the slope is derived and never
   corrected. That is deliberate for a first measurement -- a derived-only
   model costs zero bits and cannot make a block worse than the mode it
   displaces, because the mode decision is a real `D + lambda*R` comparison --
   but it is the obvious next lever if CFL is worth extending.

3. **The planar / DC-plane refinement (item 4 of the brief) was not
   built.** Time went to the three items that had tool bits and measurements
   attached. `RESULTS-intra.md` section 2 already measured the closest thing
   to it -- a 2- or 3-level in-tile pyramid -- and rejected it at 0.84-2.37 dB
   of residual *energy* for +6 to +31 % coefficients, and noted that
   directional intra has since taken the structure it was after. That
   reasoning applies to a DC-plane refinement too, and a second-level
   refinement would add a dependent step inside the tile, which is the cost
   the paper's design principle 2 exists to refuse. It should be measured
   before it is built, the way the pyramid was.
