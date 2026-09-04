# ADR 0022: Hybrid mode is not a quality tool, and does not gate Phase 3

Status: Accepted, 2026-09-04
Supersedes in part: ADR 0014 (layered bitstream with hybrid mode), ADR 0015 (compute budget verdict)

## Context

The paper (1.7, 2.9, 3.5, 6.10) expected the Pico 4 to run a hybrid decoder: a
hardware HEVC base layer plus a compute enhancement layer with pose-warped
prediction, on the assumption that the compute path would not fit the Adreno 650
budget. The hybrid experiment (hybrid/RESULTS.md, 68-point sweep plus A/B runs)
measured the quality of that arrangement against HEVC alone and against the pure
codec model at matched total bitrate.

| Mbit | HEVC alone | pure codec model | best hybrid |
|---|---|---|---|
| 50 | 35.94 | 31.58 | 35.58 at 85 percent base |
| 150 | 41.83 | 37.78 | 41.56 at 85 percent base |
| 200 | 43.38 | 39.64 | 43.07 at 85 percent base |

Quality rises monotonically with the base share and converges on plain HEVC from
below. A reduced-resolution base loses 3 to 4.6 dB at every split. The 8 to 12 ms
of MediaCodec latency is paid by HEVC and by hybrid alike, so hybrid is dominated
by plain HEVC on both axes.

The pure-codec model in the experiment sits about 4 dB behind x265, where the
paper estimated parity; the conclusion survives a 2 dB improvement of the codec
and would not survive parity, so it is to be re-run against ref/ when inter
prediction exists.

## Decision

1. Hybrid mode stays in the bitstream (layer container per SYNTAX.md) but is not
   built for Phase 3 and does not gate it.
2. On a headset where the pure compute decoder fails the Phase 0 gate, WiVRn NX
   keeps streaming plain HEVC through its existing path. NX Warp targets the
   hardware that passes the gate.
3. If hybrid is ever built, it is a full-resolution base at 85 percent or more of
   the bitrate, never a reduced-resolution base.
4. The remaining case for hybrid (loss behaviour, foveation of the enhancement
   layer, frame-rate decoupling) must be measured before any implementation
   effort is spent.

## Consequences

Phase 0's decision rule collapses to pass or fail for the pure compute path. The
K6 kernel stays informational. The Pico 4 verdict is therefore either "NX Warp at
72 or 90 Hz in pure compute" or "HEVC as today", not a third path.
