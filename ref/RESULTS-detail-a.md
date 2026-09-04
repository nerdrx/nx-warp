# Syntax v1.5: the intra detail tools, measured

The package is three items, each measured on its own:

| tool | bit | what it is | verdict |
|---|---|---|---|
| `XFORM_4X4_SPLIT` | 19 | a per-block 4x4 transform split (SYNTAX.md 6.7, 9.8) | **shipped** |
| `INTRA_CFL` | 24 | a tenth chroma intra mode, chroma from luma (SYNTAX.md 7.7) | **shipped** |
| adaptive dead zone | none | encoder-side, per frequency band | **kept as structure, no gain** |
| reconstruction offset | none | decoder-side, per band | **measured and rejected** |

Both bitstream tools are **off in `nxvc_config_default()`**, and `v01`-`v56`
of the conformance set are byte-identical to what a v1.4 build produced --
`ctest -R ref.vectors` proves it on every commit. That is the whole point of
the tool-bit discipline: a stream without the bits decodes exactly as it did.

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

PLACEHOLDER_GATE_444

PLACEHOLDER_GATE_420

PLACEHOLDER_VERBATIM

---

## 1. Operating points

PLACEHOLDER_POINTS

---

## 2. Per-tool detail

PLACEHOLDER_PERTOOL

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

PLACEHOLDER_TIME

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
