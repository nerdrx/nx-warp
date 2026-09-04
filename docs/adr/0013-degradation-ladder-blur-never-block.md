# ADR-0013: Under budget pressure, blur, never block

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 4.6.1
- **Affects**: `rc/`, `vk/encoder` (E1, E2)

## Context

This is a product requirement, not a tuning detail. H.264 at low bitrate raises QP everywhere and the
eye sees 8x8 and 16x16 blocks; blocking is the artefact users name when they complain about a stream.
The requirement is that when the budget is short the picture loses **texture** before it loses
**structure**: it should look like a scene whose textures were blurred, or whose surfaces went
low-poly, while edges, outlines and text stay crisp.

Almost all the machinery already exists in v1. What was missing was the order in which rate control
spends it.

## Decision

The encoder classifies every tile once per frame from two statistics it already computes for rate
control: the log-variance activity term and a gradient-coherence measure (the ratio of structure
tensor eigenvalues over the tile, one pass over the pixels). Four classes come out: **text** (UI
stencil, or high coherence plus high contrast), **edge** (high coherence), **texture** (high activity,
low coherence), **flat** (low activity). Under budget pressure the classes descend different ladders:

| Step | Texture tiles | Flat tiles | Edge tiles | Text tiles |
|---|---|---|---|---|
| 1 | low-pass weighting matrix drops high-frequency AC first | same | QP +2 only | untouched |
| 2 | `res_level` 1/2, bilinear upsample | `res_level` 1/2 | untouched | untouched |
| 3 | `res_level` 1/4 | `res_level` 1/4 | low-pass weighting, QP +4 | untouched |
| 4 | DC-plane only: 64 block DCs, planar interpolation | DC-plane only | `res_level` 1/2 | QP +4 within the lossless-or-near class floor |

The ladder is expressed to the section 4.6 allocator as a per-class QP floor and `res_level` cap.

Why each rung produces the wanted look: step 1 is blur, because zeroing high-frequency DCT
coefficients is a low-pass filter and the weighting matrix makes it a smooth one rather than a
threshold. Steps 2 and 3 are blur with no blocking at all, because the tile is coded small and
resampled up. Step 4 is the low-poly look: a tile reduced to its block DCs with planar interpolation
is a smooth gradient field, not a mosaic. Edge and text tiles hold their step until the others are
exhausted, so outlines and glyphs survive a frame that has turned to soft shapes around them.

## Consequences

- **No new syntax.** Every rung uses a v1 tool: the weighting matrices of paper 1.5, `res_level`, and
  the DC-plane intra of ADR-0004. The decoder pays nothing it does not already pay.
- The encoder pays one classification pass per tile per frame, reusing the activity statistic.
- The DC-plane intra decision acquires a second justification: it is the low-poly primitive.
- Two optional v2 refinements are recorded, each behind a tool bit: a 1-bit-per-8x8 edge mask in the
  tile payload so the decoder's `res_level` upsampler picks a sharper 4-tap kernel on edge blocks
  (about 8 bytes per tile, one branch per block on the decoder), and a per-tile "contour" mode coding
  a DC plane plus one straight edge with two side values. Neither is needed for the look; both make it
  cheaper.
- This is a subjective requirement, so it needs a subjective test. The paper's methodology (5.3) is
  paired comparison against uncompressed and against HEVC at matched bitrate, with a reading or
  tracking task that forces gaze to the periphery.

## Alternatives considered

- **Uniform QP escalation** (what H.264 rate control does). Rejected: it is exactly the artefact this
  decision exists to avoid.
- **A deblocking filter.** Rejected implicitly: it costs decoder time on the tightest budget in the
  system, and it treats the symptom. The codec avoids producing blocks in the first place. A learned
  deblocking filter exists as an out-of-loop Pro-profile future tool (paper 5.4).
- **Dropping frames instead of quality.** Rejected by design principle 4, degrade never stall, and by
  the temporal decoupling design (paper 2.8), which already handles a lower server frame rate.

## References

- Paper 4.6.1 (the degradation ladder), 1.5 (weighting matrices and `res_level`), 3.2.4 (DC plane),
  5.3 (subjective methodology)
- ADR-0004 (DC-plane intra)
