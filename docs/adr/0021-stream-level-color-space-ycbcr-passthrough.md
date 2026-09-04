# ADR-0021: Stream-level colour space, with a YCbCr 4:2:0 passthrough path

- **Status**: Accepted
- **Date**: 2026-09-04
- **Source**: WiVRn NX integration constraints. Supersedes the display-format reference storage of
  paper 1.3 for the 4:2:0 path.
- **Affects**: `ref/`, `vk/`, `warp/`, `android/`, [SYNTAX.md](../SYNTAX.md), [INTEGRATION.md](../INTEGRATION.md)

## Context

Paper 1.3 assumes an RGB source: the encoder converts RGB to YCoCg-R, and the client stores references
in display format (RGBA8 or RGB10A2) so that the reprojection shader samples them directly with zero
copies. That is the right design when the codec owns the pixel path end to end.

It is not what the first integration target looks like. In WiVRn NX on Linux:

- The encoder's input is **already YCbCr 4:2:0**, and it is **already foveated by the compositor**
  before the encoder sees it.
- The client **already consumes 4:2:0** from its decoder and its existing reprojection path is built
  around that.

Forcing that source through YCoCg-R would mean YCbCr to RGB to YCoCg-R on the way in and the reverse
on the way out, adding two conversions on the normative path, inflating reference storage from two
planes at 12 bits per pixel to four channels at 8 or 10 bits per channel, and introducing a rounding
step that has to be specified bit-exactly for no benefit, since the source has already discarded the
chroma resolution that YCoCg-R would preserve.

## Decision

The stream header carries a **`color_space`** field, negotiated at connect like every other capability:

- **`YCOCG_R`**: for RGB sources. Exactly the paper 1.3 design, including references stored in display
  format (RGBA8 or RGB10A2) and the exactly-invertible lifting transform that makes lossless possible.
- **`YCBCR_PASSTHROUGH`**: for sources that are already YCbCr 4:2:0. No colour conversion is performed
  on either end. The coding tools are unchanged: the same 8x8 integer DCT, the same rANS coder, the
  same tile syntax operate on the Y, Cb and Cr planes as they would on Y, Co and Cg. References are
  stored in the source's own 4:2:0 layout rather than in display format.

The Android decoder outputs **2-plane 4:2:0** in the passthrough path, which is what the existing
client reprojection consumes.

Consequently, "the client stores references in display format" (paper 1.3) is normative for the
`YCOCG_R` path only.

## Consequences

- No redundant conversions on the WiVRn NX Linux path, and reference traffic in that path is roughly
  halved against a four-channel display-format reference, which matters because the reference read is
  the largest single item in the decoder's memory budget (paper 3.2.5).
- The integration is drop-in on both ends: the server hands the encoder what it already produces, and
  the client hands the decoder's output to the reprojection pass it already has.
- **The lossless and near-lossless modes belong to the `YCOCG_R` path.** `YCBCR_PASSTHROUGH` cannot be
  mathematically lossless with respect to an RGB original, because the source is already subsampled
  and the transform to it is not integer-reversible. Lossless UI text tiles therefore require an RGB
  or 4:4:4 source; on a 4:2:0 passthrough stream they degrade to near-lossless.
- Two reference layouts exist in the decoder, which is a real cost in `vk/` and `ref/`: the predictor,
  the concealment warp and the reconstruction write path must each handle both. The bit-exactness
  rules of ADR-0010 apply identically to both, and conformance vectors must cover both.
- Chroma is at half resolution in the passthrough path even for fovea tiles, so the per-tile 4:4:4
  chroma mode of paper 5.2 has no effect there. That is a property of the source, not a codec choice.
- The foveation interaction changes for this path: the compositor has already foveated the input, so
  the codec's own `res_level` stacks on top of an existing non-uniform mapping, exactly as paper 1.1
  anticipated for the tile grid. ADR-0008 still holds, because the codec's reference is uniform in the
  streamed image whatever the compositor did before it.
- `color_space` is a stream-level field, not per tile or per frame: a stream does not change colour
  space mid-session.

## Alternatives considered

- **Always YCoCg-R, converting 4:2:0 sources on the way in.** Rejected: two conversions and a larger
  reference for a source whose chroma resolution is already gone.
- **Always YCbCr, dropping YCoCg-R entirely.** Rejected: it gives up integer-reversible lossless,
  which is what makes quad-layer text and MR passthrough alpha work, and it needs multiplies.
- **Per-tile colour space.** Rejected: it would mean two reference formats live in one frame, and the
  predictor would have to convert across them.
- **Negotiating colour space out of band, outside the bitstream.** Rejected: a stream must be
  self-describing for the reference decoder and the conformance vectors to mean anything.

## References

- Paper 1.3 (colour), 3.2.5 (memory traffic), 1.8 (lossless), 5.2 (chroma per eccentricity)
- [docs/SYNTAX.md](../SYNTAX.md) for the normative field, [docs/INTEGRATION.md](../INTEGRATION.md) for
  the WiVRn NX pixel path
- ADR-0012 (YCoCg-R), ADR-0010 (bit-exactness), ADR-0008 (foveation)
