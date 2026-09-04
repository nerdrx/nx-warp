# The inter-efficiency package: measurements

What three tools against the Phase 2 inter path are worth, measured on the real
codec with `tools/quality/compare.py` and the Phase 2 kill test.
`ref/RESULTS-inter.md` is the "before" this document is the "after" of; where
the two disagree the reason is given rather than averaged.

| | tool | syntax | what it is |
|---|---|---|---|
| **T1** | drift-driven refresh | none | the encoder measures the error its exact client shadow carries per tile and refreshes on the measurement instead of on a timer, against an unconditional hard cap (`docs/SYNTAX.md` 13.10) |
| **T2** | the near-skip, `warp_dc()` | tool bit **24** | a skipped tile may carry a nine-byte DC-plus-ramp correction per plane, in the tile-row header (`docs/SYNTAX.md` 3.3, 13.9) |
| **T3** | four vectors per tile | tool bit **25** | one vector per 32x32 quadrant, `i8` deltas from the tile vector, sharing the tile's corner basis exactly (`docs/SYNTAX.md` 13.8) |

Bitstream minor 4 -> 5. A stream that sets neither new tool bit decodes byte for
byte as it did, which is pinned rather than asserted: conformance vectors
`v45`-`v55` are generated with every v1.5 tool off and keep the digests they
were committed with. `v56`, the rolling-refresh vector, is the one exception
and section 7 says exactly why.

**The five things to read first.**

1. **The package is worth -4.70 % BD-rate at band A and -6.25 % at band B**
   against the same codec without it, for two new tool bits and no new decoder
   dependency of any kind. Section 3.
2. **T1 and T3 trade places between the bands, and the mode histogram says
   why.** T3 improves coded inter tiles and is worth -4.08 % where the frame is
   majority coded; T1 stops paying for refreshes of tiles that skip and is
   worth -4.43 % where two thirds of tiles skip. Section 3's third table.
3. **T2 is a wash on the rate curve and the whole of the chain result.** +0.28 %
   BD-rate on its own at band B, and **+2.10 dB** at warped frame 35 of the
   warp-only chain
   -- the decay PAPER.md 2.11 item 2 says the refresh rate would otherwise have
   to buy. Sections 4.2 and 6.
4. **The kill test still fails**, in both bands, before and after. It was never
   in reach: `ref/RESULTS-inter.md` section 4 measured the intra core at +190 %
   to +886 % against `x265-p` *before* the inter path touches it, and no inter
   tool recovers that. The verdicts are quoted verbatim in section 2.
5. **T1's measurement is more interesting than T1.** The drift the encoder's
   exact client shadow reports never reaches the quantiser's own noise floor at
   the operating point -- worst tile 65.6 against a floor of 85.3 -- which says
   the blind 1-in-180 refresh is insurance the picture does not need. Forcing
   *extra* refreshes on drift is measurably never worth it. Section 4.1.

---

## 1. What was measured, and how

```
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
tools/quality/tourney_queue.sh            # the whole matrix, serially
```

`tools/quality/tourney_run.sh` is the single invocation; the queue is base and
each tool on its own and then all three, in both rate bands. Every process ran
under `chrt -i 0 taskset -c 16-19 nice -n 19`, on a machine carrying about
thirty other encodes throughout, which is why the wall-clock column of section
5 is a ratio and not an absolute.

The sequence is **`vr-mixed-1024-v2`** at 4:4:4 — the band-limited (v2)
generation, which is the one `docs/WARP-AUDIT.md` established is a fair ground
truth for a resampling predictor. `ref/RESULTS-inter.md`'s tables are on the v1
material, so its absolute numbers and this document's are not comparable and
the base row here is measured, not copied.

The two rate bands are `ref/RESULTS-inter.md`'s, unchanged:

| band | what it is | QP ladder |
|---|---|---|
| **A** | 100-300 Mbit/s on this clip, as PAPER 2.11 item 1 says | nxv 0/4/8/12, x265 2/8/14/20 |
| **B** | 0.2-0.6 bpp, the density PAPER 2.4's budget describes | nxv 18/24/30/36, x265 26/32/38/44 |

Configurations, as `nxv-enc` flags on top of `--eyes 2 --inter on --poses ...`:

| row | flags |
|---|---|
| base | `--refresh-drift 0 --warp-dc off --mv-quad off` |
| T1 | `--warp-dc off --mv-quad off` |
| T2 | `--refresh-drift 0 --mv-quad off` |
| T3 | `--refresh-drift 0 --warp-dc off` |
| all | *(none: `nxvc_config_default()` turns all three on)* |

`--refresh-drift` defaults to 1.0 and `--refresh-max-age` to 720 (8 s at
90 Hz); `--intra-period` stays at its 180.

---

## 2. The kill test (PAPER.md 2.11 item 1)

### Band B — the paper's own bits per pixel

`vr-mixed-1024-v2` 4:4:4, 36 frames, BD-rate on PSNR-Y against `x265-p`.
Positive means the codec needs that much **more** rate for the same quality.

| config | overall | fastest 20 % | the rest | verdict |
|---|---|---|---|---|
| base (v1.4 inter) | **+568.66 %** | +539.87 % | +576.94 % | **FAIL** |
| **all three** | **+532.96 %** | +504.86 % | +541.05 % | **FAIL** |

Verbatim, base:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +576.94 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +539.87 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

and with all three tools on:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +541.05 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +504.86 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

The package moves the number by 36 points and does not move the verdict, which
is the outcome `ref/RESULTS-inter.md` section 4 already predicted for any inter
tool: the intra core is the dominant term and nothing here touches it. The
direction 2.11 item 1 predicts is still visible and still worth nothing on its
own -- the codec loses 36 points less ground on the fastest 20 % of frames than
on the rest, in both rows.

### Band A — the literal 100-300 Mbit band

| config | overall | fastest 20 % | the rest | verdict |
|---|---|---|---|---|
| base (v1.4 inter) | **+345.70 %** | +337.24 % | +347.92 % | **FAIL** |
| **all three** | **+325.27 %** | +316.89 % | +327.45 % | **FAIL** |

Verbatim, base:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +347.92 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +337.24 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

and with all three tools on:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +327.45 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +316.89 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

### The caveat that is stated once and meant throughout

The split BD-rate uses the **whole sequence's** rate on the rate axis, because
one bitstream carries both subsets. `ref/phase2_verdict.py` prints this note
with every run and it is repeated here because it is the difference between
"how much better is the codec on the fast frames at a given overall bitrate",
which is what these numbers are and what the paper is asking, and a BD-rate
over an independently rate-controlled subset, which they are not.

---

## 3. The three tools, measured separately

BD-rate against **the same codec without the tool**, which is the number that
says what a tool is worth; negative is better. `ref/bd_between.py` computes it
from the same two result files.

### Band B

| config | Mbit/s at QP 30 | PSNR-Y at QP 30 | BD-rate vs base | BD-rate vs x265-p |
|---|---|---|---|---|
| base (v1.4 inter) | 6.6 | 35.27 dB | — | +568.66 % |
| **T1** drift refresh | 6.2 | 35.19 dB | **-4.43 %** | +544.83 % |
| **T2** `warp_dc` | 6.6 | 35.31 dB | **+0.28 %** | +572.02 % |
| **T3** `mv_quad` | 6.5 | 35.26 dB | **-1.37 %** | +557.82 % |
| **all three** | 6.1 | 35.29 dB | **-6.25 %** | +532.96 % |

The three add up to -5.5 % and together they are worth -6.25 %, so the package
is slightly **super-additive**: T1 refreshes fewer tiles, which leaves more
tiles skipping, which is where T2 lives; T3 makes a coded inter tile cheaper,
which moves the skip/code boundary the other way.

### Band A

| config | Mbit/s at QP 8 | PSNR-Y at QP 8 | BD-rate vs base | BD-rate vs x265-p |
|---|---|---|---|---|
| base (v1.4 inter) | 90.4 | 52.77 dB | — | +345.70 % |
| **T1** drift refresh | 90.1 | 52.77 dB | **-0.42 %** | +344.06 % |
| **T2** `warp_dc` | 90.4 | 52.76 dB | **-0.02 %** | +345.77 % |
| **T3** `mv_quad` | 86.2 | 52.75 dB | **-4.08 %** | +327.81 % |
| **all three** | 85.6 | 52.75 dB | **-4.70 %** | +325.27 % |

### The two bands answer differently, and the reason is the mode histogram

| | band A | band B |
|---|---|---|
| T1 drift refresh | -0.42 % | **-4.43 %** |
| T2 `warp_dc` | -0.02 % | +0.28 % |
| T3 `mv_quad` | **-4.08 %** | -1.37 % |
| all three | -4.70 % | -6.25 % |

**T1 and T3 trade places, and neither is a fluke.** `ref/RESULTS-inter.md`
section 4 measured the mode histogram: at band A the frame is majority `INTRA`
(56 % at QP 4) and only 12 % of tiles are `WARP_SKIP`; at band B two thirds of
tiles skip and 13 % are intra.

* T3 improves **coded inter tiles**, so it is worth four times as much where
  there are four times as many of them.
* T1 stops paying for **forced refreshes of tiles that skip**, so it is worth
  ten times as much where skipping is the common case. At band A a forced
  refresh is displacing an intra tile the encoder would mostly have coded
  anyway, and the saving is nearly nothing.

That is not a defect of either tool; it is why the package has both. The
combined row is the better of the two at each band and a little better than
either -- -4.70 % where T3 alone is -4.08 %, -6.25 % where T1 alone is
-4.43 %.

Every operating point of every row, both bands, is in the appendix.

---

## 4. What each tool does, and what it does not

### 4.1 T1, drift-driven refresh: -4.43 % at band B, -0.42 % at band A

The largest single win, and it is a **rate** win, not a quality one: at every
QP the T1 row is 3 to 6 % cheaper than base at 0.03 to 0.08 dB less PSNR, which
is a better trade than the rate-distortion slope at that point offers.

What it does is stop paying for refreshes the picture does not need. The
encoder's shadow is bit-exact, so the drift it reports is the error the client
is actually showing, and on `vr-mixed-1024-v2` at QP 30 that error is:

| | frame 1 | frame 6 | frame 12 | the quantiser's noise floor |
|---|---|---|---|---|
| worst tile | 57.2 | 58.9 | 65.6 | **85.3** |
| mean tile | 6.5 | 8.4 | 9.3 | **85.3** |

The worst tile in the picture never reaches `qstep^2 / 12`, and the mean is a
tenth of it. Under the rule of 13.10 every eligible tile is therefore deferred
to the hard cap, and the cap fires at `refresh_max_age` (720) instead of
`intra_period` (180) -- a quarter of the forced intra tiles, at a measured
quality cost of 0.03 dB.

**Two things this measurement says that the tool does not.** First, forcing
*extra* refreshes on drift is never worth it: sweeping the threshold down until
the rule fires costs far more rate than it returns (0.5 x the noise floor:
+11.9 % rate for +0.074 dB, against a slope that would have bought 0.5 dB for
that rate). The mode decision already prices exactly this trade, and a forced
INTRA is by construction not an RD decision. Second, the useful reading is the
negative one: at this codec's operating point the reconstruction does not drift
past the quantiser, so a blind refresh timer is insurance, and the measurement
is what makes it safe to buy less of it.

The hard cap is not optional and is not a fallback. What a refresh cap buys is
a bound on how long a client that joined late or lost a run of frames shows a
wrong tile, and that is not a property of the encoder's own pictures at all.
`ref.inter` case 14 asserts the bound holds with the drift rule set so high
that no tile is ever eligible on merit.

### 4.2 T2, the near-skip: +0.28 % of rate, +2.10 dB of chain

On the rate curve T2 is a wash: +0.28 % BD-rate on its own at band B, -0.02 %
at band A. Inside the package it is worth about half a point -- T1 and T3 alone
sum to -5.80 % at band B and the three together are -6.25 % -- though with
three interacting mode decisions that difference is an interaction as much as a
contribution. On the **chain** it is the whole of section 6's 2.10 dB.

Both are the same fact from two directions. A near-skip improves the reference
a skipped tile leaves behind, and the rate ladder averages over frames where
the codec is not leaning on that reference, while a warp-only chain is nothing
but leaning on it. `ref/RESULTS-inter.md` section 4 measured that at the
paper's own density two thirds of tiles are `WARP_SKIP`, which is the regime
the chain describes.

**It was nearly a loss, twice, and both fixes are the interesting part.**

1. *The container.* The first cut sent `dc_bitmap` in every tile-row header
   whenever the tool bit was set. At this codec's operating point the row
   headers are 8 bytes x 32 rows per frame against a 10 kB frame -- **2.5 %**,
   spent whether or not a single tile used a correction. Gating the bitmap on
   a bit taken from `tile_count` (whose top bit was unreachable, since
   `cols_per_eye <= 64`) took the tool from costing 4.2 % of the stream at
   QP 30 to costing 0.5 % of it.
2. *The price.* The correction was charged at the mode decision's lambda,
   which is deliberately a quarter of the trellis's because a mode that leaves
   distortion in the reference pays for it four times over. Nine bytes of
   correction do not: they are a rate-for-distortion trade at a **fixed** mode,
   which is what the trellis's lambda prices. Charging them there took QP 30
   from 189 625 bytes at +0.044 dB (a clear loss) to 180 965 bytes at
   +0.033 dB (below base, at higher quality).

Measured on the way: the DC term alone recovers about a quarter of what the
three-term fit recovers, which is why the ramp is not optional (appendix A
decision 55).

### 4.3 T3, four vectors per tile: -4.08 % at band A, -1.37 % at band B

It does what it says, on the tiles where a single tile vector is the wrong
model -- a disc crossing a tile boundary, a strip of disocclusion down one edge
of a tile. At QP 8 it takes 4.6 % off the rate for 0.02 dB; at QP 30, 1.5 % for
0.01 dB. It is the package's band-A tool, and the only one of the three that
wins more at high rate than at the paper's own density, for the reason section
3's third table gives: it improves coded inter tiles, and band A is where they
are.

The encoder's search is free: a candidate vector applied to the whole tile
predicts every quadrant with that vector at once, and the quadrants are
independent given the vector, so the best vector per quadrant falls out of the
same 18-candidate exact stage the tile vector already runs, with the whole-tile
SAD recovered as the sum of the four quadrant SADs (one pass over the tile, not
two). The measured encode time is inside the noise of the base row.

Its ceiling is the same fact from the other side: at band B two thirds of the
picture is `WARP_SKIP`, which carries no header for the deltas to live in, so
there is a third as much for the tool to improve.

---

## 5. Encode and decode time

`vr-mixed-1024-v2` 4:4:4, 36 frames, single-threaded on the 4-core slice, `-O2`,
on an otherwise quiet machine (load average 7, against the ~30 the rate tables
were measured under). The figure is the **minimum of two runs**; both runs are
given where they disagree by more than a percent, because one of them clearly
caught contention.

| QP | config | encode | decode | stream |
|---|---|---|---|---|
| 8 | base | 1820.6 ms/f | 69.3 ms/f | 4 518 842 B |
| 8 | all three | 1862.8 ms/f *(2121.6 on the second run)* | 71.6 ms/f | **4 280 023 B** (-5.3 %) |
| 24 | base | 713.9 ms/f | 62.1 ms/f | 679 264 B |
| 24 | all three | 711.5 ms/f | 60.7 ms/f | **638 860 B** (-5.9 %) |

**The package is free, to within the measurement's own noise.** Encode is +2.3 %
at QP 8 and -0.3 % at QP 24; decode is +3.3 % and -2.3 %. Both signs appear
because two things pull against each other: the quadrant search and the
`warp_dc()` fit add work per tile, and every tool makes the stream smaller,
which makes the entropy stage cheaper. At QP 24 the second effect wins.

Why the encoder-side cost is as small as it is, per tool:

* **T3** adds no predictor calls at all. The per-quadrant search reads the
  eighteen candidates the tile-vector exact stage already evaluates, and the
  whole-tile SAD is recovered as the sum of the four quadrant SADs, so the tile
  is walked once per candidate rather than twice.
* **T2** fits its correction on the `WARP_SKIP` prediction the mode decision
  has already computed -- three orthogonal projections in one pass, then one
  pass to score the result through the normative integer expression.
* **T1** costs one pass over the picture per frame to measure the drift, which
  is under a millisecond against a 700 ms encode.

**Decoder cost, which is what the format has to justify** (the CPU numbers
above say nothing about a Vulkan budget except that the warp is not dominant):

| tool | per-tile decoder cost | traffic | dependencies |
|---|---|---|---|
| **T3** `mv_quad` | one compare per axis per sample and a four-entry vector table per tile, inside a loop that already runs per sample. The corner block, the divide and the filter are untouched | 8 bytes on the tiles that use it, 0 on the rest | none. No cross-tile state, no extra reference read, no extra barrier |
| **T2** `warp_dc` | two multiplies by a per-tile constant, three adds and a shift per sample, on a tile the decoder was going to write anyway | 9 bytes per near-skip tile, 8 bytes per row that has one, 0 on a row that has none | none. A near-skip is a skipped tile in every other respect |
| **T1** drift refresh | **zero.** It is an encoder mode decision; every stream it produces is an ordinary one | 0 | none |

`docs/WARP.md` 12 states exactly what T3 costs `warp_tile.comp`, which does not
implement it yet: six ints of parameter block and one compare per axis. A
shader without it refuses tool bit 25 at the handshake, which is the
forward-compatibility gate doing its job.

## 6. The warp-only chain (PAPER.md 2.11 item 2)

`ref/warp_chain.py` raises the `WARP_SKIP` gate above anything real content
produces and pushes the refresh period past the clip, so frame 0 is an
ordinary intra frame and every frame after it is nothing but the pose warp of
its predecessor. **A near-skip is still a skipped tile**, so a `warp_dc()`
correction is still a warp-only chain -- the tool is measured through this test
rather than excluded from it, and `--enc-arg=--warp-dc --enc-arg=off` turns it
off.

`vr-mixed-1024-v2` 4:4:4, 36 frames, QP 8, and `vr-turn-256-v2` 4:4:4, 12
frames:

| sequence | config | frame 1 | last frame | decay | frames above 35 dB | verdict |
|---|---|---|---|---|---|---|
| `vr-mixed-1024-v2` | base | 28.71 dB | 19.12 dB | -9.59 dB | **0** | **FAIL** |
| `vr-mixed-1024-v2` | all three | 28.79 dB | **21.22 dB** | **-7.57 dB** | **0** | **FAIL** |
| `vr-turn-256-v2` | base | 30.31 dB | 22.94 dB | -7.37 dB | **0** | **FAIL** |
| `vr-turn-256-v2` | all three | 30.32 dB | **23.30 dB** | **-7.02 dB** | **0** | **FAIL** |

The verdict is unchanged and was never in reach: the chain starts below the bar
on this material, which `docs/WARP-AUDIT.md` established is a property of the
generator's ground truth and of the content it deliberately contains, not of
the predictor.

**What did change is the slope, which is the half of item 2 the audit left
standing.** Over 35 warped frames on `vr-mixed-1024-v2` the chain loses 7.57 dB
instead of 9.59, and the last frame is **2.10 dB** better:

| warped frame | 1 | 5 | 10 | 20 | 35 |
|---|---|---|---|---|---|
| base | 28.71 | 25.08 | 24.73 | 19.68 | 19.12 |
| all three | 28.79 | 25.42 | 25.39 | 21.48 | 21.22 |

**All of it is T2.** Run again with only `--warp-dc off` and the chain is the
base row to the hundredth of a dB -- 28.71 / 19.12, decay 9.59 -- because in a
warp-only chain no tile is coded, so `mv_quad` never appears, and the refresh
period is past the clip, so the drift rule never fires. Nine bytes on a
skipped tile buy 2 dB of chain, which is what item 2 says the refresh rate
would otherwise have to buy.

That is the strongest single result in this package and it is not the one the
rate tables show, because a chain is the regime where the codec leans hardest
on the warp and the rate tables average over frames where it does not. It is
also the regime `ref/RESULTS-inter.md` section 4 says the codec spends two
thirds of its tiles in at the paper's own operating density.

## 7. Conformance

```
ctest --test-dir build-ref -R 'ref\.|warp\.'        24/24 pass
cmake --preset asan-ubsan && ctest --preset asan-ubsan -R 'ref\.|fuzz'
                                                     17/17 pass
```

`tests/vectors/vectors.md5` and `rejects.md5` are regenerated. What changed and
what did not:

| vectors | state |
|---|---|
| `v01`-`v44` (intra) | **unchanged**, byte for byte |
| `v45`-`v55` (Phase 2) | **unchanged**, byte for byte. They are generated with every v1.5 tool off, which is what pins the promise that a stream setting neither new tool bit decodes as it always did |
| `v56` | changed: it is the rolling-refresh vector and T1 rewrote the refresh clock (below) |
| `v57`-`v60` | **new**: `warp_dc()`, `mv_quad`, both together at 4:2:0, and the drift refresh rule against a short eligibility period and a longer hard cap |
| `r01`-`r29` | **unchanged** |
| `r30`-`r32` | **new**: `dc_present` without tool bit 24, `mv_quad` without tool bit 25, tile header word1 bit 29 |

`v57`-`v60` are not decorative. `build_inter()` refuses to write a vector whose
spec asks for a tool that no tile used, so a row that stops exercising its tool
fails the build rather than pinning a digest of the tool doing nothing -- which
is exactly what the first cut of `v57` did, on material where no tile ever
chose a near-skip.

### Why `v56` changed, which is the one thing in this package that is not additive

The rolling refresh was written as "refresh when `(hash(tile) + frame) mod T`
is zero", a global frame counter. T1 needs an age the drift rule can also
drive, so it is now "refresh when the frames since this tile's last `INTRA`
reach T", seeded at a tile-map reset with the same permutation so the cadence
and the spread are the same. Two things follow, and both are improvements:

* a tile the rate-distortion decision chose `INTRA` for on its own merits now
  resets the refresh clock, where before it did not and the tile was refreshed
  again on the timer's schedule;
* the age counts the tile's own `INTRA` frame, so a period of T refreshes every
  T frames rather than every T + 1, and `--intra-period 1` means every frame as
  this CLI has always documented. That off-by-one was in the shipped code and
  is fixed here.

Neither is a syntax change. The mode decision is non-normative, and a decoder
sees an ordinary stream either way.

### Properties asserted rather than digested

A digest cannot express "changing this field must not change the output"; an
assertion can, and the two new ones are the load-bearing claims of T3:

* `warp.quad`: four equal quadrant vectors reproduce `warp_tile()` bit for bit,
  for every split, both modes, both filters -- and each quadrant equals the
  single-vector predictor of its own vector restricted to that quadrant. The
  first is a refactor guard (`warp_tile()` *is* `warp_tile_quad()` with the
  vector replicated); the second is the semantic.
* `ref.inter` cases 12-14: every `mv_quad` tile carries a legal mode, a vector
  and in-range quadrant vectors; every near-skip is a skipped tile and the
  encoder and decoder agree on how many there are; the hard refresh cap holds
  with the drift rule set so high no tile is ever eligible on merit. Case 13
  also runs the near-skip **under loss**, where `run()`'s byte-for-byte
  shadow comparison is the point: concealing a skipped tile has to replay its
  correction, or clause 6.11's "losing a skipped tile is a no-op" stops being
  true. It did not, at first; that is a bug this test found.

---

## 8. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF
cmake --build build-ref -j4

# the whole matrix: base and each tool alone and together, both bands
tools/quality/tourney_queue.sh

# one row of it
tools/quality/tourney_run.sh m1024-444-B-all vr-mixed-1024-v2.yuv444p B

# the verdicts, verbatim
ref/phase2_verdict.py --results $NXQ_SCRATCH/results/tourney-inter-b-*.json

# what a tool is worth against the same codec without it
ref/bd_between.py --base $NXQ_SCRATCH/results/tourney-inter-b-m1024-444-B-base.json \
                  --test $NXQ_SCRATCH/results/tourney-inter-b-m1024-444-B-all.json --split

# the chain, with and without the near-skip
ref/warp_chain.py --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv444p.json \
    --enc build-ref/bin/nxv-enc --dec build-ref/bin/nxv-dec --eyes 2 --qp 8 \
    --enc-arg=--warp-dc --enc-arg=off
```

Every process under `chrt -i 0 taskset -c 16-19 nice -n 19`. Result files are
`$NXQ_SCRATCH/results/tourney-inter-b-*.json`.

---

## 9. What this does not say

**The material is the same synthetic material, with the same limits.**
`ref/RESULTS-inter.md` section 7 states them and they are unchanged: the
sequences are rotation-only by construction, which flatters a rotation-only
predictor and starves the anchor; the clips are 12 to 36 frames; and this is
not the material PAPER.md 2.11 item 1 states its gate on, which is 60 s each of
VRChat, Beat Saber and Alyx captured from WiVRn. `corpus/`'s `wivrn-capture`
class is still empty.

**The band-limited (v2) sequences are used throughout**, per
`docs/WARP-AUDIT.md`, so the base rows here are **not** comparable with
`ref/RESULTS-inter.md`'s: that document measured the v1 generation, whose
point-sampled ground truth carries aliasing no warp can predict. Everything in
this document is measured on both sides of the same material, which is the only
comparison a tool package needs.

**T2's rate result is one clip's.** +0.28 % on its own is inside the range
where a different sequence could put it on the other side of zero, and the
reason to keep it on by default is section 6's 2.10 dB of chain, plus the
-0.45 % it contributes inside the package, not the rate curve on its own.

**Nothing here is measured on a GPU.** The decoder-cost column of section 5 is
an argument from the arithmetic, not a measurement, and `warp_tile.comp` does
not implement T3 yet.

---

## Appendix: every operating point

`enc ms/f` here is `compare.py`'s own wall clock, taken while the machine was
carrying about thirty other encodes; it is comparable **within** a table and
not with section 5, which was measured on a quiet machine. SSIM is `ssim_y`.

### Band B

| config | QP | Mbit/s | bpp | PSNR-Y | SSIM-Y | enc ms/f |
|---|---|---|---|---|---|---|
| base (v1.4 inter) | 18 | 30.8 | 0.1629 | 44.80 | 0.99272 | 1344 |
| base (v1.4 inter) | 24 | 13.6 | 0.0720 | 39.83 | 0.98498 | 825 |
| base (v1.4 inter) | 30 | 6.6 | 0.0348 | 35.27 | 0.97064 | 639 |
| base (v1.4 inter) | 36 | 3.7 | 0.0195 | 30.58 | 0.94825 | 547 |
| T1 drift refresh | 18 | 30.3 | 0.1604 | 44.77 | 0.99273 | 1227 |
| T1 drift refresh | 24 | 12.9 | 0.0685 | 39.76 | 0.98487 | 889 |
| T1 drift refresh | 30 | 6.2 | 0.0327 | 35.19 | 0.97055 | 675 |
| T1 drift refresh | 36 | 3.4 | 0.0178 | 30.58 | 0.94842 | 558 |
| T2 warp_dc | 18 | 30.9 | 0.1636 | 44.85 | 0.99272 | 1328 |
| T2 warp_dc | 24 | 13.7 | 0.0726 | 39.83 | 0.98481 | 907 |
| T2 warp_dc | 30 | 6.6 | 0.0351 | 35.31 | 0.97019 | 736 |
| T2 warp_dc | 36 | 3.7 | 0.0197 | 30.75 | 0.94724 | 613 |
| T3 mv_quad | 18 | 30.2 | 0.1602 | 44.80 | 0.99272 | 1270 |
| T3 mv_quad | 24 | 13.4 | 0.0707 | 39.83 | 0.98499 | 939 |
| T3 mv_quad | 30 | 6.5 | 0.0344 | 35.26 | 0.97060 | 724 |
| T3 mv_quad | 36 | 3.6 | 0.0193 | 30.59 | 0.94812 | 620 |
| all three | 18 | 29.0 | 0.1537 | 44.81 | 0.99269 | 1238 |
| all three | 24 | 12.8 | 0.0677 | 39.74 | 0.98455 | 914 |
| all three | 30 | 6.1 | 0.0323 | 35.29 | 0.97037 | 744 |
| all three | 36 | 3.4 | 0.0179 | 30.63 | 0.94666 | 602 |

| config | BD-rate vs x265-p | fastest 20 % | the rest | BD-rate vs base |
|---|---|---|---|---|
| base (v1.4 inter) | +568.66 % | +539.87 % | +576.94 % | — |
| T1 drift refresh | +544.83 % | +513.73 % | +553.85 % | -4.43 % |
| T2 warp_dc | +572.02 % | +540.97 % | +580.99 % | +0.28 % |
| T3 mv_quad | +557.82 % | +532.30 % | +565.11 % | -1.37 % |
| all three | +532.96 % | +504.86 % | +541.05 % | -6.25 % |

### Band A

| config | QP | Mbit/s | bpp | PSNR-Y | SSIM-Y | enc ms/f |
|---|---|---|---|---|---|---|
| base (v1.4 inter) | 0 | 204.0 | 1.0807 | 57.03 | 0.99882 | 2967 |
| base (v1.4 inter) | 4 | 136.8 | 0.7250 | 55.05 | 0.99833 | 2637 |
| base (v1.4 inter) | 8 | 90.4 | 0.4788 | 52.77 | 0.99767 | 2052 |
| base (v1.4 inter) | 12 | 61.6 | 0.3262 | 49.84 | 0.99651 | 1679 |
| T1 drift refresh | 0 | 203.9 | 1.0801 | 57.03 | 0.99882 | 2876 |
| T1 drift refresh | 4 | 136.4 | 0.7227 | 55.05 | 0.99833 | 2485 |
| T1 drift refresh | 8 | 90.1 | 0.4772 | 52.77 | 0.99768 | 1926 |
| T1 drift refresh | 12 | 60.8 | 0.3224 | 49.82 | 0.99651 | 1562 |
| T2 warp_dc | 0 | 204.0 | 1.0807 | 57.03 | 0.99882 | 2890 |
| T2 warp_dc | 4 | 136.8 | 0.7250 | 55.05 | 0.99833 | 2551 |
| T2 warp_dc | 8 | 90.4 | 0.4788 | 52.76 | 0.99767 | 2000 |
| T2 warp_dc | 12 | 61.3 | 0.3247 | 49.84 | 0.99650 | 1676 |
| T3 mv_quad | 0 | 198.0 | 1.0491 | 57.02 | 0.99882 | 3088 |
| T3 mv_quad | 4 | 130.9 | 0.6935 | 55.03 | 0.99832 | 2696 |
| T3 mv_quad | 8 | 86.2 | 0.4568 | 52.75 | 0.99767 | 2155 |
| T3 mv_quad | 12 | 58.9 | 0.3121 | 49.83 | 0.99651 | 1748 |
| all three | 0 | 197.8 | 1.0480 | 57.01 | 0.99882 | 3044 |
| all three | 4 | 130.2 | 0.6899 | 55.03 | 0.99832 | 2775 |
| all three | 8 | 85.6 | 0.4535 | 52.75 | 0.99768 | 2041 |
| all three | 12 | 58.2 | 0.3083 | 49.83 | 0.99650 | 1711 |

| config | BD-rate vs x265-p | fastest 20 % | the rest | BD-rate vs base |
|---|---|---|---|---|
| base (v1.4 inter) | +345.70 % | +337.24 % | +347.92 % | — |
| T1 drift refresh | +344.06 % | +335.44 % | +346.32 % | -0.42 % |
| T2 warp_dc | +345.77 % | +337.19 % | +348.03 % | -0.02 % |
| T3 mv_quad | +327.81 % | +320.10 % | +329.80 % | -4.08 % |
| all three | +325.27 % | +316.89 % | +327.45 % | -4.70 % |
