# ADR-0008: Foveation is per-tile resolution and QP; the warp never sees a non-uniform grid

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 5.1, 2.11 risk 8, reconciled in 6.8
- **Affects**: `fov/`, `rc/`, `warp/`, `vk/`

## Context

Section 2 requires the homography to run in linear render space: it is exact there and its integer
corner-and-interpolate formulation depends on the grid being uniform. Section 5 defines a per-frame
foveation map. WiVRn NX today applies a continuous separable remap before encoding (the render itself
is squeezed and the client unwarps), which would put the reference in a remapped domain and force the
reprojection to unwarp, rotate and rewarp per pixel. Section 2 flagged this as a cross-section
conflict.

## Decision

- **Phase 2 runs unfoveated**, to prove the predictor on its own.
- Foveation inside the codec is expressed only as per-tile `res_level` (sample scale 1, 1/2, 1/4),
  per-tile QP offset, and per-tile chroma mode, driven by the section 5 foveation map: one R8 texel
  per tile, generated on the server from gaze or lens centre, lens model, head velocity and content
  class.
- The predictor is formed at the coded resolution: the warped reference tile is box-downsampled, the
  residual added, and the result upsampled into the full-resolution reference image. **The client
  always holds a full-resolution reference**, so the warp never sees a non-uniform grid.
- WiVRn NX's continuous foveation remap stays for the hardware codecs and is bypassed when this codec
  is active. The render-cost win moves to Variable Rate Shading driven by the same map.
- The tile grid stays axis-aligned and uniform in the encoded image, so the workgroup mapping stays
  `tile id = (x/64, y/64)`.

One map, three consumers: the app's render pass through VRS, the encoder, and the client's
reprojection pass.

## Consequences

- The warp stays exactly the integer construction of ADR-0010, with no LUT composition per pixel and
  no unwarp-rotate-rewarp chain.
- The encoder's model of the client's reference is exact even for foveated tiles, because the
  upsample is part of reconstruction and is bit-exactly specified.
- If the app ignores VRS, which most do, the encoder still downsamples; the only loss is wasted PC
  shading, not quality.
- Fixed foveation on a Pico 4 is estimated to halve the sample count (about 40 percent of tiles at
  s=1, 35 percent at 1/2, 25 percent at 1/4, giving 0.50 of full-resolution samples). Eye-tracked
  foveation is estimated at 0.18 (paper 5.1.2, 5.1.3, both design estimates).
- The Pico 4 panel is below foveal acuity everywhere (about 22 ppd at centre against 60 ppd needed),
  so there is no headroom above s=1 in the fovea and foveation cannot win in the centre.
- A tile jumping from 1/4 to 1 after a saccade has a low-resolution warped reference, so its residual
  is nearly intra-sized. Saccade landing prediction gives the encoder two frames to spread that cost
  (paper 5.1.4).

## Alternatives considered

- **Keep the continuous separable remap and compose it into the warp.** Rejected for v1: it makes the
  predictor operate in a remapped domain, which is a per-pixel function evaluation on both ends and
  breaks the "reference is a plain rectilinear image" property that everything else relies on. Noted
  as the long-term direction only if per-tile scale proves insufficient.
- **Warped (non-uniform) tile grid in lens space.** Rejected: the workgroup mapping must be trivial.
- **Foveation by QP alone, no resolution levels.** Rejected: QP alone produces blocking, which is what
  the degradation ladder forbids (ADR-0013), and resolution is where the sample-count win is.

## References

- Paper 5.1 (foveation model), 5.2 (perceptual quantisation), 2.11 risk 8, 6.8
- Geisler and Perry 1998 (cortical magnification model), Sitzmann et al. 2018 (VR gaze statistics),
  Albert et al. 2017 (latency tolerance), Arabadzhiyska et al. 2017 (saccade landing prediction).
  The paper notes that years and venues are from memory and should be checked before publication.
- ADR-0013 (degradation ladder), ADR-0010 (integer warp)
