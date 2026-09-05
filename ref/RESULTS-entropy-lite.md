# ENTROPY_LITE: a fully parallel entropy tool, measured

What this is: `docs/SYNTAX.md` 9.10, tool bit 30 (the branch wrote 9.8 and bit 24; both were renumbered by the merge). An entropy coding for the
coefficient payload with **no arithmetic coder** -- no rANS state, no
probability tables, and no serial dependency between one coded value and the
next. It exists for one reason: Pass A (`vk/decoder/passA`) is latency-bound on
the longest tile's serial symbol chain, the barriers were already removed and
bought nothing on wave64, and the levers left in the rANS design are all small.
This tool removes the chain instead of shortening it.

It costs bits. How many, and whether that trade is ever the right one, is what
this document measures.

Everything here was produced on the branch `exp/entropy-lite`. Rate and quality
come from `tools/quality/compare.py` against `x264 --keyint 1 --tune
zerolatency` through ffmpeg n9.0.1, on `vr-mixed-1024-v2` (2048x1024
side-by-side, **12 frames**, 90 fps, `synthetic:mixed:seed1:v2-bandlimited-ss4`),
4:4:4 and 4:2:0, at QP 8/12/16/20/24. Every process ran under
`chrt -i 0 taskset -c 18-19 nice -n 19`. Result files are under
`$NXQ_SCRATCH/results/entlite/`. GPU timings come from
`vk/decoder/passA/tools/nxvc-passA-test`.

---

## 0. The short version

| | |
|---|---|
| Bit cost of Lite over rANS | **+35 to +47 %**, rising with QP |
| Which variant | **`FIXED`**, except above ~140 Mbit/s where `RICE` is 1-4 % better |
| Pass A, RADV 7900 XTX, 2048 tiles | 0.651 ms rANS -> **0.158 ms Lite, 4.1x** |
| Pass A, lavapipe, 512 tiles | 33.2 ms -> **23.8 ms, 1.4x** |
| Crossover on a 12 ms entropy budget | **~140 Mbit/s** (see section 5) |
| Recommendation for a Pico profile | **ship it, `FIXED`, and switch on the budget, not on the bitrate** |

The bit cost is roughly three times what PAPER 1.6 predicted for the
`ENT_BITPLANE` fallback ("+10-15 %"). That estimate was made against a CABAC
baseline on the paper's assumed statistics; measured against this codec's own
rANS on real tiles it does not survive.

---

## 1. Candidates

Three were on the table.

**(a) `FIXED`** -- per coding unit: a significance map, a 3-bit magnitude class
selecting a fixed field width, that many bits of `|q| - 1` per nonzero, and a
sign. Every coefficient's bit position is computable, so **one thread can
decode one coefficient**.

**(b) `RICE`** -- the same, with a per-unit Exp-Golomb order in place of the
fixed width and an explicit 12-bit body length so the *unit* stays addressable.
Magnitudes are variable-length, so the finest parallelism is **one thread per
unit**.

**(c) more rANS lanes** -- `nsub_log2` up to 5 (32 lanes), which is a bitstream
lever that already exists and needs no new tool. It shortens the chain by at
most 4x and never removes it.

(a) and (b) are implemented in `ref/src/entropy_lite.{h,cpp}` behind tool bit
24, with the variant carried in the tile header's `table_set` field (which a
stream with no probability tables has nothing else to say). (c) is measured
through the existing `--nsub` flag.

### What the first cut got wrong

The first working version of (a) cost **+113 %** at QP 24, and the section
accounting (`NXVC_LITE_STATS=1`) said why. Two of the three biggest items were
structural waste rather than entropy loss, and both were fixed before any of the
numbers below were taken:

| | first cut | fixed by |
|---|---|---|
| 64-bit significance map on every coded unit | 12.7 % of the payload at QP 8 | coding significance only up to `LAST`, and not coding `LAST` itself |
| raw 4-bit intra modes | **38.5 %** at QP 24 | an MPM flag in the fixed section, a 3-bit non-MPM index in the variable one |
| one CBF bit per unit, 90 % of them zero | 9.9 % at QP 24 | a two-level map, 16 units to a group flag |
| per-unit magnitude class of 1 bit minimum | -- | a **zero-bit** class 0: a unit whose every nonzero is +-1 spends nothing on magnitudes |

Those four take QP 24 from +113 % to +47 %. What is left is the entropy loss
proper.

### Where the bits go now

`FIXED`, 4:4:4, 2 frames, `NXVC_LITE_STATS=1`:

| section | QP 8 | QP 24 |
|---|---|---|
| coded-unit map (two-level) | 4.0 % | 7.0 % |
| `LAST` + magnitude class | 11.8 % | 12.3 % |
| significance | 14.0 % | 13.7 % |
| intra modes | 8.4 % | 17.1 % |
| magnitudes | 51.6 % | 39.5 % |
| signs | 9.9 % | 9.5 % |
| section padding | 0.4 % | 1.0 % |

Signs are incompressible in both coders. The gap against rANS is the
significance map (rANS spends well under a bit on a zero in a sparse tail, Lite
spends exactly one) and the magnitude field (rANS has eight banded contexts,
Lite has one width for the whole unit).

---

## 2. Bits, per QP

Mbit/s at 90 fps, and PSNR-Y. The quantized coefficients are **identical**
between the rANS and Lite columns of a row -- the RD trellis, the table-set
choice and the mode decision all still run on the rANS rate model, so this is
two entropy coders on the same data, not two encoders. (The small PSNR
difference is sign data hiding, which tool bit 30 forbids and which the rANS
column therefore keeps.)

### 4:4:4

| QP | x264-intra | rANS | `FIXED` | `RICE` | `FIXED`/rANS | `RICE`/rANS | PSNR-Y (rANS / Lite) |
|---|---|---|---|---|---|---|---|
| 8  | 212.1 | 141.3 | 195.2 | **191.9** | 1.381 | **1.358** | 53.33 / 53.20 |
| 12 | 142.1 | 106.3 | 149.2 | **148.9** | 1.404 | **1.401** | 50.63 / 50.57 |
| 16 | 102.3 |  80.3 | **114.6** | 116.7 | **1.427** | 1.453 | 47.63 / 47.57 |
| 20 |  72.1 |  59.3 |  **85.0** |  87.9 | **1.433** | 1.482 | 44.53 / 44.55 |
| 24 |  52.5 |  44.6 |  **65.6** |  68.0 | **1.471** | 1.525 | 41.47 / 41.51 |

### 4:2:0

| QP | x264-intra | rANS | `FIXED` | `RICE` | `FIXED`/rANS | `RICE`/rANS |
|---|---|---|---|---|---|---|
| 8  | 206.1 | 118.8 | 160.9 | **154.5** | 1.354 | **1.300** |
| 12 | 140.8 |  93.7 | 125.7 | **124.6** | 1.341 | **1.330** |
| 16 | 100.7 |  73.9 | **100.7** | 106.2 | **1.363** | 1.437 |
| 20 |  72.8 |  56.0 |  **75.7** |  80.5 | **1.352** | 1.438 |
| 24 |  53.8 |  42.8 |  **58.6** |  63.3 | **1.369** | 1.479 |

### BD-rate against x264 intra

| | 4:4:4 | 4:2:0 |
|---|---|---|
| rANS (the shipped default) | +56.18 % (-4.466 dB) | +35.94 % (-3.513 dB) |
| Lite `FIXED` | +120.93 % (-7.631 dB) | +84.90 % (-6.532 dB) |
| Lite `RICE` | +119.88 % (-7.794 dB) | +83.43 % (-6.910 dB) |

`RICE` scores a marginally better BD-rate than `FIXED` while being worse at
three of the five QPs, because the BD-rate integral is taken over the PSNR
overlap and that overlap sits at the high-quality end, which is exactly where
`RICE` wins. The per-QP table is the honest one for a rate-capped link.

### Verdict on (a) vs (b)

`FIXED` wins from QP 16 down in rate, by 2 to 8 %; `RICE` wins above about
140 Mbit/s (4:4:4) or 130 Mbit/s (4:2:0), by 1 to 4 %. The crossover is at the
very top of the operating range.

That margin does not pay for what `RICE` costs: a 12-bit length field on every
coded unit (which is why it loses at high QP -- the field is 12 bits whether
the unit's body is 20 bits or 2000), and, more importantly, **it gives up
per-coefficient parallelism**. A `RICE` unit must be walked coefficient by
coefficient because the codes are variable length. `FIXED` is the winner and
is what the GPU path implements.

---

## 3. Candidate (c): more rANS lanes

Same sequence, 12 frames, 4:4:4, total stream bytes.

| QP | 8 lanes | 16 lanes | 32 lanes | `auto` (shipped) | Lite `FIXED` |
|---|---|---|---|---|---|
| 8  | 2 435 786 | 2 585 668 | 2 905 638 | **2 355 058** | 3 253 651 |
| 12 | 1 863 266 | 2 021 300 | 2 365 488 | **1 772 064** | 2 486 136 |
| 16 | 1 442 534 | 1 607 618 | 1 959 846 | **1 339 052** | 1 909 251 |
| 20 | 1 104 094 | 1 279 350 | 1 643 986 |   **988 752** | 1 416 506 |
| 24 |   870 444 | 1 050 510 | 1 418 882 |   **742 704** | 1 092 811 |

Relative to the shipped `auto`:

| QP | 32 lanes | Lite `FIXED` |
|---|---|---|
| 8  | **1.234** | 1.382 |
| 12 | **1.335** | 1.403 |
| 16 | 1.464 | **1.426** |
| 20 | 1.663 | **1.433** |
| 24 | 1.910 | **1.471** |

Every lane costs a 4-byte flush per tile, so the cost of lanes is a *fixed*
number of bytes and its share grows as the payload shrinks: 32 lanes is +23 %
at QP 8 and **+91 %** at QP 24. Lite's overhead is proportional to the
coefficients and barely moves.

So (c) is cheaper than Lite only at QP 12 and below, and even there it buys a
chain **4x shorter**, not gone: a 32-lane tile still runs a serial round loop,
still needs the shared read pointer and its ballot, and still leaves the frame
time equal to the longest tile's chain. Above QP 16 it is both more expensive
in bits *and* less parallel. Note also that `nsub_log2 > 5` is not available:
`kRansMaxLanes` is 32 and the reference decoder rejects 6, so "64 lanes" would
be a syntax change, not a flag.

**(c) is rejected.**

---

## 4. Pass A

The `FIXED` variant is implemented in `vk/decoder/passA` behind specialisation
constant 3, `ENTROPY_MODE` (0 = rANS, unchanged; 1 = Lite/`FIXED`), with a
line-for-line CPU model in `passA_model.cpp` and a test encoder that is
byte-identical to `nxvc::lite_encode_units` (checked over 400 random tiles
spanning every `res_level`, `chroma444`, alpha, `tskip` and `INTRA_DIR`
setting).

Dispatch shape: **one 64-thread workgroup per tile**, unit `u` on thread
`u % 64`. Three prefix sums -- over the section-P widths, the section-S widths
and the section-B body lengths -- and then every unit is independent. No
ballot, no shared read pointer, no round loop.

### Correctness

Zero mismatches against the CPU model, on coefficients, CBF bits, unit lengths
and the packed intra modes:

| device | subgroup | read-ptr mode | layout | result |
|---|---|---|---|---|
| RADV, 7900 XTX | 64 (default) | ballot + LDS | dense + sparse | 0 |
| RADV | 32 forced | ballot + LDS | dense + sparse | 0 |
| RADV | 64 forced | ballot + LDS | dense + sparse | 0 |
| RADV, `--intra` | default | ballot | dense + sparse | 0 |
| lavapipe | 8 | ballot + LDS | dense + sparse | 0 |
| lavapipe, `--intra` | 8 | ballot + LDS | dense + sparse | 0 |

`READ_PTR_MODE` has no meaning under Lite -- there is no shared read pointer --
and running both values is only a proof that the output does not depend on it.

The rANS path is byte-for-byte unchanged: all 11 `vk.passA.*` tests pass, and
an FNV fingerprint of the corpus bytes plus the model's complete output over
three seeds x both read-pointer modes x both layouts is identical before and
after (`37ba4a4a9105b4b6`).

### Timing

`--mode ballot`, sparse layout, best dispatch of 20, same corpus and tile
count for both rows:

| | rANS | Lite `FIXED` | speedup |
|---|---|---|---|
| RADV 7900 XTX, 2048 tiles | 0.651 ms -- 318 ns/tile | **0.158 ms -- 77 ns/tile** | **4.1x** |
| lavapipe, 512 tiles | 33.2 ms -- 64.8 us/tile | **23.8 ms -- 46.4 us/tile** | 1.4x |

Two caveats on those numbers, both real:

* **The RADV figures are bimodal.** Six back-to-back runs gave rANS 0.651 /
  0.675 / 0.676 ms in one clock state and 1.227-1.229 ms in another; Lite gave
  0.158 / 0.166 and 0.295-0.297 in the same two states. The **ratio is stable
  across both**, which is the number this document rests on. The high-clock
  state is reported because it matches the 0.645 ms already in
  `vk/decoder/passA/README.md`.
* **lavapipe gains little, and should.** It is a CPU rasteriser, it was pinned
  to the same two cores as everything else here, and it is bandwidth-bound on a
  Lite payload that is 50 % larger (916 vs 612 B/tile on this corpus). It is
  not the device the tool is for.

The RADV result is the one that matters, and it says what the design predicted:
removing the chain, not shortening it, is worth **4x**, and Pass A stops being
latency-bound on one tile at all -- 77 ns/tile is throughput, not a chain.

---

## 5. The crossover on a 12 ms entropy budget

The premise: a Pico K4 measures **12.3 ms** for rANS Pass A, against a 12 ms
per-frame entropy budget. It is over budget today.

The extrapolation, stated so it can be checked: Pass A's cost tracks the
longest tile's symbol count; the 12.3 ms figure and the 0.65 ms RADV figure are
the same 0.50 symbols/pixel corpus, and 0.50 symbols/pixel on this sequence is
about **0.75 bits/pixel**, which at 2048x1024 and 90 fps is **~141 Mbit/s** --
the QP 8, 4:4:4 operating point of section 2. So:

| | rANS | Lite `FIXED` |
|---|---|---|
| Pass A at ~141 Mbit/s of *coded quality* | 12.3 ms | ~3.0 ms |
| Rate needed for that quality | 141 Mbit/s | 195 Mbit/s (x1.38) |
| Rate at which Pass A reaches the 12 ms budget | **~138 Mbit/s** | **~565 Mbit/s** of payload |
| ... which is a quality of | ~138 Mbit/s | **~410 Mbit/s rANS-equivalent** |

That gives the crossover:

* **Below ~140 Mbit/s** rANS both fits the 12 ms budget and costs 26-32 % fewer
  bits for the same picture. **Use rANS.** There is no argument for Lite here;
  the link is the scarce thing, not the decoder.
* **Above ~140 Mbit/s** rANS is over the entropy budget, and the only way to
  stay inside it is to lower the rate -- which is to say, to refuse the link's
  own headroom. Lite converts that headroom into decode time at a fixed
  exchange rate of **+40 % bits for -75 % Pass A**. **Use Lite.**
* Lite's own ceiling on the same budget is around 565 Mbit/s of payload, about
  **4x** the rate at which rANS runs out. Between 140 and 410 Mbit/s of
  rANS-equivalent quality, Lite is strictly the better tool on this hardware:
  more quality per frame, inside the budget.

The switch is therefore **on the budget, not on the bitrate**: a decoder that
reports its Pass A time lets the encoder pick, and a client whose Pass A is
comfortably inside 12 ms should never be sent a Lite stream.

Two honest limits on this section. The 4.1x was measured on a 7900 XTX and is
assumed to carry to an Adreno; the design reason it should (the chain is gone,
not shortened) is stronger than the measurement, and a Pico K4 run would
replace both. And the 141 Mbit/s identification of the 0.50 symbols/pixel
corpus is an inference from bits per symbol, not a measurement of that corpus's
bitrate.

---

## 6. Recommendation for a Pico profile

**Ship `ENTROPY_LITE` with variant `FIXED` as a negotiated tool, off by
default, and turn it on from the decoder's measured Pass A time.**

1. **`FIXED`, not `RICE`.** `RICE` is worth 1-4 % of the rate at the very top
   of the range and costs per-coefficient parallelism plus a 12-bit length
   field per unit. Keep it in the syntax -- it is two lines of the decoder and
   it pins the variant field to something -- but do not ship it.
2. **Off by default.** The tool is a 40 % rate penalty. Every client that can
   decode rANS inside its budget should.
3. **Negotiate it like any other tool bit**, and let the *client* ask: it is
   the only party that knows its Pass A time. The tools mask is already an
   intersection of what the receiver offered (SYNTAX.md 2.3), which is exactly
   the right shape for this.
4. **Pair it with the foveation ladder, not with a QP drop.** When a Pico is
   over its entropy budget the current answer is to raise QP, which costs
   quality everywhere. Lite costs bits, which the link at that operating point
   has, and costs nothing in the picture. Below the crossover the ladder's
   existing answers are still better.
5. **Measure it on the K4 before believing section 5.** The whole argument
   turns on a 4.1x extrapolated from desktop RADV. Everything else in this
   document is measured; that number is not.

### What was not done

* The RD trellis, table-set choice and mode decision still run on the rANS rate
  model when Lite is selected. That is deliberate -- it makes the bit
  comparison exact, since both coders see the same coefficients -- but it also
  means the Lite numbers here are a **ceiling** on Lite's cost. A rate model
  matched to the Lite syntax (the trellis would stop paying for `LAST`
  positions that Lite prices differently, and would prefer magnitudes inside
  the unit's class) should recover some of the 40 %. **Done in step 10,
  below: it recovers 8 % of the rate.**
* Sign data hiding is forbidden under the tool, which costs the Lite column
  about 0.3 %.
* `nsub_log2 > 5` was not evaluated; it is not in the syntax.
* The Pass A harness's rANS test encoder still cannot code mode units, so the
  Lite-vs-rANS timing corpus carries none. This keeps that comparison
  apples-to-apples but means the mode-unit wavefront is timed only in the
  `--intra` correctness runs, not in the timing table.

## Re-measured on the merged encoder (merge-main, step 9)

The branch's headline was "about +50 % bits". Re-run on the merged encoder,
`vr-mixed-1024-v2` at QP 24, whole clip, default preset:

| format | rANS | LITE/FIXED | LITE/RICE |
|---|---|---|---|
| 4:4:4 | 2 198 918 B | 3 145 207 B (**+43.0 %**) | 3 385 884 B (+54.0 %) |
| 4:2:0 | 2 150 976 B | 2 838 277 B (**+31.9 %**) | 3 106 878 B (+44.4 %) |

4:4:4 lands inside the +40 to +50 % band the merge asked for. 4:2:0 comes in
**below** it, at +31.9 % -- Lite is cheaper on 4:2:0 than the branch claimed,
not more expensive, so it is not a regression. The first hypothesis, that the
merged RD work had enlarged the rANS denominator, does not survive measurement:
re-running 4:2:0 at `--rdoq-effort 1` gives 2 143 500 B rANS against 2 748 784 B
Lite, +28.2 %, which moves the ratio the *wrong* way by 3.7 points. The
difference is the chroma planes' own statistics -- at 4:2:0 the two subsampled
planes are a smaller and much sparser share of the coefficients, and sparse
units are where FIXED's per-class word is closest to what rANS spends -- not
anything the merge did. Quote the two numbers separately; there is no single
"+50 %" that is true of both formats.

## A rate model matched to the Lite syntax (merge-main, step 10)

Everything above priced coefficients with the rANS rate model even when Lite
was selected, so the Lite numbers were a **ceiling**. This step gives the
encoder a rate model that is the Lite syntax itself (`rdoq_unit_lite`,
`lite_unit_bits` in `ref/src/codec.cpp`, `lite_payload_bits` in
`ref/src/entropy_lite.cpp`) and measures what it recovers. Streams produced
with the tool OFF are unchanged: the tools-off check against the frozen base
passes, and the QP 24 default stream is byte-identical to the one the previous
binary wrote (same 2 198 918 B, same MD5).

### Two defects found on the way, both in the merged encoder

The measurement above was taken on streams that **do not decode correctly**.
Two default tools have no representation in the Lite payload and the encoder
went on using them:

| tool | what happened | one frame, 4:4:4, QP 24 |
|---|---|---|
| `XFORM_4X4_SPLIT` (bit 19) | 9.10 has no split-flag field; `lite_encode_units` never read `Unit::split_present`, both decoders take a Lite tile as unsplit | PSNR-Y **25.6 dB** decoded, 42.6 dB in the encoder |
| `INTRA_CFL` (bit 24) | the Lite non-MPM index is 3 bits; the ten-mode chroma alphabet has nine non-MPM values, so index 8 was written as 0 | PSNR-Cb **39.1 dB** decoded against 47.2 dB for rANS |

The fix is the one the tool already applies to `SIGN_HIDE` and
`CUSTOM_TABLES`: the encoder clears both when `ENTROPY_LITE` is selected,
`lite_encode_units` refuses a ten-mode unit rather than truncating it, and
SYNTAX.md 2 and 9.10 now say a bit-30 stream MUST NOT set bits 19 or 24. The
proper fix -- a split bit in section P and a 4-bit index in section B -- is a
syntax change that the Pass A shader would have to follow, and was not made
here. Conformance vectors v76-v80 were regenerated (v81, lossless, is
unchanged); they never set either tool, so the regeneration reflects the rate
model only.

This also means the "+43.0 % / +31.9 %" above were measured on Lite streams
that still carried the split tool's savings without paying for them. On the
decodable configuration the ceiling was higher; see the `rANS model` column.

### What the model is

Per coefficient unit, in bits: one `coded` flag; `last_bits(ncoef) + 3` of
section P (+12 for `RICE`); one significance bit per scan position below
`LAST`; and per nonzero the magnitude field plus a sign, where under `FIXED`
the field width is set by the **largest level in the unit**. Nothing depends
on the previous level, so there is no Markov chain -- but the class couples
every position, so the search runs once per candidate class (up to the one
that admits the largest candidate; every order for `RICE`), each run an
independent choice at every position followed by the choice of `LAST`, and
keeps the cheapest. The two effects the section above named fall out of it:
a zero below `LAST` costs a whole bit, so the trellis pulls `LAST` down
harder than the rANS model ever asked it to; and a level that would push the
unit up a class is charged for every other nonzero's extra bit. The per-block
mode decision prices the mode as 1 or 4 raw bits, and the tile decisions
(inter mode, QP/matrix/transform search) read the exact payload length,
padding included, instead of a table entropy plus lane flush bytes.

What it does not model: the sixteen-unit group flag. An uncoded unit is
charged its H1 bit unconditionally, which is exact whenever its group has any
coded unit and a sixteenth of a bit too much otherwise.

### Measured

`tools/quality/compare.py`, `vr-mixed-1024-v2`, **whole clip (36 frames)**,
QP 8/12/16/20/24, PSNR-Y, cubic BD-rate. Six encoders per format:

* `rANS`: the shipped default (split4x4 on, CfL on).
* `rANS-min`: rANS with split4x4 and CfL off -- the same coefficient syntax
  Lite can carry, so the entropy-only opponent.
* `FIXED`/`RICE`, `rANS model`: the previous binary, split4x4 and CfL forced
  off so the streams decode.
* `FIXED`/`RICE`, `Lite model`: this change.

**BD-rate against `rANS` (the shipped default):**

| format | rANS-min | FIXED, rANS model | FIXED, Lite model | RICE, rANS model | RICE, Lite model |
|---|---|---|---|---|---|
| 4:4:4 | +15.7 % | +63.8 % | **+50.4 %** | +70.3 % | +49.6 % |
| 4:2:0 | +11.1 % | +52.4 % | **+39.5 %** | +59.7 % | +40.9 % |

**BD-rate against `rANS-min` (the entropy-only gap), and what the model
recovers of it:**

| format | FIXED, rANS model | FIXED, Lite model | recovered | RICE, rANS model | RICE, Lite model |
|---|---|---|---|---|---|
| 4:4:4 | +41.9 % | **+30.8 %** | 26 % of the gap | +48.0 % | +29.9 % |
| 4:2:0 | +37.2 % | **+25.8 %** | 31 % of the gap | +43.8 % | +26.5 % |

**The model against the ceiling, same coder** (BD-rate, Lite model vs rANS
model; BD-PSNR in brackets):

| format | FIXED | RICE |
|---|---|---|
| 4:4:4 | **-7.9 %** (+0.86 dB) | -12.1 % (+1.48 dB) |
| 4:2:0 | **-8.3 %** (+0.99 dB) | -12.0 % (+1.66 dB) |

The weighted (6Y+Cb+Cr)/8 PSNR gives the same figures to within half a point.

**QP 24, whole clip, bytes** -- quoted because the section above quoted
bytes, and to show why bytes at one QP are the wrong figure here:

| format | rANS | FIXED, rANS model | FIXED, Lite model |
|---|---|---|---|
| 4:4:4 | 2 198 918 B, 42.55 dB | 3 364 559 B (+53.0 %), 42.09 dB | 2 913 565 B (+32.5 %), **41.30 dB** |
| 4:2:0 | 2 150 976 B, 42.54 dB | 3 067 536 B (+42.6 %), 42.14 dB | 2 628 127 B (+22.2 %), **41.30 dB** |

The Lite model lands the same QP at 20 points fewer bytes and 0.8 dB lower
PSNR: a rate model that charges a whole bit per zero and a class step per
outlier moves the trellis down the curve at a fixed lambda, and most of the
byte saving at equal QP is that move. The BD-rate figures net it out; the
honest recovery is the -8 %, not the -20 points.

### What this says

* The ceiling was real and about a fifth of it was slack: 8 % of the rate for
  the same picture, both formats, `FIXED`. The remaining +50 % (4:4:4) and
  +39 % (4:2:0) against the shipped default is the syntax -- one bit per
  significance flag, one width per unit -- plus the two tools Lite cannot
  carry, which are worth 16 and 11 points of it respectively.
* `RICE` gains more from the model (12 %) because its parameter is a code
  order the trellis can now actually search, and the two variants end within
  a point of each other. That does not change section 6: `RICE` still costs
  per-coefficient addressability for nothing at these rates.
* Against the entropy-only opponent, Lite `FIXED` is now +31 % at 4:4:4 and
  +26 % at 4:2:0. Section 5's crossover was computed at +40 %; it moves down,
  but the "switch on the budget, not the bitrate" recommendation stands.

### Where the bits went, then and now

`NXVC_LITE_STATS=1`, `FIXED`, 4:4:4, QP 24, 2 frames, both on the decodable
configuration:

| section | rANS model | Lite model |
|---|---|---|
| coded-unit map | 104.0 k (7.3 %) | 95.5 k (7.8 %) |
| `LAST` + class | 195.0 k (13.7 %) | 154.7 k (12.6 %) |
| significance | 190.9 k (13.4 %) | 172.1 k (14.0 %) |
| intra modes | 245.4 k (17.2 %) | 267.3 k (21.8 %) |
| magnitudes | 540.9 k (37.9 %) | 409.7 k (33.4 %) |
| signs | 137.0 k (9.6 %) | 114.3 k (9.3 %) |
| **total** | **1 427.7 k** | **1 227.7 k** |

The magnitude section, where the class effect lives, gives up a quarter of
its bits; `LAST` + class and the map fall with the number of coded units;
significance falls less than the coded-unit count does because the units that
stay coded are the dense ones. Intra modes go **up**: with coefficients dearer
the block-level decision buys prediction instead, and a non-MPM mode is 4 raw
bits under Lite where the rANS context sold it for less. That is the trade the
model is supposed to make, and the BD-rate says it is the right one.

### Is the lambda still right?

The trellis lambda (`kLambdaScale = 0.22`) was fitted on the rANS rate model.
Under a model that charges more per coefficient, the same lambda lands lower
on the curve (the QP 24 table above), which raises the question of whether a
different scale would land better. Three scales, first 12 frames, `FIXED`,
BD-rate against the rANS-model encoder on the same 12 frames:

| scale | 4:4:4 | 4:2:0 |
|---|---|---|
| 0.15 | -7.9 % | -8.6 % |
| **0.22** (default) | **-8.0 %** | -8.2 % |
| 0.30 | -7.3 % | -7.4 % |

Flat to within a point, with the default at or next to the best: the gain is
the model, not a lambda that happened to suit it, and no Lite-specific lambda
is introduced.
