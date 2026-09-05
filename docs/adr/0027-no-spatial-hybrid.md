# ADR 0027: No spatial hybrid; foveation inside the codec is the lever

Status: Accepted, 2026-09-05
Relates to: ADR 0022 (layered hybrid rejected)

## Context

ADR 0022 rejected the layered hybrid (HEVC base plus compute enhancement). A
spatial split was studied separately on branch exp/spatial-hybrid
(docs/SPATIAL-HYBRID.md, hybrid/sim): the hardware HEVC decoder carries the
periphery, NX Warp carries a fovea inset composited with a feather.

Measured on the v2 sequence at 40, 80 and 150 Mbit against x265 alone and x265
with a delta-QP foveation map, on eccentricity-weighted PSNR and FovVideoVDP in
the Pico 4 display model:

| Mbit | Best spatial configuration | JOD | x265 plus delta-QP map JOD |
|---|---|---|---|
| 40 | inset 128 tiles, 85/15 split | 9.453 | 9.886 |
| 80 | inset 128 tiles, 85/15 split | 9.674 | 9.937 |
| 150 | inset 128 tiles, 85/15 split | 9.809 | 9.964 |

The inset concentrates quality where intended (a 12 dB fovea-over-periphery
gap) and its fovea comes within 0.2 dB of the foveated HEVC anchor's fovea at
80 Mbit, but the periphery loses 8 to 9 dB the anchor does not lose. The inset a
fixed-foveation Pico 4 needs, the 20x15 degree eye box plus a foveal radius, is
1344x1088 px, which costs 9.8 ms of Adreno 650 time luma only and 14.6 ms with
chroma against a 5 ms gate, and only hides behind MediaCodec's own 8 to 12 ms.

## Decision

1. The spatial hybrid is not built. ADR 0022's conclusion extends to spatial
   splits.
2. The codec's periphery coding, not its fovea coding, is what loses against a
   foveated HEVC encode. Foveation inside the codec (per-tile res_level, QP and
   the temporal ladder applied to the coded picture, flat in every hybrid sweep
   so far) is the largest unexploited lever and the one a delta-QP map cannot
   copy. It is the next quality experiment after the tournament merge.
3. The Android client's MediaCodec low-latency keys are worth setting for the
   HEVC path regardless, as INTEGRATION.md item 10 already notes.

## Consequences

No bitstream inset rectangle, no second encoder instance, no second decoder on
the client. The rate-control library's spatial ladder, which measured negative
in RESULTS-percept.md, must be re-tuned on 2160-px material with the encoder's
skip decision fed to the allocator before allocation; that work is the follow-up
to this decision.
