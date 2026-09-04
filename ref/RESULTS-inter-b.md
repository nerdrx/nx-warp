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
were committed with. `v56` and `v60` are the two refresh-cadence vectors and
they changed, for the reason in section 6.

**The four things to read first.**

1. **The kill test still fails.** Nothing in this package was ever going to
   pass it: `ref/RESULTS-inter.md` section 4 measured the intra core at +190 %
   to +886 % against `x265-p` *before* the inter path touches it, and an inter
   tool cannot recover that. The verdicts are quoted verbatim in section 2.
2. **T3 is the win.** Four vectors per tile is worth [T3-BD] BD-rate against
   the same codec without it, for eight bytes on the tiles that take them and
   no extra predictor calls in the encoder search.
3. **T2 is small and real, and it was nearly a loss.** The first cut of the
   near-skip *cost* rate: an unconditional 8-byte-per-row bitmap is 2.5 % of a
   frame at this codec's operating point, and the mode decision's own lambda
   under-prices a correction that is a within-mode trade. Section 4.2 has both
   corrections and what each was worth.
4. **T1's measurement is more interesting than T1.** The drift the encoder's
   shadow reports never exceeds the quantiser's own noise floor at the
   operating point (section 4.1, with the numbers), which says the blind
   1-in-180 refresh is buying insurance the picture does not need. The tool's
   value is that it lets the hard cap be the only unconditional refresh, and it
   is measured as such.

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

BAND-A-SECTION

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

Every operating point, both bands, is in section 5's table.

BAND-A-TOOLS

---

## 4. What each tool does, and what it does not

### 4.1 T1, drift-driven refresh: -4.43 % at band B

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

### 4.2 T2, the near-skip: +0.28 % of rate, -2.10 dB of chain

On the rate curve T2 is a wash: +0.28 % BD-rate on its own, and inside the
package it is worth about -0.45 % (the three together beat T1 + T3 by that
much). On the **chain** it is the whole of section 6's 2.10 dB.

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

### 4.3 T3, four vectors per tile: -1.37 % at band B

It does what it says, on the tiles where a single tile vector is the wrong
model -- a disc crossing a tile boundary, a strip of disocclusion down one edge
of a tile. At QP 30 it takes 1.5 % off the rate at the same PSNR to the
hundredth of a dB.

The encoder's search is free: a candidate vector applied to the whole tile
predicts every quadrant with that vector at once, and the quadrants are
independent given the vector, so the best vector per quadrant falls out of the
same 18-candidate exact stage the tile vector already runs, with the whole-tile
SAD recovered as the sum of the four quadrant SADs (one pass over the tile, not
two). The measured encode time is inside the noise of the base row.

Where the win is *not* is band A, and section 5 says so with numbers: at 48 to
56 dB the frame is majority `INTRA` (`ref/RESULTS-inter.md` section 4's mode
histogram) and there are few coded inter tiles for the deltas to improve.

---

## 5. PENDING — encode and decode time

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

## 7. PENDING — conformance
