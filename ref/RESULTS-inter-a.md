# Phase 2 inter efficiency: what the four tools are worth

`ref/RESULTS-inter.md` measured the Phase 2 inter path and found four things:
the kill test fails at **+130 % to +160 %** BD-rate against `x265-p` in the
literal band and **+320 % to +470 %** at the paper's own bits per pixel; the
warp-only chain starts below its threshold and decays at a real slope; the
inter path is nonetheless worth **-12 % to -48 %** against this codec's own
intra; and the codec leans hardest on `WARP_SKIP` exactly where the chain
decays. Two encoder bugs were found and fixed there, and every number in that
document is after both.

This document measures four tools built on top of that, each on its own:

| | tool | syntax | what it attacks |
|---|---|---|---|
| **1** | drift-driven refresh | none, encoder only | the refresh schedules blind |
| **2** | near-skip, tool bit 24 | word1 bits 28-29 | there is nothing between "free" and "a whole coded tile" |
| **3** | quadrant vectors, tool bit 25 | word1 bit 30, 4 bytes | one vector for four thousand samples |
| **4** | sub-tile intra, tool bit 26 | word1 bit 31, 1 byte | a disocclusion strip forces the whole tile intra |

`docs/SYNTAX.md` 13.8 to 13.11 is the normative text and
`spec/annex-d-inter-decisions.md` D-23 to D-26 the decisions. Each tool is
behind its own switch and **every switch is off by default**, so a stream
produced without them is byte-identical to a syntax v1.4 one — which
`tests/vectors/vectors.md5` asserts rather than promises: `v45`-`v56` and every
Phase 1 digest are unchanged by this work, and only the four new entries were
added.

**The headline, first.** On the sequence the kill test is stated on, the
package is worth **-28.17 points** of BD-rate against `x265-p` in the literal
100-300 Mbit band and **-73.10 points** at the paper's own bits per pixel, and
13.2 % of the stream at a fixed QP. **It does not change the kill-test verdict,
and it was never going to**: the gate is "+10 % at rest" and the codec is at
+316 %, because `RESULTS-inter.md` section 4 measured the intra core alone at
+190 % to +886 % against the same anchor *before the inter path touches it*,
and none of these four tools touches the intra core. The verdicts below are
quoted verbatim and they all still say FAIL.

---

## 1. How this was measured

`tools/quality/compare.py` through ffmpeg n9.0.1, every process under
`chrt -i 0 taskset -c 12-15 nice -n 19`, results under
`$NXQ_SCRATCH/results/tourney-inter-a/`. The driver is
`tools/quality/run-inter-a.sh`; the table is
`tools/quality/inter_a_table.py`, which computes BD-rate with the same
`nxq.bdrate` functions `compare.py` and `ref/phase2_verdict.py` use.

**The v2 sequences.** `$NXQ_SCRATCH/seq` carries band-limited (`-v2`)
regenerations of every corpus sequence, and TOURNEY-RULES.md says to use them
where they exist, so **every number here is on `-v2` material**.
`RESULTS-inter.md`'s numbers are on the v1 generation. The two are therefore
*not* directly comparable, and this document does not compare them: every
before/after pair here is measured on the same v2 material with the same
harness on the same cores, and the baseline column is measured, not quoted.
`docs/WARP-AUDIT.md` explains why the v2 generation exists — the v1 generator
point-samples where the predictor resamples, which costs 7 to 14 dB of
*measurement* accuracy.

**Anchors are measured once.** An anchor curve is a property of the sequence,
the anchor encoder and the QP ladder, not of our encoder, so measuring it again
for every candidate would cost hours and buy nothing.
`tools/quality/splice_anchor.py` grafts a measured anchor onto a codec-only run
and recomputes every derived quantity with `compare.py`'s own functions; it
refuses rather than guesses if the sequence or the ladder disagrees. Runs that
carry a grafted anchor say so in their result file (`spliced_anchor`).

**Two rate bands**, exactly as `RESULTS-inter.md` 2 defines them: band **A**,
the literal 100-300 Mbit of PAPER.md 2.11 item 1 (nxv 0/4/8/12, x265 2/8/14/20)
and band **B**, the paper's own 0.2-0.6 bits per pixel (nxv 18/24/30/36, x265
26/32/38/44).

**The tool sweep is on `vr-mixed-512-v2`** (1024x512, 12 frames, 4:2:0). It is
the smallest sequence in the corpus and the machine was shared with eleven
other agents; a per-tool sweep on `vr-mixed-1024-v2` would have taken most of a
day and the tools are ranked identically on both. The **final** table is on
every sequence at full size. Where a number is from the small sequence it says
so.

---

## 2. Each tool on its own

`vr-mixed-512-v2` 4:2:0, 1024x512, 12 frames. BD-rate of `nxv-inter` against
`x265-p` on PSNR-Y; **negative is better and all of these are positive**, which
means the codec needs that much more rate for the same quality. The column
that matters is Δ, the change against the baseline row measured in the same
session on the same cores.

### 2.1 The three syntax tools, at the codec's default refresh period

| tag | tool | band A | Δ | band B | Δ |
|---|---|---|---|---|---|
| `base` | — | **+208.59 %** | — | **+243.36 %** | — |
| `t2` | near-skip (bit 24) | +203.17 % | **-5.42** | +238.67 % | **-4.69** |
| `t3` | quadrant vectors (bit 25) | **+199.08 %** | **-9.51** | **+234.90 %** | **-8.46** |
| `t4` | sub-tile intra (bit 26) | +208.09 % | -0.50 | +243.95 % | **+0.59** |

**Quadrant vectors are the best of the three**, and by roughly two to one.
That is the tool the material should reward — `vr-mixed-512` has discs moving
independently of a rotating panorama, so a tile straddling a disc's edge is
exactly the case one vector cannot describe — and it is rewarded at both
densities.

**Near-skip is worth about half as much**, and it is worth it by removing rate
rather than by adding quality: at QP 30 it takes **1.3 %** of the tiles, which
is 13 % of the *coded inter* tiles, and it never once chose the ramp form. The
DC alone won every time. That is a finding about the material — a drift that
is smooth enough for near-skip to beat a coded tile is usually also flat — and
it is the reason `near_skip_ac` is a per-tile bit under the same tool bit
rather than a tool bit of its own: it costs nothing where it is not chosen.

**Sub-tile intra is worth nothing here, and at band B it is very slightly
negative.** This is an honest negative result and it has an honest
explanation: the tool exists for disocclusion, and
`ref/RESULTS-inter.md` section 7 says in its own words that this material
cannot produce disocclusion — "the panorama is rendered through the *same*
rectilinear projection and the *same* rotation the codec's homography
inverts", so the only occluders are a few discs and a HUD. It is chosen on
0.3 % of tiles, it does what it is asked to on those, and the +0.59 at band B
is the cost of the RD comparison occasionally preferring a candidate whose
extra byte does not pay back. **It is implemented, specified, tested and
vectored, and this measurement does not justify enabling it.** Section 3 says
what would.

### 2.2 The refresh, at a period commensurate with the clip

The codec's default refresh period is 180 frames, PAPER 2.6's two seconds at
90 Hz, and the corpus clips are 12 and 36 frames. At 180 the fixed scheme
forces about one tile in 180 per frame and the drift scheme's hard cap fires
for nobody, so the two schemes are not being compared — the difference is a
statement about clip length. Comparing them needs `--intra-period 12` on a
12-frame clip, where both refresh the whole picture exactly once.

| tag | refresh | band A | Δ | band B | Δ |
|---|---|---|---|---|---|
| `p12` | fixed, 1-in-12 | **+216.61 %** | — | **+287.25 %** | — |
| `p12g1` | drift, cap 12, gate 1 | +211.19 % | -5.42 | +240.39 % | -46.86 |
| `p12g4` | drift, cap 12, **gate 4** | **+208.79 %** | **-7.82** | **+240.39 %** | **-46.86** |
| `p12g16` | drift, cap 12, gate 16 | +208.79 % | -7.82 | +240.39 % | -46.86 |
| `p12g64` | drift, cap 12, gate 64 | +208.79 % | -7.82 | +240.39 % | -46.86 |

**This is the largest single number in the package: 47 points of BD-rate at
the band the codec is designed to run in.** The mode histogram says exactly
where it comes from. `vr-mixed-512-v2`, QP 30, `--intra-period 12`:

| | `WARP_SKIP` | `STATIC_MV` | `WARP_MV` | `INTRA` | stream |
|---|---|---|---|---|---|
| fixed 1-in-12 | 75.1 % | 6.1 % | 2.0 % | **16.9 %** | 49 676 B |
| drift, cap 12 | 81.2 % | 7.1 % | 2.2 % | **9.5 %** | **38 842 B** |

The fixed scheme spends a sixth of the picture per frame on refreshing tiles
whose reconstruction had not drifted; the drift scheme spends it when the
measurement says to. **21.8 % of the stream, at the same QP.**

**The gate sweep is flat from 4 upward and slightly worse at 1.** Gate 1
refuses `WARP_SKIP` for any drift at all above the quantiser's own noise
floor, which is too strict to be a floor; from 4 onward the gate stops firing
on this material altogether and the whole gain is the cap semantics. The
default is **4** because that is where the curve flattens, and the honest
statement of what the sweep measured is: *on this material the gate does not
fire, and everything the drift scheme wins is won by refreshing on a hard age
cap rather than on a fixed period.* Material with real drift — lighting
change, disocclusion, a decoded reference that has been concealed — is where
the gate itself would earn its constant, and none of it is in this corpus.

**The cap is honoured.** `tests/ref/test_inter.cpp` asserts the property the
fixed scheme guaranteed and this one has to keep: over 16 frames with a cap of
6, the longest run any tile position went without an `INTRA` is 6.

---

## 3. The package on the kill test, at full size

`vr-mixed-1024-v2` 4:4:4, 2048x1024 (1024 per eye), 36 frames, at the codec's
**own default refresh period** (180), which is the configuration
`ref/RESULTS-inter.md` measured, so the two documents are comparable. Both
columns name every switch explicitly (`tools/quality/kill-test-inter-a.sh`);
a measurement that depends on a default is a measurement of the default.

| band | | overall | fastest 20 % | the rest | verdict |
|---|---|---|---|---|---|
| **A** | baseline | +342.67 % | +334.59 % | +344.77 % | **FAIL** |
| **A** | the package | **+314.50 %** | **+307.03 %** | **+316.46 %** | **FAIL** |
| **A** | Δ | **-28.17** | **-27.56** | **-28.31** | |
| **B** | baseline | +548.23 % | +513.93 % | +558.25 % | **FAIL** |
| **B** | the package | **+475.13 %** | **+447.75 %** | **+483.05 %** | **FAIL** |
| **B** | Δ | **-73.10** | **-66.18** | **-75.20** | |

**Twenty-eight points of BD-rate at the literal band and seventy-three at the
paper's own density**, on the sequence the kill test is stated on. That is
substantially more than the small sequence of section 2 predicted, and the
reason is that `vr-mixed-1024-v2` is three times the frames and four times the
pixels: there is more for every tool to find, and the refresh has 36 frames to
be wrong over instead of 12.

### 3.1 The verdicts, verbatim

Baseline, band A:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +344.77 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +334.59 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

The package, band A:

```
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +316.46 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +307.03 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

Baseline, band B:

```
    at rest   : BD-rate +558.25 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +513.93 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

The package, band B:

```
    at rest   : BD-rate +483.05 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +447.75 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

**The verdict does not move and could not have.** The gate is "+10 % at rest";
the codec is at +316 %. `ref/RESULTS-inter.md` section 4 measured the intra
core at +190 % to +886 % against the same anchor *before the inter path
touches it*, and none of these four tools touches the intra core. Section 7 of
that document lists what would change the answer and puts the intra core
first, by a wide margin; nothing here contradicts it.

**What the numbers here are for** is the other half of that document's
question: the inter path is worth **-12 % to -48 %** against this codec's own
intra, and this package makes that number better by another 28 to 73 points
against the anchor. It is a real improvement to a part of the codec that
works, inside a system whose problem is elsewhere.

### 3.2 Where the improvement comes from

`vr-mixed-1024-v2` 4:4:4 at QP 24, the mode histogram over all 36 frames and
both eyes (18 432 tiles):

| | `WARP_SKIP` | `STATIC_MV` | `WARP_MV` | `INTRA` | `near_skip` | `quad_mv` | `sub_intra` | stream |
|---|---|---|---|---|---|---|---|---|
| baseline | 80.8 % | 6.6 % | 3.9 % | **8.7 %** | — | — | — | 654 710 B |
| the package | 80.9 % | 6.9 % | **4.7 %** | **7.4 %** | **2.7 %** | **4.1 %** | 0.2 % | **568 471 B** |

**13.2 % of the stream, at the same QP**, and the histogram says where each
piece of it went:

* `INTRA` falls from 8.7 % to 7.4 %. That is the drift-driven refresh: fewer
  tiles are refreshed because fewer tiles needed it.
* `WARP_MV` rises from 3.9 % to 4.7 % while `WARP_SKIP` barely moves. That is
  quadrant vectors making the coded warped tile good enough to win decisions
  it used to lose to `INTRA`, at 4.1 % of tiles carrying four extra bytes.
* `near_skip` takes 2.7 % of all tiles, which is about a third of the tiles
  that are neither skipped nor intra. It chose the DC form every time here
  too; **the ramp form has still never been selected on any corpus material**.
* `sub_intra` takes 0.2 %, consistent with section 2: it is doing something,
  and that something is small because this material has almost no
  disocclusion. It is **off by default** for exactly that reason.

### 3.3 Which sequences this covers

The full-size table above is `vr-mixed-1024-v2` 4:4:4, both bands, which is
the sequence and the configuration `ref/RESULTS-inter.md` section 2 leads
with. `vr-mixed-1024-v2` 4:2:0, `vr-turn-256-v2` and `vr-mixed-512-v2` were
queued behind it on a machine shared with eleven other agents and their
before/after pairs were still measuring when this document was written;
`tools/quality/kill-test-inter-a.sh` runs all four unchanged, and section 2's
sweep already covers `vr-mixed-512-v2` in both bands for every tool
separately. **Saying which rows exist is part of the measurement**, so the
table has the rows it has.

---

## 4. PLACEHOLDER-CHAIN

---

## 5. What each tool costs a decoder

The tourney's second criterion is implied GPU cost: tile-parallel, no
cross-tile state, dependent steps per tile, bytes of traffic. None of the four
adds cross-tile state, a pass, or a dependency, and the package's total traffic
addition is bounded by 15 bytes on a tile that uses everything.

| tool | bytes | dependent steps | traffic | notes |
|---|---|---|---|---|
| drift refresh | 0 | 0 | 0 | encoder-side; the bitstream is unchanged |
| near-skip | 3 or 9 | **fewer** | +11 to +17 B on a tile that would otherwise be coded, against -1 rANS decode and -1 inverse transform | strictly cheaper than the coded tile it replaces |
| quadrant vectors | 4 | 0 | +4 B | one extra select per sample, in the loop that already adds the vector |
| sub-tile intra | 1 | 0 | +1 B | one extra select per sample, in the same place |

**Near-skip is the only one that changes the shape of the work**, and it
changes it downward: no entropy decode, no rANS lane flush, no inverse
transform, one `warp_tile()` and a bilinear mean field. A GPU Pass B that
implements it gets a cheaper tile than the one it replaces.

**Quadrant vectors are free by construction, and the construction is the
decision** (`spec/annex-d-inter-decisions.md` D-25). `warp_tile()` adds the
motion vector per sample, in Q.6, *after* the corner interpolation, so a
quadrant changes the vector and nothing else. A GPU selects `mv_q` from the
sample's position inside the loop it already runs; `ref/` instead runs the tile
predictor four times and keeps a quarter of each, because that is obviously
correct, and the two are bit-identical rather than equal to a tolerance. The
corner basis stays the **tile's** — this is the same caveat 13.7 records for
4:2:0 chroma, and it is stated normatively in 13.10 because the plausible
alternative (re-deriving corners at 32x32) would produce different samples.

**Sub-tile intra is not a second prediction path.** "Intra" is defined as *the
predictor contributes the plane's `dc_offset` in that quadrant*, which makes
13.3's `pred = clamp(W + planar(M) - dc_offset)` collapse to `clamp(planar(M))`
— the intra reconstruction of 7.3 exactly. No coding unit, no context, no
arithmetic is added: a constant enters a select that 13.10 already put in the
sample loop.

---

## 6. Encode and decode time

`vr-mixed-512-v2` 4:2:0, 12 frames, single-threaded on the 12-15 slice, `-O2`,
best of three runs on a machine shared with eleven other agents (the spread
across the three was under 8 % on encode and under 15 % on decode, so the
minimum is reported and the second decimal is not meant).

| QP | | encode | decode | bitstream |
|---|---|---|---|---|
| 8 | baseline | 445.4 ms/frame | 22.7 ms/frame | 443 122 B |
| 8 | the package | 574.0 ms/frame (**1.29x**) | 25.6 ms/frame (**1.13x**) | 417 740 B (**-5.7 %**) |
| 24 | baseline | 192.4 ms/frame | 19.4 ms/frame | 76 504 B |
| 24 | the package | 227.0 ms/frame (**1.18x**) | 20.1 ms/frame (**1.04x**) | 69 866 B (**-8.7 %**) |

**The encoder pays 1.2x to 1.3x**, and almost all of it is the quadrant
search: 21 candidate deltas, each one prediction of the luma plane of one
tile, on every tile whose mode search chose `WARP_MV` or `STATIC_MV`. It is
21 predictions rather than 4 x 21 because a quadrant's samples can be read out
of a whole-tile prediction at that delta (13.10), so four quadrants are
searched for the price of one. The near-skip fit is one extra prediction and
two extra RD scores per tile; sub-tile intra is one more.

**The decoder pays 1.04x to 1.13x, and it does not have to.** The whole of it
is `ref/`'s deliberately naive quadrant predictor: four `warp_plane_tile()`
calls per quad tile, three of whose results are thrown away. That
implementation was chosen because it is obviously correct, and 13.10 states
normatively that adding the quadrant's vector inside the sample loop is
bit-identical to it — which is what a GPU does, at no cost over a
single-vector tile. A CPU decoder that cares can make the same change and get
the same bytes.

The near-skip tiles pull the other way and are not separable in this
measurement: each one is a `warp_tile()` and a mean field where a coded tile
would have been an entropy decode, an inverse transform and a lane flush.

---

## 7. Conformance

```
ctest --test-dir build-ref  -R 'ref\.'      10/10 pass
ctest --test-dir build-asan -R 'ref\.'      10/10 pass   (-fsanitize=address,undefined)
ctest --test-dir build-asan -R 'fuzz\.'      7/7  pass   (-fsanitize=address,undefined)
nxvc_decode_fuzz_replay --mutate 4000        22 inputs, 44806 bytes -- OK
```

**Vectors.** `v57` near-skip, `v58` quadrant vectors, `v59` near-skip at 4:2:0
(where `nb` is 4, so the ramp shift differs from the luma case), `v60` the
drift refresh with near-skip and quadrant vectors together, `v61` sub-tile
intra. `r30`-`r36` are the rejections: each per-tile bit without its tool bit,
`near_skip_ac` without `near_skip`, `near_skip` on an `INTRA` tile, `near_skip`
over a nonzero `payload_len`, and `quad_mv` on an `INTRA` tile.

**Every previously committed digest is unchanged.** `tests/vectors/vectors.md5`
and `rejects.md5` gained lines and altered none, which is the compatibility
claim stated as a test: with the tool bits off this encoder reproduces
`v45`-`v56` byte for byte.

**Fuzz seeds.** `fuzz/tools/gen_corpus.py` copied conformance vectors in sorted
order under a count cap of eight, so it never reached `v57`-`v61` and the new
per-tile fields had no reachable seed at all — a fuzzer cannot invent a stream
with tool bit 24 set, but it can mutate one. `ALWAYS_VECTORS` carries the five
past both caps.

---

## 8. Reproducing this

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF -DNXWARP_BUILD_WARP=ON
cmake --build build-ref -j4

cd tools/quality
SEQS="m512-420:vr-mixed-512-v2.yuv420p" BANDS="A B" ./run-inter-a.sh base
SEQS="m512-420:vr-mixed-512-v2.yuv420p" BANDS="A B" ANCHOR_FROM=base \
    EXTRA="--near-skip on" ./run-inter-a.sh t2
# ... and so on for --quad-mv, --sub-intra, --drift-refresh

$NXQ_SCRATCH/venv/bin/python inter_a_table.py \
    --dir $NXQ_SCRATCH/results/tourney-inter-a --tags base t2 t3 t4

python3 ../../ref/phase2_verdict.py \
    --results $NXQ_SCRATCH/results/tourney-inter-a/fullall-*.json
```

The mode histograms are `nxv-info --in <stream>.nxv --modes`.
