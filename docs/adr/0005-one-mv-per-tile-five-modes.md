# ADR-0005: One quarter-pel vector per tile, five modes

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 2.1, 2.3, reconciled in 6.5
- **Affects**: `warp/`, `ref/`, `vk/`, `rc/`

## Context

The predictor is the previous decoded frame resampled through a global per-eye homography derived
from the head pose delta, plus a per-tile correction. The question is how much per-tile freedom the
correction gets.

The geometric argument (paper 2.1): once depth is approximated as constant per tile, any positional
parallax collapses to a per-tile 2D shift, because the plane-induced homography
`H = K (R - t n^T / d) K^-1` differs from the rotation-only homography by a term that is constant
within a fronto-parallel tile to first order. So a per-tile motion vector already subsumes "positional
warp with per-tile depth" and also covers moving objects. Sending depth to the decoder would carry no
extra information for the same bit cost.

## Decision

One quarter-pel motion vector per tile in v1, coded as a signed Exp-Golomb delta from the same tile's
previous vector (temporal prediction from per-tile decoder state), range plus or minus 64 px. Five
tile modes:

| Mode | Reference | Vector | Residual |
|---|---|---|---|
| `WARP_SKIP` | warp(prev) | none | none |
| `WARP_MV` | warp(prev) | coded | coded |
| `STATIC_MV` | prev, identity, no warp | coded | coded |
| `STEREO` | decoded left eye of this frame | coded disparity | coded |
| `INTRA` | none | none | coded |

`STATIC_MV` exists for head-locked content (menus, HUDs, laser pointers, the transport HUD), where
the warp is exactly wrong and the identity predictor is exactly right.

Depth never reaches the decoder. Depth, engine velocity buffers, stencil masks and the search all
live on the encoder; the decoder only ever sees homography plus per-tile vector plus mode.

Spatial MV prediction from neighbouring tiles (the H.264 median) is rejected because it makes tiles
depend on each other. The temporal predictor costs nothing and gives zero-delta vectors for constant
parallax and constant-velocity objects.

## Consequences

- Estimated MV cost at 8192 tiles and about 3 bits per coded vector is about 2 Mbit/s at 90 Hz,
  negligible above 50 Mbit/s (paper 2.3, estimate).
- Fast object motion within one 64x64 tile is the known weak case: estimated 15 to 25 percent worse
  than HEVC block motion in the affected tiles (paper 1.10). Four vectors per tile is a v2 tool bit.
- Quarter-pel costs no extra filter machinery, because the warp already lands on 1/64 pel positions
  and the two fractions simply add.
- The decoder needs 16 bytes of per-tile state (`held_frame_id`, `last_mv`, `age_since_intra`,
  `concealed_count`, mode/QP/flags), about 128 kB at 8192 tiles (paper 2.6).
- Concealment falls out for free: a missing tile runs `WARP_SKIP` with `last_mv`, so objects keep
  sliding and the world stays locked to the head, and there is no separate concealment code path to
  test (ADR-0006).
- The coded MV field becomes the motion-smoothing extrapolation field, which retires the server-side
  block matcher (ADR-0016). Because coded vectors are rate-distortion choices rather than true motion,
  the encoder search adds a smoothness penalty `lambda_s * |mv - neighbour_mv|`.

## Alternatives considered

- **Per-pixel depth warp (forward splatting plus hole filling).** Rejected: outside the Adreno 650
  budget, needs a depth stream, and creates a disocclusion problem the rotation-only warp does not
  have.
- **Per-tile plane homography (4 extra parameters).** Rejected: second-order gain over a shift at
  32 px tiles, at four times the parameter cost.
- **Four vectors per tile.** Deferred to a v2 tool bit; it is the answer to the fast-object-motion
  weakness if Phase 2 shows the loss is real.
- **1/8 pel vectors.** Rejected: measured in every codec since H.264, the marginal gain over 1/4 pel
  is not worth the coding cost.
- **Spatial (median) MV prediction.** Rejected: creates cross-tile dependency, which is forbidden.

## References

- Paper 2.1 (predictor), 2.2 (integer warp), 2.3 (residual motion), 2.7 (concealment), 6.5
- ADR-0006 (references), ADR-0010 (integer warp determinism), ADR-0016 (motion smoothing)
