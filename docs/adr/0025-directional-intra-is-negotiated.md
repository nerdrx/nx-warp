# ADR 0025: Directional intra is a negotiated tool; restriction A is its default schedule

Status: Accepted, 2026-09-04; numbers revised 2026-09-04 (see "Revision")
Relates to: ADR 0004 (DC-plane intra), tool bit 17 INTRA_DIR

## Context

Directional intra (tool bit 17) is worth about 1.5 dB against x264 intra on the
harness material (ref/RESULTS-intra.md). Its cost is an in-tile wavefront. The GPU
decoder measured the wavefront variants on the desktop (vk/decoder/README.md,
2048 tiles, 4:2:0, QP 24):

| Schedule | Dependent steps | Occupancy | Rate cost | Pass B on RX 7900 XTX |
|---|---|---|---|---|
| INTRA_DIR off (DC-plane) | 1 | 100 percent | 0 | 0.24 ms |
| Full, above-right allowed | 22 | 4.5 percent | 0 | 1.72 ms |
| A: no above-right reference | 15 | 6.7 percent | 0.24 percent | 1.29 ms |
| A plus 32x32 sub-tiles | 7 | 14.3 percent | 1.8 percent | 1.01 ms |

Pass B's cost is entirely fixed per tile (906 ns, slope zero across a 28x payload
range) because the wavefront runs its steps regardless of payload. Assuming the
Adreno 650 is 20 to 30x slower on a serialisation-bound kernel, the cheapest
variant is about 20 ms of Pass B against an 11.1 ms frame at 90 Hz.

## Decision

1. Directional intra stays in the format as tool bit 17 and is negotiated by
   capability: a decoder that cannot afford it does not advertise it and the
   encoder falls back to DC-plane intra for that client.
2. Restriction A (no above-right reference) is the default schedule when the tool
   is on. Adding the 32x32 sub-tile restriction buys a tenth as much decode time
   per percent of rate and is not enabled.
3. The Pico 4 stream uses DC-plane intra until measured otherwise on the device.
4. Two levers are recorded for the future: more threads per block (occupancy is
   capped at 4.5 percent by the four-lanes-per-block layout inherited from the
   transform; quadrupling touches no syntax), and sparse coefficient transfer,
   which the fixed-cost measurement shows is the larger lever for Pass A.
5. Host-side tile sorting by mode stays off by default: minus 12 percent on RADV,
   plus 12 percent on lavapipe.

## Revision, 2026-09-04: both levers in point 4 have been taken

Point 4 named two levers. Both landed (vk/decoder/README.md), and they change
the numbers this ADR rests on without changing any of its decisions.

| Schedule | Occupancy | Rate cost | Pass B on RX 7900 XTX | at 20x | at 30x |
|---|---|---|---|---|---|
| INTRA_DIR off (DC-plane) | 100 percent | 0 | 0.255 -> 0.241 ms | 4.8 ms | 7.2 ms |
| Full, above-right allowed | 4.5 -> 18.2 percent | 0 | 1.780 -> 1.183 ms | 23.7 ms | 35.5 ms |
| A: no above-right reference | 6.7 -> 26.7 percent | 0.24 percent | 1.310 -> 0.885 ms | 17.7 ms | 26.6 ms |
| A plus 32x32 sub-tiles | 14.3 -> 57.1 percent | 1.8 percent | 1.041 -> 0.732 ms | 14.6 ms | 22.0 ms |

More threads per block: the wavefront no longer inherits the transform's
four-lanes-per-block mapping (the residual is staged in the shared sample store,
which costs no memory), so a step spreads over 16 threads per block. Every
schedule is about 30 percent cheaper and their ordering is unchanged, so points
1, 2 and 3 stand as written; the trade for restriction A is now 1.24 ms per
percent of rate against 0.10 for the sub-tile restriction, a ratio of twelve
rather than ten.

Sparse coefficient transfer: coefficient traffic per 2048-tile 4:2:0 frame falls
from 25.6 MB at every QP to 0.93 MB at QP 36 and 11.6 MB at QP 24. Point 4 called
this "the larger lever" and **that was wrong**. On a 7900 XTX it costs Pass B
about 5 percent (the per-coefficient length check) and buys no time at all,
because 960 GB/s was never the constraint; on the scaled Adreno estimate the
whole memory system is 2.6 ms of an 11.1 ms budget dense and 1.5 ms sparse,
against a wavefront that is 15 to 36 ms. What the layout actually bought was the
per-unit length, which lets an uncoded unit skip its transform outright: that,
plus a fast path for planes whose intra modes are all mode 0, took Pass B's
fixed cost from 890 ns per tile to 248 ns. See ADR 0026.

Two consequences for this ADR's own reasoning:

* **Pass A is now the pass that misses the budget.** At QP 36 with INTRA_DIR off
  -- the Pico 4 configuration of point 3 -- Pass B scales to 4.8 to 7.2 ms and
  Pass A to 6.8 to 16.2 ms, against 11.1 ms. Point 3 stands, and the work it
  implies has moved to the entropy decoder's three barriers per scheduling round.
* **Point 5 is weaker, not wrong.** Tile sorting is still off by default, but the
  RADV delta against the cheaper Pass B ranges -14 to +5 percent across runs
  rather than a steady -12, so "worth 12 percent on one ICD and costs 12 on the
  other" has become "not clearly worth anything on either". It still needs the
  Adreno number.

## Consequences

The Phase 1 gate on the Pico 4 is stated against DC-plane intra plus RDO, contexts
v2 and sign hiding, not against directional intra. The desktop and future-headset
gate includes it. spec/08 profiles must list INTRA_DIR as Full-profile optional and
Lite-profile absent.

## Revision, 2026-09-05: the Adreno 650 is measured, and A1 was wrong by 40x

Point 3 said "the Pico 4 stream uses DC-plane intra until measured otherwise on
the device". It has now been measured on the device. The decoder's conformance
suite cross-builds for arm64 and runs as a plain executable on a Pico 4
(Adreno 650, driver 1.1.128) -- vk/decoder/README.md, "Android" -- and the
numbers below are best-of-10 on the same 2 x 2048^2 4:2:0 frame, 2048 tiles,
with the GPU ramped to 587 MHz.

The estimate this ADR and vk/decoder/README.md rested on was assumption A1:
"compute and serialization scale by 20 to 30x from the 7900 XTX", hedged as
"a floor rather than a bracket". The hedge was right and nowhere near large
enough.

| Schedule, QP 24 | RX 7900 XTX | estimate at 20-30x | **measured, Adreno 650** | actual ratio |
|---|---|---|---|---|
| INTRA_DIR off (DC-plane) | 0.241 ms | 4.8 - 7.2 ms | **296 - 368 ms** | 1200 - 1500x |
| 0 -- as written | 1.183 ms | 23.7 - 35.5 ms | **1348 ms** | 1140x |

and Pass A, which the same estimate made the larger pass:

| Pass A | RX 7900 XTX | estimate at 20-30x | **measured, Adreno 650** | actual ratio |
|---|---|---|---|---|
| QP 24 | 1.26 ms | 25 - 38 ms | **402 - 415 ms** | ~330x |
| QP 36 | 0.54 ms | 11 - 16 ms | **62 ms** | ~115x |

Three things follow, and none of them changes a decision in this ADR.

1. **The decisions stand, with far more room to spare than they were made
   with.** Directional intra was 2 to 4x over an 11.1 ms budget on the
   estimate; it is **121x** over it on the measurement (1348 ms against 11.1).
   Points 1, 2 and 3 -- negotiate the tool by capability, restriction A as the
   default schedule when it is on, DC-plane intra for the Pico 4 stream -- were
   already the conservative reading and the measurement only widens the margin.
   Nothing about the *ordering* of the schedules has been measured on device,
   because at 1348 ms for schedule 0 the ordering is not the question.

2. **A v1 stream does not fit either, and that is the new finding.** The
   previous revision said "a v1 stream (INTRA_DIR off) fits, and comfortably".
   It does not: DC-plane Pass B alone is 296 - 368 ms against 11.1 ms, and
   Pass A adds another 62 - 415 ms. The whole decode is **340 ms at QP 36 and
   830 ms at QP 24**, 30x to 75x the frame budget. This is not a schedule
   problem and no entry in the 7.6 menu addresses it.

3. **Pass B scales far worse than Pass A** -- roughly 1200x against 330x for
   the same part -- which is the shape of a kernel limited by serialization and
   register pressure rather than by throughput. Pass B is the kernel that takes
   136 VGPRs and 13.3 KB of LDS on RDNA3; what an Adreno 650 does with that
   footprint has never been looked at, and
   `VK_KHR_pipeline_executable_properties` is present on the part and still
   unused (bench/README.md, "Known gaps"). That is the first thing to look at,
   and it is a *decoder* question rather than a syntax question.

Point 4's two levers were taken in the previous revision and are not undone by
this: they were real improvements on RADV and there is no reason to think they
are not real here. They are simply an order of magnitude short of what the
target part needs, and the gap was hidden for as long as the only numbers were
scaled ones.

**What this revision does not do is re-open the format.** The store format was
the one lever with a measured 3x behind it (bench/README.md: an integer storage
image costs about 3x a UNORM one on Adreno). Pass B can now write its 8-bit
output through UNORM images, the substitution is proved exact on RADV, lavapipe
and the Adreno 650, and it is worth **-7 % of Pass B at QP 24 and +2 % at
QP 36** -- because Pass B is not store-bound on this part either. See
vk/decoder/README.md, "The UNORM store". The 3x was real and it was measured on
a pure copy kernel; Pass B is not a copy kernel.
