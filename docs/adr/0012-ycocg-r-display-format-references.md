# ADR-0012: YCoCg-R, with references stored in display format

- **Status**: Accepted. Partially superseded by ADR-0021 for 4:2:0 sources.
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.3
- **Affects**: `ref/`, `vk/`, `warp/`

## Context

The colour transform sits on the normative path, so it must be integer-reversible (the lossless mode
depends on it), cheap on both ends, and free of live patents. It also determines what the reference
image is, which determines the memory traffic of the single most bandwidth-heavy stage of the decoder.

## Decision

YCoCg-R, full range, 8-bit in v1 with a 10-bit tool bit, 4:4:4 or 4:2:0 selected per tile.

YCoCg-R (Malvar and Sullivan, 2003) is a lifting transform: `Co = R - B; t = B + (Co >> 1);
Cg = G - t; Y = t + (Cg >> 1)`. It is exactly invertible in integers, costs 4 adds and 2 shifts per
pixel per direction, and needs one extra bit on the chroma planes (9-bit chroma for 8-bit RGB). It was
offered royalty-free for H.264 FRExt and is in any case past 20 years.

Chroma subsampling for a 4:2:0 tile is a rounded 2x2 average of Co and Cg; upsampling on decode is the
fixed half-phase bilinear with weights 3/4 and 1/4. Both are bit-exactly specified so the encoder's
reference model matches.

**The client stores references in display format** (RGBA8 or RGB10A2), one image per reference slot,
four slots. YCoCg-R round-trips exactly through RGB, so there is no drift, and the reprojection shader
samples the reference image directly with zero copies.

Alpha is a fourth plane coded with the luma tools at its own QP, or a constant, or absent, which is
what makes quad layers and MR passthrough first-class without a second stream.

## Consequences

- Lossless is reachable: `tskip = 1` at QP 0 with 4:4:4 is exactly reversible end to end.
- Display-format references cost 6 ops per pixel each way at prediction and reconstruction time, and
  save the 2 bytes per pixel of extra bandwidth a planar 16-bit YCoCg reference would cost. The paper
  puts that saving at 56 MB per stereo frame, about 1.4 ms of pure traffic at 40 GB/s (estimate).
- The output image doubles as the next reference, so there is one write, not two.
- **This applies to RGB sources.** Where the source is already YCbCr 4:2:0, converting to YCoCg-R and
  storing RGB references would add two conversions and inflate reference traffic for nothing. That
  case is handled by the stream-level `color_space` field in ADR-0021, which supersedes the
  display-format reference storage for the 4:2:0 path.
- BT.709 YCbCr as the codec's own transform was rejected because it is not integer-reversible, which
  kills the lossless mode, and it needs multiplies. ICtCp is deferred to an HDR tool bit.

## Alternatives considered

- **BT.709 YCbCr as the coding transform.** Rejected: not integer-reversible, needs multiplies.
- **ICtCp.** Deferred to an HDR tool bit: it requires the PQ nonlinearity and does not matter for SDR
  game output.
- **Planar 16-bit YCoCg references.** Rejected on bandwidth, see above.
- **RGB with no colour transform.** Rejected: it wastes about 30 percent of the bits for nothing.

## References

- Malvar and Sullivan 2003, YCoCg-R (offered royalty-free for H.264 FRExt)
- Paper 1.3 (colour), 1.8 (lossless), 3.2.5 (memory traffic)
- ADR-0021 (stream-level colour space and the 4:2:0 passthrough path)
