# The tournament merge

What this is: the record of merging the five tournament packages, the
perceptual rate-control wiring and `exp/entropy-lite` onto one branch, together
with the checks each step had to pass and the measurement of the result. It is
written for someone deciding whether to trust the merged encoder, so it states
what was measured, what was not, and where the merged code deviates from what
the judges recommended.

The branch is `merge-main`, started at `7830232` (main plus the detail-a step).
Nine steps, each one or two commits, in the order `docs/MERGE-PLAN.md` sets.

## 1. What the merge is, and what it is not

This was reference-codec engineering, not git merging. Every step had a design
question in it that neither branch had answered, because neither branch had to
live with the other's tool:

* **`UnitCtx`.** ctx-a conditions the entropy model per coding unit; detail-a's
  4x4 split changes what a coding unit *is*. Neither branch's accessor could
  express the other's state. The merge introduces one object carrying `ucls`,
  `v3`, `split`, `split_present`, `band_shift` and `level_fixed`, whose
  `level()` composes the split band mapping, the scan-group shift and the
  context choice in one place. That object is the merge's central design
  decision; everything downstream reads it rather than re-deriving it.
* **The transform family invariant.** xform-a shipped 16x16 and 32x32 as a
  second family beside the 8x8. Merged, there is one family over {4, 8, 16, 32}
  with a single stated invariant: *the quantiser sees orthonormal coefficients
  at unit scale at every size.* The internal 2D gains are 2^20, 2^20, 2^21 and
  2^22, and `ref.transform_gain` measures each size against a float DCT-II to
  within 0.1 %. That test is written against floating point, not against the
  codec, which is what caught the merge author's own wrong scale constant.
* **The near-skip's placement.** inter-a made it a tile structure on a word1
  bit; inter-b made it a tile-row header record. The row-header form is what
  merged, and it is what dissolved the word1 overflow the tournament was going
  to spend a ninth tile-header byte on.

## 2. The steps, and the mandatory checks

Three checks after every step. (a) **tools-off byte-identity**: encode
`vr-mixed-1024-v2` at 4:4:4 and 4:2:0, QP 16 and 32, every tournament tool off,
and `cmp` against a frozen build of `e4e85af`. (b) **ref.\* and fuzz.\* green**
under the `asan-ubsan` preset. (c) **the package's own gain re-measured** on the
merged encoder with the main harness, required to land within 25 % of the
judge's number for that package.

| # | step | commit | (a) byte-identity | (b) sanitiser ctests | (c) gain vs the judge's number |
|---|---|---|---|---|---|
| 2 | ctx-b: `TAB_V2` and `table_iters` | `29790e5` | PASS 4:4:4 + 4:2:0, QP 16/32 | 17/17 | n/a — encoder-side table fitting, judged with step 3 |
| 3 | ctx-a: `CTX_V3`, 27 contexts per coding unit | `fe4fa64` | PASS | 17/17 | ctx-a's model alone **-6.12** vs the judge's **-5.42** (12.9 % dev., inside 25 %); merged package -8.33 intra / -13.23 low-rate |
| — | fuzz: cherry-pick `fd9a878`, fold F11 into F10 | `d19ffaf` | (no encoder change) | 17/17 | n/a |
| 4 | xform-a: `XFORM_LARGE`, one family over {4,8,16,32} | `7826e2f` | PASS | 18/18 incl. new `ref.transform_gain` | 4:4:4 **-29.17**, 4:2:0 **-38.08** vs the judge's **-29.17 / -38.08** — reproduced to the decimal, BD-PSNR included |
| 5 | inter-a's package, inter-b's near-skip placement | `54a5a25` | PASS | 32/32 incl. new `warp.quad` | band A **-23.84** vs the judge's **-24.65** (3.3 % dev.) |
| 6 | rdo-b's rate model plus five rdo-a ports | `203fc6a` | **FAILS by construction** — substitute check below | green | rdo alone **-44.33** vs the judge's **-44.56** (0.5 % dev.); rdo+inter -53.06 |
| 7 | percept: `EncDriver` wiring, `--rc` off by default | `a070938` | **byte-for-byte identical to step 6's output**, zero vectors moved | green | n/a — inert on the default path, which is the property it must have |
| 8 | the renumbering pass, python bindings, one SYNTAX | `d55ff26` | unchanged from steps 6-7, no new drift | **56/56 full ctest** | n/a |
| 9 | `exp/entropy-lite`: `ENTROPY_LITE` on bit 30 | `ad02dae` | substitute PASS, tool set `0x6200cd` / `0x6200c5` | **57/57** | bits **+43.0 %** 4:4:4 (in the +40-50 % band), **+31.9 %** 4:2:0 (below it); Pass A on lavapipe **zero mismatches** |
| 9b | the renumber's stragglers, python `ENTROPY_LITE` rule | `1f490f4` | (docs, comments and python only) | 57/57, python 707 passed | n/a |

### Where byte-identity stopped being available, and what replaced it

Check (a) is a check on **bitstream** tools: a tool that ships off must leave
the default encoder's output alone. Step 6 is an encoder-only package. `rdo`
has no tool bit and no off switch — it changes *which* stream the encoder emits
for the same input, which is the entire point of it, and both
`docs/TOOLBITS.md` 5 and `MERGE-PLAN` 4.3 record in advance that it rewrites
every existing conformance vector. 69 vectors moved at that step.

So from step 6 the check was run in the form that carries its intent, and it is
reported as a substitute rather than as a pass:

1. the merged encoder with every tournament tool off must declare the **same
   tool set** as the frozen `e4e85af` build — `0x6200cd` at 4:4:4 and
   `0x6200c5` at 4:2:0, no bit at or above 19 set; and
2. the **frozen v1.4 decoder** must accept and fully decode all four merged
   streams.

Both hold at every step from 6 onward (`judge-base/toolsoff.sh`). The point of
the original check — that a bitstream tool has leaked onto the default path —
is exactly what condition 1 tests, and condition 2 adds that nothing in the
encoder-side work made a stream a v1.4 decoder cannot read.

## 3. Where the merged code deviates from the brief

Three places. Each was a measured decision, not a slip, and each is recorded in
its step's commit message.

**The near-skip carries ramps, and they are not optional.** The brief asked for
inter-b's row-header placement with "DC only, no ramp". The placement merged as
asked. The ramps did not: inter-a flagged them with a second word1 bit and its
encoder then never chose them, so `near_skip_ac`, three of the record's nine
bytes and the `>> log2(nb)` arithmetic its spec spends a paragraph on were
exercised by nothing — not a vector, not `ref.inter`. Shipping a DC-only record
would have kept dead syntax alive in the spec while removing the only form the
encoder can actually reach. The record is one size, always nine bytes, always
fitted. `spec/annex-d-inter-decisions.md` D-24 states the amendment rather than
quietly applying it.

**Near-skip is never chosen at vector scale.** Two RD charge models were
measured for it. The persistence-consistent one — the one that charges the
reference-persistence factor exactly once, which is the defect step 6 fixed in
rdo — selects zero near-skip tiles at every QP. The simpler one selects them and
buys -6.0 % at QP 20. The measured, consistent model is what shipped. Both
numbers are on the record, and the two conformance vectors that would have
pinned an empty near-skip path were dropped rather than committed pinning
nothing.

**`ENTROPY_LITE`'s bit cost is not one number.** The brief states +40 to +50 %
at QP 24. On this corpus that is true of 4:4:4 (+43.0 %) and not of 4:2:0
(+31.9 %) — where Lite is *cheaper* than advertised. The first hypothesis, that
the merged RD work had enlarged the rANS denominator, does not survive
measurement: at `--rdoq-effort 1` the 4:2:0 ratio moves to +28.2 %, the wrong
way by 3.7 points. It is the chroma planes' own statistics. Quote the two
formats separately.

## 4. What the checks caught

The value of a check is what it stops, so:

* **`NXVC_TOOL_XFORM_LARGE` was still on bit 24** after the step-4 merge,
  colliding with `INTRA_CFL`. The header merged cleanly and the whole tree
  compiled; only the streams were wrong. The vector generator refused `v68`.
  This is precisely the failure `MERGE-PLAN` 4.6 predicts, and it happened
  twice — `ENTROPY_LITE` arrived on bit 24 as well at step 9.
* **An ASan global-buffer-overflow reading `kScan4Split`.** The per-tile
  transform-size search copied the tile params and overwrote `xform_size`,
  leaving `split4 = 1` at 16x16 — a combination the syntax forbids. Fixed by
  moving the derivation into `TileCoder::setup()` so it cannot be bypassed by
  copying a struct.
* **`ref.transform_gain` failed on its first run at all four sizes** — the
  merge author's own wrong scale constant, not the transform. Because the test
  is written against a float DCT-II rather than against the codec, it said so
  instead of agreeing with the bug.
* **The python parser found a real encoder bug.** `--tskip auto` decided tskip
  *after* the tile params were built, so a tile could carry `split4x4` and
  `tskip` together, which is illegal. The first fix called `setup()` there and
  wiped the loaded tile; the round-trip tests caught that in turn, and it
  narrowed to `apply_tskip_to_split()`.
* **`build_inter()` ignored the ctx and tab columns of its own spec table**, so
  `v66` and `v67` pinned nothing and `v67` was byte-identical to `v53`.
* **A discrepancy that was not one.** Band A first measured at -17.98 against
  the judge's -24.65, a 27 % deviation and a stop-and-report. It was measured on
  the *ctx* judge's ladder. On `JUDGE-inter.md`'s own ladder — QP 0/4/8/12
  against x265-p at 2/8/14/20 — it is -23.84, a 3.3 % deviation. Verify the
  ladder before declaring a discrepancy.

## 5. The tool map as it ships

Every tournament tool ships **off**. That is not caution, it is the design:
each of them is a negotiated tool whose value depends on something only one end
of the link knows.

| bit | tool | default | who decides, and on what |
|---|---|---|---|
| 19 | `XFORM_4X4_SPLIT` | off | the encoder, per tile, by RD |
| 24 | `INTRA_CFL` | off | the encoder, by RD |
| 25 | `CTX_V3` | off | the encoder; requires `CTX_V2` |
| 26 | `TAB_V2` | off | the encoder; requires `CUSTOM_TABLES` |
| 27 | `XFORM_LARGE` | off | the encoder, per tile, by RD |
| 28 | `NEAR_SKIP` | off | the encoder, from the measured drift |
| 29 | `QUAD_MV` | off | the encoder, from the warp residual |
| 30 | `ENTROPY_LITE` | off | **the decoder**, from its own measured Pass A time |
| 31+ | reserved | — | `RESERVED_FROM = 31`; bit 30 was the last free bit and this merge spent it |

`ENTROPY_LITE` is the one whose asker is on the other end. The encoder cannot
know a client's Pass A time; the client cannot know the link's spare bits. The
tools mask is already an intersection of what the receiver offered
(`SYNTAX.md` 2.3), which is the right shape for exactly this, and it is why the
tool is specified as negotiated rather than as an encoder preset.

Tile-header word1 is now full: bit 28 `split4x4`, bits 29-30 `xform_size`,
bit 31 `quad_mv`. There is no reserved bit left in it — `r09`, the rejection
vector that pinned "word1 bit 28 is reserved", moved to bit 31 and then to
word0 bit 3. The near-skip's move into the tile-row header is what made that
fit; had it stayed a tile structure the tournament would have spent a ninth
tile-header byte.

`ENTROPY_LITE` is mutually exclusive with `SIGN_HIDE` and `CUSTOM_TABLES`: the
table-free coder transmits no table for `CUSTOM_TABLES` to compact and has no
arithmetic state for a hidden sign to ride on. That rule is enforced in the
encoder, in the C decoder, in `StreamHeader.validate` on the python side, and
by rejection vectors, because a rule enforced in only some of those is a rule
the four models disagree about.

## 6. The final measurement

*(numbers below are the merged encoder on `vr-mixed-1024-v2`, the **full** clip
at 36 frames, not the judges' 12-frame runs; the QP ladders were chosen by
probe so that every codec point lands inside the gate's 100-400 Mbit band.)*

### Phase 1, 4:4:4 — intra against x264 intra

```
  BD-rate of merged on PSNR-Y (negative is better):
    vs x264-intra     +36.04 %   BD-PSNR -1.936 dB   (overlap 53.93-57.97 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -2.440 dB at 229.5 Mbit/s, mean -2.044 dB over 123.8-229.5 Mbit/s
```

| QP | rate | PSNR-Y | SSIM | | anchor QP | rate | PSNR-Y | SSIM |
|---|---|---|---|---|---|---|---|---|
| 0 | 229.5 Mbit/s | 57.97 dB | 0.9991 | | 6 | 264.5 Mbit/s | 61.59 dB | 0.9995 |
| 2 | 183.5 Mbit/s | 56.46 dB | 0.9987 | | 10 | 174.6 Mbit/s | 58.14 dB | 0.9990 |
| 4 | 162.1 Mbit/s | 55.84 dB | 0.9985 | | 14 | 119.7 Mbit/s | 55.81 dB | 0.9985 |
| 6 | 141.6 Mbit/s | 55.00 dB | 0.9983 | | 18 |  87.1 Mbit/s | 53.72 dB | 0.9980 |
| 8 | 123.8 Mbit/s | 53.93 dB | 0.9980 | | 22 |  62.9 Mbit/s | 49.19 dB | 0.9974 |

**This is the first gate verdict the project has that is neither FAIL-on-a-band-
nobody-covered nor "not evaluated".** Every judge in the tournament reported one
of those two, because the mandated ladder `--qp 16,20,24,28,32` puts every
operating point of both codecs *below* the 100-400 Mbit band on this sequence.
Probing the ladder first — QP 0/2/4/6/8 against x264-intra at 6/10/14/18/22 —
puts all five codec points and three anchor points inside the band, so the gate
returns a real verdict for the first time: **FAIL by 1.44 dB at its worst
point**, needing to be within 1.0 dB and being 2.44 dB out.

That is a considerably smaller gap than the tournament's numbers suggest. The
judges' BD-rates against this anchor run from +79 % to +115 %; the merged
encoder with everything on is **+36.04 %**. Part of that is the tools composing
and part of it is the band: measured up where the gate actually is, the codec
is closer to x264 intra than the low-rate measurements made it look. Both
effects are real and neither of them passes the gate.

### Encode and decode time

```
    codec             enc ms/frame  dec ms/frame   x the 6.8 ms budget
    x264-intra                13.5           9.6                   3.4
    merged                  3511.1          84.4                 528.7
```

**3.5 seconds per frame.** This is the number that should stop anyone reading
the BD-rate above and feeling encouraged. The reference encoder at its default
preset runs a full RD trellis, a per-tile transform-size search, a per-tile QP
search and four intra modes, single-threaded per tile over cores 20-23; it is a
correctness model that happens to compress, not an encoder. The decoder's
84.4 ms is the meaningful half of the pair, and it is 12x the 6.8 ms budget on
the CPU reference path — which is the entire reason the Vulkan decoder and
`ENTROPY_LITE` exist.

### Phase 1, 4:2:0 — intra against x264 intra

```
  BD-rate of merged on PSNR-Y (negative is better):
    vs x264-intra     +18.89 %   BD-PSNR -1.124 dB   (overlap 53.88-57.98 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -1.421 dB at 110.1 Mbit/s, mean -1.110 dB over 110.1-193.9 Mbit/s
```

| QP | rate | PSNR-Y | SSIM | | anchor QP | rate | PSNR-Y | SSIM |
|---|---|---|---|---|---|---|---|---|
| 0 | 193.9 Mbit/s | 57.98 dB | 0.9991 | | 6 | 262.9 Mbit/s | 61.60 dB | 0.9995 |
| 2 | 158.3 Mbit/s | 56.46 dB | 0.9987 | | 10 | 173.4 Mbit/s | 58.16 dB | 0.9990 |
| 4 | 140.8 Mbit/s | 55.85 dB | 0.9985 | | 14 | 118.8 Mbit/s | 55.79 dB | 0.9985 |
| 6 | 124.3 Mbit/s | 55.01 dB | 0.9983 | | 18 |  86.8 Mbit/s | 53.74 dB | 0.9980 |
| 8 | 110.1 Mbit/s | 53.88 dB | 0.9980 | | 22 |  62.6 Mbit/s | 49.21 dB | 0.9974 |

```
    codec             enc ms/frame  dec ms/frame   x the 6.8 ms budget
    x264-intra                10.8           6.6                   2.6
    merged                  1975.9          47.7                 297.6
```

**+18.89 %, and 0.42 dB from the gate.** This is the closest the codec has come
to Phase 1 in the project's history, and it is worth being precise about why,
because two different things are contributing and only one of them is
engineering:

* **The tools.** 4:2:0 is where `XFORM_LARGE` pays most — the judge measured
  -38.08 points there against -29.17 at 4:4:4, because a subsampled chroma
  plane is the smoothest surface in the picture and the 32x32 transform is
  built for exactly that. The 4:4:4/4:2:0 spread in the merged number, +36.04
  against +18.89, is the same effect at full strength.
* **The band.** Measuring inside 100-400 Mbit rather than at 25-80 flatters
  every rate-distortion curve that was fitted low, and the tournament's numbers
  were all fitted low.

The honest summary is that the gate is **FAIL on both formats**, by 1.44 dB at
4:4:4 and by 0.42 dB at 4:2:0. The second of those is close enough that it is
now a legitimate target rather than a category error, which was not true of any
number this project had before this merge.

### Phase 2 kill test, band A — stereo, inter on, against x265-p

Verdict verbatim, `ref/phase2_verdict.py` on the run:

```
=== vr-mixed-1024-v2.yuv444p  (killA-yuv444p.json)
  codec merged against x265-p, PSNR-Y
  velocity split at the 20th percentile = 43.4 deg/s (8 of 36 frames)
    overall (all frames)          BD-rate +240.39 %  BD-PSNR -5.584 dB
    fastest 20 % of frames        BD-rate +235.11 %  BD-PSNR -5.452 dB
    the remaining frames          BD-rate +241.61 %  BD-PSNR -5.621 dB
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +241.61 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +235.11 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

| QP | rate | PSNR-Y | SSIM | | x265-p QP | rate | PSNR-Y | SSIM |
|---|---|---|---|---|---|---|---|---|
| 0 | 160.5 Mbit/s | 56.93 dB | 0.9989 | | 2 | 114.3 Mbit/s | 61.34 dB | 0.9995 |
| 4 |  98.2 Mbit/s | 54.71 dB | 0.9984 | | 8 |  50.3 Mbit/s | 56.63 dB | 0.9988 |
| 8 |  64.4 Mbit/s | 52.42 dB | 0.9978 | | 14 |  24.3 Mbit/s | 53.80 dB | 0.9983 |
| 12 |  40.9 Mbit/s | 49.38 dB | 0.9967 | | 20 |  12.3 Mbit/s | 50.47 dB | 0.9973 |

**+240.39 %, against the tournament's +335 to +352 %.** The individual judges
measured this same band at +351.87 % for the base, +328.03 % with the inter
package and +298.81 % with rdo and inter together, all on 12 frames. With every
package on and the full clip it is +240.39 %. That is a real improvement of
roughly 110 points and it does not come close to mattering: the test asks for
+10 % at rest and the codec is at +241.61 %.

The verdict was never in doubt and the reason is structural.
`ref/RESULTS-inter.md` 5 measured the **intra core alone** at +190 % to +886 %
against this anchor before the inter path touches anything, and not one of the
seven packages in this merge touches the intra core. Everything merged here
improves the residual coding of a picture whose prediction is the problem.

### Phase 2 kill test, band B — the low-rate band

```
=== vr-mixed-1024-v2.yuv444p  (killB-yuv444p.json)
  codec merged against x265-p, PSNR-Y
  velocity split at the 20th percentile = 43.4 deg/s (8 of 36 frames)
    overall (all frames)          BD-rate +312.33 %  BD-PSNR    n/a
    fastest 20 % of frames        BD-rate +320.81 %  BD-PSNR    n/a
    the remaining frames          BD-rate +309.99 %  BD-PSNR    n/a
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +309.99 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +320.81 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

**Read this one with the caveat the harness itself printed**, because the number
is weaker evidence than band A's:

```
        BD-PSNR unavailable: the two curves do not overlap in rate
        (anchor spans [0.8831, 12.29], test spans [15.1, 64.41]);
        pick matched operating points closer together
```

x265-p at QP 20-44 sits between 0.88 and 12.3 Mbit/s while the codec at QP 8-20
sits between 15.1 and 64.4. The curves overlap in *quality* (42.81-50.47 dB),
which is what BD-rate integrates over, so the figure is computed — but every
rate it compares against is extrapolated past the end of the anchor's measured
range. **+312.33 % is indicative, not a measurement**, and the honest form of
the band B result is that the codec spends 15 Mbit/s where x265-p spends 1.
Band A, where both curves have real points in the same decade, is the number to
quote.

For the record it still improves on the tournament: `JUDGE-xform.md` measured
band B at +454.96 % rising to +398.86 %, and `JUDGE-inter.md` at +459.53 % and
+513.88 % for its two branches.

### The four verdicts together

| band | format | anchor | BD-rate | verdict |
|---|---|---|---|---|
| Phase 1 | 4:4:4 | x264-intra | **+36.04 %** (BD-PSNR -1.936 dB) | **FAIL** — worst -2.440 dB, needs 1.0 |
| Phase 1 | 4:2:0 | x264-intra | **+18.89 %** (BD-PSNR -1.124 dB) | **FAIL** — worst -1.421 dB, needs 1.0 |
| Kill test A | 4:4:4 stereo | x265-p | **+240.39 %** (BD-PSNR -5.584 dB) | **FAIL** |
| Kill test B | 4:4:4 stereo | x265-p | +312.33 % (curves do not overlap in rate) | **FAIL** |

Encode and decode, milliseconds per frame, all four runs:

| run | codec enc | codec dec | anchor enc | anchor dec |
|---|---|---|---|---|
| Phase 1 4:4:4 | 3511.1 | 84.4 | 13.5 | 9.6 |
| Phase 1 4:2:0 | 1975.9 | 47.7 | 10.8 | 6.6 |
| Kill test A | 3207.0 | 76.2 | 46.3 | 7.0 |
| Kill test B | 1979.8 | 66.4 | 22.1 | 6.2 |



## 7. What this merge did not settle

* **Phase 1 is not passed and this merge was never going to pass it.** The
  packages are worth between -6 and -38 BD-rate points each against a gap of
  roughly +100 %. Every judge says so about its own pair; the merged number
  says it again with all of them switched on at once. What the merge buys is
  that the tools compose — the gains are not mutually exclusive and the
  bitstream stayed coherent — not that the codec is competitive with x264
  intra.
* **The kill test is FAIL in every configuration anyone has measured**, and
  `ref/RESULTS-inter.md` 5 explains why: the intra core is +190 % to +886 %
  against x265-p before the inter path touches it, and nothing in any of these
  seven packages touches the intra core.
* **`ENTROPY_LITE`'s 7.5x is a Pico 4 measurement, not a merge measurement.**
  What this merge verified is correctness — zero mismatches through the Vulkan
  decoder's Pass A on lavapipe, on the dense, sparse and mode-unit corpora —
  and the bit cost. The speedup is the branch's number on the branch's device.
* **The Lite rate model is still the rANS one.** The trellis, the table-set
  choice and the mode decision all price coefficients as rANS would when Lite
  is selected. That makes the bit comparison exact, and it makes the Lite
  column a *ceiling*: a rate model matched to the Lite syntax should recover
  some of the cost. Nobody has measured how much.
* **`RICE` is specified and unreachable by default.** It costs about 11 more
  points of bits than `FIXED` for no Pass A win that justifies a second decoder
  path. It stays in `SYNTAX.md` 9.10 because removing a specified variant is a
  syntax change and keeping it costs nothing.
* **Sub-tile intra is withdrawn.** Measured at -0.50 and +0.59 points, shipped
  off, and it spent word1's last reserved bit. Bit 26 is free.

## 8. Reproducing any of this

The frozen base encoder, the check scripts and the measurement drivers are in
`$NXQ_SCRATCH/judge-base` (`NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp`):

| script | what it does |
|---|---|
| `byteid.sh <enc> [off flags...]` | the original byte-identity check against the frozen `e4e85af` build |
| `toolsoff.sh <enc> [off flags...]` | the substitute: same declared tool set, and the frozen v1.4 decoder accepts |
| `measure.sh <label> <outdir> <pix> [flags...]` | one row on a judge's own band, 12 frames, `BAND=intra\|low\|gate\|killA\|killB\|rdo` |
| `final.sh <band> <pix> [flags...]` | the runs in section 6: full clip, PSNR + SSIM, `BAND=phase1\|killA\|killB` |
| `final-rest.sh` | the three runs after Phase 1 4:4:4, strictly one encode at a time |

Two traps worth inheriting, both of which cost time here:

* **Verify the ladder before believing a deviation.** A package's gain is only
  comparable against the ladder its judge measured it on. Band A on the ctx
  judge's ladder reads -17.98 and looks like a 27 % discrepancy; on
  `JUDGE-inter.md`'s own ladder it is -23.84 against -24.65.
* **Never sequence background jobs with `pgrep -f` on the script's own
  argument text.** The first version of `final-rest.sh` waited with
  `while pgrep -f 'compare.py'`, and the wrapper shell it was launched from
  carries the whole command text including that string — so it matched itself
  and would have waited forever. It is commented in the script now.

The CPU discipline every number above was measured under:
`chrt -i 0 taskset -c 20-23 nice -n 19`, `-j4`, `ffmpeg -threads 4`, one encode
at a time, never cores 0-15. Numbers measured under different load are not
comparable to these, and the encode-time column especially is not.
