# Phase 2 inter: measurements

What the pose-warped predictor, the per-tile vector, the reference ring and the
shadow model are worth, measured on the real codec rather than on the predictor
in isolation. `warp/RESULTS.md` is the predictor's own record and this document
does not repeat it; where the two overlap they are cross-checked and the
agreement is noted.

Everything here was produced by `tools/quality/compare.py` through ffmpeg
n9.0.1, every process under `chrt -i 0 taskset -c 28-31 nice -n 19`, with
result files under `$NXQ_SCRATCH/results/`. The verdicts are computed by
`ref/phase2_verdict.py` from those files; the warp-only chain by
`ref/warp_chain.py`.

**The four things to read first.**

1. **The kill test fails**, on every sequence and in both rate bands
   (section 2). The verdict is quoted verbatim there.
2. The warp-only chain **also fails** its threshold, independently reproducing
   what `warp/RESULTS.md` measured on the predictor alone (section 3).
3. Most of the gap to the anchor is **not the inter path**. Against this
   codec's own intra, inter is worth **-12 % to -24 %** BD-rate at the high-rate
   band and **-43 % to -48 %** at the paper's own bits-per-pixel. Against
   x265-p, the intra core alone is +190 % to +227 % (high band) and +675 % to
   +886 % (low band); inter roughly halves that. The inter tools work; the
   thing they are bolted to is what loses (section 4).
4. Two encoder bugs were found by this measurement and by nothing else, both
   in the mode decision, together worth about 6 dB. Section 5. Every number
   above is after both fixes.

---

## 1. What was measured

| sequence | coded size | frames | pix | source |
|---|---|---|---|---|
| `vr-mixed-1024` | 2048x1024 (1024 per eye) | 36 | 4:4:4 and 4:2:0 | `gen_synthetic.py --motion mixed`, generated for this run |
| `vr-turn-256` | 512x256 | 12 | 4:4:4 | `corpus/` |
| `vr-mixed-512` | 1024x512 | 12 | 4:2:0 | `corpus/` |

`vr-mixed-1024` is not in `corpus/MANIFEST.json`: it was generated into
`$NXQ_SCRATCH/seq` because the manifest's largest mixed-motion entry is
`vr-mixed-512`. Its generator arguments are in section 8, so it is
reproducible; adding it to the manifest is a change to `corpus/`, which has an
owner.

The codec is driven as

```
nxv-enc --eyes 2 --inter on --poses <seq>.poses.json --qp N ...
```

so each frame is two pictures, the warp matrix is derived per eye from the
sequence's own pose log, and the anchor sees the same side-by-side frame as one
picture. The anchor is `x265-p`: `libx265`, zerolatency, P-only, one reference,
one IDR at the start — the configuration PAPER.md 2.11 item 1 names.

### Two rate bands, and why there are two

PAPER.md 2.11 says "100 to 300 Mbit". That band is stated for the paper's own
target: **8.4 Mpix stereo at 90 Hz**, where 150 Mbit/s is 0.198 bits per pixel.
`vr-mixed-1024` is 2.1 Mpix, so the *same bits per pixel* is 25 to 75 Mbit/s
there, and the literal 100-300 Mbit band on this clip is a **four to eight
times higher quality per pixel** than the paper's working point — 48 to 56 dB
PSNR-Y, effectively visually lossless.

Both are reported, because they answer different questions and they do not
agree:

| band | what it is | QP ladder |
|---|---|---|
| **A, "the literal band"** | 100-300 Mbit/s on this clip, as 2.11 item 1 says | nxv 0/4/8/12, x265 2/8/14/20 |
| **B, "the paper's bpp"** | 0.2-0.6 bpp, the density 2.4's 0.28 bpp budget describes | nxv 18/24/30/36, x265 26/32/38/44 |

Band A is where the kill test is *stated*. Band B is where the codec is
*designed to run*. Reporting only one of them would be a choice about which
answer to give.

---

## 2. The kill test (PAPER.md 2.11 item 1)

> Kill test: record 60 s each of VRChat, Beat Saber and Alyx as raw frames plus
> pose logs from WiVRn; encode with x265 (zerolatency, P-only) and with the
> Phase 2 codec; report BD-rate overall and on the 20 percent of frames with
> the highest angular velocity. **Success: within 10 percent at rest and at
> least 30 percent better on the motion frames.** Failure means the codec's
> case rests on latency and loss behaviour alone, which should be decided
> explicitly rather than assumed.

### Band A — 100-300 Mbit on this clip

BD-rate of `nxv-inter` against `x265-p` on PSNR-Y. Negative is better; these
are all positive, which means the codec needs that much **more** rate for the
same quality.

| sequence | overall | fastest 20 % | the rest | verdict |
|---|---|---|---|---|
| `vr-mixed-1024` 4:4:4 | **+160.70 %** | +151.16 % | +163.51 % | **FAIL** |
| `vr-mixed-1024` 4:2:0 | **+130.94 %** | +122.20 % | +133.52 % | **FAIL** |
| `vr-turn-256` 4:4:4 | **+156.49 %** | +142.26 % | +161.44 % | **FAIL** |
| `vr-mixed-512` 4:2:0 | **+150.00 %** | +137.30 % | +154.32 % | **FAIL** |

Velocity split at the 20th percentile: 43.4 deg/s on `vr-mixed-1024` (8 of 36
frames), 129.1 deg/s on `vr-turn-256` (3 of 12), 55.2 deg/s on `vr-mixed-512`
(3 of 12).

### Band B — the paper's own bits per pixel

| sequence | overall | fastest 20 % | the rest | verdict |
|---|---|---|---|---|
| `vr-mixed-1024` 4:4:4 | **+469.11 %** | +433.63 % | +479.52 % | **FAIL** |
| `vr-mixed-1024` 4:2:0 | **+447.83 %** | +416.63 % | +456.96 % | **FAIL** |
| `vr-mixed-512` 4:2:0 | **+321.98 %** | +286.49 % | +336.05 % | **FAIL** |

### The verdict, in the paper's own terms

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +163.51 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +151.16 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

That is `vr-mixed-1024` 4:4:4; the other seven runs print the same two words
with different numbers. **The consequence the paper attaches to this outcome
applies:** "the codec's case rests on latency and loss behaviour alone, which
should be decided explicitly rather than assumed."

### The one thing that did go the paper's way

On **every** sequence and in **both** bands, the codec loses *less* ground on
the fastest 20 % of frames than on the rest: 9 to 19 points at band A, 20 to 50
points at band B. That is the direction 2.11 item 1 predicts — the anchor
degrades faster than we do when the head turns — and it is consistent across
eight independent runs, so it is a signal rather than noise. It is also, by
itself, worth nothing: 12 points off 160 is not 30 percent better than the
anchor, it is 148 percent worse.

### A caveat that is stated once and meant throughout

The split BD-rate uses the **whole sequence's** rate on the rate axis, because
one bitstream carries both subsets and neither this harness nor ffmpeg reports
per-frame sizes for every anchor. So "BD-rate on the fast frames" means "at a
given overall bitrate, how much better is the codec on the fast frames" —
which is the question the paper is asking, but it is not a BD-rate over an
independently rate-controlled subset. `ref/phase2_verdict.py` prints this note
with every run.

---

## 3. The warp-only chain (PAPER.md 2.11 item 2)

> Test: PSNR of a 2 s warp-only chain under recorded head motion with bilinear
> and Catmull-Rom; **if the Full profile filter does not hold above 35 dB for
> 30 frames on textured content** the per-tile refresh rate must rise and the
> bit budget in 2.4 is wrong.

Produced on the real codec by `ref/warp_chain.py`, which raises the
`WARP_SKIP` gate above anything real content produces and pushes the refresh
period past the clip, so frame 0 is an ordinary intra frame and every frame
after it is nothing but the pose warp of its predecessor.

| sequence | frame 1 | last frame | decay | frames held above 35 dB | verdict |
|---|---|---|---|---|---|
| `vr-mixed-1024` 4:4:4, 36 frames | 24.40 dB | 18.44 dB | -5.96 dB | **0** | **FAIL** |
| `vr-turn-256` 4:4:4, 12 frames | 29.48 dB | 19.39 dB | -10.09 dB | **0** | **FAIL** |

The chain **starts** below the bar, so the threshold is not missed narrowly:
it is missed at the first warped frame. Version 1 is bilinear only
(`docs/SYNTAX.md` 13.4), so only the bilinear half of the paper's comparison is
measurable through the codec; `warp/RESULTS.md` has the filter comparison on
the predictor in isolation and puts Catmull-Rom about 2 dB ahead over 30
frames, which would not change the verdict.

**This independently reproduces `warp/RESULTS.md` section (b)**, which measured
28.9 / 26.7 / 25.0 dB for slow / medium / fast drift with Catmull-Rom and frame
1 at 32.4 dB. Our numbers are 3 to 5 dB lower because they are full-frame
PSNR on material that deliberately contains content the warp cannot predict —
independently moving discs, a head-locked HUD — and because frame 0 here is a
QP-8 encode rather than a lossless reference. Two implementations, two
harnesses, the same conclusion.

The practical consequence is already visible in the codec: at the QP where the
paper's bit budget lives, 67 % of tiles are `WARP_SKIP` and the picture is
being carried by the chain, which is exactly the regime this test says is
unsafe without a higher refresh rate.

---

## 4. Where the gap actually is

The kill test compares the whole codec. This section splits it.

### The inter path against this codec's own intra

Same material, same harness, `nxv-enc` with and without `--inter on`.

| sequence | band A | band B |
|---|---|---|
| `vr-mixed-1024` 4:4:4 | **-20.80 %** | **-43.27 %** |
| `vr-mixed-1024` 4:2:0 | **-21.18 %** | **-45.03 %** |
| `vr-turn-256` 4:4:4 | -12.45 % | — |
| `vr-mixed-512` 4:2:0 | -24.49 % | **-47.94 %** |

### The intra core against the same anchor

| sequence | intra vs x265-p, band A | inter vs x265-p, band A | intra vs x265-p, band B | inter vs x265-p, band B |
|---|---|---|---|---|
| `vr-mixed-1024` 4:4:4 | +227.60 % | +160.70 % | +885.95 % | +469.11 % |
| `vr-mixed-1024` 4:2:0 | +190.60 % | +130.94 % | +879.01 % | +447.83 % |
| `vr-turn-256` 4:4:4 | +190.44 % | +156.49 % | — | — |
| `vr-mixed-512` 4:2:0 | +224.90 % | +150.00 % | +675.49 % | +321.98 % |

**Read those two tables together.** The inter path does what an inter path is
supposed to do: it takes 20 % off the rate at high quality and 45 % off at the
paper's operating density, and it roughly halves the distance to x265-p. What
it cannot do is close a gap that was +190 % to +886 % before it started. The
Phase 2 kill test, on this material, is failed by the Phase 1 codec, and Phase
2 is the thing making it *less* bad.

### Why the win is not concentrated on the motion frames

Against x265-p the codec is relatively better on fast frames. Against **its own
intra** it is not: the split is flat to within half a point on every sequence
(-21.00 % fast vs -20.74 % rest; -43.61 % vs -43.17 %). So the differential is
the *anchor* degrading on head turns, not the warp paying off there.

The mode histogram says why. On `vr-mixed-1024` 4:2:0:

| QP | `WARP_SKIP` | `WARP_MV` | `STATIC_MV` | `INTRA` |
|---|---|---|---|---|
| 4 | 17.0 % | 11.8 % | 14.7 % | 56.5 % |
| 12 | 37.6 % | 7.5 % | 19.0 % | 35.9 % |
| 24 | 67.4 % | 5.3 % | 14.1 % | 13.2 % |

At band A the frame is **majority intra**, because at 48-56 dB almost nothing
survives the rate-distortion comparison as a skip. The warp is barely engaged,
so it cannot be the thing that wins or loses. At band B it carries two thirds
of the picture — and that is precisely where section 3 says the chain is
already below its threshold. The two failures are the same failure seen from
two sides: **the predictor is not accurate enough for the rate at which the
codec wants to lean on it.**

`STATIC_MV` earning 14-19 % everywhere is worth noting on its own. The material
has a head-locked HUD panel and near-field discs, and the identity predictor
wins on them exactly as PAPER.md 2.3 says it should.

---

## 5. Two bugs the measurement found

Neither could have been caught by a unit test: both produce perfectly valid,
bit-exactly-reproducible streams that are simply worse pictures. Both were in
the encoder's mode decision, which is non-normative — so both fixes change the
bitstream without changing the format.

### 5.1 The skip gate was stated on SAD

The `WARP_SKIP` early-out compared the predictor's **sum of absolute
differences** against a multiple of the quantiser step. Warp error is
impulsive: it concentrates on hard edges, so a tile can have a mean absolute
error of 0.12 LSB and a mean *squared* error of 8.7. On `vr-turn-256` at QP 0
the top row of both eyes skipped every frame with SAD around 0.2 LSB per
sample, and reconstructed at 41.9 dB while its neighbours were at 56.1 dB — one
eighth of the frame area dragging the whole frame from 56.5 to 47.4 dB.

Fixed by stating the gate on squared error against the quantiser's own noise
floor, `qstep^2 / 12` per sample, which is scale-free across QP:

```
  vr-turn-256 4:4:4, QP 0:   47.42 dB  ->  54.53 dB
```

### 5.2 Skip's distortion was charged once

A coded tile's error is bounded by its quantiser and is corrected again next
frame. A skipped tile's error is the raw prediction error, it goes straight
into the reference, and it stays there until the tile is next coded. Charging
it once makes the locally optimal choice globally wrong, and the encoder
was giving up 4 dB to save 12 % of the rate at QP 0.

The penalty is applied to the **excess** over the best coded candidate rather
than to the distortion itself, which matters: a skipped tile whose error merely
repeats what a coded tile would have left behind costs nothing extra, and that
is the repeated-frame case, where every tile must skip and does. `kSkipPersist`
is 4, the order of the interval between codings at the refresh rates this codec
runs at.

Together the two fixes are worth about 6 dB at the top of band A, and they are
why the numbers in section 2 are not the numbers this document would have
carried yesterday.

### What did NOT change

The mode-decision lambda barely matters. Sweeping it over 1.0 / 0.5 / 0.25 /
0.1 (relative to the trellis's) moved the operating points along the same
rate-distortion curve rather than off it — 51.03 dB at 24.5 Mbit against
51.28 dB at 25.7 Mbit. The default is 0.25 because an all-reference stream
should spend a little more, not because the measurement demanded it.

---

## 6. Encode and decode time

`vr-mixed-1024`, 36 frames, one core of the 4-core slice, `-O2`:

| configuration | encode | decode |
|---|---|---|
| 4:2:0, QP 24, inter | 564 ms/frame | 44.8 ms/frame |
| 4:2:0, QP 24, intra | 654 ms/frame | 27.3 ms/frame |
| 4:4:4, QP 24, inter | 819 ms/frame | 54.2 ms/frame |
| 4:4:4, QP 24, intra | 1236 ms/frame | 45.3 ms/frame |

At QP 24 **inter encodes faster than intra**: two thirds of the tiles are
skipped, and a skipped tile costs one warp instead of a full quantisation. At
QP 8, where the frame is majority intra, it is the other way round —
1218 ms/frame against 749 for 4:2:0 — because the inter path pays for the
search and for scoring three fully-quantised candidates per tile on top of
everything intra already does.

Decode is 1.2 to 1.6 times intra. The extra work is one `warp_tile()` per
plane per tile plus the reference-ring write; it is a CPU reference and these
numbers say nothing about the Vulkan budget except that the warp is not
dominant.

---

## 7. What this material can and cannot say

The synthetic sequences are rotation-only by construction, and that cuts both
ways. It is worth being explicit about which way each effect cuts, because it
is tempting to list only the flattering half.

**It flatters the warp.** The panorama is rendered through the *same*
rectilinear projection and the *same* rotation the codec's homography inverts,
so the warp is geometrically exact up to quantisation and resampling. Real
content has translation, parallax, disocclusion, moving lights and mirrors that
no rotation-only predictor recovers. A warp that fails here fails for the
codec's own reasons — which is why `corpus/README.md` chose this material — but
a warp that succeeds here has not yet been tested.

**It starves the anchor.** The generator's motion is smooth, the panorama is
static apart from a few discs and a HUD, and the clips are 12 to 36 frames. All
three favour x265: a global smooth pan is what block motion estimation with
sub-pel refinement is best at, and a short clip lets its first IDR amortise
over few frames. The paper's premise is that HEVC *breaks* on fast turns and
roll; the peak here is 120 deg/s on `vr-mixed-1024` and 150 deg/s on
`vr-turn-256`, sustained for a handful of frames, and x265 stays above 46 dB
throughout. The frames where the paper expects the anchor to fall apart are
not really present.

**It is not the material the gate is stated on.** PAPER.md 2.11 item 1 asks
for 60 s each of VRChat, Beat Saber and Alyx, captured from WiVRn with pose
logs. `corpus/README.md` says the same thing in its own words: the
`wivrn-capture` class is the one the phases are decided on, and it is empty.
Everything above is the plumbing proving itself on synthetic material. **The
verdict in section 2 is a real failure of a real measurement, and it is not the
measurement the paper asked for.** Both halves of that sentence matter.

**What would change the answer, in order of expected size.**

1. The intra core. It is +190 % to +886 % against x265-p before inter touches
   it, and it is the dominant term in every row of section 4. No amount of
   inter tooling recovers that.
2. The predictor's accuracy. Section 3 says the chain starts below its
   threshold; section 4 says the codec leans on the chain for two thirds of the
   picture at its own operating point. Sub-pel warp accuracy, a sharper filter,
   or a higher refresh rate all attack that, and `warp/RESULTS.md` already
   measures what the filter is worth (about 2 dB, not enough).
3. Real captures. The one place where the paper's central claim could still be
   true is content where the anchor genuinely breaks, and none of the material
   here contains it.

---

## 8. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
export NXW_CORPUS=$NXQ_SCRATCH/corpus
cmake -S . -B build-ref -DNXWARP_BUILD_WARP=ON && cmake --build build-ref -j4
python3 corpus/fetch.py --sync                    # vr-turn-256, vr-mixed-512

# the sequence that is not in the manifest
python3 tools/quality/capture/gen_synthetic.py --out $NXQ_SCRATCH/seq \
    --name vr-mixed-1024 --frames 36 --eye-width 1024 --eye-height 1024 \
    --motion mixed --full --pix yuv444p,yuv420p

# band A, one sequence (the others differ only in --seq and the ladders)
python3 tools/quality/compare.py \
    --seq $NXQ_SCRATCH/seq/vr-mixed-1024.yuv444p.json \
    --codec-enc "build-ref/bin/nxv-enc --quiet --eyes 2 --inter on \
                 --poses $NXQ_SCRATCH/seq/vr-mixed-1024.poses.json" \
    --codec-dec "build-ref/bin/nxv-dec --quiet" --codec-name nxv-inter \
    --anchors x265-p --qp 0,4,8,12 --anchor-qp 2,8,14,20 --no-vmaf \
    --out $NXQ_SCRATCH/results/kill-mixed1024-444.json

# band B: --qp 18,24,30,36 --anchor-qp 26,32,38,44
# the intra baseline: drop "--eyes 2 --inter on --poses ..." from --codec-enc

python3 ref/phase2_verdict.py --results $NXQ_SCRATCH/results/kill-*.json

python3 ref/warp_chain.py --seq $NXQ_SCRATCH/seq/vr-mixed-1024.yuv444p.json \
    --enc build-ref/bin/nxv-enc --dec build-ref/bin/nxv-dec --eyes 2 --qp 8
```

Everything under `chrt -i 0 taskset -c 28-31 nice -n 19`. The conformance side
is `ctest --test-dir build-ref -R 'ref\.'`, and the same suite under
`--preset asan-ubsan`, which is where the inter path is required to be clean.
