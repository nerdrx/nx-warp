# ADR-0015: The compute budget verdict is pending Phase 0

- **Status**: Proposed. Resolves when the Phase 0 table exists.
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 3.1, 3.4, 3.12, reconciled in 6.10
- **Affects**: `bench/`, and every scheduling decision downstream

## Context

The whole project rests on one number that has not been measured: how long Pass A plus Pass B take on
an Adreno 650 for two eyes at 2048 squared.

The estimate (paper 3.1, 3.2.5, all design estimates):

| Quantity | Value |
|---|---|
| Frame | 8.39 Mpixel |
| Frame period at 90 Hz | 11.1 ms |
| GPU already spent per vsync by WiVRn reprojection and compositor | about 2 to 3 ms |
| Decoder budget we can defend | 5 ms p50, 7 ms p99 |
| Adreno 650, assumed sustained | about 300 G int32 simple ops/s |
| Adreno 650 memory, usable by the GPU | about 25 GB/s |
| ALU: 8.39 Mpixel x 150 ops | 4.2 ms |
| Memory: about 105 MB per frame | 4.2 ms |
| Estimated total | 4 to 6 ms p50, 7 ms p99 |

ALU and memory overlap only partially. That is the whole risk in one line: the decoder lands at 4 to
6 ms if everything goes right, in the same band as the CAS sharpening pass that hurt in the field.

## Decision

**No codec code is written against an assumed budget.** The Phase 0 gate (paper 3.4) is a standalone
Android app, NDK, Vulkan 1.1, no OpenXR, run on the Pico 4 at 90 Hz with the display active and a
fullscreen dummy reprojection pass submitted every vsync so the decoder competes with realistic
co-tenant work. Kernels K1 to K6 are real code that will be reused, timed with timestamp queries, 600
frames after 120 warm-up, reporting p50, p95, p99 plus a throttling check over 10 minutes.

| Kernel | Pass threshold |
|---|---|
| K1 copy | reports achievable GB/s, expect over 20 |
| K2 gather-4 warp | under 3.0 ms p50 |
| K2b sampler | informational: the cost of bit-exactness |
| K3 idct | under 2.5 ms p50 |
| K4 rans | under 1.5 ms p50 |
| K5 full (Pass A + Pass B) | under 5.0 ms p50, 7.0 ms p99 with the dummy reprojection running |
| K6 hybrid | MediaCodec decode p50 under 15 ms, Pass C under 2.0 ms |

Decision rule, fixed in advance:

- **K5 passes**: pure compute is the default on Pico 4.
- **K5 between 5 and 8 ms**: pure compute at 72 Hz or with 1.5x foveated tile reduction; hybrid is the
  default.
- **K5 over 8 ms**: the Pico 4 is a hybrid-only device; the pure compute path continues on PC and
  next-generation Adreno.

The paper's own expectation (6.10) is that the Pico 4 lands in hybrid mode, with pure compute becoming
the default on the next Adreno generation and on PC-class clients. **That is an acceptable outcome,
not a failure of the design.**

## Consequences

- The Phase 0 table is a blocking dependency for Phase 1 and for the profile assignments in
  [COMPATIBILITY.md](../COMPATIBILITY.md), which is why every row there says "expected".
- Phase 0 must measure decode watts, not only milliseconds: compute decode heats the XR2 more than the
  hardware decoder, and thermal throttling can turn a 5 ms decoder into a 7 ms one over a session
  (paper 4.11).
- The bench app stays in the tree afterwards as the regression benchmark; the nightly runner is meant
  to fail if Pass B p99 on the Pico 4 regresses by more than 5 percent (paper 3.9).
- If K5 is within 20 percent of its threshold, the sparse coefficient layout (roughly 4x smaller than
  the dense int16 round trip, which is 16.8 MB each way per frame) should be prototyped in Phase 1
  (paper 3.12).
- This ADR is superseded by whichever outcome the measurement produces, and the superseding ADR
  carries the measured table.

## Alternatives considered

- **Build the codec first and measure later.** Rejected explicitly by the paper's closing section
  (7.4): nothing else should be written until that table exists.
- **Design for the Adreno 740 and treat the Pico 4 as out of scope.** Rejected: the Pico 4 is the
  first target device and the hybrid path exists precisely so that a "no" on K5 is survivable.

## References

- Paper 3.1 (working numbers), 3.2.5 (traffic and time estimate), 3.4 (the exact benchmark),
  3.12 (open risks), 6.10, 7.4
- `bench/README.md`, [docs/PERFORMANCE.md](../PERFORMANCE.md)
- ADR-0011 (compute over fixed function), ADR-0014 (hybrid)
