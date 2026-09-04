# ADR-0004: Intra is a DC plane, not directional modes

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.4, 3.2.4, reconciled in 6.4
- **Affects**: `ref/`, `vk/decoder/passB`, `vk/encoder` (E2)

## Context

Spatial intra prediction in the H.264 or HEVC sense creates a dependency chain across the 64 blocks
of a tile: a 15-step wavefront over the block diagonals, with barriers, and only about 4 active lanes
per block at the start and end. That is the exact shape design principle 2 forbids.

Intra tiles are 5 to 10 percent of tiles under rolling refresh (paper 3.2.4, estimate), so this is
not the workhorse path. But a slow path still costs p99, and p99 is what the deadline controller
reacts to.

## Decision

v1 intra has no directional pixel prediction.

- Blocks of an INTRA tile are predicted from the tile mean, coded once in 8 bits.
- The 64 DC coefficients of the luma blocks form an 8x8 low-resolution image which is itself passed
  through a second-level 8x8 DCT and coded first, 64 symbols. H.264's intra16x16 uses the same idea
  with a Hadamard (2003, expired).
- Pass B decodes the DC plane, then predicts each pixel by bilinear interpolation between the four
  nearest block DCs, planar-like. Fully parallel, no barrier beyond the transpose, about 12 ops per
  pixel.

Directional intra (four modes, decoded as a 15-step wavefront) is specified as tool bit `INTRA_DIR`
for v2 and is promoted to v1 only if Phase 1 measures more than a 40 percent bit gap against x264
intra on VR captures.

## Consequences

- Intra tiles are estimated to cost 25 to 35 percent more bits than x264 intra (paper 1.4), possibly
  30 to 40 percent against x265 intra at equal PSNR (paper 1.10). Both are design estimates and both
  are Phase 1 measurements.
- The intra path has the same dispatch shape as the inter path, so there is no p99 cliff on refresh
  frames and no second code path in Pass B.
- The DC plane doubles as the fourth rung of the degradation ladder: a tile reduced to its block DCs
  with planar interpolation is a smooth gradient field, which is the low-poly look the ladder wants
  (ADR-0013). The tool is needed anyway.
- The Phase 1 exit criterion "within 1.0 dB PSNR of x264 intra" is the check on this decision. If it
  fails by more than the stated margin, `INTRA_DIR` moves to v1 and this ADR is superseded.

## Alternatives considered

- **Directional intra, four modes, wavefront decode.** Rejected for v1 on the serialisation cost,
  kept as a v2 tool bit with a defined promotion criterion.
- **No second-level DC transform, DC coded per block.** Loses most of the intra-tile DC redundancy for
  a saving of one 64-point transform per tile. Rejected.
- **5/3 wavelet intra.** The wavelet was the serious alternative to the DCT and gives multi-resolution
  for free, but it needs six barriers per tile, a whole-tile dependency chain, and a different
  coefficient model. Reserved as `XFORM_WAVELET` for a v2 experiment (paper 1.4).

## References

- Paper 1.4 (transform and intra), 3.2.4 (intra without a wavefront), 6.4, 3.11 (Phase 1 exit)
- ADR-0013 (degradation ladder), ADR-0002 (tile size)
