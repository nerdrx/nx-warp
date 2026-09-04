# Entropy and context modelling: measurements

What the syntax v1.5 entropy package is worth, tool by tool, measured with
`tools/quality/compare.py` on the **v2 (band-limited)** sequences in
`$NXQ_SCRATCH/seq` -- `vr-mixed-1024-v2`, 2048x1024 side-by-side, 90 fps,
`synthetic:mixed:seed1:v2-bandlimited-ss4`. Every process ran under
`chrt -i 0 taskset -c 28-31 nice -n 19`; result files are under
`$NXQ_SCRATCH/results/tourney-ctx-a/`. The first 12 frames of the 36-frame
sequence are used, which for intra is 12 independent measurements of the same
content class and for inter is a complete refresh cycle plus its P frames.

The package is four items, and they are reported in the order they were
measured:

| # | tool | bit | what it is |
|---|---|---|---|
| 1 | `CTX_V3` | 24 | 27 coefficient contexts: neighbour-conditioned `CBF`/`LAST`, a `LEVEL` context for scan position `LAST`, a `LEVEL` context for the DC term of a DC plane (SYNTAX.md 9.8) |
| 2 | `VEC_ENT` | 25 | the tile vector is the payload's first coding unit instead of two raw header bytes (SYNTAX.md 9.9) |
| 3 | — | none | encoder only: a table set is transmitted only when it pays for itself, and each tile's set is chosen against the tables in force (SYNTAX.md decision 57) |
| 4 | — | none | two experiments: 12-bit probabilities and 16 lanes. **Both rejected**, with the numbers in section 5 |

Item 3 needs no tool bit and no decoder change, which is the whole point of it:
`tables_present` already says which sets a frame transmits, and a decoder that
never saw the old behaviour cannot tell the difference. It is therefore
measured as the first cumulative row, so that items 1 and 2 are measured on top
of an encoder that is not wasting header bytes.

---

## 1. Phase 1 intra: the gate, cumulatively

Each row adds one item to the row above. `v1.4 default` is the shipped state
before this package -- directional intra, `CTX_V2` and sign data hiding, all on
-- built from the tournament's fork point and measured with its own binaries,
so the "before" column is a real encoder and not a flag on a later one.

`vr-mixed-1024-v2`, 12 frames, against `x264 --keyint 1 --tune zerolatency`
over the 100-400 Mbit band.

**4:4:4**

| | BD-rate vs x264 intra | BD-PSNR | mean deficit | worst deficit | verdict |
|---|---|---|---|---|---|
| v1.4 default | +62.74 % | -4.350 dB | -4.726 dB | -6.487 dB at 133.2 Mbit/s | FAIL |
| + table economics (no tool bit) | +62.23 % | -4.322 dB | -4.694 dB | -6.421 dB at 132.1 Mbit/s | FAIL |
| + `CTX_V3` (bit 24) | **+59.70 %** | **-4.205 dB** | **-4.573 dB** | -6.363 dB at 129.0 Mbit/s | FAIL |

**4:2:0**

| | BD-rate vs x264 intra | BD-PSNR | mean deficit | worst deficit | verdict |
|---|---|---|---|---|---|
| v1.4 default | +39.39 % | -3.364 dB | -3.825 dB | -6.110 dB at 128.8 Mbit/s | FAIL |
| + table economics (no tool bit) | +38.79 % | -3.320 dB | -3.767 dB | -6.059 dB at 128.2 Mbit/s | FAIL |
| + `CTX_V3` (bit 24) | **+37.66 %** | **-3.260 dB** | **-3.673 dB** | -6.033 dB at 125.9 Mbit/s | FAIL |

Verbatim, the final gate lines. 4:4:4:

```
  BD-rate of nxv-ctxv3 on PSNR-Y (negative is better):
    vs x264-intra     +59.70 %   BD-PSNR -4.205 dB   (overlap 47.12-57.14 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -6.363 dB at 129.0 Mbit/s, mean -4.573 dB over 129.0-400.0 Mbit/s
```

4:2:0:

```
  BD-rate of nxv-ctxv3 on PSNR-Y (negative is better):
    vs x264-intra     +37.66 %   BD-PSNR -3.260 dB   (overlap 47.06-57.15 dB)

  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):
    FAIL: worst -6.033 dB at 125.9 Mbit/s, mean -3.673 dB over 125.9-400.0 Mbit/s
```

**The gate is still not met**, and nothing in this package was ever going to
meet it: entropy coding is the third item of `README.md`'s gap analysis, the
one it estimated at "perhaps 5-8 %". The package moves the intra gate by
**3.04 points of BD-rate on 4:4:4** and **1.73 on 4:2:0** -- 0.15 dB and
0.10 dB of mean deficit -- of which `CTX_V3` is 2.53 and 1.13 and the table
economics are 0.51 and 0.60. For scale, `CTX_V2` was worth 2.3 points on 4:4:4
when it landed, so the second context split is worth about what the first one
was, on top of it.

Note that these are the **band-limited v2 sequences**, which are harder for
this codec than the v1 material `RESULTS-intra.md` was measured on (+62.74 %
here against +40.35 % there for the same encoder configuration). The two
documents' absolute numbers are therefore not comparable; the before/after
pairs inside each are.

### Operating points, 4:4:4

| QP | v1.4 Mbit/s | v1.4 PSNR-Y | v1.5 Mbit/s | v1.5 PSNR-Y |
|---|---|---|---|---|
| 0 | 754.8 | 57.29 | 730.2 | 57.14 |
| 4 | 570.1 | 55.35 | 553.6 | 55.29 |
| 8 | 424.7 | 53.33 | 411.8 | 53.26 |
| 12 | 320.1 | 50.63 | 313.6 | 50.53 |
| 16 | 241.0 | 47.63 | 235.7 | 47.62 |
| 20 | 177.6 | 44.53 | 172.2 | 44.45 |
| 24 | 133.2 | 41.47 | 129.0 | 41.41 |

### Operating points, 4:2:0

| QP | v1.4 Mbit/s | v1.4 PSNR-Y | v1.5 Mbit/s | v1.5 PSNR-Y |
|---|---|---|---|---|
| 0 | 602.0 | 57.29 | 586.0 | 57.15 |
| 4 | 461.4 | 55.36 | 453.2 | 55.30 |
| 8 | 358.3 | 53.33 | 352.2 | 53.27 |
| 12 | 282.5 | 50.65 | 278.7 | 50.55 |
| 16 | 223.0 | 47.64 | 216.0 | 47.63 |
| 20 | 168.9 | 44.51 | 163.9 | 44.41 |
| 24 | 128.8 | 41.47 | 125.9 | 41.41 |

Every point is smaller at essentially the same quality, which is the shape an
entropy tool should have: it changes how many bits the same decisions cost, not
which decisions the encoder makes. The 0.06-0.15 dB that does move is the RD
trellis and the table-set choice seeing a different rate model, which is a
second-order consequence rather than the tool.

---

## 2. Phase 2 inter: the kill test

`nxv-enc --eyes 2 --inter on --poses vr-mixed-1024-v2.poses.json`, 12 frames,
against `x265-p`, in the two rate bands `ref/RESULTS-inter.md` section 1
defines. Verdicts computed by `ref/phase2_verdict.py` from the result files.
4:4:4 is run in both bands because it is the configuration whose verdict
`RESULTS-inter.md` quotes verbatim; 4:2:0 is run at band B, which is where a
fixed per-tile cost like the vector bytes is the largest fraction of a frame.

| band | pix | v1.4 default | + `CTX_V3` | + `VEC_ENT` |
|---|---|---|---|---|
| A (100-300 Mbit) | 4:4:4 | +383.41 % | **+377.20 %** | +377.62 % |
| B (the paper's bpp) | 4:4:4 | +588.19 % | **+580.29 %** | +583.71 % |
| B (the paper's bpp) | 4:2:0 | +518.73 % | +524.63 % | +520.02 % |

**The kill test fails in every row**, exactly as it did before this package and
for the same reason: `RESULTS-inter.md` section 4 established that the gap to
x265-p is the intra core, not the inter path, and an entropy tool worth 2-8
points does not move a 400-point deficit. Verbatim, band A 4:4:4 with
`CTX_V3`:

```
    overall (all frames)          BD-rate +377.20 %  BD-PSNR -7.554 dB
    fastest 20 % of frames        BD-rate +347.73 %  BD-PSNR -6.997 dB
    the remaining frames          BD-rate +387.30 %  BD-PSNR -7.740 dB
  Phase 2 kill test (PAPER.md 2.11 item 1):
    "within 10 percent at rest and at least 30 percent better on the motion frames"
    at rest   : BD-rate +387.30 % (allowed up to +10 %)  FAIL
    on motion : BD-rate +347.73 % (needs -30 % or better)  FAIL
    VERDICT   : FAIL
```

(These are the band-limited v2 sequences, so the absolute numbers are much
larger than `RESULTS-inter.md`'s +160.70 %; the before/after pair inside this
table is what it is for.)

### `CTX_V3` on inter

Per operating point, the bytes and the luma PSNR:

| band, pix | QP | v1.4 bytes | `CTX_V3` bytes | delta | dB moved |
|---|---|---|---|---|---|
| A, 4:4:4 | 0 | 10 197 554 | 9 841 462 | -3.49 % | -0.09 |
| A, 4:4:4 | 4 | 6 812 714 | 6 604 404 | -3.06 % | -0.07 |
| A, 4:4:4 | 8 | 4 478 928 | 4 336 600 | -3.18 % | -0.11 |
| A, 4:4:4 | 12 | 3 052 528 | 2 948 132 | -3.42 % | -0.12 |
| B, 4:4:4 | 18 | 1 514 620 | 1 485 836 | -1.90 % | -0.06 |
| B, 4:4:4 | 24 | 654 710 | 640 818 | -2.12 % | -0.02 |
| B, 4:4:4 | 30 | 311 976 | 314 238 | **+0.73 %** | -0.02 |
| B, 4:4:4 | 36 | 168 512 | 155 654 | -7.63 % | -0.08 |

The tool is worth 2-3.5 % of the bytes across band A, which is the -6.2
BD-rate points in the table. Band B is noisier and one point goes the wrong
way: at QP 30 two thirds of the tiles are `WARP_SKIP` and carry no payload at
all, so the frame is a few dozen coded tiles plus the table sets, and the
encoder's table-set decisions move it more than the context model does. That
is the same thing the 4:2:0 band B row is: +5.90 points overall, made of
-0.61 %, +0.78 %, +0.76 % and -7.24 % on the four points. **On a frame that
is mostly skip, this package is measuring table overhead, not coefficient
statistics.**

### `VEC_ENT` on inter: what it actually removes, and why the BD-rate cannot see it

`VEC_ENT` does one thing exactly: it deletes the two raw bytes a tile with
`mv_present` carries and codes the same value in the payload. Measured
directly with `nxv-enc --stats`, summed over the 12-frame 4:2:0 stereo
sequence:

| QP | tile-header bytes, `CTX_V3` | with `VEC_ENT` | removed | vector tiles |
|---|---|---|---|---|
| 8 | 53 564 | 50 328 | -6.0 % | 1618 |
| 18 | 33 588 | 30 584 | -8.9 % | 1502 |
| 24 | 22 578 | 20 400 | -9.6 % | 1089 |
| 30 | 16 702 | 14 928 | -10.6 % | 887 |
| 36 | 12 436 | 11 248 | -9.6 % | 594 |

That part is unambiguous and is exactly `2 * (vector tiles)`. What the tool
then spends is the coded vector itself -- a class symbol plus raw bits plus a
sign, six to ten bits for the vectors this material produces -- so the *net*
is six to ten bits per vector tile, roughly 800 to 1400 bytes at QP 24.

The whole-stream numbers do not resolve that. At QP 24 the stream goes from
453 278 to 443 026 bytes (-2.26 %) and at QP 18 from 956 426 to 961 452
(+0.53 %): the tool's own effect is an order of magnitude smaller than the
swing the encoder's table-set and trellis decisions take when the rate model
under them changes. The BD-rate rows above say the same thing -- +0.42, +3.42
and -4.61 points against `CTX_V3`, with no sign.

**Verdict: specified, implemented, conformance-vectored, and left off by
default.** It is correct -- `ref.inter` proves the encoder's shadow and the
decoder agree byte for byte over 40 frames with random tile loss in mono and
stereo, and that the same sequence coded both ways reports identical vectors
-- and the header bytes it removes are real. But on this material the tile
population it acts on is small at both ends of the ladder (band A is majority
`INTRA`, band B is two thirds `WARP_SKIP`), so it is below the resolution of
the measurement that decides the package, and a tool that cannot be shown to
pay should not be the default. The number to re-measure it against is the
tile-header fraction of a frame, not BD-rate: at the low-rate operating point
of `docs/SYNTAX.md` Appendix B, tile headers are 13.7 % of the frame, and this
takes 10 % of that.

### Why both tools are off by default

`nxvc_config_default()` leaves `ctx_v3` and `vec_ent` at 0, so an existing
caller writes a v1.4 stream and every v1.4 bitstream is byte-identical to what
a v1.4 build produced. `CTX_V3` is worth turning on by default the day
`vk/decoder/passA` implements tool bit 24; today it does not, and the Vulkan
decoder refuses bit 24 at the handshake (`nxvc_vkdec_parse.cpp`), so an
encoder default of `--ctx v3` would make the reference and the GPU decoder
disagree about what a default stream is. `passA/README.md` prices the shader
work; this is the decision that flips when it lands.

### Encode and decode time

One 2048x1024 frame, single threaded, best of three, under the standard CPU
discipline. `CTX_V3` costs neither side anything measurable -- it selects a
different row of a table that was already there.

| | v1.4 encode | v1.4 decode | `CTX_V3` encode | `CTX_V3` decode |
|---|---|---|---|---|
| 4:4:4 QP 12 | 1.64 s | 0.092 s | 1.50 s | 0.092 s |
| 4:4:4 QP 24 | 1.43 s | 0.086 s | 1.60 s | 0.125 s |
| 4:2:0 QP 12 | 1.50 s | 0.088 s | 1.55 s | 0.088 s |
| 4:2:0 QP 24 | 1.58 s | 0.076 s | 1.58 s | 0.083 s |

The spread inside each column is the measurement's own noise on this machine;
there is no systematic difference in either direction.

---

## 3. Table economics (encoder only, no tool bit)

Two things were wrong with how the encoder handled its eight probability table
sets, and neither is visible to a decoder.

**It transmitted a set that did not pay for itself.** A set was sent whenever
any tile chose it, however little that choice was worth. A set costs
`nctx * 16 * 5` bits of frame header -- 160 bytes under `CTX_V2`, 270 under
`CTX_V3` -- and the encoder already knows, from the histogram it trained the
set on, exactly how many bits the set saves against the built-in default it
would otherwise use. It now sends the set only when the saving exceeds the
cost. On a 2048x1024 4:4:4 frame of the harness material, `tables_present`
goes from 6 sets to 5 at QP 8, 4 to 4 at QP 16, 4 to 3 at QP 24 and 3 to 3 at
QP 32 -- roughly one set a frame, 160 to 270 bytes.

**It chose each tile's set against the wrong tables.** `select_set()` compared
the tile's histogram against the *built-in defaults* in both encoder passes,
including the second pass, which encodes with the trained tables the first pass
had just derived. A tile was therefore assigned to the set that would have been
cheapest under a rate model the encoder had already replaced. Pass 0 still
selects against the defaults -- that is all a tile can be coded with while the
tables are being trained -- and the emit pass now selects against the tables in
force. This is one Lloyd reassignment step and can only lower the total, given
fixed tables.

Both are recorded as SYNTAX.md Appendix A decision 57. The measured effect is
the `+ table economics` row of section 1: it is the first cumulative row, so
that `CTX_V3` is measured on top of an encoder that is not wasting header
bytes on tables it does not need.

**Per tile row was considered and not built.** A table set per tile row would
need a syntax change -- `table_set` is a 3-bit field and would have to become a
row-header field or grow -- and it would cost `rows` transmitted sets instead
of at most 8: on the 2048x1024 frame above, 16 rows x 270 bytes = 4320 bytes
against the 810 to 1350 the encoder now actually spends. The measurement that
kills it is the one immediately above: at every QP on this material the
encoder, once it is charged honestly for a set, chooses to transmit **fewer
than eight**. The content does not support the eight clusters the syntax
already offers, so offering sixteen is offering a finer partition of something
that is not there.

---

## 4. What was measured and rejected inside the tools

### 4.1 A second context for the intra mode symbol

The obvious companion to `VEC_ENT` -- "better binarisation for the mode as well
as the vector" -- is to split the `MODE` symbol's single context (15) in two on
whether the block's left and above neighbours agreed, since an MPM derived from
two agreeing neighbours is much stronger than one derived from `min()` of two
that disagreed. It was built, given its own row in the v3 family, retrained
with `nxv-gentables`, and measured.

It **loses**, on this material, at both ends of the ladder:

| stream | without the mode split | with it |
|---|---|---|
| 12-frame inter 4:2:0, QP 8 | 2 617 446 B | 2 635 750 B (+0.70 %) |
| 12-frame inter 4:2:0, QP 24 | 440 358 B | 448 340 B (+1.81 %) |

Two reasons, and they compound. The mode symbol is one symbol per 8x8 block per
plane, so a context split halves the training data behind each row while the
`MPM` itself already carries most of the conditioning the split would add; and
the extra row costs 10 bytes in every transmitted table set, which at QP 24 is
a larger number than the split recovers. The code was removed rather than left
switchable: an unused row in the context ladder is a byte tax on every stream
that transmits a table.

### 4.2 What "transform size" turned out to mean here

Item 1 of the brief asks for contexts conditioned on transform size. In v1
there is only one transform: 8x8. Tool bit 19 `XFORM_4X4_SPLIT` is reserved and
unimplemented, so no block unit ever has a different size. The one size that
does vary is the **DC plane**, which is `nb * nb` values with `nb` following
`res_level` and the chroma format -- 64, 16, 4 or 1. Splitting the DC-plane
contexts on that was not built: `res_level != 0` is a foveation feature the
harness sequences do not use at all, so there was nothing to measure it on, and
spending three context rows (30 bytes a table set on every stream) on a case
the measurement cannot see would be the mistake 4.1 just made. `CTX_V3` gives
the DC plane one new row -- `LEVEL` at scan position 0, contexts 24 -- which
conditions on position rather than size and is exercised by every stream.

### 4.3 `ref_delta`, and what is left in the tile header

`ref_delta` is not a bitstream field. It is the transport's **advisory copy**
of the tile header's `ref_sel` (`docs/SYNTAX.md` 4.1), with one extra value for
"no temporal reference"; `ref_sel` is authoritative and is two bits of a
tile-header word that is a fixed 8 bytes whatever those bits hold. Entropy
coding it would save nothing at all -- the header does not get shorter -- so
the only thing that could pay is moving the *whole* 8-byte tile header into the
payload, which is a different package: it is 3 % of a frame in the Phase 1
band and 13.7 % at the low-rate operating point of Appendix B, and it trades
away the property that a receiver can act on a tile before parsing it. This
package took the two bytes that are pure overhead and left the eight that are
also a transport interface.

---

## 5. The two experiments

### 5.1 12-bit probabilities: measured, rejected, and what to do with it

The probability precision is 10 bits (`M = 2^10`, SYNTAX.md 9.5), so every
context row is a 16-way partition of 1024 slots. The question is what that
quantisation costs against exact probabilities, and how much of it 12 bits
would recover.

Measured directly on the reference encoder's own per-tile symbol histograms
(`nxvc_debug_tile_histograms`, the same hook `nxv-gentables` uses), on the left
eye of frame 0 of `vr-mixed-1024-v2` 4:4:4 at the shipped default settings with
`CTX_V3`, aggregated over all 256 tiles and all 29 contexts. "ideal" is the
empirical entropy of the symbol stream; each column is the cost of coding the
same symbols with a row normalised to that precision by the procedure in 9.4.

| QP | ideal | 10-bit | 12-bit | 16-bit | 10 -> 12 |
|---|---|---|---|---|---|
| 8 | 68 188 B | +0.304 % | +0.071 % | +0.004 % | **-0.233 %** |
| 16 | 41 379 B | +0.486 % | +0.116 % | +0.007 % | **-0.369 %** |
| 24 | 22 353 B | +0.888 % | +0.213 % | +0.013 % | **-0.670 %** |
| 32 | 12 005 B | +1.664 % | +0.410 % | +0.025 % | **-1.234 %** |

The shape is the expected one: quantisation error is a fixed number of bits per
context row, so it matters more the fewer symbols there are to amortise it
over, and the payload shrinks by 6x across this ladder.

**What 12 bits would cost a decoder: nothing.** The decode step is
`slot = x & (M-1)`, a 4-step branchless binary search over the same 16
cumulative frequencies, one multiply and two adds. `cum` values reach 4096
instead of 1024, which still fits the `uint` the Pass A table is already made
of, so `s_cum` does not grow by a byte and the search does not gain a step.
On the encoder the renormalisation bound `x >= (f << 22)` becomes `f << 20`,
which still holds for every legal `f <= 4095`.

**What it would cost as a tool bit: a lot.** A per-stream probability precision
makes the mask and the shift in the innermost loop of Pass A runtime values
rather than constants, and it invalidates all three built-in table families and
every conformance vector at once.

**Verdict: not adopted, and recorded as the thing to take at the next
incompatible revision** (SYNTAX.md Appendix A decision 59). At the Phase 1
operating band, QP 0 to 24, it is 0.23 % to 0.67 % -- smaller than either of
the tools in this package -- and it is free exactly when the format is already
breaking.

### 5.2 16 lanes per tile: measured, rejected

`nsub_log2` already permits 16 and 32 lanes; no syntax change was needed to
measure it. One 2048x1024 4:4:4 frame, `--ctx v3`, everything else default:

| QP | 8 lanes (`--nsub 3`) | 16 (`--nsub 4`) | 32 (`--nsub 5`) | the encoder's own choice (`auto`) |
|---|---|---|---|---|
| 8 | 196 300 B | 208 742 B (+6.3 %) | 236 244 B (+20.4 %) | **189 466 B** |
| 16 | 117 298 B | 131 576 B (+12.2 %) | 161 560 B (+37.7 %) | **108 822 B** |
| 24 | 70 560 B | 85 786 B (+21.6 %) | 116 368 B (+64.9 %) | **60 242 B** |
| 32 | 45 736 B | 61 002 B (+33.4 %) | 92 376 B (+102.0 %) | **34 152 B** |

Against the encoder's own per-tile choice, 16 lanes is +10.2 % at QP 8 and
+78.6 % at QP 32.

The reason is not subtle: a lane costs 4 bytes of initial rANS state in every
tile that uses it, and the encoder already spends lanes only where the payload
can carry them -- at this band it averages 1 to 3 lanes per tile, not 8. More
lanes is more flush on a payload that is not getting any shorter.

The GPU side offers nothing to trade against that. The shared table is indexed
by **table set**, not by tile, so halving the tiles per workgroup does not
shrink it; a 16-lane cluster needs `subgroupSize >= 16`, which forces the LDS
fallback read-pointer path on every ICD with a subgroup of 8 (lavapipe today);
and the reference already caps its automatic choice at 8 for the same reason
the table above shows.

**Verdict: not adopted** (SYNTAX.md Appendix A decision 60). 8 lanes stays the
v1 configuration and the number a GPU decoder assumes as the maximum.

---

## 6. The GPU decoder after each step

The brief asks for the LDS budget and the per-symbol decode ops after each
change; `vk/decoder/passA/README.md` is the authority and this is the summary.
A workgroup is 8 tiles x 8 rANS lanes and the shared table is indexed by
**table set**, not by tile, so it does not scale with the tile count.

| state | `s_cum` | + scan + geometry | total | per-symbol decode |
|---|---|---|---|---|
| v1 (12 contexts) | 6 144 B | ~2 024 B | ~8.0 KiB | mask, 4-step branchless binary search over `cum[16]`, one multiply, two adds, conditional 16-bit load |
| `CTX_V2` (16) | 8 192 B | ~2 024 B | ~10.0 KiB | unchanged |
| `CTX_V3` (27) | 13 824 B | ~2 024 B | ~15.5 KiB | unchanged; the context index costs one compare and one add on two registers the lane already holds |
| + `VEC_ENT` (29) | 14 848 B | ~2 024 B | ~16.5 KiB | unchanged; one extra unit on lane 0 per vector tile, at most two symbols and sixteen bypass bits |

Against a 32 KiB budget, so the package spends about a fifth of the headroom
and leaves 15 KiB. Nothing in it adds a barrier, a buffer, a cross-lane read
or a dependent step inside a tile: the v3 neighbour is defined by the lane
precisely so that it cannot (SYNTAX.md Appendix A decision 53), and the vector
unit is a unit like any other in the schedule the kernel already runs. The one
structural change `VEC_ENT` asks for is that Pass A write the tile record's
vector rather than the host deriving it from the tile header.

The 12-bit-probability variant of section 5.1 would leave every number in that
table unchanged, which is the reason it is recorded as the thing to take at the
next version break rather than as a tool bit.

---

## 7. Where this leaves the package

| item | tool bit | measured | shipped |
|---|---|---|---|
| `CTX_V3`, 27 coefficient contexts | 24 | **-2.53** BD-rate points on 4:4:4 intra, **-1.13** on 4:2:0, **-6.2** on 4:4:4 inter at band A | yes, off by default until Pass A implements bit 24 |
| `VEC_ENT`, the coded tile vector | 25 | removes 6-11 % of the tile-header bytes; net effect below the noise of the encoder's own decisions on this material | yes, off by default |
| table economics | none | **-0.51** points on 4:4:4 intra, **-0.60** on 4:2:0 | yes, unconditionally: it is encoder-only |
| the mode-symbol context split | — | +0.70 % to +1.81 % of an inter stream, i.e. a **loss** | no, removed |
| 12-bit probabilities | — | recovers 0.23 % to 1.23 % of the coefficient payload | no; take it at the next version break, where it is free |
| 16 lanes per tile | — | +6.3 % to +33 % against 8 lanes | no |

The intra gate moves by 3.04 points on 4:4:4 and 1.73 on 4:2:0 and remains a
**FAIL**; the Phase 2 kill test remains a **FAIL** in both bands. Neither was
in reach: `README.md`'s gap analysis put the entropy coder at "perhaps 5-8 %"
of the deficit and this package collects about half of that, which is roughly
what a second context split on top of the first should be worth. What it also
does is spend the context budget in a way that a GPU decoder can implement
without a barrier, and leave the two levers it did not take -- 12-bit
probabilities and the 8-byte tile header -- priced rather than guessed.


---

## 8. How to reproduce

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
cmake -S . -B build-ref -G Ninja -DNXWARP_BUILD_VK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-ref -j4

# the whole measurement matrix: three intra configurations and three inter
# ones, into $NXQ_SCRATCH/results/tourney-ctx-a/
tools/quality/queue-ctx-a.sh

# the verdicts
python3 ref/phase2_verdict.py --results $NXQ_SCRATCH/results/tourney-ctx-a/kill*.json
```

`tools/quality/run-ctx-a.sh <intra|inter> <tag> <bindir> [encoder flags]` is the
single run; `queue-ctx-a.sh` is the six of them in order, with the "before"
column built from the fork point into
`$NXQ_SCRATCH/tourney-ctx-a/base-src/`. Everything runs under
`chrt -i 0 taskset -c 28-31 nice -n 19`.

The two experiments in section 5 are reproduced by
`nxv-enc --nsub 3|4|5|auto` (5.2) and by the histogram analysis over
`nxvc_debug_tile_histograms` described in 5.1.

Conformance:

```sh
ctest --test-dir build-ref -R 'ref\.'
cmake --preset asan-ubsan -DNXWARP_BUILD_VK=OFF
cmake --build --preset asan-ubsan -j4
ctest --preset asan-ubsan -R 'ref\.|^fuzz\.'      # 17/17
build-ref/tests/ref/nxv-vectors --check tests/vectors
```
