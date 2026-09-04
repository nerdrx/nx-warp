# ADR-0011: Vulkan compute on both ends, not fixed-function video hardware

- **Status**: Accepted
- **Date**: 2026-09-04 (design paper draft 1)
- **Source**: paper abstract, 3.1, 5.6, 5.8
- **Affects**: everything

## Context

Every existing PC-to-headset streamer (ALVR, WiVRn, Virtual Desktop, Steam Link, Meta Air Link,
CloudXR) uses hardware H.264, HEVC or AV1 through NVENC, AMF, VAAPI and MediaCodec. That path is
mature, free in engineering effort, and decodes at very low power. It also fixes the ceiling:

- Frame-granular latency by construction. MediaCodec wants whole access units and adds an estimated
  8 to 12 ms on an XR2 Gen 1, plus one or two frames of queue when low-latency mode silently fails.
- A fixed pixel rate. The XR2 Gen 1 decoder cannot decode two 2160x2160 streams at 90 Hz, which is why
  every hardware-codec streamer encodes below panel resolution.
- A fixed format list: no 4:4:4, no alpha, no tiles as loss units, no per-tile references.
- Vendor session ceilings and a hard NVENC/AMF/VAAPI dependency on the PC.
- No way to put the head pose into the predictor, and no way to refresh one lost tile without an IDR
  or a slice trick.

Against that, compute decoding costs GPU time the hardware path does not spend at all: an estimated
4 to 6 ms per frame on an Adreno 650 that already struggled with a single per-pixel sharpening pass
(paper 3.1, 3.2.5).

## Decision

Both the encoder and the decoder are Vulkan compute. The codec is vendor-neutral on the PC, which
removes the NVENC/AMF/VAAPI dependency and its session ceilings; on the headset the decoder is
compute where the device can afford it and a hybrid hardware base plus compute enhancement where it
cannot (ADR-0014).

The bet is stated plainly in the paper (5.8): headset SoC compute multiplies every generation
(XR2 Gen 2 is about 2.5x the GPU of Gen 1, plus an NPU) while hardware video decoders stay bound to a
fixed pixel rate, a fixed format list and a fixed feature set that changes only with new silicon. A
compute codec's ceiling rises with every SoC and its tools change with a software update.

## Consequences

- The project owns its own latency: row-band pipelining, deadline presentation and per-tile references
  all become possible, and the estimated render-to-photon floor is 12 to 23 ms against about 100 ms
  measured today for the hardware path (paper 4.2, 7.1).
- It also owns the risk: the Pico 4 may not have the GPU time. The paper's own expectation (6.10) is
  that the Pico 4 lands in hybrid mode. See ADR-0015.
- Power is a real cost, not just time: compute decode heats the XR2 more than the hardware decoder,
  and the Phase 0 gate must measure decode watts, not only milliseconds (paper 4.11).
- On the Pico 4 the hardware H.264 decoder costs no GPU time at all, so this codec cannot beat it on
  headset GPU cycles. It has to win on bits during head motion, on latency, on loss behaviour, and on
  what a low bitrate looks like (paper 4.6.1). Those four are the contest and they are stated as the
  contest.
- Every tool in the codec is now constrained by "one workgroup per tile, no serial state", which is
  what rules out CABAC, directional intra wavefronts, and per-pixel depth warping.

## Alternatives considered

- **Hardware codecs with better plumbing** (sliced encode, low-latency mode, tuned de-jitter). This is
  what Meta Link and CloudXR do; it is real and it works, and its floor is frame-granular. Rejected as
  the goal, kept as the hybrid base (ADR-0014).
- **Object-space streaming** (Shading Atlas Streaming, QuadStream). The strongest alternative
  architecture, and it needs engine integration, which this project refuses to require (paper 5.6).
- **Intra-only wavelet codecs in compute** (JPEG XS, VC-2). They prove the compute decode point and
  are the shape of Phase 1, but with no temporal redundancy 4K90 stereo sits at 500 Mbit to 1 Gbit,
  which is USB only (paper 5.6).
- **A zero-compute fallback**: each tile as an ASTC 8x8 block set, 2 bpp fixed, decoded for free by
  the sampler. 1.7 Gbit/s at 2160 square stereo 90 Hz, viable over USB 3 with foveation. Noted in the
  paper (5.6) as a useful idea, not part of v1.

## References

- Paper abstract, 3.1 (working numbers), 3.2.5 (honest CAS comparison), 5.6 (landscape), 5.8, 7.1, 7.2
- ADR-0014 (hybrid), ADR-0015 (compute budget verdict)
