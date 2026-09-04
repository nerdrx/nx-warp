# Hybrid mode: design consequences

Companion to PAPER.md 1.7 (layers), 2.9 (the BASE predictor), 3.5 (MediaCodec
into AHardwareBuffer), 5.7 (patents) and 6.10 (the Pico 4 verdict). The
measurements behind every claim here are in [`../hybrid/RESULTS.md`](../hybrid/RESULTS.md);
this document is what those measurements mean for the bitstream, the
transport, the decoder and the FTO review.

Hybrid mode is the path where **layer 0 is an ordinary HEVC stream decoded by
the device's hardware decoder** and layer 1 is our tile format, predicted from
two hypotheses: the upsampled base, and the pose-warped previous output. It
exists because PAPER.md 6.10 expects the Adreno 650 in the Pico 4 not to
afford the full compute decoder, and because a weak client and a strong client
should differ in one `layer_desc` field rather than in a second codec.

---

## 1. Bitstream

### 1.1 Layer 0 as an HEVC NAL container

`layer_desc[0].type` is `HEVC_NAL` instead of `NATIVE`. Everything above
layer 0 is byte-identical in both paths -- same 64x64 tile grid, same tile
headers, same transform, same entropy coder. That invariant is the whole
value of hybrid mode and must not be traded away for a local optimisation in
the base path.

Layer 0 packets carry whole HEVC access units, because MediaCodec will not
accept less (3.5). Concretely:

* The base stream is a single-IDR, P-only, `ref=1` HEVC stream: no B frames,
  no reordering, no lookahead, `scenecut=0`. VPS/SPS/PPS are repeated at
  every IDR and cached by the client so a mid-stream join needs no side
  channel.
* Each access unit is prefixed by a small layer-0 packet header carrying the
  frame index, the presentation timestamp and a flag for "this AU is an IDR".
  The frame index is the *same counter* the enhancement layer uses; this is
  the only sync primitive the design needs (section 2).
* The base AU is fragmented across datagrams by size alone. It is not tiled,
  it has no per-tile loss behaviour, and it cannot be partially decoded. A
  lost base fragment invalidates the whole AU and the client must fall back
  to enhancement-only reconstruction for that frame (section 4.3).

This is a real regression against the pure path, where the datagram is a tile
run and loss is local (PAPER.md 4.1, 4.5). Hybrid mode gives up the codec's
best property on the layer that carries most of the picture. It should be
described that way in release notes, not as a mode with a latency footnote.

### 1.2 What the enhancement layer must signal

Per stream, once, in `layer_desc[0]`:

| Field | Bits | Meaning |
|---|---|---|
| `type` | 4 | `HEVC_NAL` / `H264_NAL` / `NATIVE` |
| `base_w`, `base_h` | 16 + 16 | base resolution, which need not divide the enhancement resolution |
| `base_range` | 1 | limited (0) or full (1) range of the decoder output |
| `base_matrix` | 3 | BT.601 / BT.709 / BT.2020 for the YCbCr -> YCoCg-R step |
| `upsample_filter` | 2 | bilinear / Catmull-Rom / (reserved) |
| `base_profile_tier_level` | 24 | so the client can reject a stream its decoder cannot open before any pixels move |

Per frame, in the enhancement frame header, in addition to what the pure path
already carries (the nine Q8.24 homography coefficients per eye, 2.2):

| Field | Bits | Meaning |
|---|---|---|
| `base_frame_index` | 16 | which base AU this enhancement frame is predicted from |
| `base_valid` | 1 | encoder-side assertion that the base for this index was emitted |
| `wgt_alphabet` | 1 | 2-bit (4 weights) or 3-bit (5 weights) blend alphabet for this frame |

Per tile, in addition to the pure path's mode, QP delta and MV:

| Field | Bits | Meaning |
|---|---|---|
| `wgt` | 2 (or 3) | blend weight in units of the spatial hypothesis |

The measured cost of the per-tile weight is about 2 bits per 64x64 tile, i.e.
2048 tiles x 2 bits x 90 Hz = 0.37 Mbit/s at the Pico 4 operating point --
under 0.4% of a 100 Mbit stream. It is not worth compressing.

**The weight alphabet.** PAPER.md 1.7 specifies five weights
{0, 1/4, 1/2, 3/4, 1}. The sweep ran the 2-bit subset {0, 1/4, 1/2, 1} as the
default. RESULTS.md reports what the fifth weight is worth; the field is
signalled per frame so the encoder can switch, and v1 clients must implement
both.

### 1.3 Why the weight is per tile and not per stream

The measurement that matters most in RESULTS.md is the *tile class breakdown*
of which hypothesis wins. The answer is not uniform across a frame: flat
regions are indifferent, high-frequency texture and text prefer the temporal
hypothesis (it carries full-resolution detail the base has thrown away), and
screen-space movers prefer the base (the warp cannot predict them and a
per-tile MV only partly can). A per-stream or per-frame weight would have to
pick one of those and lose on the rest. This per-tile split is the single
largest structural difference from LCEVC (section 5) and it is where hybrid
mode earns its keep.

---

## 2. Synchronisation

The enhancement layer for frame N is useless without base frame N. Three
facts make this harder than it sounds:

1. MediaCodec is a queue with its own latency, not a function call. The base
   for frame N arrives 8 to 12 ms after its last slice, and can arrive a full
   frame late if `KEY_LOW_LATENCY` silently fails on a vendor stack (3.5).
2. The enhancement tiles for frame N arrive incrementally and *earlier*,
   because they are tile runs that pipeline by row (4.2).
3. The presentation deadline is fixed (4.3). Something must be shown.

The design consequence: **the enhancement layer is buffered against the base,
never the other way round.**

* The client keeps a small map from `base_frame_index` to the imported
  `AHardwareBuffer`. Enhancement tiles that arrive before their base sit in
  their tile buffer; they cost nothing to hold because they are already in
  GPU memory.
* At the deadline, if base N has not been delivered, the decoder runs the
  frame with `wgt` forced to 0 for every tile -- pure temporal hypothesis --
  and sets the per-tile `extrapolated` flag, exactly as loss concealment does
  in 2.7. The encoder can replay this decision because `base_valid` and the
  deadline policy are deterministic given what the client acknowledges; it is
  not a new code path.
* If the base arrives *late but before the next deadline*, it is dropped, not
  applied retroactively. Applying a stale base would introduce a reference
  the encoder did not model and the layer would drift.
* `base_frame_index` mismatch is a hard error, not a warning: it means the
  client's base decoder and the encoder's mirror have diverged, and the only
  safe response is to request an IDR.

The encoder mirrors the base by decoding its own HEVC stream (2.9), which
costs 1 to 2 ms on the PC with hardware decode. It must decode, not merely
re-encode from the source, because the enhancement residual is defined
against the *decoded* base.

### 2.1 The bit-exactness boundary

This is where the normative story changes, and it deserves to be stated
plainly. In the pure path every step from bitstream to output image is
bit-exact by construction (2.2, "Determinism: integer warp"), so the encoder's
reference and the decoder's reference are the same image on every vendor.

In hybrid mode that guarantee stops at the base. HEVC decoding is itself
normative and bit-exact, so the decoded YCbCr samples agree; but the step from
the decoder's output to our domain does not, unless it is pinned:

* The hardware decoder emits limited-range YCbCr in a vendor-tiled format
  (typically UBWC NV12), sampled through a `VkSamplerYcbcrConversion`.
  Sampler-based conversion is *not* bit-exact across vendors.
* Therefore the spatial hypothesis must be produced by a **defined integer
  conversion**: limited-range YCbCr -> full-range -> YCoCg-R with specified
  rounding, done in the shader from the raw planes, not by the sampler's
  built-in colour conversion. 3.5 permits the sampler for the base "because
  the base is not in the normative bit-exact path"; that is only tenable if
  the *residual* is the sole normative quantity, which in turn requires the
  encoder to reconstruct exactly what the client will. The cheaper and safer
  rule is: pin the conversion, keep the whole layer bit-exact, and treat the
  sampler shortcut as a profile the conformance suite must catch.
* Chroma siting for the upsample must be specified too (the base is 4:2:0 and
  the enhancement grid is not).

Recommendation for v1: `base_range` and `base_matrix` are signalled, the
conversion is integer and specified, and the conformance vectors in
`tests/vectors/` gain a hybrid case whose base is a fixed HEVC AU committed to
the repo, so a vendor mismatch is a test failure rather than a field report.

---

## 3. Decoder pass structure on Android

The pure path is two dispatches: Pass A (interleaved rANS) and Pass B (one
workgroup per tile) (3.2.1). Hybrid adds a base import and one more pass.
The ordering below is chosen so that the only thing that waits on MediaCodec
is the pass that actually needs base pixels.

```
  network  --> layer 0 fragments  --> MediaCodec input  (whole AUs)
           \-> layer 1 tile runs  --> Pass A: rANS decode, per tile row
                                      Pass B: dequant + inverse DCT -> residual
                                              and the pose warp of Out(N-1)
                                              -> temporal hypothesis T
   MediaCodec output --> AImageReader --> AHardwareBuffer + sync fd
                                      --> VkImage (external format, cached)
                                      Pass C: S = upsample(base)
                                              P = w*S + (1-w)*T
                                              Out(N) = clamp(P + residual)
```

Consequences:

* **Pass A and Pass B do not depend on the base at all.** They can and must
  run as the tile rows arrive, which preserves tile-row pipelining for the
  enhancement layer even though the base cannot pipeline. This is the single
  most important scheduling decision in hybrid mode: it recovers most of the
  pipelining the base gives up, for the layer that carries the detail.
* **Pass C is the only base-dependent dispatch**, and it waits on a binary
  `VkSemaphore` imported from the AImageReader sync fd (`VK_KHR_external_semaphore_fd`,
  `SYNC_FD`). It is a single full-frame pass: upsample, blend, add, write.
* The buffer pool is small and recycled, so each distinct `AHardwareBuffer` is
  imported once and the import is cached by buffer identity; the
  `VkSamplerYcbcrConversion` and its descriptor set are cached with it.
  Re-importing every frame is a measurable stall.
* Release is asynchronous: Pass C's submit signals a semaphore exported as a
  sync fd and handed to `AImage_deleteAsync`. No `vkCmdCopyImage` anywhere.
* Extra memory traffic over the pure path is one full-resolution read of the
  base plus the blend. At 2 x 2048^2, 4:2:0, that is about 12.6 MB read per
  frame, 1.1 GB/s at 90 Hz -- roughly 3% of the memory bandwidth, negligible
  next to the 8-12 ms of decoder latency.

### 3.1 The latency ledger

| | Pure compute | Hybrid |
|---|---|---|
| Base decode | -- | 8-12 ms, whole-frame, before Pass C |
| Enhancement Pass A/B | pipelined by tile row | pipelined by tile row (unchanged) |
| Pass C | -- | ~0.5 ms |
| Tile-row pipelining applies to | everything | enhancement only |
| Loss granularity | one tile | one tile (enhancement), whole frame (base) |

The 8 to 12 ms is not amortised and not overlapped: it is a serial addition to
the client's critical path, because Pass C cannot start without it. At 90 Hz
one frame is 11.1 ms, so hybrid mode spends roughly one whole frame of
latency to avoid the compute decoder. Whether that is worth paying is the
question RESULTS.md answers with rate-distortion numbers; the answer must be
read together with this table, because a bitrate win of a few tenths of a dB
does not buy back a frame of latency in VR.

---

## 4. Rate control and failure modes

### 4.1 Two rate controllers, one budget

The base has its own rate control (x265's, or a hardware encoder's) and the
enhancement has ours (4.6). They must not fight. The design that works:

* The **split is fixed per operating point**, not per frame: the base gets a
  constant share of the total, chosen from the table in RESULTS.md, and its
  VBV is sized to about two frames so it actually spends its share evenly
  rather than hoarding for the next I-frame.
* The enhancement layer takes what is left, measured from the *actual* base
  AU sizes of the last few frames rather than from its nominal bitrate. The
  base overshoots on head turns; the enhancement must absorb that, because it
  is the layer that can degrade gracefully (4.6.1, blur before block).
* The base must never be given a share that pushes the enhancement below the
  floor where it can carry a per-tile QP delta and a weight for every tile
  (about 0.4 Mbit/s of pure signalling at the Pico 4 point). Below that the
  hybrid layer is doing nothing but adding latency and the encoder should
  switch the stream to base-only.

### 4.2 Head-turn behaviour

The base is a block-motion codec with a bounded search range, so it does what
WiVRn users already see: a bitrate spike, or a fallback to intra, on fast
head turns (2.2). In hybrid mode that spike lands *inside a fixed budget*,
which means it steals bits from the enhancement layer exactly when the pose
warp is most valuable. Two mitigations, both cheap:

* Give the base a lower share at high angular velocity. The server knows the
  pose derivative before it encodes; a two-point schedule (normal share,
  head-turn share) is enough.
* Let the enhancement's weight decision absorb it: on a head turn the base is
  poor and the temporal hypothesis is good, so tiles will select `wgt = 0`
  on their own. This is automatic and needs no signalling, and it is the
  clearest demonstration that the two-hypothesis design is not decoration.

### 4.3 What happens when the base is lost

A lost base AU is not recoverable per tile. The client:

1. reconstructs the frame with `wgt = 0` everywhere (temporal only) and marks
   every tile `extrapolated`;
2. reports the base AU as missing;
3. the encoder, on the same escalation ladder as 2.7, first continues (the
   enhancement layer alone holds up for a few frames because the temporal
   hypothesis is full-resolution), then requests a base IDR if the miss
   repeats.

A base IDR is expensive and visible. The FEC priority scheme of 4.4 should
therefore treat base AUs as the highest-priority class in hybrid mode -- the
opposite of the pure path, where foveal tiles come first, because in hybrid
mode a lost base costs a whole frame and a lost tile costs a tile.

---

## 5. LCEVC differentiation (for the FTO review)

PAPER.md 5.7 scopes the FTO review to four areas, of which
"the enhancement-over-hardware-base structure" is this document's. LCEVC is
MPEG-5 Part 2, commercially licensed by V-Nova, and it is the closest
published relative. The differences below are the ones that matter to a
claims reading, ordered by how load-bearing they are.

**1. We predict pixels; LCEVC predicts residuals.** LCEVC's enhancement is
defined as a residual on the upsampled base, and its temporal layer predicts
*that residual* from the previous residual with zero motion (its "temporal
prediction" of the residual plane). Our enhancement layer codes
`Out(N) - P` where `P` is a predictor of the *picture*, and the reference we
warp is `Out(N-1)`, the previous final output picture, not a residual plane.
There is no residual-on-residual anywhere in our design, and no residual
image is retained between frames. This is the cleanest structural
distinction and it should lead the review.

**2. Our temporal hypothesis is pose-warped, not zero-motion.** LCEVC's
temporal residual prediction is a co-located copy with a per-block "temporal
signalling" flag. Ours resamples the previous output through a homography
derived from tracked head pose, with a per-tile motion-vector correction.
Prior art for the mechanism is MPEG-4 Part 2 global motion compensation
(1999, expired) and AV1's corner-then-interpolate global motion (royalty-free);
the pose-derived part needs its own search against Meta / Qualcomm / NVIDIA
split-rendering filings, which 2.2 already flags.

**3. Two hypotheses with an explicit per-tile blend.** LCEVC has one
prediction of the residual and a flag. We have two hypotheses of the picture
and a weight from an explicit alphabet. The blend weights {0, 1/4, 1/2, 3/4,
1} are the MPEG-2-era bi-prediction weighting, expired. RESULTS.md quantifies
what the blend is worth over picking the better single hypothesis; if that
figure is small the fallback in item 6 gets cheaper.

**4. Our transform and entropy coder are the base codec's, not a separate
small-transform layer.** LCEVC uses 2x2 and 4x4 Hadamard-like directional
decomposition transforms specific to its specification. We use the same 8x8
DCT and the same rANS lanes in the enhancement layer as in the pure path,
with no layer-specific transform at all. Nothing in our transform path is
LCEVC-shaped.

**5. Our layers are the same tile format at different scales, with no
special "enhancement sub-layer" structure.** LCEVC defines two enhancement
sub-layers (LoQ-1 at half and LoQ-0 at full) with different tooling. We have
1 to 4 layers of identical geometry and identical syntax; only
`layer_desc.type` distinguishes the base.

**6. The documented fallback.** If a claims reading goes against the
two-hypothesis structure, the safe retreat named in 1.7 is to disable the
temporal hypothesis in enhancement layers and ship spatial-only scalability,
which is H.263 Annex O (1998, expired). The cost of that retreat is exactly
the quality delta between the full blend and the `wgt = 1` column of the
sweep, which RESULTS.md reports for that purpose. **This number should be in
front of counsel before the review, because it prices the fallback.**

**7. Practical note on the base.** In both hybrid paths the HEVC or H.264
bitstream is produced by a licensed encoder and consumed by the device's own
licensed hardware decoder. We do not implement HEVC. The HEVC pool exposure
is the same as WiVRn's today and is unchanged by this design (5.7).

### 5.1 What to hand the reviewer

1. This section, with the RESULTS.md numbers for items 3 and 6 filled in.
2. The tile header layout of 1.2 showing `wgt` alongside the pure-path
   fields, as evidence that the enhancement syntax is not a second format.
3. The statement that no residual plane persists between frames, with the
   decoder pass structure of section 3 as evidence.
4. The written record of public-domain sources for every tool (1.9), which
   1.9 already requires from day one.

---

## 6. Open questions this experiment did not close

* **The 8 to 12 ms is a literature number, not ours.** It should be measured
  on the actual Pico 4 with `KEY_LOW_LATENCY` plus the Qualcomm vendor key,
  and again with the key removed, because the failure mode of that key is a
  silent extra frame of pipelining (3.5). Until then the latency column of
  section 3.1 is the weakest number in this document.
* **Foveation interacts with the split and was held flat here.** A foveated
  enhancement layer should be able to take a smaller share of the budget for
  the same perceived quality, which would move the recommended split. That is
  a second sweep, with the foveation model of 5.1 switched on.
* **Loss was not modelled.** Section 4.3's escalation is a design, not a
  measurement.
* **The base encoder here is x265, which is better than a hardware encoder at
  the same bitrate.** Every hybrid row is therefore slightly optimistic about
  the base. A repeat with a real Pico-class encoder in the loop would tighten
  it.
