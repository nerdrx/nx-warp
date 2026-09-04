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
