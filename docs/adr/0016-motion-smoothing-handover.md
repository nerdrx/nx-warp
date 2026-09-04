# ADR-0016: Motion smoothing consumes the codec's MV field

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 2.8, reconciled in 6.11
- **Affects**: `warp/`, `rc/`, WiVRn NX client and server

## Context

WiVRn NX today computes a motion field on the server with a block matcher and sends it alongside the
video, so the client can synthesise in-between frames when the server sends at 45 Hz and the panel
runs at 90 Hz. That block matcher is a second motion estimator running next to the encoder's, on the
same content, producing a field that is not tile-aligned with anything.

The codec already computes a per-tile motion vector, and the part of it that is not head motion (the
coded MV minus the warp-induced shift at the tile centre) is exactly the residual-motion field the
smoother needs.

## Decision

When the codec is active, the server-side block matcher is retired. The client-side warp consumes the
codec's per-tile MV field, which is free and already tile-aligned. `STATIC_MV` tiles are excluded from
extrapolation, because head-locked content must not be warped.

Because coded MVs are rate-distortion choices rather than true motion (the same caveat Oculus ASW 1.0
had when it used the video encoder's motion estimation), the encoder search adds a smoothness penalty
`lambda_s * |mv - neighbour_mv|` that biases toward physically plausible fields at negligible bit
cost.

## Consequences

- One motion estimator instead of two, and the surviving one runs on the GPU as part of E1.
- A known motion-smoothing artefact is fixed by construction: menus and HUDs stop being warped,
  because `STATIC_MV` marks them and `STATIC_MV` is excluded.
- The frame-rate governor can trade server render rate against bits per frame without changing the
  decoder's per-frame cost.
- The smoothness penalty is a rate-distortion distortion: it makes the encoder choose a slightly worse
  vector for a better field. The bit cost is stated as negligible and is unmeasured.
- This is a change to WiVRn NX behaviour, not only to the codec, and it only applies when the codec is
  active. The block matcher stays for the hardware codec paths.

## Alternatives considered

- **Keep the block matcher and ignore the codec's vectors.** Rejected: duplicate work and an
  unaligned field.
- **Use the coded vectors raw, with no smoothness penalty.** Rejected on the ASW 1.0 experience: raw
  rate-distortion vectors produce visible warping artefacts on flat and periodic content.
- **Send a separate true-motion field alongside the MVs.** Rejected: it is bits and encoder time for
  something the penalised search already approximates.

## References

- Paper 2.8 (temporal decoupling and frame-rate scaling), 6.11
- ADR-0005 (one MV per tile, five modes)
