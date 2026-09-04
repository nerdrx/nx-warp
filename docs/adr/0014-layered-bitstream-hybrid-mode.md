# ADR-0014: One layered bitstream serves pure compute and hybrid decode

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper 1.7, 2.9, 3.5
- **Affects**: `hybrid/`, `ref/`, `vk/`, `android/`

## Context

The pure compute decoder may not fit on an Adreno 650 (ADR-0015). The obvious fallback, a separate
hardware-codec path, means two codecs to maintain, two transports, two loss models and two rate
controllers. Design principle 5 says instead: one format, many decoders, with capability bits rather
than forks.

## Decision

A stream has 1 to 4 layers. Layer 0 is the base and its type is declared in `layer_desc[0]`:

- `NATIVE`: the pure compute path. Layer 0 is ordinary codec tiles.
- `HEVC_NAL` or `H264_NAL`: the hybrid path. Base packets carry ordinary access units for MediaCodec.

Everything above the base is the identical tile format in both paths. Each enhancement tile has two
predictor hypotheses: the upsampled reconstruction of the layer below (spatial) and the pose-warped
previous reconstruction of the same layer (temporal). The `wgt` field blends them with weights of 0,
1/4, 1/2, 3/4 or 1 in units of the spatial hypothesis, which is MPEG-2 era bi-prediction weighting,
expired.

In the hybrid path the decoded `AHardwareBuffer` is imported into Vulkan with no copy
(`vkGetAndroidHardwareBufferPropertiesANDROID`, `VkExternalFormatANDROID`, a
`VkSamplerYcbcrConversion`, and the sync fd imported as a binary semaphore) and Pass C reads the base
through the YCbCr sampler. The sampler is allowed there because the base is not on the normative
bit-exact path; only the residual is (ADR-0010).

The client's `tools` mask and `layer_desc` type are the only things that differ. A weak headset gets
pose-warped enhancement on top of a half-resolution HEVC base; a strong headset drops MediaCodec
entirely.

## Consequences

- The tiling, transport, FEC, reference tracking, shadow model, rate control and telemetry are shared.
  Hybrid is not a second codec to maintain.
- Hybrid gives up the properties that matter most: MediaCodec needs whole access units, so a full HEVC
  decode (estimated 8 to 12 ms on an XR2 Gen 1) happens before any enhancement tile can be
  reconstructed, tile-row pipelining applies only to the enhancement layer, and a lost base packet
  goes through HEVC's own reference invalidation rather than per-tile concealment. **Hybrid is the
  compatibility mode, not the latency path**, and the paper says so repeatedly.
- Bit-exactness of the base holds only as far as the hardware HEVC decoder is deterministic across
  devices. The encoder must model the base as "approximately what the client decoded", by decoding its
  own HEVC stream (hardware decode on the PC, adding an estimated 1 to 2 ms), and must bound drift
  with a periodic native refresh tile (paper 1.12 risk 4, 2.9).
- Pass C costs one extra image write and read (the residual image, 33.5 MB at full resolution or
  8.4 MB at half for Lite) over the pure compute path.
- Patent exposure: the enhancement-over-hardware-base structure is the closest existing shape to
  LCEVC, which is commercially licensed by V-Nova, and it is one of the four items in the FTO review
  (ADR-0017). The safe fallback if the review goes badly is to disable the temporal hypothesis in
  enhancement layers, leaving plain spatial scalability as in H.263 Annex O (1998, expired).

Differences from LCEVC, stated so the review has something to work with (paper 1.7): LCEVC codes
residuals with small Hadamard-like transforms and predicts the residual layer temporally with zero
motion, so its enhancement is residual-on-residual. This codec predicts pixels, not residuals, uses
the same DCT and rANS tools as the base, warps the temporal hypothesis by head pose with a per-tile MV
correction, and blends two hypotheses with explicit weights.

## Alternatives considered

- **A separate hardware-codec fallback path.** Rejected: two of everything, and no shared loss model.
- **Pure compute only, and no support for weak devices.** Rejected: the first target device is a
  Pico 4, which the paper expects to land in hybrid mode.
- **LCEVC itself as the enhancement layer.** Rejected on licensing.
- **Spatial-only enhancement (no temporal hypothesis).** Kept as the safe fallback, not the default,
  because the temporal hypothesis is where the detail the base lacks comes from.

## References

- Paper 1.7 (layers and the LCEVC comparison), 2.9 (hybrid mode), 3.5 (implementation and the
  zero-copy data path), 6.10
- ADR-0015 (compute budget), ADR-0017 (FTO scope)
