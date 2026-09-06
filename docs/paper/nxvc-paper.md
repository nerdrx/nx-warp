# nxvc: a pose-warped tile codec for VR streaming, and what it actually measures

**Skeleton, draft 0. 2026-09-06.** Successor to
[docs/PAPER.md](../PAPER.md) (design paper, draft 1, 2026-09-04).

---

## How to read this document, and what it is not

This is a **skeleton**, not a finished paper. Every section below states what it
will argue and carries the measurements it will argue from, each with a source:
an ADR, a figure in [GALLERY.md](../GALLERY.md), a report under
`tools/quality/reports/`, or a named log. Prose that has not been written yet is
marked `[TO WRITE]`. Numbers that do not exist yet are marked **UNMEASURED** and
are never given a value.

The relationship to the design paper matters, because they make different kinds
of claim:

| | [docs/PAPER.md](../PAPER.md) | this document |
|---|---|---|
| Dated | 2026-09-04 | 2026-09-06, open |
| Claims | what the design *should* do | what the implementation *does* |
| Numbers | design estimates, labelled as such | measured, with a source each |
| Status of its errors | [ERRATA.md](../ERRATA.md), text left as drafted | corrected in place |

The design paper is left as written. Where it and a measurement disagree, the
measurement wins and [ERRATA.md](../ERRATA.md) already records twenty-odd such
disagreements found during implementation.

**Three rules this paper holds itself to.**

1. **Every number carries its source.** File, figure number, or report.
2. **Unmeasured is written as unmeasured.** No estimate is ever promoted to a
   result by being quoted without its label. The design paper's estimates may be
   cited *as estimates* and must be marked.
3. **The negatives get the same space as the positives.** Section 6 is the
   longest section in this outline on purpose.

---

## Abstract `[TO WRITE]`

Must contain, and nothing it cannot source:

- The thesis: a VR streaming codec should be built on what the renderer knows —
  head pose, stereo redundancy, lens falloff, and the display's willingness to
  show a stale tile — and on the constraint that every stage runs as one GPU
  workgroup per tile with no cross-tile state.
- The atlas result, which is the strongest measured claim in the tree: a
  per-tile reference atlas plus a per-frame mode switch matches or beats a
  whole-picture reference model at every head velocity tested without being told
  which regime it is in (§4, Figure 1).
- The honest counterweight, stated in the abstract and not buried: against
  `x264`/`x265` on the project's own gate material the codec **fails every
  compression gate it has** (§7).
- The scope limit: **nxvc has no end-to-end device measurement.** The device
  numbers in §1.2 are the *baseline being replaced*, not this codec's, and the
  per-eye decode figures in §8.1 are point measurements whose absolutes are
  inflated ~1.57x (§8.2) and which are therefore quotable only as ratios.

The one-sentence version, if the abstract needs one: *sixty per cent of the
decoder's Pass B on a Pico 4 is spent reconstructing tiles that are identical to
what is already on screen; this paper is about a reference model that does not
reconstruct them, and about the six ways of exploiting that which were measured
and did not work.*

---

## 1. The problem `[TO WRITE]`

### 1.1 Wi-Fi VR at panel rate on a mobile GPU

The argument: general-purpose codecs are built for storage and broadcast —
whole-frame slices, reference lists, serial entropy coding, fixed-function
hardware whose latency and session limits are not ours to change. VR streaming
has structure they cannot see. Source for the framing: PAPER.md abstract and
§5.6.

The durable form of the claim, after the 2026-09 industry scan: **the hardware
decoder has the wrong interface, not too little throughput**
([ERRATA.md](../ERRATA.md), row "5.6 / 5.8", and
[RESEARCH-INDUSTRY.md](../RESEARCH-INDUSTRY.md)). This is the version to defend,
because `VK_KHR_video_encode_quantization_map` and
`VK_KHR_video_encode_intra_refresh` now make a foveated, IDR-free hardware HEVC
streamer buildable without this codec.

### 1.2 The measured operating point today

One 90-second capture on a **Pico 4**, 1088x1088 per eye as one stereo frame,
QP 22, Wi-Fi 5 GHz at RSSI -52, nothing else running on the device; means over
46 two-second report windows. This is the strongest measured statement of the
problem the codec exists to solve, and every number in it has a raw log behind
it.

| | ms | what it is |
|---|---|---|
| wall | **22.78** | the frame |
| nxvc GPU | **16.69** | the decode kernels: Pass A 3.0, Pass B 13.7 |
| queue wait | 5.05 | fence wait minus both GPU durations |
| submit | 0.60 | handing the unit to nxvc |
| staging copy | 0.33 | into the pool image |

The server paces to **38.5 fps** against a 90 Hz panel, and the displayed pose
is **92.1 ms old on average** (worst 109.0 in that window, 131.8 and 132.1 in
two others).

**The finding, and it is the paper's opening argument.** The client's own
reprojection pass and the decode share one Adreno hardware ring:

```
render GPU load : 6.37 ms x 43.64/s = 278 ms/s
decode GPU load : 16.69 ms x 38.5/s = 643 ms/s
total           :               921 ms/s  = 92 % of the GPU
```

At 92 % duty a submission that arrives while the other consumer is running
waits for it, and the wall is the queue, not the network — holes 0, refused 0,
reassembly 0.020 ms a frame.

**And where that 13.7 ms of Pass B goes** is the single number this codec's
atlas model exists to delete:

```
passB 13.7 ms = warp 0.10 + skip 8.25 + coded 5.41 + dir 0.00 + other 0.00
```

**Sixty per cent of Pass B, and a third of the entire GPU budget of the device,
is `WARP_SKIP` tiles running the normative integer pose warp** — 289 skipped
tiles reconstructed every frame so that they can look exactly like what was
already on screen. §4 is the proposal to stop doing that, and §8.1 is the
independent per-eye measurement of the same waste.

> **Attribution, and it must survive into the final text.** These figures are
> **not nxvc's**. They are the shipping NX Warp client on the WiVRn side,
> measured in a different repository — `docs/CLIENT-DECODE-WALL.md` on branch
> `nx-warp-atlas-wiring` (worktree `nx-scratch/wt-atlas-wiring`), raw capture
> `nx-scratch/live/cap76.log` at `a779904b`. Neither that document nor the log
> is in this repository, and a paper that presents them as this codec's results
> would be misrepresenting them. They are the **baseline**.
>
> Two of the four are weaker than they look and must be quoted carefully. The
> **92 ms pose age appears in no prose document at all** — its only source is
> the raw log, so either the log becomes a citable artefact or the number is
> re-measured. And **16.69 ms is per stereo frame, both eyes**, not per eye;
> "16.7 ms per eye pair" is right only if "pair" means the whole frame.

### 1.3 What the codec is allowed to assume

Tiles are independent; the encoder knows the pose that produced each frame and
the pose the next will be rendered at; the two eyes share nearly all content;
the display would rather show a stale tile than wait. Sources: PAPER.md
"Design principles" 1-7, ADR-0002 (64x64 tiles), ADR-0006 (no IDR).

---

## 2. The codec `[TO WRITE]`

### 2.1 Tiles, transform, quantisation

**64x64 luma tiles**, fixed per stream, the same grid on every native layer; a
1-bit `tile_size` header field reserves 32x32 for a future Lite profile and v1
encoders always emit 64 (PAPER.md 1.1; ADR-0002). At 2160x2160 an eye that is
1156 tiles against 32x32's 4624, and about 10 % fixed overhead against 20 %.

**8x8 integer DCT** for all planes plus a per-tile transform-skip mode, using
the Loeffler-Ligtenberg-Moschytz (1989, expired) factorisation with the
project's own 9-bit constants and 16-bit intermediates (PAPER.md 1.4). The
paper's stated shift split is wrong and [ERRATA.md](../ERRATA.md) corrects it:
the normative chain is **7 then 13** (total 20, unit gain), and any split
summing to 20 is correct.

**Quantisation**: `step = 2^(QP/6)`, QP 0..63, so QP 0 is step 1 (lossless with
transform skip) and QP 51 is step 362. Dead-zone on the encoder,
`q = sign(c) * floor((|c| + f*step) / step)`, `f = 1/3` INTRA, `1/6` INTER
(PAPER.md 1.5). Per-tile `res_level` and a 6-bit per-tile QP delta are normative
design, and per-tile QP is **untested, not rejected** — see §6.4.

### 2.2 Prediction: the pose warp and the five tile modes

The five modes are `WARP_SKIP`, `STATIC_MV`, `WARP_MV`, `INTRA`, `STEREO`
(SYNTAX.md:552, normative table 13.1 at :2629). `STATIC_MV` exists for
head-locked content — menus, HUDs, laser pointers, a transport overlay — *where
the warp is exactly wrong and the identity predictor is exactly right*, which is
a nice statement of why a pose-warped codec still needs a zero vector.

> **A naming trap worth one sentence in the paper.** `NEAR_SKIP` is **not** a
> tile mode. It is tool bit 28 gating a **tile-row header** structure — a
> nine-byte block-mean correction applied to a skipped tile
> (SYNTAX.md:163, TOOLBITS.md:208, and 13.9). It appears as a *candidate* in the
> reference's float decision and as a column in every mode histogram in
> ENCODER-DECISION.md §7, where it is a **subset of skip**, not a sixth class.

### 2.3 Entropy coding

Default: **interleaved rANS, static per-frame tables**, 1 to 32 substreams per
tile chosen by the encoder from the symbol count; 32-bit state, `L = 2^16`,
`M = 2^10`, 12 contexts x 16 symbols (PAPER.md 1.6; ADR-0003 fixes eight lanes).

**`ENTROPY_LITE` is tool bit 30**, a table-free fully parallel entropy tool
(SYNTAX.md:165, normative 9.10 at :2389; two variants `FIXED` and `RICE` via a
reinterpreted `table_set`). It was developed on bit **24** and renumbered
(TOOLBITS.md:93, commit `ad02dae`); bit 24 is `INTRA_CFL`. Any paper text that
cites the development-era number will be wrong.

Its trade, both halves, because only quoting one of them would be advocacy:

- **It costs rate**: **+43.0 %** of the payload at 4:4:4 and **+31.9 %** at
  4:2:0, QP 24, over the full clip (TOOLBITS.md:372-386).
- **It buys parallelism**: **Pass A 138.5 ms -> 18.4 ms, 7.5x** (same source).

> **An unresolved inconsistency the paper must settle before publication.** That
> 7.5x is recorded as a **Pico 4 measurement** (TOOLBITS.md; MERGE-REPORT.md:394
> re-states it as "a Pico 4 measurement, not a merge measurement"), while
> [ROADMAP.md](../../ROADMAP.md) and [docs/README.md](../README.md) both state
> that **nothing has been measured on a headset**. Both cannot be true. Until it
> is resolved, this paper cites the 7.5x **as disputed** and does not use it to
> support any claim. See §10.3.

A further Lite result, **-7.9 % (4:4:4) / -8.3 % (4:2:0) BD-rate**, exists on
branch `merge-main` (`1d211a1`) and is **not on `atlas`**. It is also not an
absolute win: it is the gain from pricing the Lite syntax with a matched rate
model instead of the rANS model, i.e. a correction to a mis-priced baseline.
Cite it that way or not at all.

### 2.4 Effort levels and the integer requantiser

Source: `vk/encoder/README.md:712` "The effort levels, measured" — **not**
ADR-0028, which contains no such number.

Level 0 is the plain dead-zone quantiser; **level 1 is the integer
requantiser**, which drops a level of +-1 whose squared error is worth less than
the bits it saves. It is byte-identical on GPU, the CPU model and lavapipe, and
costs 64 extra integer compares per block — inside the run-to-run spread of
level 0. Measured BD-rate against level 0 on `pan8` (RADV RX 7900 XTX, 8 frames,
QP 22/26/30/34/40): **-1.36 % / -3.53 %** (rANS / Lite) at 289 tiles and
**-1.54 % / -3.60 %** at 578 tiles. That is the "-1.4 to -3.6 %" figure and the
README recommends level 1 "for any budget".

> **The recommendation has been withdrawn, and the paper must carry the
> withdrawal.** On branch `effort-vrroom` (`a010fd2`, not on `atlas`) the same
> ladder was re-run on the rendered corpus, and **effort 1 is negative on
> exactly one fixture** — `pan8`, the one the low-poly work already called
> "unusually kind". On the other five it *costs* bytes:
>
> | clip | effort 1, rANS / Lite |
> |---|---|
> | `pan8` | **-2.41 / -4.38** |
> | `still` | +1.09 / +0.12 |
> | `rest` | +3.19 / +2.99 |
> | `mid` | +2.35 / +1.15 |
> | `objmotion` | +2.43 / +2.73 |
> | `fast` | +2.65 / +2.59 |
>
> Both readings reproduce; neither on its own is the answer. This is the
> cleanest example in the tree of a **corpus-dependent result presented as a
> general one**, and it deserves a paragraph in its own right — see §6.8 and
> GALLERY Figure 2, which independently finds the ladder worth 0.03 dB on vrroom.

**Why there is no level 2** (`vk/encoder/README.md:769`): every candidate that
pays cannot cross to a GPU. The full trellis is **-6.64 %** and does not cross;
`--qp-search 2` is -6.79 % and needs a rate model on the device; `--int-decision
off` is -8.63 % and is the thing ADR-0028 exists to avoid. The two candidates
that *do* cross — a wider MV sweep and a retuned dead zone across 12 profiles —
are worth **+0.00 %** and **-0.26 %**: they cross, and they are worth nothing.
The blocker is that `log2` is not the same function on host libm and device, and
a trellis over the scan is a serial dependency.

The reference-side **exact-integer trellis** (`--int-trellis`, within 0.02 % of
the double trellis, -3.2 % rANS / -4.9 % Lite against effort 1) landed on
`main` via `046ab4b` and is **not on `atlas`**.

### 2.5 What the format deliberately does not have

**No deblocking filter and no loop filter** ([SYNTAX.md](../SYNTAX.md):2513).
This is load-bearing for §6.9: a tile-boundary step from intra quantisation is
structural and permanent until the tile is re-coded, and there is no in-loop
mechanism to hide it. The consequence is measured in Figures 12-13.

---

## 3. The reference encoder's tile decision `[TO WRITE]`

Largely written already: [ENCODER-DECISION.md](../ENCODER-DECISION.md) is the
long form and this section is its digest. What must survive the compression:

- The decision in evaluation order, as integers: hard intra cap (180 frames,
  absolute), drift gate, displacement bound, skip early-out, the
  `STATIC_MV`-then-`WARP_MV` search order, the skip charged
  `(kSkipPersist - 1) x excess` with `kSkipPersist = 4`.
- Why `kSkipPersist` exists: without the excess term the decision is locally
  right and globally wrong — measured, it gave up **2.7 dB (36.62 -> 33.91)** to
  save 15 % of the rate (ENCODER-DECISION.md §3, and the comment it cites in
  `ref/src/codec_impl.inc`).
- The float/integer split and why `D + lambda*R` cannot be integerised: `R` is a
  `log2` of a symbol histogram (ADR-0028).
- The three gates that *can* be integerised, exactly, all clearing to
  `x * 786432 <= q*q*k*npix` with **zero** measured difference
  (ENCODER-DECISION.md §5).
- The trap worth a sentence in any paper about portable encoders:
  `nxvc_config_default` leaves `int_coded_vectors` at 0, and 0 means "search
  nothing" (ENCODER-DECISION.md §6).

---

## 4. The atlas model and the per-frame mode switch `[TO WRITE]`

Source: [ADR-0029](../adr/0029-atlas-reference.md), normative text
[SYNTAX.md](../SYNTAX.md) 13.12.

### 4.1 The model

The reference is not a picture but a **per-tile atlas**: each tile position
holds its own entry with its own composed warp `C`, and display is one warp step
from it. Two tiles in the same frame therefore predict through *different*
matrices, which is the single most important difference from a picture-model
encoder (13.12.4; ENCODER-DECISION.md §1).

### 4.2 The mode switch, and why it is a per-frame MODE and not a profile

A frame is `ATLAS` or `PICTURE`, chosen per frame from the atlas's own
accumulated corner displacement against a threshold `D`
(13.12.11; ADR-0029 commit `c0408f4`, "the atlas is a per-frame MODE, not a
profile"). The distinction is the point: a profile would make it a stream-level
bet, and §4.3 shows the right answer changes with head velocity *within* a
session.

The normative model is SYNTAX.md **13.12, eleven subsections**, and the paper
needs a map of it rather than a re-statement:

| | |
|---|---|
| 13.12.1 | the two per-stream objects: atlas pixels, and per-tile entry state |
| 13.12.2 | composition — `P = C . H`, right-multiplying `warp_ext()`'s `H_N` one step at a time |
| 13.12.3 | the frame process; step 1 **Advance** composes `C` for every valid entry |
| 13.12.4 | what a coded tile reads: its **own** position's entry, after Advance |
| 13.12.5 | display, **not normative** — one warp step, any filter permitted |
| 13.12.6 | loss: **there is no concealment process under `ATLAS`** |
| 13.12.7 | `NEAR_SKIP` under `ATLAS` — the one case where a skipped tile writes to the atlas |
| 13.12.8 | neighbour-aware gather (bit 33), with a two-step rule to stay non-circular |
| 13.12.9 | the base layer as a patch source |
| 13.12.10 | rebase (bit 34): replace pixels with what display would show, `C := I` |
| 13.12.11 | the two frame modes (frame flags bit 5) |

Two of these carry most of the paper's argument. **13.12.5 being non-normative**
is what lets the decoder spend nothing on a skipped tile — the still clip's zero
decoder warps in §4.4 is that clause cashed in. And **13.12.6** — no concealment
process at all — is the strongest form of ADR-0006's no-IDR claim: a client that
missed a tile does nothing, and the entry keeps composing.

### 4.3 The headline result

**Figure 1** — *Rate-distortion on the vrroom corpus* (GALLERY.md). At QP 26,
stereo, `--eyes 2`, D=8:

| fixture | all-ATLAS | all-PICTURE | D=8 | winner |
|---|---|---|---|---|
| **still** 0.043 deg/s | **41.28 / 150 B** | 41.25 / 159 B | 41.28 / 150 B (0 % PIC) | tie |
| rest 2.7 deg/s | **38.94 / 1643 B** | 38.02 / 4208 B | 38.92 / 1777 B (6.5 % PIC) | ATLAS, +0.92 dB at 2.6x fewer bytes |
| mid 26.2 deg/s | 36.62 / 9780 B | **38.03 / 7031 B** | 37.71 / 7366 B (48.4 % PIC) | PICTURE, +1.41 dB at 28 % fewer bytes |
| fast 99.1 deg/s | 32.65 / 13843 B | **38.21 / 6566 B** | 38.21 / 6566 B (100 % PIC) | PICTURE, +5.56 dB at half the bytes |
| **objmotion** | **38.79 / 3510 B** | 37.97 / 5668 B | 38.74 / 3693 B (6.5 % PIC) | ATLAS, +0.82 dB at 38 % fewer bytes |

Source: ADR-0029 "Re-run on rendered content", GALLERY Figure 1.

**The claim to make is the mode switch, not the atlas.** At every velocity
`D = 8` lands on whichever of the two models wins there without being told
which: 6.5 % PICTURE frames at rest, 48.4 % at mid, 100 % at fast.

### 4.4 The still floor

**Figures 10-11** — *The seated trajectories, and the true rest floor*.

`rest` was named for a head at rest and is not one: 2.678 deg/s, dragging every
atlas entry's corners **0.97 samples in one frame and 3.83 over four**, so a
whole-sample identity is never available in it. `still` — a seated head, 0.030
deg postural drift at 0.11 Hz plus 0.001 deg tremor at 8 Hz — measures
**0.043 deg/s** with corner displacement pinned at **0.016 samples** for the
whole clip.

On it the atlas does exactly what it exists for: **100 % skip, zero decoder
warps, 150 B/frame at 41.28 dB** for a stereo 1088x1088 pair (125 B/frame on the
integer decision, which has no `NEAR_SKIP` to spend). At QP 34 and 40 it
converges to the **structural floor of 119 B/frame** — 40 B frame header, 72 B
`warp_ext()`, 5 B `row_present` bitmap, 2 B slack; **85.7 kbit/s at 90 Hz**.

The honest reading, which belongs in the paper and not only in the ADR: **at
true stillness the atlas and the picture model tie** (41.28 against 41.25). The
atlas's saving is the skip warp, and when nothing moves the picture model skips
everything too. The advantage needs *some* motion, and `rest` at 2.7 deg/s is
where it appears.

`objmotion-still` prices independent object motion on its own at about
**2650 B/frame** over the still floor.

Sources: ADR-0029, GALLERY Figures 10-11, `nx-scratch/atlasprice/vt-still.log`,
`vt-objmotion-still.log`, ENCODER-DECISION.md §7.

---

## 5. The low-poly planar mode `[TO WRITE]`

Source: [LOWPOLY-MODE.md](../LOWPOLY-MODE.md).

> **Branch note.** On `atlas` this is a **proposal with a measured CPU
> prototype**: "Nothing here is implemented in the codec, and no syntax has been
> allocated" (LOWPOLY-MODE.md:3). The implementation — tool bit 35, mode 5,
> SYNTAX 13.13, and the level 1 / level 2 encoder decision — is on `main` via
> `edefcdb`. The final paper should be written against `main`.

**The interesting claim is look versus PSNR**, and it is the one place in the
project where the metric and the eye are documented as disagreeing. At 35-45
bytes a tile the piecewise-planar mode is level with or ahead of the transform
codec; by 90 bytes it is a decibel and a half behind and falling
(LOWPOLY-MODE.md:172). The transform codec at those rates is "a soft,
colour-smeared blur"; the planar mode at the same bytes keeps every boundary,
and fails differently — boundaries quantised to the sub-block grid come out as a
**staircase**. That is the ADR-0013 ladder (*blur, never block*) meeting a case
where blur is what the metric rewards.

**The headline number did not reproduce, and the paper must lead with that.**
Commit `ea5317c`: "The proposal's +1.6 dB at 45 B/tile DOES NOT REPRODUCE. Its
transform baseline is about 15 dB adrift." Re-measured on `pan8`/`pan8s`, intra,
four frames, at equal bytes the transform is **5 to 12 dB ahead on luma** at
every configuration and the planar mode is **2 to 6 dB ahead on chroma** — the
reverse of what the proposal found.

So the mode ships **off**, as a decision in two levels:

| level | rule | measured |
|---|---|---|
| `planar = 1` | take it only where it is both cheaper **and** no worse | **neutral**: within 0.015 dB and 2.4 % of the tool being off at QP 34/40/46 on both fixtures, chosen on 2-17 % of tiles |
| `planar = 2` | take it wherever it is cheaper | **the look as a setting**, and it costs **2.1 to 4.3 dB** |

A mode whose honest description is "level 2 costs you 2 to 4 dB and you may want
it anyway" is the most interesting thing in this section and the hardest to
write, because it cannot be defended with PSNR and the project has no adopted
perceptual metric that captures it (§8).

> **Not from this section.** The seam ratios 1.14 / 1.53 / 1.81 belong to
> ADR-0029's coarse-refresh result (§6.2), not to the planar mode.
> LOWPOLY-MODE.md contains no seam-ratio measurement.

---

## 6. The measured negatives `[TO WRITE]`

The longest section, on purpose. Each entry gets its idea in one sentence, the
number that killed it, and its figure.

### 6.1 Two-level refresh: coarse first, refine later

Land a stale tile at `res_level 1`, then send a refinement later; needs no
syntax, being only a per-frame `res_map`. **Dominated at every quantiser on both
fixtures**: at equal rate, **-1.85 to -1.99 dB at 25.2 deg/s** for `R=1`
(-4.19 to -5.27 dB at `R=2`); at equal QP it costs **20 % more bytes for 1.9 dB
less**. The configuration that came closest did so with **0 refinements out of
1179 landings** — i.e. its winning half was not the two-level idea at all but
single-level coarse, which is §6.2. Source: ADR-0029:916-971. No figure.

### 6.2 Single-level coarse landing (`--atlas-coarse-disp`)

**Figures 7-9** — *Coarse landings are blocky, not soft*.

Land a stale tile at `res_level 1` and never refine it. **There is no threshold
`T` that gains at fast turn**: every `T` that engages loses (-1.33 to -3.17 dB
at `T=2`, -0.95 to -1.96 dB at `T=8`) and the ones that do not lose are the ones
that have stopped firing. The decoder-side saving is priced at **0.22 ms for
3.17 dB**, about 0.2 dB per 1 % of Pass B.

The visual number is the one that matters and is why this belongs in a paper
about a *degradation ladder*: seam ratio **1.14 -> 1.53 -> 1.81** against a
source of 0.93, while high-frequency energy **falls** 3.04 -> 2.76 -> 2.61. Soft
interiors separated by hard tile-aligned edges is the definition of blocking,
and ADR-0013 commits this codec to *blur, never block*. Source:
ADR-0029:973-1044; GALLERY Figures 7-9.

### 6.3 The ranked refresh scheduler (`--atlas-sched-bytes`)

Score every tile by `gradient x displacement x (1 + age) + measured drift`,
rank, and code the top tiles until a frame byte budget is spent. At the best
point it reaches per rate: **-2.28 dB on near-still**, **+0.01 dB at mid**,
**-0.34 dB at fast turn**. The criterion was *wins everywhere, or is neutral
where it does not*, and it is neither. Raising `D` with it on costs **-7.9 to
-8.9 dB at `D=12`**. Seam ratio is worse than baseline at every fixture.

The structural finding is the quotable one, and it generalises past this
codec: **at a byte budget, move the quantiser, not the tile set.** Squared error
is additive over tiles, so concentrating the budget is the wrong direction.

A second, more practical finding sits inside it: the drift gate and the byte
budget disagree about precedence, and the gate wins — it **coded 31 tiles in a
frame the scheduler had allowed 4**, producing a 6876-byte frame. A rate
controller layered over a correctness gate does not control the rate. Source:
ADR-0029:1046-1154. No figure.

### 6.4 The spatial hybrid (hardware HEVC periphery, compute fovea inset)

Not an atlas result but the same shape of negative, and the one that decided the
project's foveation strategy. Against an `x265` + delta-QP foveation-map anchor,
scored in JOD by FovVideoVDP on a Pico 4 display model, the best spatial
configuration reaches **9.453 / 9.674 / 9.809** at 40 / 80 / 150 Mbit against the
anchor's **9.886 / 9.937 / 9.964** — it loses at every rate. The periphery loses
**8 to 9 dB** the anchor does not, and the 1344x1088 inset costs **9.8 ms**
Adreno 650 luma-only (**14.6 ms** with chroma) against a 5 ms gate. Source:
ADR-0027:13-38. No figure.

> **Correction to the brief.** There is **no measured-and-rejected per-tile QP
> or delta-QP experiment** anywhere in the tree. The delta-QP map is the
> *anchor that wins* in ADR-0027, and per-tile QP delta (6 bits) is normative
> design (PAPER.md 1.5). ADR-0027 names foveation inside the codec — per-tile
> `res_level`, QP, temporal ladder — as "the largest unexploited lever and the
> one a delta-QP map cannot copy": **untested, not rejected.** It belongs in
> §10, not here.

### 6.5 Alternate-eye coding

Refresh one eye per frame and synthesise the other by making every tile of the
off eye skip — expressible as a per-frame skip map with the codec untouched.
Mean PSNR moves only 1.2 dB and hides everything: the synthesised eye's **worst
64x64 tile falls to 11.5 dB at fast turn and 15.4 dB at mid**, against 28.1 and
22.0 baseline. A 12 to 17 dB collapse in one tile of one eye while the other eye
is correct is the worst possible shape of error, because the failure class is
binocular rivalry — **the viewer does not average the eyes, they fight**.
Rejected in general; viable only at rest, where it is +0.19 dB at 13 % fewer
bytes. Source: ADR-0029:1340-1370. No figure.

**This is the entry that most needs a figure**, and none exists. See §10.3.

### 6.6 Disparity synthesis

Synthesise the right eye from the true left plus an integer horizontal disparity
field, measured as a *ceiling* (the disparity searched against the true right
eye, so no estimator can beat it). At 8x8 blocks the field alone costs
**6655-6927 B/frame**, which is **4.2x the entire stereo frame at rest**
(1643 B at QP 26) — to save one eye it spends more than both eyes cost. At 64x64
the field is nearly free (77-90 B/frame) but the synthesised eye lands at
**30.5-31.9 dB where simply coding it reaches 38.8 dB**, and the ~10 % of
failing tiles are the near-geometry and occlusion ones the viewer is looking at.
Source: ADR-0029:1372-1416. No figure.

### 6.7 The atlas's own repair attempts

Structurally different from the above — these are fixes for a defect rather than
standalone proposals — but all four measured negative: neighbour-aware gather,
displacement-bounded skip, rolling rebase, base-layer refresh. At equal rate at
fast turn the best of them is still **7.8 dB behind the picture model**.
Re-posing the whole atlas every frame recovers 7.31 dB of the 9.89 dB deficit
and costs **9.83 ms per eye per frame** — "the entire 8.8 ms the atlas exists to
save, plus interest". This is what made the per-frame mode switch (§4.2) the
answer instead: stop repairing the atlas and leave it for a frame. Source:
ADR-0029:667-816. No figure.

### 6.8 The effort ladder and the planar intra mode

**Figure 2** — *The effort ladder and the planar mode do not pay*. `int_rdoq` 0,
`int_rdoq` 1 and the full trellis land within **0.03 dB** of each other on all
four vrroom fixtures. `--intra-dir layer` ("planar prefer") costs **0.12 to
0.27 dB** *and* more bytes on every fixture — a dominance result in the wrong
direction. Source: GALLERY Figure 2.

> **Two items in the brief could not be sourced and are therefore not written.**
> **"Compositor-pose"**: no measured negative about taking the pose from the
> compositor exists in the tree. The nearest adjacent material is ADR-0029's
> accepted cheat 2, *pose late-latching*, and a patent-claim reading in
> FTO-BRIEF.md — neither is a rejection. **"V2"**: every `v2` in `docs/` is
> either a v1 exclusion deferred behind a v2 tool bit (four MVs per tile,
> `XFORM_WAVELET`, directional intra, the rANS lane-count field) or a fixture /
> wire-format version name. There is no version-2 proposal that was measured and
> rejected. What *does* exist under that name is §7's corpus regeneration, where
> the material moved by 12 dB and every verdict stayed put.

### 6.9 The still clip's seams

**Figures 12-13** — *The still clip's seams are frame 0's, and nothing adds to
them*.

The `still` row's seam ratio (3.27 at QP 26, 6.88 at QP 40) reads like the atlas
letting the 64-sample grid surface on a scene that is not moving. Measured per
frame it is not:

| | frame 0 | frame 1 | frame 31 | min | max |
|---|---|---|---|---|---|
| QP 26 | 3.262 | 3.262 | 3.270 | 3.262 | 3.277 |
| QP 40 | 6.925 | 6.925 | **6.820** | 6.820 | 6.925 |

Flat, and at QP 40 falling. `3.27 -> 6.88` is the **QP axis, not the time
axis**. The whole value is present at frame 0, the all-intra frame, before any
warp and before an atlas entry exists.

Three mechanisms were proposed and all three are measurably absent:

| proposed mechanism | test | result |
|---|---|---|
| accumulating per-frame resampling | per-frame trace, 32 frames | flat at QP 26, **-1.5 %** at QP 40 |
| per-tile `C` diverging by rounding between neighbours | synthetic exactly-zero-motion clip | **3.262 for all 32 frames**, identical to the picture model |
| tile-local corner rounding | min/max of the trace | real, worth **+0.5 %** — the atlas's entire contribution |

The control that settles it: with `--atlas off`, the plain picture codec with no
atlas at any point, the trace is **3.262 flat and 6.925 flat**, the same numbers
to three decimals.

It is the format (§2.5): no deblocking, no loop filter, so an intra
tile-boundary step is permanent until the tile is re-coded. The moving clips
hide theirs *by* re-coding — on `rest` the boundary gradient collapses
**3.452 -> 1.756** across the clip while the interior gradient barely moves
(1.166 -> 1.044), and the atlas erases seams **harder** than the picture model
(1.682 against 2.508 at frame 31). **The atlas is the seam-reducing mechanism
here, not the seam-creating one.**

The refresh sweep that closes it, and the reason the paper says *no change*:

| `--intra-period` | QP 26 B/f | QP 26 seam f31 | QP 40 B/f | QP 40 seam f31 |
|---|---|---|---|---|
| 4 | 9648 (**7.2x**) | 3.255 | 3790 (**6.6x**) | 6.978 **worse** |
| 16 | 2537 | 3.270 | 1035 | 6.856 worse |
| 180 (default) | **1341** | **3.270** | **575** | **6.820** |

Re-coding a static tile from an unchanged source at an unchanged QP reproduces
the *same* reconstruction. Seven times the bytes to make the artefact worse is
not a fix.

Source: ADR-0029, GALLERY Figures 12-13,
`nx-scratch/atlasprice/seamframe.py`, `seamdiag.py`.

---

## 7. Where the codec stands against the alternatives `[TO WRITE]`

**This section is not optional and must not be softened.** A paper that reports
§4 and omits §7 would be describing a different codec.

Source: [`tools/quality/reports/gates-v2-2026-09-04.md`](../../tools/quality/reports/gates-v2-2026-09-04.md),
[ROADMAP.md](../../ROADMAP.md), `ref/RESULTS-intra.md`, `ref/RESULTS-inter.md`.

**Ten gate verdicts, ten FAILs.**

| gate | material | verdict | headline, v1 -> v2 |
|---|---|---|---|
| Phase 1 intra (PAPER 3.11) | `vr-mixed-1024` 4:4:4 | **FAIL** | BD-rate +36.46 % -> **+61.43 %**; mean deficit -3.76 -> **-3.72 dB** |
| Phase 1 intra | `vr-mixed-1024` 4:2:0 | **FAIL** | +23.31 % -> **+38.17 %**; -2.78 -> **-2.36 dB** |
| Phase 2 kill, band A | `vr-mixed-1024` 4:4:4 | **FAIL** | +160.70 % -> **+342.67 %** |
| Phase 2 kill, band A | `vr-mixed-1024` 4:2:0 | **FAIL** | +130.94 % -> **+273.44 %** |
| Phase 2 kill, band A | `vr-turn-256` 4:4:4 | **FAIL** | +156.49 % -> **+206.17 %** |
| Phase 2 kill, band B | `vr-mixed-1024` 4:4:4 | **FAIL** | +469.11 % -> **+548.23 %** |
| Phase 2 kill, band B | `vr-mixed-1024` 4:2:0 | **FAIL** | +447.83 % -> **+478.92 %** |
| Phase 2 kill, band B | `vr-turn-256` 4:4:4 | **FAIL** | -> **+219.45 %** |
| Warp chain | `vr-mixed-1024` 4:4:4 | **FAIL** | 0 frames above 35 dB; start 24.40 -> **28.71 dB**, decay -5.96 -> **-9.59 dB** |
| Warp chain | `vr-turn-256` 4:4:4 | **FAIL** | 0 frames; start 29.48 -> **30.31 dB**, decay -10.09 -> **-7.37 dB** |

The criterion for the first row is *within 1.0 dB of x264 intra*; the measured
deficit is 3.72 dB.

**The reconciliation this paper owes the reader** is between §4 and §7, and it
is the most interesting thing in the document: the atlas result is measured on
the **vrroom** corpus against the codec's *own* picture model, and the gate
failures are measured on **vr-mixed-1024-v2** against **x264/x265**. They are
not in contradiction, they are different questions — "is the atlas the right
reference model for this codec?" (yes, measured) and "is this codec competitive
with a mature one?" (no, measured, by a wide margin). Any final text that lets a
reader take §4 as an answer to the second question is dishonest.

---

## 8. Method `[TO WRITE]`

### 8.1 The device situation, stated plainly

The repository states in two places that **nothing has been measured on a
headset** ([docs/README.md](../README.md), [ROADMAP.md](../../ROADMAP.md)), and
[PERFORMANCE.md](../PERFORMANCE.md)'s measured column is empty on purpose. The
Phase 0 gate has not been run on a device; one headless host run exists
(`bench/results/results-host.json`, RADV 7900 XTX, K1-K5 PASS, K6 skipped) and
`bench/README.md` states host numbers are a regression signal and never the
verdict.

> **That blanket statement is now false, and the paper must not repeat it.**
> At least three Pico 4 measurements exist in the tree:
>
> | number | source |
> |---|---|
> | decoder budget per eye, **12.293 ms today** (Pass A 1.534, Pass W 0.661, Pass B 10.760), of which **8.889 ms is `reconstruct_skip_store`** over ~250 skipped tiles | [ATLAS-DECODER.md](../ATLAS-DECODER.md):17-40, Adreno 650 at **490 MHz**, 1088x1088/eye, 289 tiles, `ENTROPY_LITE`, one eye — **absolutes inflated ~1.57x, see §8.2** |
> | atlas update (compose + renorm, 289 tiles) **0.0048 ms**, 0.0096 ms for 578 entries | ADR-0029:323, same device and clock |
> | `ENTROPY_LITE` Pass A **138.5 -> 18.4 ms, 7.5x** | TOOLBITS.md:381 — **disputed**, see §2.3 |
>
> The correct statement is narrower and still damning: **no Phase 0 gate run and
> no end-to-end timing exist on a device**, and the device numbers that do exist
> are point measurements on a thermally loaded unit, quotable as ratios (§8.2).
> Reconciling `docs/README.md` and ROADMAP.md with these is §10.3 work.

The 8.889-of-10.760 ms figure is the load-bearing one for the whole atlas
argument: it is the deletion the atlas is *for*. Under `ATLAS` those ~250
skipped tiles are not reconstructed at all, and the projected per-eye total
falls to ~2.4-3.0 ms — **4-5x, essentially all of it one deletion.** Two caveats
the source states and the paper must repeat: the compose dispatch is new and
**unmeasured**, and the client's own display warp (13.12.5) is **not in that
budget** — it is new work on the same GPU, and it is the trade ADR-0029 makes
deliberately, one non-normative filtered warp at panel rate instead of 250
normative integer ones at decode rate.

### 8.2 What a device number is allowed to claim

Three rules, and the paper should state them before it quotes a single
millisecond.

**Timestamps are masked, not trusted.** `timestampPeriod` is applied and the
delta masked to `timestampValidBits`, so a wrapping counter still yields the
right interval; a device without timestamp support returns `valid() == false`
and every timing call becomes a no-op rather than an error — a decoder that
cannot measure itself still decodes (`vk/README.md`:154).

**The bench may not report a GPU time it cannot justify.** The on-device bench
was found reporting GPU time in excess of wall time — most likely a wrong
`timestampPeriod` — which puts its absolutes about **1.57x too high**. A
self-check now requires summed GPU <= summed wall and **fails the run loudly**
otherwise, printing the ratio beside `timestampPeriod` and `timestampValidBits`;
`NXVC_VKD_TS_PERIOD` overrides the tick rate and is used to prove the gate can
fail (on RADV, `=400` gives 5.317 and a loud failure).

> **This invalidates the absolutes in §8.1 and the paper must say so where it
> quotes them.** Named explicitly as affected: the `12.293`, the `10.760`, the
> `1.534`, the `8.889`, and the ~34 us/tile. Their **ratios survive**; their
> millisecond values do not.
>
> *Off-branch:* the caveat and the gate live on `hybrid-gpu-time`
> (`nx-scratch/wt-hybridgpu`, merged to `main` as `40b33df`). Neither the 1.57x
> block nor `NXVC_VKD_TS_PERIOD` exists anywhere on `atlas`, so this skeleton's
> own copy of ATLAS-DECODER.md is the uncorrected one.

**Ratios carry; absolutes do not.** The project states this in at least six
places independently, which is itself worth reporting; the sharpest form is
`vk/decoder/passB/README.md`:373 — ***"The ratio is the measurement; the absolute
is not."*** Its reason is given right above it: the headset is awake running
SLAM during these runs, which holds the GPU at 490 MHz and 52-57 C, so the
absolutes sit ~30 % above a cold device. ATLAS-DECODER.md:40 says the same of
its own table (61-66 C at the end of a long session). The bench has a 10-minute
thermal mode for the same reason and notes its continuous rather than periodic
operation *inflates* thermal pressure relative to a real session.

> **A correction to the brief, and a naming job for the paper.** There is **no
> "contamination gate"** in either repository — the phrase does not appear. In
> this project *contamination* is a **codec quality** term (ADR-0029: the
> atlas's cross-tile gather across a mosaic of capture times is
> "displacement-proportional contamination that consumes the whole tile once the
> head turns fast enough"), and using it for measurement hygiene would collide
> with that. The measurement-hygiene mechanisms that do exist are unnamed and
> should be given a name in the paper:
>
> - the **idle gate** and a sha256 either side of the push before every launch;
> - **`gpuclk` sampled on the device while the run is in flight**, with
>   temperature reported either side (`scripts/passb-device-rows.sh`);
> - the GPU <= wall **`timing_selfcheck()`** above.

**And one claim in the brief that the tree contradicts: the clock is not
"490 MHz flat".** 490 MHz is well sourced as the clock *observed* during those
runs, but nothing pins it — it is held there incidentally by the headset's own
SLAM tracking. `bench/README.md` records that "the devfreq nodes that would pin
it are not reachable without root, so the gate has no clock control";
`bench/run.sh` parks a dozed GPU at its 305 MHz minimum OPP, and
`vk/decoder/passA/README.md` reports `gpuclk` ranging **305-490 MHz**. The
92 %-duty capture in §1.2 records no clock at all.

**The consequence for this paper.** Every device figure in §1.2 and §8.1 is
quoted as a ratio or with its thermal state attached, or it is not quoted.

### 8.3 The seam ratio

Mean `|x[i] - x[i-1]|` over sample pairs straddling the 64-sample tile grid,
divided by the same over pairs inside tiles, on decoded luma. **1.0 means a tile
edge looks like any other pair; above 1 the grid is visible.** Scale-free, so
configurations at different bitrates compare directly. Source: GALLERY.md, "The
seam ratio, since every entry above quotes it"; `nx-scratch/atlasprice/seams.py`.

### 8.4 Measurement hygiene

Every measurement in §4, §6 and §7 ran under `chrt -i 0 taskset -c <slice> nice
-n 19` on a shared machine, with test builds kept separate from measurement
builds. The gate report records its own pinning, its build hash, and that no
metric window is narrower than the encode it scores
(gates-v2-2026-09-04.md preamble).

---

## 9. The Blender corpus `[TO WRITE]`

`tools/quality/capture/gen_vrroom.py`, outputs under
`nx-scratch/fixtures/vrroom/`. **Figures 3-6** show one frame per trajectory.

Every ADR-0029 verdict before 2026-09-06 rested on `gen_synthetic.py`, which
[LOWPOLY-MODE.md](../LOWPOLY-MODE.md) called *"unusually kind"*: a band-limited
procedural panorama with no specular, no thin geometry, no text and no
independently moving objects. `vrroom` is the deliberate opposite — text panels
at reading distance, a mirror-like specular floor, thin high-contrast bars,
checkerboard walls, two textured humanoid meshes that move independently of the
head, and a skybox. Blender 5.2 EEVEE, `Standard` view transform, stereo 63 mm,
1088x1088 an eye, 100 degrees, 90 Hz, 32 frames.

Six trajectories, indexed by the quantity every ADR-0029 verdict is indexed by
(mean angular velocity, from the pose sidecars' `angular_velocity_deg_s`):

| track | deg/s | what it is |
|---|---|---|
| `still` | **0.043** | a seated head: postural drift plus physiological tremor |
| `objmotion-still` | 0.043 | the same head, meshes walking |
| `rest` | 2.678 | *not* a head at rest; the original "rest" |
| `objmotion` | 2.678 | `rest` with the meshes walking |
| `mid` | 26.160 | a deliberate look-across |
| `fast` | 99.095 | a fast turn, peaking at 796 deg/s across a one-frame 8-degree step |

One file, two halves: run under the system Python it drives Blender per
trajectory and packs the result; run under Blender it builds the scene and
renders. The whole fixture is reproducible from one file rather than from a
recipe in a commit message.

A seventh fixture exists only for §6.9: `vrroom-zero`, `still` frame 0 repeated
32 times at one fixed pose — an exactly-zero-motion control.

---

## 10. Open work `[TO WRITE]`

### 10.1 Unmeasured

- **Everything on a headset.** Phase 0 K1-K6 on a Pico 4; the pure-versus-hybrid
  decision. **UNMEASURED.**
- Pass B p50/p99 on device; encoder time on an RX 580. **UNMEASURED.**
- Loss-drift under 5 % segment loss. **UNMEASURED.**
- 24 h fuzz clean. **UNMEASURED.**
- Glass-to-glass latency at 150 Mbit on Wi-Fi 6. **UNMEASURED.**
- The FTO review (ADR-0017). **NOT PERFORMED.**

### 10.2 Measured and failing, i.e. the actual research problem

The Phase 1 and Phase 2 compression gates (§7). The paper's open question is
whether the atlas's structural wins (§4) can be made to pay against a mature
codec, or whether they only pay against this codec's own alternatives.

### 10.3 Threads left open by this document

- §1.2's operating-point figures need their source and their attribution fixed.
- §5 and §6.1-6.8 need their numbers pulled from the ADRs.
- ADR indexing: `docs/adr/README.md` still says *"Nothing in these records has
  been measured"*, which was true when written and is now false for ADR-0028 and
  ADR-0029. It needs a correction, not an edit to the ADRs.
- **The `ENTROPY_LITE` 7.5x contradiction (§2.3).** TOOLBITS.md and
  MERGE-REPORT.md record it as a Pico 4 measurement; ROADMAP.md and
  docs/README.md say nothing has been measured on a headset. One of the four
  documents is wrong and the paper cannot cite the number until it is known
  which.
- **Branch scope.** This skeleton is written against `atlas`. Three things it
  needs are on other branches and must be merged or cited as off-branch: the
  planar mode's implementation (`main`, `edefcdb`), the reference integer
  trellis (`main`, `046ab4b`), and the effort-1 withdrawal (`effort-vrroom`,
  `a010fd2`). `XFORM_FAST`, a multiply-free 8x8 transform on tool bit 28, exists
  only on `exp/xform-fast` (`6567184`) and has no result on `atlas`; note that
  bit 28 is `NEAR_SKIP` in the shipped table, so its tool-bit assignment is
  provisional at best.
- **Figures that do not exist and should.** §6.5 (alternate-eye) is the strongest
  negative in the tree with no figure: a worst-tile collapse to 11.5 dB in one
  eye is exactly the result a reader will not believe from a table. §6.3's
  scheduler and §6.6's disparity synthesis are also figureless. The gallery's
  house rule is that a figure illustrates a number; these numbers deserve one.

---

## Figure index

Every figure is [GALLERY.md](../GALLERY.md)'s, by its number there. Append-only:
the gallery never renumbers, so these references are stable.

| Fig | Caption | Used in |
|---|---|---|
| 1 | Rate-distortion on the vrroom corpus | §4.3 |
| 2 | The effort ladder and the planar mode do not pay | §2.4, §5, §6.8 |
| 3-6 | The vrroom corpus, one frame per trajectory | §9 |
| 7-9 | Coarse landings are blocky, not soft | §6.2 |
| 10-11 | The seated trajectories, and the true rest floor | §4.4 |
| 12-13 | The still clip's seams are frame 0's, and nothing adds to them | §6.9 |

**Six of the ten negatives in §6 have no figure** (§6.1, §6.3, §6.4, §6.5, §6.6,
§6.7). The gallery's house rule — *a figure that illustrates no number does not
belong here* — has the useful converse: a number this load-bearing that has no
figure is a gap. §10.3 lists the three worth drawing first.
