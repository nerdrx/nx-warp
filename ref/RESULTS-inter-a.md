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

**The headline, first.** The package is worth a few percent of rate at the
band the codec is designed to run in and approximately nothing in the literal
100-300 Mbit band. **It does not change the kill-test verdict, and it was never
going to**: `RESULTS-inter.md` section 4 measured the intra core alone at
+190 % to +886 % against the same anchor before the inter path touches it, and
a tool that makes coded inter tiles cheaper cannot recover that. The verdicts
below are quoted verbatim and they all still say FAIL.

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

## 3. PLACEHOLDER-FULL

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

## 6. PLACEHOLDER-TIME

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
