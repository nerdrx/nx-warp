# Glossary

Every term of art used in the NX Warp documents, with the paper section that defines it. Terms that
name a bitstream field are written as `code`. Where a number appears it is a design estimate from the
paper unless the entry says otherwise; nothing in this project has been measured yet.

## A

**AEAD** - authenticated encryption with associated data. Each datagram is AES-256-GCM with the
24-byte header as associated data, so the header is authenticated but readable by the receiver before
decryption. ChaCha20-Poly1305 is the negotiated fallback for hosts without AES instructions.
(paper 4.1, [THREAT_MODEL.md](THREAT_MODEL.md))

**`age`** - per tile position in a presentation ring slot, the number of frames since this position was
last decoded. 0 means fresh. Drives the HUD staleness heatmap. (paper 4.3)

**`age_since_intra`** - per-tile decoder state, 8 bits: frames since this tile was last coded intra.
Forces a refresh when it exceeds T = 180. (paper 2.6)

**`alpha_mode`** - tile header field: 0 opaque, 1 constant (a byte follows), 2 coded, 3 reserved. Alpha
is what makes quad layers and MR passthrough first class without a second stream. (paper 1.2, 1.8)

**AIMD** - additive increase, multiplicative decrease. The loss-driven half of the bitrate controller
inherited from WiVRn NX. (paper 4.6)

**ASW** - asynchronous spacewarp. Oculus's frame synthesis from motion vectors. Cited as the source of
the caveat that coded motion vectors are rate-distortion choices, not true motion. (paper 2.8)

## B

**Band** (row band) - six tile rows, 384 pixel lines, six bands per frame with the last one four rows.
The unit of pipelining, of feedback, of FEC grouping and of the presentation deadline. Bands rather
than single rows because each Vulkan submit costs an estimated 50 to 100 us on Adreno. (paper 4.2)

**Base layer** - layer 0 of a stream. `NATIVE` on the pure compute path, `HEVC_NAL` or `H264_NAL` on
the hybrid path. (paper 1.7, [ADR-0014](adr/0014-layered-bitstream-hybrid-mode.md))

**`base_qp`** - frame header field, 0 to 63. Per-tile `qp_delta` is relative to it. (paper 1.2)

**BBR** - the delivery-rate and RTT-gradient half of the bitrate controller, and the per-path rate
estimator used for multipath striping. (paper 4.6, 4.8)

**BD-rate** - Bjontegaard delta rate: the average bitrate difference between two codecs at equal
quality. The Phase 2 exit criterion is stated in BD-rate against x265 zerolatency. (paper 2.11, 3.11)

**Bypass symbol** - a raw bit coded with a uniform distribution directly on the rANS state
(`x = (x << k) | bits` on encode), 2 ops. Used for signs and escape suffixes. (paper 1.6)

## C

**CABAC** - context-adaptive binary arithmetic coding, the entropy coder of H.264 and HEVC. Rejected
here: serial per bin, and HEVC/VVC context designs are patent-live.
([ADR-0003](adr/0003-rans-eight-lanes.md))

**CAS** - contrast-adaptive sharpening. The per-pixel pass that hurt on the Pico 4 in the field, used
throughout the paper as the calibration point for what 4 to 5 ms of Adreno 650 GPU time feels like.
(paper 3.2.5)

**`CBF`** - coded block flag, one of the three rANS symbol types: is this 8x8 block coded at all.
2 symbols, 2 contexts (luma and chroma). (paper 1.6)

**Class A / B / C** - tile priority classes assigned from the foveation map, used by FEC and by
multipath striping. A is the fovea plus base-layer quad-layer (UI) tiles, B is mid eccentricity, C is
periphery and enhancement layers. (paper 4.4)

**`color_space`** - stream header field: `YCOCG_R` for RGB sources, `YCBCR_PASSTHROUGH` for sources
that are already YCbCr 4:2:0.
([ADR-0021](adr/0021-stream-level-color-space-ycbcr-passthrough.md))

**`concealed_count`** - per-tile decoder state, 8 bits: consecutive frames this tile was concealed.
At 3 the encoder escalates the tile to intra. (paper 2.6, 2.7)

**Concealment** - what the decoder does with a tile that did not arrive by the deadline: run the same
prediction kernel in `WARP_SKIP` with `last_mv`. There is no separate concealment code path, which is
why the encoder can replay it exactly. (paper 2.7)

## D

**DC plane** - the 64 DC coefficients of a tile's luma blocks, treated as an 8x8 low-resolution image,
transformed with a second-level 8x8 DCT and coded first. Pixels are then predicted by bilinear
interpolation between the four nearest block DCs. This is v1's entire intra prediction, and the fourth
rung of the degradation ladder. (paper 3.2.4,
[ADR-0004](adr/0004-dc-plane-intra-no-directional-modes.md))

**Dead-zone quantiser** - `q = sign(c) * floor((|c| + f * step) / step)` with f = 1/3 for intra and
1/6 for inter. Reconstruction has no offset, so the decoder does one multiply and one shift per
coefficient. (paper 1.5)

**Deadline** - the moment the client stops waiting for tiles and presents what it has:
`predicted_display_time - reproject_budget - runtime_margin`. It moves 1 ms earlier (up to 4 ms) when
more than 10 percent of tiles are late for 5 consecutive frames, and relaxes 0.2 ms per clean second.
(paper 4.3)

**Degradation ladder** - the normative order in which rate control spends a shortfall, per tile class,
so that the picture loses texture before structure. See "blur, never block". (paper 4.6.1,
[ADR-0013](adr/0013-degradation-ladder-blur-never-block.md))

**Display format** - RGBA8 or RGB10A2. What the client stores references in on the `YCOCG_R` path, so
the reprojection shader samples them with zero copies. (paper 1.3)

**DPB** - decoded picture buffer, the reference list of a conventional codec. This codec has no DPB: it
has a four-slot ring and per-tile reference selection. (paper 2.6, 4.5)

## E

**E0 to E5** - the six encoder compute passes: E0 warp, E1 analyze, E2 transform, E3 reconstruct,
E4 entropy, E5 packetize. E3 is byte-identical SPIR-V to the decoder's Pass B. (paper 3.6)

**`enc_us`** - datagram header telemetry field: encode finish minus render finish for this band.
(paper 4.1, 4.9)

**`ENT_BITPLANE`** - tool bit for the bit-plane entropy coder, the Lite-profile fallback that needs no
tables and no LDS lookup table. (paper 1.6)

**`ENT_OFFSET_TABLE`** - tool bit for per-substream byte offsets, the fallback where subgroup ballot is
unavailable or slow. About 8 extra bytes per tile. (paper 1.6, 3.12)

**Eccentricity** - angular distance from gaze (or from the lens axis under fixed foveation). Drives
sample scale, QP offset and chroma mode. (paper 5.1)

## F

**FEC** - forward error correction. Reed-Solomon over GF(256), systematic, k = 10 data datagrams per
group, groups never crossing a band boundary, parity count by tile class (3 / 1 / 0 for A / B / C).
Blended overhead about 14.5 percent. (paper 4.4)

**Feedback packet** - client to server, one per band, about 100 bytes, cumulative over the last three
bands so a lost feedback costs nothing. Carries the received-tile bitmap, `decode_us`, conceal and late
tile counts, per-path loss and RTT, and FEC recovered/failed counts. (paper 4.4)

**Foveation map** - one R8 texel per codec tile, generated per frame on the server from gaze or lens
centre, lens model, head velocity and content class. One map, three consumers: the app's render pass
through VRS, the encoder, and the client's reprojection. (paper 5.1.1)

**FovVideoVDP** - Mantiuk et al. 2021, the primary objective quality metric for this project. It takes
gaze, display geometry, luminance and temporal content and outputs a JOD score, and it is run in
display space, after the real reprojection shader, so that warped-reference concealment is charged for
what it actually shows. (paper 5.3)

**Frame ring** - four full-size images per stream into which the decoder writes; tile (N, t) lands in
slot N mod 4. Also the reference ring, since the output doubles as the next reference. (paper 4.3)

**Frameless presentation** - presenting at a deadline with whatever tiles arrived, each output tile
warped from its own `pose_seq` to the display pose. It lives inside the client's reprojection pass,
immediately before `xrEndFrame`; true per-row scanout is out of reach on Android runtimes. (paper 4.3)

**FTO** - freedom to operate. The scoped review of four items required before Phase 3 ships.
([ADR-0017](adr/0017-fto-review-scope.md))

## G

**Governor** (decode-time governor) - the control loop that keeps decode time at or under 40 percent of
the frame period by removing pixels of work, never bits: drop class C enhancement, class C to the
half-resolution base, shrink the fovea radius 10 percent, drop class B enhancement, 90 to 72 Hz. Steps
down after 3 bad frames, up after 180 good ones. (paper 4.7)

**Gradient coherence** - the ratio of structure-tensor eigenvalues over a tile, one pass over the
pixels. With log-variance activity it classifies tiles as text, edge, texture or flat for the
degradation ladder. (paper 4.6.1)

## H

**H (homography)** - the per-eye rotation-only warp matrix, `H = K_e R_{N-1}^T R_N K_e^-1`, quantised
to nine int32 in Q8.24 with `h22 = 2^24` and sent in the frame header, 36 bytes per eye. (paper 2.2)

**Hybrid mode** - layer 0 is a hardware HEVC or H.264 stream through MediaCodec and the enhancement
layers are compute tiles. A compatibility mode, not the latency path.
([ADR-0014](adr/0014-layered-bitstream-hybrid-mode.md))

## I

**IDR** - instantaneous decoder refresh, the whole-frame keyframe of conventional codecs. This codec
has none. Full intra happens only on stream start, profile change, or a bitmap history gap.
([ADR-0006](adr/0006-acknowledged-neighbourhood-references-no-idr.md))

**`INTRA_DIR`** - v2 tool bit for directional intra, promoted to v1 only if Phase 1 shows more than a
40 percent bit gap against x264 intra. (paper 1.4, 6.4)

**Interleaved rANS** - Giesen's construction: N rANS substreams read from one byte stream in a fixed
lockstep order, with each lane's read offset computed from a subgroup ballot. No offset table.
(paper 1.6, 3.2.2)

## J

**JOD** - just objectionable difference, the unit FovVideoVDP reports. (paper 5.3)

## L

**`LAST`** - the second rANS symbol type: the zigzag index of the last nonzero coefficient in a block.
16 symbols, 2 contexts. (paper 1.6)

**`last_mv`** - per-tile decoder state: the previous vector, used both as the temporal MV predictor and
as the concealment vector. (paper 2.6)

**Layer** - one of 1 to 4 per stream, described by `layer_desc[n]`: type (`NATIVE`, `HEVC_NAL`,
`H264_NAL`) and scale (1/1, 1/2, 1/4). (paper 1.2, 1.7)

**LCEVC** - MPEG-5 Part 2, V-Nova's base-plus-enhancement codec. The closest existing shape to the
hybrid path and the strongest patent overlap in the design. (paper 1.7, 5.6,
[ADR-0017](adr/0017-fto-review-scope.md))

**LDS** - local data share, the GPU's on-chip shared memory. 32 KB on Adreno 650, which is what bounds
the rANS lookup tables (8 KB) and the transform transpose buffer (8 KB). (paper 1.6, 3.2.2, 3.2.3)

**`LEVEL`** - the third rANS symbol type: `min(|q|, 14)` for every coefficient up to `LAST` in reverse
zigzag order, plus ESC followed by Exp-Golomb order-3 raw bits. 16 symbols, 8 contexts. (paper 1.6)

**Lite / Full / Pro** - the three profiles. See [ARCHITECTURE.md](ARCHITECTURE.md#9-profiles-and-capability-negotiation).

**Loeffler factorization** - Loeffler, Ligtenberg and Moschytz 1989 (expired), the 8-point DCT
factorization used here with the project's own 9-bit integer constants. (paper 1.4)

## M

**MediaCodec** - Android's hardware codec API. Needs whole access units, which is why the hybrid path
cannot pipeline its base layer per row. (paper 1.7, 3.5)

**`mode`** - tile header field selecting `WARP_SKIP`, `WARP_MV`, `STATIC_MV`, `STEREO` or `INTRA`.
(paper 2.3, 6.5)

**MV** - motion vector. One per tile, quarter-pel, coded as a delta from the same tile's previous
vector, range plus or minus 64 px.
([ADR-0005](adr/0005-one-mv-per-tile-five-modes.md))

## N

**`nsub_log2`** - tile header field: the number of rANS substreams is 2^n. v1 fixes eight lanes; the
field survives so v2 can vary it under a tool bit. (paper 1.2, 6.3)

## P

**Pass A / B / C** - the decoder's compute passes. A is entropy decode (64 threads, 8 tiles per wave,
8 rANS lanes per tile), B is reconstruct (256 threads, one 64x64 tile), C is hybrid enhancement.
(paper 3.2)

**`path_id` / `path_seq`** - per-path identifier and sequence number in the datagram header, so loss
and reordering are measured per path and a stall on one path never blocks the other. (paper 4.1, 4.8)

**`pose_seq`** - 16-bit index into the client's own two-second pose ring: the render pose the server
used. The pose is not sent downstream except in the frame header.
([ADR-0007](adr/0007-pose-travels-twice.md))

**ppd** - pixels per degree. `ppd_render(theta) = ppd_center / cos^2(theta)` for a rectilinear target;
`ppd_needed(e) = 60 / (1 + e/2.3)` for the eye. Their ratio picks the sample scale. (paper 5.1.2)

## Q

**`qp_delta`** - tile header field, signed 6 bits, -32 to +31 relative to `base_qp`. The primary
perceptual control. (paper 1.2, 1.5)

**Quad layer** - an OpenXR composition layer that is not the projection view: menus, HUDs, desktop
panels. Usually head-locked (`STATIC_MV`), often text (lossless class), and class A for FEC.
(paper 2.3, 5.5)

## R

**rANS** - range asymmetric numeral systems (Duda). 32-bit state, L = 2^16, 16-bit renormalisation,
10-bit probability precision. ([ADR-0003](adr/0003-rans-eight-lanes.md))

**`ref_delta`** - 2-bit datagram header field: the reference frame is `frame_id - 1 - ref_delta`, and
3 means intra. (paper 4.5)

**Reference eligibility** - the rule that a tile may only reference a frame whose 3x3 tile
neighbourhood around it is fully acknowledged, because the warp reads across tile borders.
([ADR-0006](adr/0006-acknowledged-neighbourhood-references-no-idr.md))

**`res_level`** - tile header field: the tile is coded as a 64x64, 32x32 or 16x16 image and upsampled
at reconstruction. The predictor is formed at the coded resolution and the client always holds a
full-resolution reference. (paper 1.5, 6.8)

**Retinal slip** - content motion the eye does not track. Head rotation alone does not blur the retinal
image because the vestibulo-ocular reflex counter-rotates the eye; slip is what `dQ_motion` responds
to, and the codec knows it exactly as the per-tile residual motion after the pose warp. (paper 5.2)

**Rolling intra refresh** - 1/T of the tiles coded intra every frame, T = 180 (2 s), selected by a
fixed pseudo-random permutation so there is no visible refresh wave. Estimated cost about 0.014 bpp.
(paper 2.6)

## S

**SATD** - sum of absolute transformed differences, the 4x4 Hadamard distortion measure used in the
encoder's mode decision. (paper 2.3)

**Shadow (client shadow)** - the encoder's mirror of what the headset holds: the last eight frames as
bitstream plus decoded pictures, with lost tiles filled by replaying the client's deterministic
concealment. Prediction is only ever formed against the shadow. (paper 2.6, 4.5)

**`SKIP_STATIC`** - see `STATIC_MV`. The tile header of paper 1.2 and the mode table of paper 2.3 use
different names for the same idea; the reconciled names are those of paper 6.5.

**`skip_bitmap`** - tile-row header field, 64 bits: bit i set means tile i of this row is `WARP_SKIP`
and is not sent. A static periphery tile costs one bit rather than a header. (paper 1.2)

**`STATIC_MV`** - tile mode: predict from the previous frame with no warp. For head-locked content,
where the warp is exactly wrong and the identity predictor is exactly right. Excluded from
motion-smoothing extrapolation. (paper 2.3, 2.8)

**`STEREO`** - tile mode: the right eye predicts from the decoded left eye of the same frame, with a
per-tile disparity vector seeded from f * IPD / d. Phase 4, off in Lite. Expected 5 to 10 percent
overall and 30 to 40 percent on intra-heavy frames. (paper 2.5)

**Substream** - one rANS lane within a tile. Substream i owns 8x8 blocks i, i+N, i+2N and so on in
block order, so each lane's coefficient context is causal within the lane and needs no cross-lane
traffic. (paper 1.6)

**Subgroup ballot** - the cross-lane operation (`subgroupBallot` plus an exclusive bit count) that
gives each renormalising rANS lane its read offset from the shared cursor. Its availability and cost
on Adreno is the load-bearing assumption of the entropy layout and a Phase 0 gate check.
(paper 1.6, 1.12)

## T

**Tile** - 64x64 luma samples, its own bitstream, decoded by one workgroup. The unit of decode, mode,
motion vector, QP, reference selection and concealment.
([ADR-0002](adr/0002-64x64-tiles.md))

**Tile directory** - the 4-byte per-tile entry (QP, mode, byte length) at the head of a tile run's
payload. (paper 4.1)

**Tile run** - the datagram: a contiguous sequence of tiles from one tile row, packed to a 1400-byte
budget. The loss unit.
([ADR-0001](adr/0001-datagram-is-a-tile-run.md))

**Timeline semaphore** - the Vulkan synchronisation primitive that carries every handoff in the system.
Value 8F + g means frame F, band g. (paper 3.6)

**`tools`** - the 64-bit mandatory tool bitmask in the stream header. Negotiation is an intersection:
the server may only set bits the client offered, and a decoder that sees an unknown mandatory bit
refuses the stream. Bits 0 to 31 are defined in v1. (paper 1.2)

**`tskip`** - tile header bit: the whole tile is transform-skip, the lossless path. (paper 1.2, 1.8)

**`tx_ts`** - datagram header field: server clock in microseconds, wrapping at 71 minutes, used with
the clock-offset estimator for one-way delay. (paper 4.1, 4.9)

## V

**VRS** - variable rate shading (`VK_KHR_fragment_shading_rate`). Where the render-cost win of
foveation goes once the codec stops relying on a continuous remap. (paper 5.1.1, 6.8)

## W

**`WARP_MV`** - tile mode: predict from the pose-warped previous frame with a coded vector and a coded
residual. The workhorse inter mode. (paper 2.3)

**`WARP_SKIP`** - tile mode: predict from the pose-warped previous frame with no vector and no
residual. Signalled only through the row skip bitmap, so it costs one bit. Also the concealment mode.
(paper 2.3, 2.7)

**`wgt`** - tile header field: the blend weight (0, 1/4, 1/2, 3/4, 1) between the spatial hypothesis
(the upsampled layer below) and the temporal hypothesis (the pose-warped previous reconstruction of
the same layer) in an enhancement layer. MPEG-2 era bi-prediction weighting, expired. (paper 1.7)

## X

**`XFORM_WAVELET`** - v2 tool bit reserving the 5/3 wavelet transform, the serious alternative to the
8x8 DCT: multi-resolution for free, but six barriers per tile and a whole-tile dependency chain.
(paper 1.4)

**XR2 Gen 1** - the Snapdragon SoC in the Pico 4. Adreno 650 GPU, an estimated 300 G int32 simple ops
per second sustained and about 25 GB/s of GPU-usable memory bandwidth. The device the whole compute
budget argument is about. (paper 3.1)

## Y

**YCoCg-R** - the reversible lifting colour transform (Malvar and Sullivan 2003):
`Co = R - B; t = B + (Co >> 1); Cg = G - t; Y = t + (Cg >> 1)`. Integer-reversible, 4 adds and 2
shifts per pixel per direction, one extra bit on chroma.
([ADR-0012](adr/0012-ycocg-r-display-format-references.md))
