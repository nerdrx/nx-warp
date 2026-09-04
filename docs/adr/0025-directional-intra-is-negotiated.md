# ADR 0025: Directional intra is a negotiated tool; restriction A is its default schedule

Status: Accepted, 2026-09-04
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

## Consequences

The Phase 1 gate on the Pico 4 is stated against DC-plane intra plus RDO, contexts
v2 and sign hiding, not against directional intra. The desktop and future-headset
gate includes it. spec/08 profiles must list INTRA_DIR as Full-profile optional and
Lite-profile absent.
