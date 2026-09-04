# A Vulkan Compute Video Codec for Low-Latency VR Streaming

**Design paper, draft 1. 2026-09-04.**
**Target integration: WiVRn NX. First target hardware: Pico 4 (Snapdragon XR2 Gen 1) streamed from a PC.**

## Abstract

General-purpose video codecs are the wrong tool for VR streaming. H.264, HEVC and AV1 are built for
file storage and broadcast: whole-frame slices, reference lists, B-frames, serial entropy coding and
fixed-function hardware whose latency and session limits we cannot change. VR streaming has structure
those codecs cannot see. The encoder knows the head pose that produced every frame and the pose the
next frame will be rendered at. The two eyes share nearly all content. The lens throws away most of
the periphery. The display would rather show a slightly stale tile than wait for a whole frame.

This paper designs a codec that is built around those facts and nothing else. The frame is a set of
independent tiles, each its own bitstream, each decoded by one GPU workgroup. Prediction is
pose-warped reprojection of the previous decoded frame, with small per-tile corrections, so motion
search largely disappears and a lost tile conceals itself. The bitstream is layered so a weak headset
can decode a hardware HEVC base layer and add a compute enhancement layer, while a strong headset
decodes everything in compute. Rate control, entropy coding, packetization and telemetry all run in
Vulkan compute with no CPU on the hot path. The codec is vendor-neutral on the PC, which removes the
NVENC/AMF/VAAPI dependency and its session ceilings.

The paper states the compute budget honestly. The first target GPU, Adreno 650, is weak, and the
project begins with a go/no-go benchmark before any codec code is written.

## Design principles

1. **Latency before bits.** Every tool is judged first by what it does to the time between render
   finish and photons, and only then by compression ratio.
2. **The GPU is the codec.** No tool may require serial state across tiles. If it cannot run as one
   workgroup per tile, it does not exist.
3. **Use what the renderer knows.** Pose, depth, motion, stereo and layer structure are inputs, not
   things to rediscover.
4. **Degrade, never stall.** Every loss, every deadline miss, every budget overrun has a defined
   graceful output.
5. **One format, many decoders.** Hybrid hardware-plus-compute and pure compute read the same
   bitstream. Capability bits, not forks.
6. **Bit-exact and simulatable.** Integer normative path, a CPU reference decoder, conformance
   vectors, fuzzing. The encoder runs the real decoder.
7. **Patent hygiene.** Public-domain and expired tools only, with a formal review before release.

## Contents

1. Bitstream format and coding tools
2. Prediction, references and loss concealment
3. Vulkan implementation: encoder, decoder, and the compute budget
4. Transport, rate control, timing and scalability
5. Perception, foveation, future tools and the competitive landscape
6. Reconciliation: decisions across sections
7. Conclusion and roadmap

---

# 1. Bitstream format and coding tools

This section fixes the v1 wire format and the coding tools behind it. The guiding constraint is not compression ratio; it is that one 64-lane workgroup on an Adreno 650 must be able to take one tile from bytes to pixels with no cross-tile state, in roughly 100-150 simple ops per pixel, using only techniques that are public domain, expired, or explicitly royalty-free. Every choice below is judged against that first and against bits per pixel second.

## 1.1 Tile geometry

**Decision: 64x64 luma tiles, fixed per stream, same grid on every native layer.** A 1-bit `tile_size` field in the stream header allows 32x32 for a future Lite profile, and decoders must accept both (it only changes the number of block passes per workgroup), but v1 encoders always emit 64.

Why 64 and not 32:

| | 32x32 | 64x64 |
|---|---|---|
| Tiles per eye at 2160x2160 | 4624 | 1156 |
| Pixels per lane (64 lanes, 4:2:0) | 24 | 96 |
| Per-tile fixed overhead at 100 Mbit / 72 Hz (avg tile 37 vs 75 bytes) | ~20% | ~10% |
| Foveation granularity in lens space | finer | acceptable (fovea ~ 10 tiles wide) |
| Intra efficiency (no cross-tile context) | worse | better |

At 100 Mbit and 72 Hz the whole stereo frame is about 174 KB, or 75 bytes per 64x64 tile on average. Fixed per-tile cost therefore has to stay under about 8 bytes, which drives most of the header and entropy-coder decisions below. With 32x32 tiles the overhead doubles for no decode-time gain, since the workgroup would be underfed.

The tile grid is defined in the lens-space image the renderer produces (after WiVRn's existing foveated warp, if enabled) so the codec's per-tile resolution levels stack on top of the render-side foveation rather than replacing it.

## 1.2 Header layouts

All multi-byte fields are little endian. Bit fields are listed LSB first.

**Stream header** (sent at connect, repeated on every IDR-equivalent "tile map reset"; ~64 bytes):

```
magic          u32   'NXV1'-style constant
version        u8    format version, 1
profile        u8    0=Lite 1=Full 2=Pro
level          u8    max tiles/frame class and max decode work
tile_size      u8    bit0: 0=64x64 1=32x32; bits1-7 reserved (must be 0)
width, height  u16 x2 per eye, luma samples
eyes           u8    1 or 2
bit_depth      u8    8 or 10
num_layers     u8    1..4
layer_desc[n]  u32   type(4): 0=NATIVE 1=HEVC_NAL 2=H264_NAL; scale(2): 0=1/1 1=1/2 2=1/4; flags(26)
tools          u64   mandatory tool bitmask (see 1.9)
ext_len        u16   length of TLV extension area (decoder skips unknown TLVs)
```

Capability negotiation is an intersection: the client sends its own `tools` mask and `profile`/`level` in the connect handshake; the server must only set bits the client offered. A decoder that sees an unknown mandatory bit refuses the stream instead of guessing. Tool bits 0-31 are defined in v1; 32-63 are reserved and must be zero. This is the same forward-compatibility scheme as Vulkan feature bits and it needs no version arithmetic.

**Frame header** (~48 bytes plus optional tables; replicated in the first datagram of every tile row so a lost datagram never orphans a whole frame):

```
frame_number   u16
pose           7 x f16 (quat) + 3 x f32 (pos) = 26 bytes  (used by the pose-warp predictor)
base_qp        u8    0..63
chroma_qp_off  i8
quant_matrix   u8    0..3 built in, 255 = custom matrix follows (64 bytes)
tables_present u8    bit i: probability table set i is transmitted, else built-in default i
tables         variable, see 1.6
ref_slots      u8    which of 4 reference slots this frame will overwrite
flags          u8    bit0: frame is a tile-map reset; bit1: stereo inter-view enabled; rest reserved
```

**Tile-row header** (one per datagram; the datagram itself is defined in the transport section):

```
frame_number   u16
row_index      u8
tile_count     u8      tiles present in this datagram
skip_bitmap    u64     bit i set = tile i of this row is SKIP_WARP and is NOT sent
```

The skip bitmap is the single biggest overhead saving: a static periphery tile costs one bit instead of a header.

**Tile header** (8 bytes fixed, 2 optional):

```
word0 (u32): layer(2) eye(1) rsvd(1) tile_index(12) payload_len(16)
word1 (u32): mode(2)        0=SKIP_WARP 1=SKIP_STATIC 2=INTER 3=INTRA
             res_level(2)   0=64x64 1=32x32 2=16x16 coded, upsampled
             chroma444(1)
             alpha_mode(2)  0=opaque 1=constant(byte follows) 2=coded 3=reserved
             qp_delta(6)    signed, -32..+31 relative to base_qp
             table_set(3)   probability table set 0..7
             nsub_log2(3)   number of rANS substreams = 2^n, 0..5
             mv_present(1)  one quarter-pel MV (2 x i8) follows
             ref_sel(2)     reference slot: 0 = newest, 1..3 = older
             tskip(1)       whole tile is transform-skip (lossless path)
             wgt(2)         base/temporal blend weight for enhancement layers (1.7)
             rsvd(7)
[mv_x i8, mv_y i8]   if mv_present
[alpha u8]           if alpha_mode == 1
payload              rANS-coded, payload_len bytes
```

A SKIP_STATIC tile (copy the unwarped reference, for quad layers and HUDs that are attached to the head) is 8 bytes. An INTER tile costs 8-10 bytes of header plus the entropy-coded payload. Everything with per-block granularity (coded-block flags, last-significant position, coefficients) lives inside the rANS payload so that no bit-level parsing happens outside the entropy decoder.

## 1.3 Color

**Decision: YCoCg-R, full range, 8-bit v1 with a 10-bit tool bit; 4:4:4 or 4:2:0 selected per tile.**

YCoCg-R (Malvar and Sullivan, 2003) is a lifting transform: `Co = R - B; t = B + (Co >> 1); Cg = G - t; Y = t + (Cg >> 1)`. It is exactly invertible in integers, costs 4 adds and 2 shifts per pixel per direction, and needs one extra bit on the chroma planes (9-bit chroma for 8-bit RGB). It was offered royalty-free for H.264 FRExt and is in any case past 20 years. BT.709 YCbCr was rejected because it is not integer-reversible (kills the lossless mode) and needs multiplies; ICtCp is deferred to an HDR tool bit because it requires the PQ nonlinearity and does not matter for SDR game output.

Chroma subsampling for a 4:2:0 tile is a rounded 2x2 average of Co and Cg; upsampling on decode is the fixed half-phase bilinear (weights 3/4, 1/4). Both are bit-exactly specified so that the encoder's reference model matches.

**The client stores references in display format** (RGBA8 or RGB10A2, one image per reference slot, 4 slots like NVENC's DPB depth 4). YCoCg-R round-trips exactly through RGB, so there is no drift, and the reprojection shader samples the reference image directly with zero copies. The cost is 6 ops per pixel each way at prediction and reconstruction time, which is cheaper than the extra 2 bytes per pixel of bandwidth that a planar 16-bit YCoCg reference would cost (56 MB per stereo frame at 40 GB/s is 1.4 ms of pure traffic).

Alpha is a fourth plane coded with the luma tools at its own QP (`alpha_mode = 2`), or a constant, or absent. This is what makes quad layers and MR passthrough first-class without a second stream.

## 1.4 Transform

Cost model for one 64x64 4:2:0 tile on Adreno 650 (64-lane wave, roughly 4-cycle dependent-op latency, 32 KB local memory). The layout is lane-per-column: 8 lanes handle one 8x8 block, so 64 lanes process 8 blocks per pass; 12 passes cover 64 luma + 32 chroma blocks.

| Candidate | Ops/pixel (2D) | Registers/lane | Barriers/tile | Multi-res for free | Notes |
|---|---|---|---|---|---|
| 4x4 integer (H.264 style) | ~5 | 4 | 1 per pass | no | best for low-energy inter residual; worse intra |
| 8x8 integer DCT, Loeffler factorization | ~12 | 8 | 1 per pass (transpose through local memory) | no | 11 mul + 29 add per 8-point 1D |
| 8x8 direct matrix (HEVC style) | ~30 | 8 | 1 | no | too many multiplies |
| 5/3 wavelet, 3 levels | ~8 | 2-4 | 6 (2 dims x 3 levels) | yes | dependencies span the whole tile; edge extension at 64-px boundaries; intra prediction awkward |
| No transform (PCM residual) | 0 | 1 | 0 | no | lossless / text |

**Decision for v1: 8x8 integer DCT for all planes, plus a per-tile transform-skip mode.** The 8x8 wins over 4x4 on the intra and refresh tiles that dominate bits after a loss, and pose-warped residuals on game content are not as flat as classic inter residuals (the warp is exact only for the far field). The factorization used is Loeffler-Ligtenberg-Moschytz (1989, expired) with our own 9-bit integer constants and a defined two-stage shift (7 bits after the first dimension, 12 after the second, 16-bit intermediates), so the decoder is bit-exact by definition and the coefficient set is not HEVC's claimed matrix. Register pressure of 8 values per lane keeps occupancy high, which matters more on Adreno than raw op count.

The 5/3 wavelet was the serious alternative. Its multi-resolution structure gives the per-tile resolution levels for free and it is what VC-2 (BBC, royalty-free) does. It was rejected for v1 because the six barriers per tile and the whole-tile dependency chain fight the lane-per-block structure that the entropy coder and predictor want, and because blockless intra needs a different coefficient model. It stays as tool bit `XFORM_WAVELET` for a v2 experiment; the per-tile format is otherwise unchanged because the coefficient payload is opaque to the headers.

v2 also reserves `XFORM_4x4_SPLIT` (per-8x8-block flag to split into four 4x4 transforms for inter residual), which is a one-symbol addition to the payload.

**Intra prediction inside a tile**: v1 has no directional pixel prediction. Blocks of an INTRA tile are predicted from the tile mean (coded once, 8 bits), and the 64 DC coefficients of the luma blocks are themselves passed through a second-level 8x8 DCT (H.264 intra16x16 uses the same idea with a Hadamard; 2003, expired). This costs an extra 64-point transform per tile and captures most of the DC redundancy without a serial dependency across blocks. Directional intra (four modes, decoded as a 15-step wavefront over the 8x8 block diagonals of the tile) is specified as tool bit `INTRA_DIR` for v2. Phase 1 measures the gap against x264 intra; the expectation is 25-35% more bits for intra tiles, which matters only on refresh.

## 1.5 Quantization and per-tile resolution

- QP scale: `step = 2^(QP/6)` with QP 0..63, so QP 0 is step 1 (lossless with transform skip) and QP 51 is step 362. 10-bit content uses the same table with coefficients scaled by 4.
- Dead-zone quantizer on the encoder: `q = sign(c) * floor((|c| + f * step) / step)` with `f = 1/3` for INTRA, `1/6` for INTER, matching the JPEG/MPEG-2 era practice. Reconstruction is plain `c = q * step * w[i] >> shift` with no reconstruction offset, so the decoder does exactly one multiply and shift per coefficient.
- Perceptual weighting `w[i]`: four built-in 8x8 matrices (flat, JPEG-luma-like, a stronger high-frequency roll-off for periphery, and one for chroma) selected per frame, or a custom matrix. Per-tile QP delta (6 bits) is the primary perceptual control: the encoder raises QP with eccentricity, during fast head rotation, and in dark tiles, per the perceptual section.
- Rate-distortion: the encoder does RDOQ-style zeroing of trailing isolated coefficients (trellis quantization is 1990 prior art) and picks per-tile QP from a lambda tied to the transport's bitrate controller. The decoder does not care.

**Per-tile resolution (foveation)** is `res_level`: the tile is coded as a 32x32 or 16x16 image (16 or 4 blocks per plane) and upsampled to 64x64 at reconstruction with the same fixed bilinear used for chroma. The predictor is also formed at the coded resolution: the warped reference tile is box-downsampled, the residual is added, and the result is upsampled into the full-resolution reference image. The client therefore always holds a full-resolution reference and the encoder's model of it is exact. A level-2 periphery tile at QP 40 typically costs 15-30 bytes, of which 8-10 are header.

## 1.6 Entropy coding

Three families were evaluated against the same test: 64 lanes, no divergence in the hot loop, no serial dependency longer than the symbol count divided by the lane count.

| Scheme | Decode cost per symbol | Parallelism | Adaptivity | Bits/coef vs CABAC-class | Patent status |
|---|---|---|---|---|---|
| Interleaved rANS, static tables (Duda 2009/2014, Giesen 2014) | ~10 ops + 1 LUT | N independent substreams in lockstep | per frame | +3-6% | public domain by author; see 1.9 for the Microsoft caveat |
| Adaptive binary (CABAC/M-coder style) | ~15 ops, serial per bin | one bin at a time per stream; would need 64 streams | per bin | reference | H.264 CABAC expired 2023-2024 in most places, but HEVC/VVC context designs are live; complex to keep lockstep |
| Bit-plane significance + Golomb (EZW/SPIHT lineage, VC-2 style) | ~4 ops per bit-plane pass | per block, all lanes | none | +10-15% | EZW 1993 and SPIHT 1996 expired; VC-2 royalty-free |

**Decision: interleaved rANS with static per-frame tables, 1 to 32 substreams per tile, chosen by the encoder from the symbol count.** Bit-plane coding is kept as the Lite-profile fallback (tool bit `ENT_BITPLANE`) because it needs no tables and no local-memory LUT, but its 10-15% cost on the fovea tiles is real.

Concrete v1 layout:

- **State**: 32-bit rANS state, `L = 2^16`, 16-bit renormalization, probability precision `M = 2^10`. A decode step is `slot = x & 1023; s = lut[ctx][slot]; x = freq[ctx][s] * (x >> 10) + slot - cum[ctx][s]; if (x < L) x = (x << 16) | read_u16()`. With 12 contexts of 16 symbols the LUT is 12 KB of u8 plus 768 bytes of freq/cum, which fits Adreno's 32 KB local memory alongside the 8x8 transpose buffer.
- **Substreams**: `N = 2^nsub_log2`, where the encoder picks the smallest N such that `symbols / N <= 128`. Substream i owns 8x8 blocks `i, i+N, i+2N, ...` in the tile's block order (luma raster, then Co, then Cg, then alpha). Each lane decodes its own blocks sequentially, so the coefficient context (below) is causal within the lane and needs no cross-lane traffic. Lanes with fewer symbols idle until the longest lane finishes; the lockstep cost is the maximum, not the sum.
- **Single interleaved byte stream, no offset table**: all N substreams read from one buffer in the fixed order "symbol step t, lanes 0..N-1", exactly Giesen's interleaved rANS. The decoder computes each renormalizing lane's read position with `subgroupBallot` and `subgroupBallotExclusiveBitCount`, then one `subgroupAdd`-style broadcast of the popcount advances the shared cursor. Adreno 6xx exposes ballot, arithmetic and shuffle in Vulkan 1.1 compute; this is a Phase 0 gate check. On hardware without ballot the decoder falls back to a per-substream offset table, which the encoder can emit under tool bit `ENT_OFFSET_TABLE` at N x 2 bytes. N is capped at 32 so that a 32-wide desktop wave (AMD wave32, NVIDIA) decodes a whole tile in one subgroup; a 64-lane Adreno wave uses lanes 32-63 for the second 16-bit read prefetch and the block-order table.
- **Flush cost**: 4 bytes per substream naively. The encoder uses the initial state as payload (the first symbols are folded into the choice of initial state, so effective overhead is about 2 bytes per substream). A 4-substream tile pays ~8 bytes; a 32-substream fovea tile pays ~64 bytes on a payload of 1-2 KB.
- **Alphabet and contexts**. Everything is one of three symbol types on the same rANS state:
  1. `CBF` (2 symbols, 2 contexts: luma/chroma): coded-block flag per 8x8 block.
  2. `LAST` (16 symbols, 2 contexts): zigzag index of the last nonzero coefficient, 4-bit class plus raw bits for classes above 8.
  3. `LEVEL` (16 symbols: 0, 1, ..., 14, ESC; 8 contexts): `min(|q|, 14)` for every coefficient up to `LAST` in reverse zigzag order, context = band(position: DC, 1-3, 4-9, 10-63) x class of the previously decoded level in the same block (0, 1, >1), collapsed to 8. ESC is followed by Exp-Golomb order-3 raw bits. Sign is one raw bit.
  Raw bits are "bypass" symbols coded with a uniform distribution directly on the rANS state (`x = (x << k) | bits` on encode), which costs 2 ops and keeps a single stream.
- **Tables**: a table set is 12 contexts x 16 symbols of 10-bit frequencies, transmitted as 5-bit log-domain deltas from the built-in default (~120 bytes per set). The encoder computes histograms of the actual quantized coefficients in one atomic-add dispatch after quantization, builds up to 8 sets per frame (typically split by QP class and INTRA/INTER), and each tile picks one with `table_set`. Built-in defaults exist for every set index so that `tables_present = 0` is a valid, loss-tolerant frame. Bin-adaptive probability estimation was rejected: rANS encodes in reverse, so adaptive models require the encoder to record the model trajectory forward and replay it backward (a full second pass over every symbol with per-symbol state), and adaptivity on the decoder is a serial dependency across lanes unless each lane keeps its own model, which loses most of the adaptation benefit anyway. Per-frame static tables recover most of the gain for the 6-8% price quoted above.

Expected efficiency at a mid-rate operating point (QP 24-28, fovea): about 3.2 bits per nonzero coefficient including sign, 0.9 bits per zero before LAST, 0.85 bits per CBF flag. This is within 5% of a CAVLC-class coder and roughly 8% behind CABAC on the same coefficient statistics, which the pose-warp predictor more than pays for.

**Encoder cost**: rANS encodes backward, so the GPU encoder (a) quantizes the tile into local memory, (b) counts symbols per substream with a prefix sum, (c) encodes each substream in reverse in lockstep, using the same ballot trick to write bytes backward from the end of the tile's output slot. This is the same order of work as the decoder plus one histogram dispatch per frame. On an RX 580 the whole stereo encode (predict, transform, quantize, entropy code) is estimated at 3-4 ms; on a 7900 XTX or any current NVIDIA part it is under 1 ms and is not the bottleneck.

**Decoder cost estimate on Adreno 650** for a 2312-tile stereo frame: entropy decode ~1 ms (the largest tiles have ~3000 symbols at N=32, i.e. ~95 lockstep steps; small tiles have 100-400 symbols at N=1..4), inverse transform ~1.5 ms, warped prediction ~2 ms (bandwidth bound on the reference reads), reconstruction and write ~0.5 ms, total ~5 ms of a 11.1 ms frame at 90 Hz. These are back-of-envelope numbers and the Phase 0 gate exists to replace them with measurements.

## 1.7 Layers, hybrid decode and the LCEVC comparison

A stream has 1 to 4 layers. Layer 0 is the base. Each additional native layer is a full tile grid of the same 64x64 geometry at the layer's resolution scale, and every tile in a layer above 0 has two predictor hypotheses: the upsampled reconstruction of the layer below (spatial) and the pose-warped previous reconstruction of the same layer (temporal). `wgt` in the tile header blends them with weights (0, 1/4, 1/2, 3/4, 1) in units of the spatial hypothesis, which is the MPEG-2-era bi-prediction weighting, expired.

Layer 0 can be `NATIVE` (pure-compute path) or `HEVC_NAL` / `H264_NAL` (hybrid path). In the hybrid path the base packets carry ordinary access units for MediaCodec; the decoded AHardwareBuffer is imported into Vulkan, converted from YCbCr to YCoCg-R (a defined integer approximation, since the hardware decoder's output is limited-range YCbCr and this conversion is where bit-exactness of the base ends and must be modelled by the encoder running the same HEVC decoder, or at least the same reference output, on the PC), and used as the spatial hypothesis for layer 1. Everything above the base is the identical tile format in both paths; the client's `tools` mask and `layer_desc` type are the only things that differ. A weak headset thus gets pose-warped enhancement on top of a 1/2-resolution HEVC base, and a strong headset drops MediaCodec entirely.

Differences from LCEVC (MPEG-5 Part 2, V-Nova, patented): LCEVC codes residuals with small Hadamard-like transforms and predicts the residual layer temporally with zero motion; its enhancement is defined as residual-on-residual. Ours predicts pixels, not residuals, uses the same DCT and rANS tools as the base, warps the temporal hypothesis by head pose with a per-tile MV correction, and blends two hypotheses with explicit weights. The hybrid mode still needs a claims review before shipping; the safe fallback is to disable the temporal hypothesis in enhancement layers (spatial only), which is plain spatial scalability as in H.263 Annex O (1998, expired).

Open risk: MediaCodec on the Pico 4 needs whole access units, so the hybrid path adds a full HEVC decode (8-12 ms measured class) before any enhancement tile can be reconstructed; tile-row pipelining only applies to the enhancement layer. The hybrid path is a compatibility mode, not the latency path.

## 1.8 Lossless, near-lossless, alpha and skip

- **Lossless**: `tskip = 1`, `qp_delta` such that QP = 0, `chroma444 = 1`. Residual samples are coded directly with the LEVEL alphabet using a fifth band context; there is no separate spatial predictor because the pose-warped or static predictor already handles quad-layer text well and a JPEG-LS style MED predictor would serialise the lane. A worst-case 64x64 4:4:4 8-bit lossless tile is 12 KB (bounded by 3 x 4096 x 8 bits plus escape overhead), which exceeds one datagram. The transport section's "tile = packet" rule therefore holds for lossy profiles only; Pro-profile tiles may be fragmented into up to 4 quadrant substream groups (cross-section note).
- **Near-lossless**: normal DCT at QP 0-6 with a flat matrix, which gives PSNR above 50 dB on game content at about 1 bpp.
- **Skip**: `SKIP_WARP` is expressed only through the row skip bitmap (0 bytes); `SKIP_STATIC` is an 8-byte tile. Both are exact copies, so a tile that stays skipped forever never drifts.
- **Alpha**: coded plane with `alpha_qp = base_qp + alpha_qp_off` or constant; the reference image's A channel stores it.

## 1.9 Patent hygiene

Safe by expiry or explicit grant: DCT (1974) and the Loeffler factorization (1989); YCoCg-R (2003, offered royalty-free); Exp-Golomb; JPEG-style weighting matrices and dead-zone quantization; trellis quantization (1990); H.263/MPEG-2 bi-prediction weights and spatial scalability; H.264 baseline tools filed 2002-2003 including the 4x4 integer transform and intra16x16 DC Hadamard (expired 2023-2024 in the US and EU; check any FRExt-era claims individually); rANS (Duda placed it in the public domain; Giesen's interleaving is a blog post with public-domain code); EZW and SPIHT for the bit-plane fallback; VC-2 (BBC royalty-free declaration).

Avoid: HEVC-specific tools (its transform matrices, SAO, merge/AMVP design, its CABAC context tables); AV1 tools (the AOM patent license covers implementations of AV1 itself, not reuse of its tools such as CDEF, loop restoration or its adaptive multi-symbol CDF scheme in another codec); LCEVC's residual temporal prediction and transforms; JPEG XS's specific bit-plane-count coding (RAND pool); 3D-HEVC view synthesis prediction, which is the closest patented relative of pose-warped prediction and needs a claims review in the prediction section (cross-section note).

Specific caveat: Microsoft was granted rANS-related patents in 2022 concerning selective switching of state precision and related encoder features. The v1 design uses a single fixed state width and a single fixed probability precision, precisely to stay outside those claims; this should be verified by counsel before release.

## 1.10 Bits per pixel and a comparison guess

Stereo 2160x2160 at 72 Hz is 672 Mpixel/s, so 1 bpp is 672 Mbit. Foveation is assumed as 20% of tiles at level 0, 30% at level 1, 50% at level 2.

| Operating point | Fovea bpp | Mid bpp | Periphery bpp | Mean bpp | Bitrate | Notes |
|---|---|---|---|---|---|---|
| Lite wireless (QP 30/36/42) | 0.20 | 0.05 | 0.012 | 0.061 | ~41 Mbit | fovea at HEVC-QP-28 class quality |
| Standard wireless (QP 24/30/36) | 0.55 | 0.15 | 0.03 | 0.17 | ~115 Mbit | comparable in the fovea to today's 150 Mbit HEVC |
| USB / WiFi 7 near-lossless (QP 4, no foveation levels) | 1.0 | 1.0 | 1.0 | 1.0 | ~670 Mbit | PSNR > 50 dB |

Comparison against HEVC, honestly: intra-only tiles will need roughly 30-40% more bits than x265 intra at equal PSNR because there is no directional prediction and no adaptive context modelling. Inter tiles on camera-motion content (the VR common case) should reach parity or better because the pose warp removes most of the motion that HEVC spends bits describing with block vectors, and because the per-tile QP/resolution model spends bits where the eye is. On fast object motion with a single MV per tile the codec will lose against HEVC's block-level motion by an estimated 15-25% in the affected tiles, which is why v2 reserves four MVs per tile. Net at the standard point: about the same bitrate as HEVC for the same fovea quality, at a fraction of the latency and with loss behaviour HEVC cannot offer. These are estimates to be replaced by Phase 1 and Phase 2 measurements.

## 1.11 Worked example: one INTER fovea tile

Content: 64x64 luma, 4:2:0, level 0, QP 24 (step 16), pose-warped predictor with a small MV correction.

After prediction and transform, 40 of 64 luma blocks quantize to all-zero; 24 luma blocks have on average LAST = 11 (12 coded positions) with 6 nonzero levels. Of the 32 chroma blocks, 8 are coded with 2 nonzero levels each and LAST = 3.

| Element | Count | Bits each | Bits |
|---|---|---|---|
| CBF, luma | 64 | 0.85 | 54 |
| CBF, chroma | 32 | 0.7 | 22 |
| LAST, luma | 24 | 4.2 | 101 |
| LAST, chroma | 8 | 2.5 | 20 |
| Zero levels before LAST | 24x6 + 8x2 = 160 | 0.9 | 144 |
| Nonzero levels | 24x6 + 8x2 = 160 | 2.3 | 368 |
| Signs | 160 | 1.0 | 160 |
| Escapes | ~2 | 8 | 16 |
| Payload total | | | 885 bits = 111 bytes |

Symbol count for lane assignment: 96 CBF + 32 LAST + 320 levels + 160 signs = 608, so the encoder picks N = 8 (76 steps per lane), costing ~16 bytes of rANS flush. Header: 8 bytes fixed + 2 bytes MV. Tile total: 8 + 2 + 111 + 16 = 137 bytes, or 0.27 bpp over 4096 pixels.

Decode on one 64-lane workgroup: 8 lanes decode 76 lockstep rANS steps (~76 x 12 dependent ops ≈ 3600 cycles wall, the other 56 lanes prefetching); dequantization and 12 passes of lane-per-column 8x8 IDCT (~12 x 100 ops per lane); the warped predictor is sampled once per pixel (bilinear, 4 reads); YCoCg-R to RGB and one storage-image write per pixel. About 150 ops per pixel including the entropy stage amortised over the tile, within the 150-300 budget the field data allows.

## 1.12 Open risks

1. Adreno subgroup ballot availability and cost is the load-bearing assumption of the single-stream rANS layout; the offset-table fallback exists but costs 2 bytes per substream.
2. The no-directional-intra decision may cost more on refresh tiles than estimated; if Phase 1 shows a gap above 40% against x264 intra, `INTRA_DIR` moves from v2 to v1.
3. Frame-header replication per tile row is ~50 bytes x 34 rows per frame (about 1 Mbit at 72 Hz), acceptable, but per-frame table sets must not be replicated; they are sent once and the built-in defaults cover the loss case.
4. The hybrid base path is only as bit-exact as the hardware HEVC decoder is deterministic across devices; the encoder must model the base as "approximately what the client decoded" with a periodic native refresh tile to bound drift.

---

# 2. Prediction, references and loss concealment

This section specifies the inter-frame predictor of the codec: pose-warped prediction with per-tile motion corrections, the reference model shared between encoder and decoder, what happens when tiles are missing, and how the same machinery drives frame-rate scaling and the hybrid HEVC mode. Numbers assume the Pico 4 target: two eyes at 2048x2048 streamed (8.4 Mpix per frame), 90 Hz, 32x32 tiles (8192 tiles) or 64x64 tiles (2048 tiles).

## 2.1 The predictor in one sentence

For every tile, the prediction is the previous decoded frame resampled through a global per-eye homography (from the head pose delta) plus a per-tile 2D vector. Nothing else. Depth, engine velocity buffers, stencil masks and the search all live on the encoder; the decoder only ever sees "homography + per-tile vector + mode".

Rationale: any positional (translation-induced) parallax, once the depth is approximated as constant per tile, collapses to a per-tile 2D shift (the plane-induced homography H = K (R - t n^T / d) K^-1 differs from the rotation-only homography by a term that is constant within a fronto-parallel tile to first order). So a per-tile motion vector subsumes "positional warp with per-tile depth" and also covers moving objects. Sending depth to the decoder would carry no extra information for the same bit cost, and per-pixel depth warping (forward splatting, hole filling) is out of the Adreno 650 budget and creates the disocclusion problem we otherwise do not have.

Decision: rotation-only global warp, per-tile MV correction, no depth on the decoder. Per-pixel depth warp rejected (cost, holes, needs a depth stream). Per-tile plane homography rejected (second-order gain over a shift for 32 px tiles, costs 4 extra parameters per tile).

## 2.2 Rotation-only reprojection

The server rendered frame N-1 with view rotation R_{N-1} and frame N with R_N (per eye, including the IPD offset in the eye pose; only the rotation part is used). For a target pixel x in frame N the reference position is

    x_ref = H x,   H = K_e * R_{N-1}^T * R_N * K_e^{-1}

with K_e the eye's (asymmetric) projection in streamed-pixel units. This is the same matrix Oculus TimeWarp and the WiVRn reprojection shader already use, only applied in the opposite direction (target to source). Roll and the perspective divide are handled exactly, which is where block-translation codecs lose: a 3 degree roll moves the frame corners by 50 px in opposite directions and HEVC needs a different MV for every block, plus sub-pel refinement, to follow it.

Magnitudes at 90 Hz: a 300 deg/s head turn is 3.3 deg per frame, about 70 px at the 2048 px / 95 deg FOV of the Pico 4. HEVC hardware encoders at low-latency presets have effective search ranges of 32 to 64 px and fall back to intra on such frames, which is the visible "bitrate spike on head turn" WiVRn users know. The warp makes that motion cost zero bits.

Disocclusion: with rotation only there is none except at the frame border. The strip revealed on the leading edge (up to 70 px at 300 deg/s, i.e. two or three tile columns) is predicted from clamp-to-edge reference samples and the encoder will pick intra for those tiles. Because the predictor is always dense (a homography plus a shift never produces holes) the decoder never needs a hole-filling pass.

### Determinism: integer warp

The encoder runs the decoder to build its references, so both sides must compute identical predictions. Floating point in shaders is not portable: Vulkan permits 2.5 ULP for fp32 division, FMA contraction differs between compilers, Adreno honours RelaxedPrecision aggressively, and AMD, NVIDIA and Qualcomm all round differently. A single ULP difference in a sampling coordinate flips a rounding decision and the mismatch then propagates through every later frame. The warp is therefore defined in integer arithmetic only:

1. The server computes H per eye in double precision, then quantises it to nine int32 in Q8.24 (after scaling so h22 = 2^24). These 36 bytes per eye go in the frame header. The encoder uses the quantised matrix itself; the quantisation error lands in the residual.
2. Per tile, the decoder computes source coordinates for the four tile corners only:
   num_x = h00*x + h01*y + h02, num_y = h10*x + h11*y + h12, den = h20*x + h21*y + h22, all as 64-bit products via OpUMulExtended/OpSMulExtended (core SPIR-V, no shaderInt64 required), then x_src = (num_x << 6) / den with a fixed 32-iteration restoring division. Result in Q.6 (1/64 pel). Four divisions per tile, not per pixel.
3. Inside the tile the source coordinate is bilinearly interpolated from the four corners with integer adds (the homography is smooth enough that the interior error at 32 px is under 1/32 pel for any head rotation that occurs at 90 Hz; at 64 px tiles it stays under 1/16 pel below 250 deg/s, which is one reason 32x32 is the default in the Full profile).
4. The per-tile MV (1/4 pel, in Q.2) is added, the sum is rounded to 1/16 pel, and the sample is taken with an integer filter: bilinear (weights 0..16, Lite profile) or 4-tap Catmull-Rom with integer coefficients over 64 from a 16-entry table (Full profile). Rounding is "add half, shift", defined once.

The result is bit-exact on every Vulkan implementation by construction; the CPU reference decoder used for fuzzing implements the same 30 lines. Prior art: MPEG-4 Part 2 global motion compensation (sprite warping, 1999, patents expired) also warped corner points with integer arithmetic and interpolated; AV1's global motion uses the same corner-then-interpolate structure under a royalty-free licence. Neither ties the parameters to a tracked pose; that step should get a patent search (Meta, Qualcomm and NVIDIA have filings on pose-based prediction for split rendering, 2016 to 2020). The AV1 and MPEG-4 prior art makes the mechanism itself safe.

Resampling blur: with a moving head every tile is resampled at a fractional position every frame, unlike a static camera where skip blocks copy exactly. Bilinear resampling applied 90 times per second turns fine texture to mush within about a second unless the residual keeps correcting it, and correcting costs bits. This is why Catmull-Rom (slight sharpening lobe) is the Full profile default despite 4x the sampling cost; Phase 2 must measure the PSNR decay of a warp-only chain over 2 s for both filters.

### Decoder cost

Per pixel: coordinate interpolation 4 ops, MV add and rounding 3, filter fetch 4 (bilinear) or 16 (Catmull-Rom) LDS reads with 8 or 24 MACs, residual add and clamp 3. Total about 20 ops (Lite) or 50 ops (Full). The reference region of a tile (tile plus MV range plus filter margin, at most 48x48 for a 32x32 tile with |MV| <= 6 px after warp) is loaded once into shared memory: 2.25 texel reads per pixel amortised. Larger vectors fall back to direct texture fetches for that tile. Memory traffic: about 5 bytes per pixel (read reference 4:2:0, write output), 3.8 GB/s at 8.4 Mpix x 90 Hz, under 10 percent of the memory bandwidth. Prediction is roughly a quarter of the decoder's per-pixel budget; entropy decode and the inverse transform take the rest.

## 2.3 Residual motion: per-tile vectors

Mode per tile (3 bits, in the tile header):

| Mode | Reference | Vector | Residual | Typical bits per 32x32 tile |
|---|---|---|---|---|
| WARP_SKIP | warp(prev) | 0 | none | 3 to 4 |
| WARP_MV | warp(prev) | coded | coded | 40 to 1500 |
| STATIC_MV | prev (identity, no warp) | coded | coded | as WARP_MV |
| STEREO | decoded left eye of this frame | coded (disparity) | coded | as WARP_MV |
| INTRA | none | none | coded | 1500 to 4000 |

STATIC_MV exists for head-locked content (menus, HUDs, laser pointers, the WiVRn transport HUD): the warp is exactly wrong there and the identity predictor is exactly right.

MV coding: the MV is coded in the tile's own substream as a delta from the same tile's previous vector (temporal prediction from per-tile decoder state; see 2.6), signed Exp-Golomb, range +-64 px at 1/4 pel. Spatial prediction from neighbouring tiles (H.264 median) is rejected because it makes tiles depend on each other; the temporal predictor costs nothing and gives zero-delta vectors for constant parallax and constant-velocity objects. Cost at 8192 tiles and a typical 3 bits per coded vector: about 2 Mbit/s at 90 Hz, negligible above 50 Mbit/s and the reason the Lite profile at 20 Mbit/s uses 64x64 tiles.

Sub-pel: yes, 1/4 pel. The warp already lands on 1/64 pel positions, so sub-pel MVs cost no extra filter machinery; the two fractions simply add. Measured in every codec since H.264, quarter-pel is worth 10 to 15 percent on textured content over integer-pel and the marginal gain of 1/8 pel is not worth the coding cost.

Encoder search (compute shader, one workgroup per tile, runs on the PC GPU):

1. Candidate seeds: zero; the tile's previous MV; the parallax vector f * t_lateral / d_tile from head translation and per-tile depth if a depth buffer exists (XR_KHR_composition_layer_depth is already a standard extension and Monado exposes it); the median of the engine velocity buffer over the tile if supplied; the disparity seed f * IPD / d_tile for STEREO.
2. Coarse search: 4x downsampled tile (8x8 samples for a 32x32 tile), full search +-16 px (33x33 = 1089 candidates x 64 samples = 70 k SAD ops per tile, 0.6 G ops per frame, under 0.2 ms on an RX 580) around the best seed.
3. Refinement: +-1 px integer diamond, then the 8 quarter-pel neighbours, on the full-resolution warped reference with the real interpolation filter.
4. Decision by rate-distortion cost D + lambda * R for each mode using SATD (4x4 Hadamard) for D and a bit estimate for R; head translation per frame (11 mm at walking speed) gives parallax of 12 px at 1 m and 40 px at 30 cm, so the +-16 px coarse range is enough for the world and the depth seed carries the hands.

Engine inputs via a proposed vendor OpenXR extension (one extra composition layer struct chained per projection view): velocity buffer (RG16F, screen-space motion in pixels per frame, the same buffer engines produce for TAA), depth (already standard), and an 8-bit stencil with bits for head-locked, lossless text and transparent. They plug in as: velocity replaces search step 2 with a single candidate (verified by SATD, never trusted blindly); depth seeds parallax and STEREO; stencil forces STATIC_MV, lossless intra, or higher lambda tolerance for alpha regions. None of this changes the bitstream.

## 2.4 Where the warp fails and what it costs

Content the warp cannot predict: objects moving in the world, head-locked UI, the player's own hands and controllers, mirrors, transparent and additive layers whose visible result changes with the background, full-screen post effects (bloom flashes in Beat Saber light shows), and disoccluded borders. Estimated area coverage and bit cost per frame at a quality equivalent to HEVC at 150 Mbit/s, 32x32 tiles, 8.4 Mpix:

| Content | VRChat | Beat Saber | HL: Alyx | Mode | bits per pixel |
|---|---|---|---|---|---|
| Static world after warp | 60 to 75 % | 75 to 85 % | 80 to 88 % | WARP_SKIP / WARP_MV small residual | 0.01 to 0.08 |
| Moving avatars / enemies / blocks | 10 to 20 % | 5 to 10 % | 3 to 8 % | WARP_MV | 0.4 to 1.0 |
| Own hands, controllers, weapon | 3 to 5 % | 3 % | 6 to 10 % | WARP_MV (depth seed) | 0.3 to 0.8 |
| Head-locked UI / HUD | 0 to 30 % | 2 % | 1 % | STATIC_MV, mostly skip | 0.02, text tiles lossless 1 to 2 |
| Mirrors, particles, light shows | 0 to 30 % | 0 to 40 % (bursts) | 2 % | WARP_MV / INTRA | 1.0 to 3.0 |
| Border disocclusion (300 deg/s) | 3 % | 3 % | 3 % | INTRA | 2.0 to 3.0 |

Worked frame (VRChat, moderate head motion, no mirror): 70 % x 0.04 + 15 % x 0.7 + 4 % x 0.5 + 5 % x 0.02 + 3 % x 1.5 + 3 % x 2.5 = 0.28 bpp, about 2.3 Mbit per frame, 210 Mbit/s at 90 Hz for HEVC-150 quality. That is honest: at rest the codec is roughly at parity with a good HEVC encoder, because HEVC's per-block translation approximates a small rotation reasonably well. The bitrate win is concentrated in the frames where HEVC breaks (fast turns, roll, sub-pel drift over textured floors), typically a 2x to 4x reduction on those frames, which is exactly where the AIMD controller today drops quality. The rest of the case for the predictor is latency and loss behaviour, not average bitrate.

## 2.5 Stereo inter-view prediction

The right eye can predict from the decoded left eye of the same frame (STEREO mode), with a per-tile disparity vector seeded from f * IPD / d and refined by the search. MVC (H.264 Annex H) reports 20 to 25 percent savings on the dependent view versus simulcast, MV-HEVC 25 to 30 percent, on camera-captured content. Rendered VR content is ideal for it (perfect vertical alignment, identical lighting) but the temporal reference right(N-1) is a better predictor than left(N) for everything that was already visible last frame. Inter-view mode therefore mostly replaces INTRA tiles: content that is new to both eyes at once (disoccluded strips, spawned objects, scene transitions) is coded once and copied. Expected overall gain: 5 to 10 percent on average, 30 to 40 percent on intra-heavy frames, which flattens exactly the bit spikes we care about.

Pipeline consequence: right-eye tiles that use STEREO must decode after their left-eye reference. Dispatch order per frame is L row r, R row r, interleaved, so with tile-row pipelining the right eye lags by one row's decode time (about 40 us) and total decode time is unchanged. A STEREO tile whose left reference tile has not arrived by the deadline is treated as lost (concealed, NACKed); the encoder's shadow model (2.6) handles it like any other loss. With multipath striping, left and right tiles of the same row are put on the same path where possible so out-of-order arrival between paths does not stall the right eye. Alternative rejected: predicting right(N) from warped left(N-1), which needs no ordering but loses the one case (new content) where stereo helps. STEREO is Phase 4 and off in the Lite profile.

## 2.6 Reference model

References: exactly one previous decoded frame per eye (the frame buffer is ping-ponged, because the warp reads outside the tile the previous frame must be complete and immutable while the next decodes) plus, for the right eye, the current left eye. No DPB, no long-term references, no B-frames.

Per-tile decoder state (16 bytes, 128 kB at 8192 tiles):

| Field | Size | Use |
|---|---|---|
| held_frame_id | 32 bit | frame whose data this tile last decoded successfully |
| last_mv | 2 x 16 bit | temporal MV predictor and concealment vector |
| age_since_intra | 8 bit | refresh scheduling, drift bound |
| concealed_count | 8 bit | consecutive frames concealed |
| mode, qp, flags | 32 bit | last mode, quantiser, "pixels are extrapolated" flag |

The client sends, once per frame, a bitmap of received tiles (1 kB at 8192 tiles) for the last four frames, piggybacked on the pose packet that already goes at 500 Hz or so; loss of that packet costs nothing since the next one repeats the history.

Encoder shadow: the encoder keeps the last K = 8 frames (90 ms) as bitstream plus decoded pictures, and a "client shadow" frame buffer that mirrors what the headset holds. It encodes frame N+1 optimistically from its own decode of N. When the bitmap reports that tile t of frame N was lost, the encoder replays: shadow_N = for each tile, decode(N) from shadow_{N-1} if received, else conceal(shadow_{N-1}) with the same integer kernel the client used. The replay is a full-frame decode on the PC GPU (about 0.2 ms on a 7900 XTX, 1 ms on an RX 580) and is exact because concealment is deterministic and the client's decode of received tiles used the same shadow reference. From that frame on, the encoder predicts from the true client state; every tile whose prediction footprint touched the concealed region gets a residual computed against what the client actually shows, and the drift is corrected in a single frame at the tile's normal quantiser.

This is the key property: after a loss, visible drift lasts one RTT plus jitter (10 to 30 ms on WiFi 6), then heals completely without an intra refresh and without a frame-level IDR. The existing invalidate -> refresh -> IDR ladder in WiVRn NX degenerates to: shadow resync (always), per-tile intra (if the loss is older than K frames or the tile has been concealed 3 times), full intra (only on stream start, profile change, or bitmap history gap).

Rolling intra refresh stays, for three reasons: bitmap gaps, shadow model bugs, and late joiners in multi-user. Every frame, 1/T of the tiles are coded INTRA regardless of mode decision, T = 180 (2 s), selected by a fixed pseudo-random permutation of tile indices (no visible wave, unlike x264's column-based refresh), and forced also when age_since_intra > T. Cost: 1/180 of the frame at 2.5 bpp is about 0.014 bpp, under 5 percent of the budget.

Multipath and ordering: within a frame tiles are independent and arrive in any order; a tile of frame N that arrives after the frame N deadline is discarded (the frame buffer has moved on) and stays reported as lost, so the shadow model remains consistent. The only intra-frame dependency is STEREO, handled above.

## 2.7 Loss concealment

At the presentation deadline (vsync minus decode time minus reprojection time minus margin, measured by the adaptive de-jitter logic already in the client) the decoder dispatches with whatever arrived. A missing tile runs the same prediction kernel in WARP_SKIP with vector = last_mv (objects keep sliding, the world stays locked to the head), sets the "extrapolated" flag and increments concealed_count. This makes concealment identical to a legitimately skipped tile, which is why the encoder can replay it exactly. There is no separate concealment code path to test.

Stale reference policy: the error of a concealed tile is bounded by the true per-frame change of its content, so a static world stays perfect indefinitely while a moving hand becomes wrong after two or three frames. The client does nothing clever about that; it keeps reporting the tile as missing and the encoder escalates: first replay-resync (inter from shadow), then after concealed_count >= 3 or when the lost frame is older than K, INTRA for that tile with elevated FEC priority. The reprojection shader may optionally read the extrapolated flag per tile and blend the tile 20 percent toward the previous output to hide flicker, which is a client-side choice and does not affect the reference. Rejected: any client-side inpainting of lost tiles (non-deterministic relative to the encoder, and worse than the warp on VR content).

## 2.8 Temporal decoupling and frame-rate scaling

When the server sends at 45 Hz and the panel runs at 90 Hz, the client synthesises the in-between frame from its latest decoded frame using the same warp with its own newest pose (this is ordinary asynchronous timewarp and already exists in the reprojection shader) plus the per-tile residual-motion field (the coded MV minus the warp-induced shift at the tile centre, i.e. the part of the motion that is not the head) extrapolated by half a frame. That field is exactly what the WiVRn NX motion-smoothing feature computes today with a server-side block matcher and sends alongside the video. Decision: when the codec is active, the block matcher is retired and its client-side warp consumes the codec's MV field, which is free and already tile-aligned. Caveat, the same one Oculus ASW 1.0 had when it used the video encoder's motion estimation: coded MVs are rate-distortion choices, not true motion, so the encoder search adds a smoothness penalty (lambda_s times the difference from the neighbouring tiles' vectors) that biases toward physically plausible fields at negligible bit cost. Tiles in STATIC_MV are excluded from extrapolation (head-locked content must not be warped), which fixes a known motion-smoothing artefact on menus for free. The frame-rate governor can then trade server render rate for bitrate per frame without changing the decoder's per-frame cost.

## 2.9 Hybrid mode (hardware HEVC base)

For headsets that cannot run the full decoder, the base layer is a plain HEVC stream through MediaCodec and the enhancement layer is decoded in compute. HEVC decoding is normative and bit-exact, so the encoder can mirror the base by decoding its own HEVC stream (hardware decode on the PC, adding 1 to 2 ms to the encode pipeline). Each enhancement tile chooses between two predictors: (a) the upsampled base tile of frame N (LCEVC-style, drift-free, no motion needed) and (b) warp(Out(N-1)), the pose-warped previous full-resolution output, which retains the detail the base lacks. The residual codes Out(N) minus the chosen predictor with the same transform and entropy tools; the mode table gains one entry (BASE) and everything else is unchanged. The hybrid mode gives up per-tile loss behaviour on the base (a lost base packet goes through HEVC's own reference invalidation) and inherits the 10 to 20 ms MediaCodec latency, so it is the compatibility fallback, not the low-latency path. Its value is that the enhancement layer, the tiling, the transport and the shadow model are shared with the full codec, so it is not a second codec to maintain.

## 2.10 Pseudo-code

Decoder, one workgroup (16x16 threads, 4 pixels each) per 32x32 tile:

```
predict_tile(tile, hdr, frame_hdr, ref, left_ref, out, state):
    H    = frame_hdr.H[tile.eye]                        // 9 x int32, Q8.24
    mv   = state[tile].last_mv + hdr.mv_delta           // Q.2 (1/4 pel)
    if hdr.mode == INTRA:           pred = 0
    elif hdr.mode == STEREO:        src = left_ref; corner[i] = tile_corner[i] << 6
    elif hdr.mode == STATIC_MV:     src = ref;      corner[i] = tile_corner[i] << 6
    else:                           src = ref
        for i in 0..3:                                 // 4 corners, 64-bit int math
            nx = H00*cx[i] + H01*cy[i] + H02;  ny = H10*cx[i] + H11*cy[i] + H12
            d  = H20*cx[i] + H21*cy[i] + H22
            corner[i] = ( (nx << 6) / d, (ny << 6) / d )   // Q.6, restoring division
    bbox = bounds(corner) + mv + filter_margin
    if bbox fits 48x48: load src[bbox] into shared memory (clamp-to-edge)
    for each pixel p owned by this thread:
        c   = bilerp_int(corner, p.local)                // Q.6, integer adds only
        c  += mv << 4;  c = (c + 2) >> 2                 // Q.4 (1/16 pel)
        v   = filter16(src_or_lds, c)                    // bilinear or Catmull-Rom, integer
        out[p] = clamp(v + residual[p], 0, maxval)       // residual from IDCT stage
    if hdr.mode != INTRA and hdr.mode != STATIC_MV: state[tile].last_mv = mv
    state[tile].held_frame_id = frame_hdr.id; state[tile].age_since_intra++ or = 0
```

Encoder, per tile, after the frame's global H is quantised:

```
decide_tile(tile, cur, shadow_prev, cur_left_decoded, depth, velocity, stencil, state, lambda):
    seeds = {0, state.last_mv}
    if depth:    seeds += parallax(head_translation, depth_median(tile))
    if velocity: seeds += median(velocity[tile])
    if stencil.head_locked: force = STATIC_MV
    if stencil.text:        force = INTRA_LOSSLESS
    cands = []
    cands += (WARP_SKIP,  mv=state.last_mv)              // free vector, no residual
    for ref, mode in [(warp(shadow_prev), WARP_MV), (shadow_prev, STATIC_MV), (cur_left_decoded, STEREO if right eye)]:
        mv = refine_qpel(diamond(full_search_4x(ref, seeds, +-16)))
        cands += (mode, mv, residual = cur - sample(ref, mv))
    cands += (INTRA)
    if rolling_refresh_due(tile) or state.age_since_intra > T or state.concealed_count >= 3: cands = [INTRA]
    best = argmin over cands of SATD(cur - reconstruct(cand, qp)) + lambda * bits(cand) + lambda_s * |mv - neighbour_mv|
    encode(best); update shadow tile with reconstruct(best)  // encoder runs the real decoder
```

## 2.11 Risks and the Phase 2 experiments

1. Parity risk: the warp may not beat HEVC's block ME at rest by any margin, and the gain during head motion may be smaller than the 2x to 4x estimated. Kill test: record 60 s each of VRChat, Beat Saber and Alyx as raw frames plus pose logs from WiVRn; encode with x265 (zerolatency, P-only) and with the Phase 2 codec; report BD-rate overall and on the 20 percent of frames with the highest angular velocity. Success: within 10 percent at rest and at least 30 percent better on the motion frames. Failure means the codec's case rests on latency and loss behaviour alone, which should be decided explicitly rather than assumed.
2. Resampling blur: warp-only chains may degrade faster than the residual can affordably fix. Test: PSNR of a 2 s warp-only chain under recorded head motion with bilinear and Catmull-Rom; if the Full profile filter does not hold above 35 dB for 30 frames on textured content the per-tile refresh rate must rise and the bit budget in 2.4 is wrong.
3. Adreno budget: the prediction kernel plus entropy decode plus inverse transform must fit about 4 ms at 8.4 Mpix x 90 Hz including the LDS staging. Phase 0 gates this; Phase 2 re-measures with the real kernel, and the fallback is 64x64 tiles with bilinear.
4. Shadow model correctness: any divergence between encoder shadow and client state is a permanent artefact until the next refresh. Test: loss injection (random, bursty, per path) in the PC-side simulator with a bit-exact assertion of shadow versus the real decoder every frame; the fuzzer must run for hours with zero mismatches before Phase 3.
5. Missing depth and velocity: most OpenXR applications submit neither, so the search must carry hands at 40 px parallax alone; the coarse +-16 px range plus the temporal seed may miss fast hands and the encoder must fall back to intra gracefully rather than smear.
6. Head-locked and mirror content: STATIC_MV covers the UI, but mirrors (very common in VRChat) are effectively a second moving camera and will cost intra-level bits; no tool here fixes that.
7. Patents: the integer warp follows expired MPEG-4 GMC and royalty-free AV1 global motion, but "pose delta as global motion parameters for a streamed VR frame" needs a proper search against Meta, Qualcomm, NVIDIA and Microsoft filings before anything ships.
8. Foveation interaction (cross-section): the homography is exact in linear render space, not in a foveated (non-uniform) streamed space. The foveation section must define the foveation map as an integer LUT so the warp can be composed as foveated -> linear -> warp -> linear -> foveated deterministically, at one extra LUT read per pixel. Phase 2 runs without foveation.

---

# 3. Vulkan implementation: encoder, decoder, and the compute budget

This section turns the tile architecture into dispatches, buffers and milliseconds. The governing constraint is the headset: an Adreno 650 that already struggled with one per-pixel sharpening pass. Every decision below is made with that number in front of it, and the Phase 0 gate exists because the estimate could be wrong by 2x in either direction.

## 3.1 Working numbers

| Quantity | Value |
|---|---|
| Frame | 2 views x 2048 x 2048 = 8.39 Mpixel |
| Frame period at 90 Hz | 11.1 ms (13.9 ms at 72 Hz) |
| GPU time already spent per vsync by WiVRn | reprojection + compositor, about 2 to 3 ms |
| Decoder budget we can defend | 5 ms p50, 7 ms p99 (pure compute) |
| Adreno 650 peak | 1.2 TFLOPS FP32, roughly 600 G int32 simple ops/s; assume 300 G/s sustained |
| Adreno 650 memory | LPDDR5 on a 64-bit bus, about 44 GB/s peak, 25 GB/s usable by the GPU |
| Bitstream per frame | 150 Mbit: 208 KB; 400 Mbit: 555 KB; 1 Gbit: 1.39 MB |
| Bitstream per 64x64 tile (2048 tiles) | 102 B at 150 Mbit, 270 B at 400 Mbit, 680 B at 1 Gbit |

The last row matters for the whole design. At 150 Mbit a 32x32 tile would carry 25 bytes. Per-tile headers, rANS flush bytes and per-datagram overhead are not free, so the normative tile is 64x64 (see 3.3 for the consequences for "tile = packet").

A frame-level ALU estimate: 8.39 Mpixel x 150 ops = 1.26 G ops = 4.2 ms at 300 G/s. Memory: about 105 MB per frame (table in 3.2.5) = 4.2 ms at 25 GB/s. ALU and memory overlap only partially. That is the whole story of the risk: the decoder lands at 4 to 6 ms on this GPU if everything goes right.

## 3.2 Decoder pipeline on the headset

### 3.2.1 Two dispatches, not one

The decoder is split into two dispatches per frame, plus an optional third for hybrid mode:

| Pass | Unit of work | Workgroup | Reads | Writes |
|---|---|---|---|---|
| A: entropy decode | 8 tiles per wave (8 rANS lanes per tile) | 64 threads | bitstream, tile table | coefficient buffer (int16), per-tile mode/MV/QP |
| B: reconstruct | one 64x64 tile | 256 threads | coefficient buffer, reference image | output storage image |
| C: hybrid enhancement | one 64x64 tile | 256 threads | HEVC base (AHardwareBuffer), previous residual image | output image, residual image |

A single fused kernel per tile was considered and rejected. Entropy decoding is inherently serial per rANS state, so it wants few lanes per tile and many tiles in flight; transform and prediction want many lanes per tile. Fusing them means either 8 of 256 lanes busy during the entropy stage or 64 rANS states per tile, and 64 states cost about 128 bytes of flush per tile against a 102 byte payload. The split costs a 16 MB round trip through a coefficient buffer (about 0.7 ms of overlapped bandwidth) and buys three things: the right dispatch shape for each stage, independent LDS budgets (Pass A needs its symbol tables in LDS, Pass B needs the transpose buffer), and incremental execution. Pass A runs on each tile-row group as its packets arrive (indirect dispatch, count patched by the network thread), so at the presentation deadline only Pass B remains. That hides most of the entropy cost under network arrival time.

Aggregate entropy work is small if occupancy is good: at 0.3 coded symbols per pixel, a tile has about 1200 symbols, 150 per lane, about 25 cycles each, which is 2048 x 8 x 3750 lane-cycles = 61 M lane-cycles, under 0.2 ms on 512 lanes. The measured number in Phase 0 will be larger because of memory latency on the table lookups, but the point stands: Pass A is cheap if there are enough tiles in flight, and 2048 tiles x 8 lanes is enough.

### 3.2.2 Pass A: interleaved rANS with a shared read pointer

Per tile: K = 8 interleaved rANS states (Duda's rANS, Giesen's interleaving; both public domain, no known patent claims on the basic scheme), 32-bit state, L = 2^16, 16-bit renormalization, 10-bit probability scale. All arithmetic fits in uint32: decode is `x = freq * (x >> 10) + (x & 1023) - cum`, with freq < 2^10 and x >> 10 < 2^22. No int64, no division in the decoder.

The 8 states read from one byte stream in a deterministic order. Each lane decides whether it renormalizes this step; a subgroup ballot masked to the 8-lane cluster and a bit count give each lane its offset from the shared pointer, and the cluster advances the pointer by the popcount. This requires ballot and a subgroup size of at least 8 with clusters not straddling subgroups; 32, 64 and 128 are all multiples of 8, so a 64-thread workgroup holds 8 tiles on Adreno (one wave) or 2 x 4 tiles on 32-wide hardware, and the code is identical. Flush cost is 8 states x 4 bytes, of which roughly 2 bytes per state are real overhead (the final state carries 16 useful bits), so about 16 bytes per tile.

Symbol decoding uses a 1024-entry cumulative-to-symbol table per context, 1 byte per entry, 8 contexts (significance, magnitude class, sign is bypass, DC plane, MV, mode) = 8 KB in LDS, loaded once per workgroup. Frequency tables are static per QP class in v1 and transmitted per frame in v2 (256 x 2 B per context).

Output: coefficients as int16 in block-raster order (8 KB per tile, 16 MB per frame) and a 16-byte tile record: mode (skip/inter/intra), 4 corner displacements in Q4 fixed point (int16 each) for the warp, QP, flags. Skip tiles write no coefficients; Pass A sets a bit in a skip mask and Pass B takes the cheap path.

Register budget: 8 rANS lanes x (state, pointer, cum, freq, symbol) plus the coefficient write pointer, under 32 VGPRs. Occupancy is limited by LDS (8 KB per group) rather than registers.

### 3.2.3 Pass B: one workgroup per 64x64 tile

256 threads. A 64x64 tile holds 64 blocks of 8x8; 4 threads per block, each thread owns 2 rows (16 coefficients).

1. Load 16 int16 coefficients (coalesced, 32 bytes per thread), dequantize: `c = (q * scale[QP][pos] + 8) >> 4`, all int32.
2. Row transform: 8-point integer DCT (the HEVC core transform is patent-encumbered by implementation detail in places; use the AV1/VP9-style or a Loeffler-derived integer lifting transform with published coefficients, or the JPEG XS style 5-3 wavelet for the lossless profile). Two 1D transforms of 8 points, about 44 adds and shifts each.
3. Write to LDS transposed: 64 x 64 x 2 B = 8 KB. Barrier.
4. Column transform: each thread reads 2 columns of its block, transforms, clamps to the residual range.
5. Prediction. Inter: the warp coordinate for each pixel is bilinear interpolation of the 4 transmitted corner displacements (Q4, 6 integer ops), then a bit-exact 4-texel bilinear from the reference image (4 imageLoad, integer weights, `(w00*p00 + w01*p01 + w10*p10 + w11*p11 + 128) >> 8`). Intra: DC-plane prediction (3.2.4). Skip: prediction only, no coefficients.
6. `out = clamp(pred + res)`, YCoCg-R to RGB (5 adds/shifts), one imageStore of RGBA8 or RGB10A2.

The hardware sampler is not used for the normative predictor. Sampler weight precision is vendor-specific (8-bit fractions on AMD and NVIDIA, undocumented on Adreno), and the encoder always runs on a different vendor than the decoder, so a sampler-based predictor would drift by +-1 LSB per frame until the next refresh. Four explicit loads hit the same cache line for almost every pixel; Phase 0 measures the real cost of gather-4 against one sampler tap. If gather-4 is the bottleneck, the fallback is loading the tile footprint plus a 16-pixel margin into LDS with coalesced loads (96x96x4 = 36 KB, over Adreno's 32 KB limit, so it would have to be 2 bytes per texel in a luma/chroma split), which is deferred as an optimization.

Register budget for Pass B is the tight one: 16 coefficients, 4 texel loads in flight, weights, coordinates. Target under 64 VGPRs so four groups fit per shader core. Adreno's compiler is opaque about spilling; the Phase 0 bench reports spill via the `VK_KHR_pipeline_executable_properties` statistics where available and by timing otherwise.

### 3.2.4 Intra without a wavefront

Spatial intra prediction in the H.264/HEVC sense creates a dependency chain across the 64 blocks of a tile: 15 wavefront steps with barriers and 4 active lanes per block. That is a poor fit and intra tiles are only 5 to 10 percent of tiles under rolling refresh, but a slow path still costs p99. Decision: the 64 DC coefficients of a tile form an 8x8 low-resolution image that is transformed and coded first (a second-level 8x8 DCT, 64 symbols). Pass B decodes the DC plane, and the predictor for each pixel is bilinear interpolation between the four nearest block DCs (planar-like). Fully parallel, no barrier beyond the transpose, about 12 ops per pixel. It gives up directional intra modes; at VR bitrates intra tiles are a refresh mechanism, not the workhorse, so the loss is acceptable and is measured in Phase 1. This is a bitstream decision and needs to be reflected in the syntax section.

### 3.2.5 Memory traffic and time estimate

| Traffic per frame (pure compute, 2 x 2048^2) | MB |
|---|---|
| Bitstream read | 0.2 to 1.4 |
| Pass A coefficient write (int16 dense) | 16.8 |
| Pass B coefficient read | 16.8 |
| Reference read (gather-4 through texture cache, nominal) | 33.5 (effective 40 to 50) |
| Output write (RGBA8 / RGB10A2, doubles as next reference) | 33.5 |
| Total | about 105 |

At 25 GB/s that is 4.2 ms if nothing overlaps. A sparse coefficient layout (run-length, roughly 4x smaller at typical QP) is the first optimization if Pass B is bandwidth-bound; it is not in v1 because the dense layout keeps Pass A trivial.

| Per-pixel op budget, inter tile | ops | fetches | stores |
|---|---|---|---|
| Entropy decode, amortized (0.3 symbols/pixel x 25) | 8 | | |
| Coefficient load, dequant | 6 | 0.06 (coalesced) | |
| Row + column 8x8 integer DCT (2 x 44 / 8) | 22 | | |
| LDS transpose traffic | 6 | | |
| Warp coordinate | 6 | | |
| Bit-exact bilinear (4 loads, weights, blend) | 14 | 4 (same line) | |
| Add, clamp, YCoCg-R to RGB | 10 | | |
| Store | 1 | | 1 |
| Total | about 75 | 4 | 1 |

That is inside the 150 to 300 budget on paper. Expected time on Adreno 650 at 90 Hz:

| Pass | Estimate |
|---|---|
| A (all tiles, if not hidden under arrival) | 0.5 to 1.0 ms |
| B | 3.5 to 5.0 ms |
| Total | 4 to 6 ms p50, 7 ms p99 |

Honest comparison with the CAS field data. The CAS pass was 9 texel taps plus one store per pixel per vsync on 2 x 2160^2; at the texture rates of this GPU that is 4 to 5 ms, and it ran on top of everything else every vsync, which is why it hurt. Pass B is 4 (cache-friendly) fetches, one store and about 5x the ALU, once per frame. It should land in the same 4 to 5 ms band. Two things must be said plainly. First, at a 90 fps stream "once per frame" and "once per vsync" are the same rate; the argument only helps at 72 Hz or when the stream runs below the panel rate. Second, this is GPU time the hardware HEVC path does not spend at all; the pure-compute decoder buys latency and prediction quality with 4 to 6 ms of GPU that the Pico 4 barely has. If Phase 0 measures 8 ms, the Pico 4 is a hybrid-mode device and pure compute waits for Adreno 740 class hardware (about 2.5x), and that is an acceptable outcome, not a failure of the design.

### 3.2.6 Subgroup portability rules for the shaders

- Workgroup sizes are 64 (Pass A) and 256 (Pass B); never assume a workgroup is one subgroup. Every cross-lane exchange beyond an 8-lane cluster goes through LDS with a barrier.
- Use `VK_EXT_subgroup_size_control` with `REQUIRE_FULL_SUBGROUPS` where offered; query `subgroupSize` at pipeline creation and refuse subgroups smaller than 8 (Mali Bifrost at 4 is unsupported for the pure-compute path; it gets hybrid).
- Cluster operations use `subgroupBallot` plus masks derived from `gl_SubgroupInvocationID & ~7`, never `subgroupClustered*` (weaker support on Adreno's proprietary compiler).
- Same SPIR-V binary everywhere; specialization constants for subgroup size and 10-bit mode, no vendor #ifdefs in normative code.

## 3.3 Consequence for transport: tile is not a packet

At 150 Mbit a tile is about 100 bytes and 2048 tiles at 90 fps would be 184 k datagrams per second, where UDP/IP overhead is 46 bytes each and the receiving CPU melts. The bitstream unit stays the tile; the transport unit must be a tile-row segment: consecutive tiles of one row packed to about 1200 bytes (12 tiles at 150 Mbit, 2 at 1 Gbit). Loss granularity becomes a segment, still concealed per tile. This contradicts the "tile = one datagram" rule in the design consensus and the transport section should adopt segments.

## 3.4 Phase 0 gate: the exact benchmark

A standalone Android app (NDK, C++, Vulkan 1.1, no OpenXR) run on the Pico 4 at 90 Hz with the display active and a fullscreen dummy reprojection pass (one sampler tap, one store, 2 x 2160^2) submitted every vsync so the decoder competes with realistic co-tenant work. Nothing is synthetic in shape; only the data is random.

Kernels, each real code that will be reused, timed with `VK_QUERY_TYPE_TIMESTAMP` pairs around each dispatch, `timestampPeriod` applied, 600 frames after 120 warm-up, reporting p50, p95, p99 and the on-device clock check for throttling (run 10 minutes, report the last-minute p50 against the first):

| Kernel | What it does | Pass thresholds |
|---|---|---|
| K1 copy | 8.39 Mpixel RGBA8 image to image via compute | reports achievable GB/s; expect over 20 |
| K2 gather-4 | per pixel: warp coordinate + bit-exact 4-load bilinear + store, from a full-frame reference | under 3.0 ms p50 |
| K2b sampler | same with one sampler tap | informational, quantifies the cost of bit-exactness |
| K3 idct | Pass B without prediction: coefficient load, dequant, 8x8 int DCT through LDS, store | under 2.5 ms p50 |
| K4 rans | Pass A on random symbol streams of 0.5 symbols/pixel, all 2048 tiles | under 1.5 ms p50 |
| K5 full | Pass A + Pass B as designed | under 5.0 ms p50, 7.0 ms p99 with the dummy reprojection pass running |
| K6 hybrid | MediaCodec HEVC 2x2048^2 at 90 fps into AHardwareBuffer, imported, plus Pass C | decoder latency p50 under 15 ms, Pass C under 2.0 ms |

Decision rule: K5 passes at 90 Hz: pure compute is the default on Pico 4. K5 between 5 and 8 ms: pure compute at 72 Hz or with 1.5x foveated tile reduction, hybrid the default. K5 over 8 ms: Pico 4 is hybrid-only; the pure-compute path continues on PC and next-generation Adreno. K6 failing on latency means MediaCodec low-latency mode is not working on this firmware and the hybrid path needs the vendor key `vendor.qti-ext-dec-low-latency.enable` verified.

## 3.5 Hybrid mode implementation

Base layer: HEVC through MediaCodec, exactly as WiVRn does today. Enhancement: the codec's tiles carry a residual relative to the base, temporally predicted from the warped previous residual (LCEVC-like layering, but LCEVC's specific tools are MPEG-5 Part 2 and licensed, so the enhancement syntax is the codec's own tile syntax with a different predictor, not LCEVC).

Data path without a copy:
1. `AImageReader_newWithUsage(..., AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, maxImages = 4)`; its `ANativeWindow` is the MediaCodec output surface. `KEY_LOW_LATENCY = 1` plus the Qualcomm vendor key; one output buffer released per input, never queued.
2. On the image-available callback, `AImageReader_acquireLatestImageAsync` gives the `AHardwareBuffer` and a sync fd. The buffer pool is small and recycled, so each distinct buffer is imported once: `vkGetAndroidHardwareBufferPropertiesANDROID`, a `VkImage` with `VkExternalFormatANDROID` (the decoder emits a vendor-tiled YCbCr format, typically UBWC NV12), memory bound via `VkImportAndroidHardwareBufferInfoANDROID`, and a `VkSamplerYcbcrConversion` on the external format. Imports are cached by buffer identity.
3. The sync fd is imported into a binary `VkSemaphore` (`VK_KHR_external_semaphore_fd`, `SYNC_FD`) and waited on by the Pass C submit. Pass C reads the base through the YCbCr sampler (one tap; here the sampler is allowed because the base is not in the normative bit-exact path, only the residual is), reads the warped previous residual with the bit-exact gather, adds the decoded delta, writes both the output image and the new residual image (signed 8-bit RGBA, 33.5 MB, or half resolution at 8.4 MB for the Lite profile).
4. Release: the submit signals a semaphore exported as a sync fd, passed to `AImage_deleteAsync`. No vkCmdCopyImage anywhere; the only extra traffic over pure compute is the residual image write and read.

Latency: the hardware decoder on XR2 Gen 1 delivers a 2x2048^2 HEVC frame 8 to 12 ms after the last slice arrives in low-latency mode and can add one full frame of pipelining if that mode silently fails; WiVRn's current 100 ms end to end includes this. Hybrid also loses tile-row pipelining on the base (MediaCodec delivers whole frames) although the enhancement tiles still decode incrementally. The hybrid path is a floor for weak devices, not the low-latency path.

## 3.6 Encoder pipeline on the PC

On Linux the WiVRn server's encoders already run on Monado's `VkDevice`, so the compositor's render target is a `VkImage` in the same device: no external memory, only an image memory barrier to `GENERAL` (storage read) and, if the encoder uses a dedicated compute queue, a queue family ownership transfer. Timeline semaphores carry the handoff: the compositor signals value F when frame F is rendered; the encode submit waits on it and signals its own timeline.

Passes per frame, all indirect where the tile count varies, no CPU between them:

| Pass | Shape | Work |
|---|---|---|
| E0 warp | fullscreen, 8x8 threads | warped reference from previous reconstruction and pose delta; writes the warped image and the per-tile corner displacements |
| E1 analyze | one group per tile | SAD of source against warped reference at 0 and 8 candidate integer offsets (+-4 px), variance, skip test; picks mode and MV, assigns QP = base + foveation offset + activity term + rate feedback; appends tile index to the inter/intra/skip lists via atomics |
| E2 transform | one group per listed tile | residual, forward 8x8 integer DCT, quantization with deadzone, RDO-lite (coefficient zeroing when rate estimate exceeds distortion gain, from a table), writes int16 coefficients and an exact symbol count |
| E3 reconstruct | one group per tile | the decoder's Pass B, same SPIR-V, writes the new reference |
| E4 entropy | 8 lanes per tile, 8 tiles per group | rANS encoding runs backwards over the symbol list, into a per-tile slot of bounded size (2 bytes per coefficient plus header); writes actual byte count |
| E5 packetize | one group of 1024 threads per view | prefix sum of tile sizes, compaction into tile-row segments, headers written by the shader (tile ids, sizes, pose id, timestamp, sequence), segment descriptor table for the network thread; rate feedback: actual bytes versus budget into the controller state buffer |

E3 being byte-identical shader code to the decoder is the single most important rule in the project: the encoder never has a "reference" that the decoder cannot reproduce. rANS encoding needs `x / freq`; integer division on GPU is 20 to 40 instructions but this is the PC side and 8 lanes per tile; a reciprocal table (`OpUMulExtended` is core SPIR-V) is an optimization.

Rate control on the GPU: the CPU's AIMD/BBR controller from WiVRn NX writes one number per frame, the byte budget, into a uniform ring. E1 converts it to a base QP through a per-stream model (bytes as a function of QP and activity, updated by E5 from the previous frame's actual bytes: a proportional correction with a clamp of +-2 QP per frame). No intra-frame two-pass; overshoot on a frame is absorbed by the pacer and corrected next frame. Per-tile-row re-encoding of outliers is a v2 option.

Output buffer to the network thread: E5 writes segments directly into a `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` buffer (system memory). GPU writes to it are DMA and coalesced by the compaction; host reads are cached, so `sendmmsg` sends straight from the mapped pointer. Where no cached host-visible heap exists (it exists on RADV, NVIDIA, ANV and Windows AMD/NVIDIA drivers), fall back to device-local plus `vkCmdCopyBuffer` at the end of each row group. Writing into the device-local host-visible BAR heap was rejected: host reads of write-combined memory are uncached and slow. `VK_EXT_host_image_copy` does not apply; it moves images, host to device, not packet buffers.

Row pipelining on the encoder: the frame is split into 4 row groups per view; each is its own command buffer signaling timeline value 8F + g. The network thread does `vkWaitSemaphores` on the next expected value and one `sendmmsg` per group. Per-frame CPU cost target: under 300 us at 90 fps, consisting of one `vkQueueSubmit` of pre-recorded command buffers (per-frame data through push constants and a uniform ring, no descriptor updates: all frame images live in one descriptor array indexed by frame slot), 8 semaphore waits and 8 `sendmmsg` calls.

Expected encoder time: RX 580 (6 TFLOPS, 256 GB/s) about 2.5 to 4 ms per frame for both views; 7900 XTX under 1 ms. The E0 and E3 fullscreen passes dominate on the RX 580 by bandwidth (about 250 MB per frame), which is fine against a 35 to 50 fps AMF ceiling.

## 3.7 Vendor differences

| Target | Subgroup size | Ballot | int64 | int16 storage | Notes |
|---|---|---|---|---|---|
| AMD GCN4 (RX 580, RADV and Windows) | 64 | yes | yes | yes | reference PC encoder platform |
| AMD RDNA (7900 XTX) | 32 or 64, driver chooses | yes | yes | yes | never assume which |
| NVIDIA | 32 | yes | yes | yes | |
| Intel ANV | 8, 16 or 32 per shader | yes | yes | yes | force 32 with subgroup size control; clusters of 8 work at any size |
| Adreno 6xx (Pico 4) | 64 (128 on 7xx) | yes | unreliable | yes | proprietary compiler; avoid clustered ops |
| Mali Valhall | 16 | yes | no | yes | hybrid only unless Phase 0 style bench passes |
| Mali Bifrost | 4 to 8 | partial | no | yes | unsupported for pure compute |
| Apple via MoltenVK | 32 | yes | no | yes | Metal has no 64-bit integer; sampler behavior differs |

Bit-exactness rules for the normative path (decoder and E3):
- int32 arithmetic only, with int16 storage. No float, no fp16, no int64, no integer division or modulo. Rounding shifts are written as `(x + (1 << (s - 1))) >> s` with arithmetic shift; SPIR-V defines this exactly.
- Shift amounts are compile-time constants or masked to the operand width: SPIR-V leaves out-of-range shifts undefined.
- No `OpSDiv`, `OpSRem`, `OpSMod` anywhere in normative shaders; no `precise`/`fast-math` questions arise because nothing is float.
- Every buffer and image load is bounds-clamped in the shader; `robustBufferAccess` behavior differs across vendors (zero versus garbage) and the codec cannot depend on it.
- Coefficient clamping ranges are normative so that overflow cannot differ by vendor.
- The CPU reference decoder is the specification; SPIR-V is validated against it, not the other way round.

## 3.8 Windows port

The Windows helper receives per-eye `ID3D11Texture2D` from SteamVR. Path: create a shared texture on the helper's D3D11 device (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`), copy the SteamVR texture into it on the D3D11 side (one GPU copy, unavoidable because SteamVR's texture is not created with sharing flags we control), then import the shared texture into Vulkan through `VK_KHR_external_memory_win32` with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` and a `VkImage` created with a matching format and `VkExternalMemoryImageCreateInfo`, checked with `vkGetPhysicalDeviceImageFormatProperties2`. Synchronization: prefer a D3D11.4 shared fence (`ID3D11Device5::CreateFence` with `D3D11_FENCE_FLAG_SHARED`) imported as a timeline semaphore via `VK_KHR_external_semaphore_win32` (`D3D12_FENCE` handle type), which makes the D3D11 copy and the Vulkan encode one timeline. Fallback where the driver lacks it: `VK_KHR_win32_keyed_mutex` with `VkWin32KeyedMutexAcquireReleaseInfoKHR` in the submit. Both work on AMD and NVIDIA Windows drivers; Intel needs the fence path. The RX 580 on Windows is GCN4 wave64 with full subgroup support, so the encoder shaders are the same binaries as on Linux.

## 3.9 Testing

- Reference decoder: single-threaded C++20, integer only, no SIMD, about 3000 lines, no dependencies. It is the normative specification; the syntax document is derived from it.
- Reference encoder: slow, exhaustive-ish, CPU, used only to produce conformance streams that exercise every syntax element (max magnitude coefficients, max displacement, all-skip frames, DC-plane extremes, 10-bit, lossless tiles, truncated tiles).
- GPU versus CPU diff harness: decodes each conformance stream on every available device, hashes the RGBA output per tile, reports the first mismatching tile and pixel. Runs on lavapipe and SwiftShader in CI without a GPU (both implement the required subgroup features; lavapipe subgroup size is 8, which is exactly why the cluster size is 8), on RADV and NVIDIA on the developer machines, and on the Pico 4 via a self-hosted adb runner nightly.
- Cross-vendor determinism: encode on AMD, decode on NVIDIA, lavapipe and Adreno; all hashes equal to the reference decoder. This test is the definition of done for Phase 1 and Phase 2.
- Fuzzing: libFuzzer on the reference decoder with a structure-aware mutator (tile boundaries, rANS state fields); property: never reads out of bounds, always emits a frame. The GPU decoder is fuzzed with the same corpus under `VK_LAYER_KHRONOS_validation` with GPU-assisted validation on lavapipe; timeouts are bugs.
- Quality harness: a server-side dump tool records raw render targets plus poses from WiVRn NX sessions; the harness encodes with the codec, x264 and x265 (`--tune zerolatency`, intra refresh, no B-frames, single reference, matched bitrates) and with NVENC/AMF captures where available, and reports PSNR, SSIM and VMAF (libvmaf via ffmpeg) plus BD-rate. Loss simulation drops tile segments at 1, 5 and 10 percent and reports drift versus the reference.
- Performance CI: the Phase 0 bench app remains in the tree as the regression benchmark; the nightly runner fails if Pass B p99 on the Pico 4 regresses by more than 5 percent.

## 3.10 Project structure

```
codec/
  CMakeLists.txt          C++20, CMake 3.25+, presets for linux, windows, android
  core/                   header-only: syntax constants, tables, tile record structs
                          (single .h shared by C++ and GLSL through a common-subset macro layer)
  ref/                    reference decoder and conformance encoder (no deps)
  shaders/                GLSL 4.60, Vulkan semantics, glslang to SPIR-V 1.4 at build time,
                          embedded as arrays; recon.comp is included by both decoder and encoder
  vk/                     device capability probe, pipeline cache, timeline and external-memory helpers
  vk-decoder/             Pass A/B/C, AHardwareBuffer import, decode-time telemetry
  vk-encoder/             E0 to E5, rate-control state, segment table, win32 interop
  tools/                  bench (Phase 0 app), conform, diff, fuzz, quality, dump
  tests/
```

Language: C++20, not Rust. WiVRn is C++/CMake on both ends, the Android client is NDK C++, and the codec's substance is in shaders and integer tables; Rust would add cargo-ndk and an FFI seam for no gain in the hot path. Shader language: GLSL through glslang, matching WiVRn (`reprojection.glsl`), Monado and every driver in the table; `GL_EXT_shader_explicit_arithmetic_types_int16` and `GL_KHR_shader_subgroup_ballot` are the only extensions needed. Slang was considered for its generics and module system and rejected for now because it adds a toolchain that neither WiVRn nor the Android build has, and the codec has perhaps fifteen shaders. HLSL via DXC was rejected on integer-semantics history and because there is no D3D target. `spirv-val` and `spirv-opt` run in CI.

Linking: `nx_codec` is a static library with a small C ABI (`nxcodec.h`) plus C++ convenience headers. WiVRn NX server gets a `video_encoder` implementation that owns the encoder passes and exposes segments to the existing pacer and FEC; the client gets a `decoder` implementation next to the MediaCodec one, selected by a new value in the protocol's codec enum (the protocol section should reserve it). The decoder's output image is handed to `reprojection.glsl` as a plain RGBA storage image, replacing the YCbCr sampler path.

## 3.11 Milestones and exit criteria

| Phase | Deliverable | Exit criteria (all measurable) |
|---|---|---|
| 0 (3 weeks) | bench app, capability probe | Phase 0 table filled on Pico 4; pure/hybrid decision recorded; K1 to K6 numbers in the tree |
| 1 (8 weeks) | intra-only codec: ref decoder, GPU decoder, GPU encoder, conformance, diff, quality harness | bit-exact on lavapipe, RADV, Adreno; within 1.0 dB PSNR of x264 intra (`--keyint 1`, zerolatency) at 100 to 400 Mbit on VR captures; Pass B under 5 ms p50 on Pico 4; encoder under 4 ms on RX 580; fuzz corpus 24 h clean |
| 2 (10 weeks) | pose-warped inter, DC-plane intra refresh, skip, per-tile reference tracking | cross-vendor determinism test green; BD-rate within 15 percent of x265 zerolatency single-reference on head-rotation sequences at 100 to 200 Mbit; 5 percent segment loss shows no drift beyond one refresh period; Pico 4 p99 under 7 ms at 90 Hz |
| 3 (6 weeks) | WiVRn NX integration, hybrid mode, telemetry | glass-to-glass under 40 ms at 150 Mbit on WiFi 6 measured by the existing HUD path, against about 100 ms today; encode plus decode under 12 ms combined; 1 hour session without a crash on Pico 4; hybrid mode selectable and functional |
| 4 (open) | stereo inter-view, foveated tiles, 4:4:4 fovea, depth stream | at least 25 percent bitrate saving at equal VMAF on the fovea region; decoder time not above the Phase 2 numbers |

## 3.12 Open risks

- The 4 to 6 ms estimate for Pass B rests on assumed Adreno int32 throughput and cache behavior for gather-4; a 2x miss puts the Pico 4 in hybrid-only mode.
- Adreno's proprietary compiler may spill or serialize the ballot-based shared-pointer scheme; the fallback is per-stream byte ranges in the tile header (about 8 extra bytes per tile).
- MediaCodec low-latency behavior on Pico firmware is unverified; the hybrid latency floor depends on it.
- The 16 MB coefficient round trip may be the bandwidth item that tips Pass B over; the sparse layout is the planned fix and should be prototyped in Phase 1 if K5 is within 20 percent of its threshold.
- Thermal throttling over a session can turn a 5 ms decoder into a 7 ms one; the decode-time governor from the design consensus must be wired from day one of Phase 3.

---

# 4. Transport, rate control, timing and scalability

This section defines how tiles leave the encoder, cross the air, and reach the display, and which control loops keep that path inside its time and bit budgets. Numbers are for the first target: 2 eyes x 2160x2160 rendered side by side (4320x2160), 64x64 tiles (68 x 34 = 2312 tiles per frame), 90 Hz (11.1 ms period), 150 Mbit/s baseline, WiFi 6 or USB 3 tethering.

## 4.1 Datagram layout: tile runs, not tile-as-packet

The brainstorm consensus said "tile = packet". The arithmetic does not support it. At 150 Mbit/s and 90 Hz a frame is 208 KB, so the average 64x64 tile is 90 bytes (22 bytes at 32x32). A 52-byte UDP/IP/codec header per tile would double the bitrate and produce 208k packets/s (832k at 32x32); the Android UDP receive path on an XR2 tops out between 50k and 100k packets/s even with `recvmmsg`. Conversely, at 1 Gbit/s a fovea tile is 4 to 8 KB, above any MTU.

Decision: the datagram is a **tile run**: a contiguous sequence of tiles from one tile row, packed until the payload budget is reached. Each tile remains an independently decodable bitstream; the datagram is only the loss unit. The header enumerates which tiles it carries, so a lost datagram maps to a known set of lost tiles and the per-tile reference tracking (section 4.5) still works. At 150 Mbit/s this gives about 150 datagrams per frame (13.5k pps); at 1 Gbit/s about 1000 per frame (90k pps), which is the honest ceiling of the client receive path and is discussed in 4.10.

Header (fixed 24 bytes, little endian, sent in the clear as AEAD associated data):

| Field | Bits | Notes |
|---|---|---|
| version/flags | 8 | 4 bits version, 4 bits flags: keyframe-run, partial-frame, lossless, last-run-of-frame |
| stream_id | 8 | WiVRn stream (per quad layer / eye pair geometry) |
| frame_id | 16 | wraps every 12 min at 90 Hz |
| tile_first | 16 | linear tile index in lens-space grid |
| tile_count | 8 | tiles in this run (1..255) |
| layer_id | 4 | 0 = base, 1..3 = enhancement |
| ref_delta | 2 | reference frame = frame_id - 1 - ref_delta; 3 = intra (see 4.5) |
| frag_idx / frag_count | 2 + 2 | fragments of an oversize tile (lossless tiles only) |
| pose_seq | 16 | index into the client's own pose history: the render pose the server used |
| path_id / path_seq | 2 + 14 | per-path sequence for loss detection and reordering |
| fec_group / fec_idx / fec_k | 8 + 4 + 4 | see 4.4; fec_idx >= fec_k marks a parity datagram |
| tx_ts | 32 | server clock, microseconds, wraps 71 min |
| payload_len | 16 | bytes of encrypted payload |
| enc_us | 16 | encode-finish minus render-finish for this row band, telemetry |

Total 192 bits = 24 bytes. Per-tile decoder data (QP, mode, byte length) is a 4-byte tile directory entry inside the payload, so a run of 20 average tiles costs 104 header bytes against 1800 payload bytes: 5.5% overhead, versus 30-50% for tile-as-packet.

The pose is never sent downstream. The headset generated the render pose, the server echoes `pose_seq`, and both ends compute the pose delta for warped prediction from the same history. The client keeps a 2 s pose ring (about 1000 entries at WiVRn's tracking rate). This is also what makes frameless presentation (4.3) free: every tile knows the pose it was rendered for.

**MTU.** Ethernet gives 1472 bytes of UDP payload over IPv4; we budget 1400 to leave room for WireGuard/Tailscale-style tunnels users do run WiVRn over, and for 802.11 encapsulation. WiFi A-MPDU/A-MSDU aggregation amortizes per-frame air overhead, so smaller datagrams cost little on the air; they cost in packets-per-second on the headset CPU. USB NCM/RNDIS tethering sometimes accepts a 9000 byte MTU; the connect handshake probes DF datagrams of 1400, 4000 and 8900 bytes on each path and uses the largest that echoes. On a jumbo USB path the run budget rises and pps falls by 6x, which is what makes 1 Gbit/s realistic on USB.

**Oversize tiles.** A tile that exceeds the run budget is not fragmented in normal modes: the rate controller caps each tile at `max_tile_bytes` (1400 - 24 - 4 = 1372 bytes) and the encoding workgroup re-encodes the tile with QP + 6 if the entropy coder overruns, at most twice, in the same dispatch. The exception is lossless mode (UI text tiles), whose worst case is 12 KB for a 64x64 4:4:4 8-bit tile; those are split into up to 4 fragments, and a tile with any fragment missing is a lost tile. The rate controller keeps fragmented tiles below 1% of the stream by demoting lossless tiles to near-lossless when the budget is tight.

**Encryption.** AES-256-GCM per datagram, 16-byte tag, header as associated data, nonce derived from (stream_id, path_id, path_seq, epoch counter) so no nonce goes on the wire; per-path subkeys via HKDF(session_key, path_id). GCM is a stream mode, so no padding and no alignment constraint on the tile bitstreams. ChaCha20-Poly1305 is the negotiated fallback for hosts without AES instructions. Decryption stays on the CPU network thread (ARMv8 crypto extensions do 2 to 4 GB/s per core, far above 125 MB/s at 1 Gbit/s), writing plaintext straight into a host-visible ring buffer the decoder dispatch reads. This is the one deliberate exception to "nothing on the CPU in the hot path": the CPU moves bytes and checks tags, it never parses the bitstream.

Rejected: RTP (12 bytes and a timestamp model that fights per-tile poses), QUIC datagrams (userspace stack cost on the XR2, no gain over raw UDP with our own FEC), IP fragmentation (one lost fragment loses the datagram).

## 4.2 Tile-row pipelining timeline

Encode, send, decode and present overlap per **row band** of 6 tile rows (384 pixel lines; 6 bands per frame, the last one 4 rows). Bands rather than single rows because each Vulkan submit on Adreno costs 50 to 100 us of overhead and each `sendmmsg` call has a floor; 34 dispatches per frame would waste more than they save.

Assumptions: 7900 XTX encodes the whole frame in 3.0 ms (0.5 ms per band), RX 580 in 8 ms; XR2 compute decode budget 4.0 ms per frame (0.67 ms per band); one-way network delay 3 ms on WiFi 6 with a quiet channel, 1 ms on USB; pacing spreads a band's datagrams over its encode time.

| t (ms) | Band 0 | Band 5 |
|---|---|---|
| 0.0 | render finished, band 0 encode starts | |
| 0.5 | encode done, packetize, first send | |
| 3.6 | last datagram of band 0 arrives (WiFi) | |
| 4.3 | band 0 decoded into the frame ring | |
| 3.0 | | encode done, send |
| 6.1 | | arrives |
| 6.8 | | decoded, frame complete |

Frame complete 6.8 ms after render finish on WiFi (4.8 ms on USB), versus the serial path where encode (3 ms), transfer (208 KB at 300 Mbit/s effective = 5.5 ms) and hardware HEVC decode (8 to 15 ms for 4K-class frames through MediaCodec, plus its 1 to 2 frame queue) add to 17 to 25 ms. On the RX 580 the encoder is the long pole (8 ms) and the pipeline finishes 12 ms after render, since network and decoder trail each band by a constant 3.8 ms.

Latency floor, render-finish to photons: 6.8 ms pipeline + compositor phase wait (0 to 11.1 ms, average 5.5) + runtime reprojection and scanout (about 5 ms on the Pico 4 LCD) = 12 to 23 ms. Motion to photons adds pose uplink (1 to 2 ms) and render (up to 11 ms) for a floor of 25 to 35 ms, against about 100 ms today. The gap is mostly waiting: whole-frame encode, MediaCodec's queue, a de-jitter buffer sized for whole-frame bursts. The air term (3 ms plus jitter) is not the long pole; the phase wait is, and only frameless presentation (4.3) and render-side pacing (server section) attack it.

## 4.3 Frameless, deadline-driven presentation

Every Android OpenXR runtime owns the panel. We submit a swapchain image plus the pose it was rendered for; the runtime reprojects to its own predicted display pose and scans out. We cannot present per row to the panel and we cannot change Pico's timewarp. "Frameless" therefore lives inside the WiVRn client's reprojection pass, immediately before `xrEndFrame`.

Mechanism:

1. The decoder writes into a **frame ring** of 4 full-size images per stream (4320x2160, 4:2:0 10-bit, about 60 MB total). Tile (N, t) lands in slot N mod 4.
2. Every tile position has a 4-byte metadata entry in a per-slot SSBO: `pose_seq`, `age` (frames since this position was last decoded, 0 = fresh), and `state` (decoded, concealed, undecodable).
3. When a band's tiles are missing at the deadline, the concealment dispatch fills them by warping the same tile region from slot N-1 by the pose delta between `pose_seq(N-1, t)` and the frame's render pose. This warp is deterministic and bit-exact, and the server reproduces it (4.5), so concealed pixels are legal references.
4. The client's frame loop wakes at `predicted_display_time - reproject_budget - runtime_margin` (runtime margin about 3 ms on Pico). It takes slot N as it stands, runs the reprojection shader with a per-tile pose lookup (each output tile warps from its own `pose_seq` to the requested display pose, sampling from slot N), and submits with the display pose. The runtime's timewarp then has almost nothing left to correct.
5. Tiles arriving after the deadline are still decoded into slot N so they serve as references and so the next frame's concealment starts from the best data. They are reported in the feedback as received-late, which for reference tracking is the same as received.

A frame with any concealed tile carries the partial-frame flag in telemetry; the HUD heatmap shows `age` per tile. Deadline policy: if more than 10% of tiles miss the deadline for 5 consecutive frames, the deadline moves 1 ms earlier (up to 4 ms), trading latency for fewer holes; it relaxes 0.2 ms per clean second. This is WiVRn's adaptive de-jitter buffer restated per band.

True per-row scanout ordering of the panel is out of reach: no Android runtime accepts per-row poses. The design stops at per-tile poses in our own warp.

## 4.4 Prioritized FEC

Loss on WiFi is bursty (an A-MPDU drop takes 8 to 64 consecutive datagrams) and on USB it is rare but comes as stalls. FEC repairs random single losses at fixed latency; re-prediction from acknowledged references (4.5) repairs bursts at the cost of one round trip of degraded prediction. We use both, with FEC deliberately small.

Tile classes are assigned by the foveation map: class A = fovea and base-layer tiles of quad layers (UI), class B = mid-eccentricity, class C = periphery and enhancement layers. Reed-Solomon over GF(256), systematic, k = 10 data datagrams per group, groups never cross a band boundary (so no extra latency), and parity count by class:

| Class | Share of bits | Parity per 10 | Overhead | Rationale |
|---|---|---|---|---|
| A | about 35% | 3 | 30% | a fovea hole is the one artifact users notice at once |
| B | about 40% | 1 | 10% | concealment is good here |
| C | about 25% | 0 | 0% | warped reference is near-indistinguishable |

Blended overhead is about 14.5% of bits, close to today's XOR 8+1 (12.5%) but concentrated where it matters; parity scales to 2/0/0 below 0.1% measured loss and 4/2/1 above 2%. RS rather than XOR because XOR cannot recover 3 of 13, and the CPU cost is under 1% of one core at 150 Mbit/s with NEON GF tables. A low-bitrate band with fewer than 10 datagrams uses k = its datagram count. GPU parity was rejected: the packetized bytes are already in host memory for `sendmmsg`.

**Feedback packet** (client to server, one per band, about 100 bytes, cumulative over the last 3 bands so a lost feedback costs nothing):

```
feedback {
  u16 frame_id; u8 band; u8 flags;
  u32 rx_ts_first, rx_ts_last;           // client clock, us
  u16 decode_us, conceal_tiles, late_tiles;
  u8  received_bitmap[tiles_in_band / 8]; // 68*6 = 408 bits = 51 bytes
  u8  path_loss[2], path_rtt_ms[2];       // per path
  u8  fec_recovered, fec_failed;
}
```

Uplink cost: 6 per frame x 90 Hz x about 100 bytes = 0.4 Mbit/s. The `received_bitmap` covers tiles, not datagrams: a tile is set only if its datagram decrypted and its bitstream decoded without error, so a corrupt-but-delivered tile is treated as lost.

## 4.5 Per-tile reference tracking and the reference epoch

The core problem: pose-warped prediction for tile t reads reference pixels from a window that crosses into neighbouring tiles. If the encoder assumes those neighbours hold frame N-1 and the client actually lost them, prediction diverges silently and the error spreads. The encoder therefore only references frame states it has been told are exact.

Rule: for tile t of frame N, the reference is the newest frame M in {N-1, N-2, N-3} whose 3x3 tile neighbourhood around t is fully acknowledged (received, or lost and therefore concealed by the deterministic warp, which the encoder replays on its mirror ring once the feedback says which tiles were lost). `ref_delta = N-1-M`. If no such M exists, the tile is coded intra (`ref_delta = 3`). Both ends keep the 4-slot ring; the encoder's ring is a mirror of the client's, reconstructed from feedback rather than assumed.

On USB (feedback RTT about 2 ms) nearly all tiles reference N-1; on WiFi (5 to 8 ms) the top bands reference N-1 and the bottom bands often N-2, because feedback for the bottom of N-1 has not arrived when the bottom of N is encoded. Pose-warped prediction makes this cheap: head motion between M and N is compensated by the warp, only scene motion is one frame older. Estimated cost, from HEVC experiments with a fixed 2-frame reference distance, is 5 to 10% more bits at equal PSNR; it is the price of never sending an IDR and is a Phase 2 measurement. With no feedback for 4 frames every tile goes intra at the capped size: a QP jump, not a stall. The invalidate/refresh/IDR ladder and the NVENC DPB=4 workaround both disappear.

## 4.6 Rate control

Inputs: the frame bit budget `B` from the existing controller (AIMD for loss, BBR-style for delivery-rate and RTT-gradient; both produce a target bit/s, and `B = target / fps` minus FEC and header overhead), the foveation map, and the per-tile analysis pass the encoder runs anyway (warped-SAD, variance, mean luminance, warp confidence).

Per-tile weight:

```
w_t = fov_t * percep_t * cplx_t
fov_t    = foveation weight from eccentricity, 1.0 at centre down to 0.15 at the edge
percep_t = 1 / (1 + 0.5 * head_speed_norm) * lum_mask(mean_luma) * class_boost
cplx_t   = clamp(warped_SAD_t / mean_warped_SAD, 0.25, 4)
bits_t   = B * w_t / sum(w)
```

QP from bits with the standard model where bits halve per 6 QP steps: `QP_t = 6 * log2(a_t / bits_t)`, with per-tile state `a_t <- a_t * (actual_t / predicted_t)^0.6` after each frame, clamped to 4x. One-pass predictive, like the R-lambda model in the HEVC reference software but per tile rather than per CTU row; it converges in 2 to 3 frames because a tile position sees similar content frame to frame. Two-pass was rejected: it doubles encoder time on the RX 580, the long pole.

GPU dispatch shape per frame:

1. Analysis: 2312 workgroups, per-tile stats to an SSBO (already required for mode decision).
2. Allocation: one workgroup of 256 lanes does the weight reduction and a prefix sum over 2312 entries, writes QP_t, class_t, budget_t. About 20 us.
3. Encode: 2312 workgroups, each writes its tile bitstream into a fixed-stride slot (1372 bytes, the cap) with its length; overrun handling as in 4.1.
4. Packetize: one workgroup per row computes a prefix sum over tile lengths, greedily assigns tiles to runs under the payload budget, writes headers and copies bitstreams into the host-visible send ring in send order. The CPU thread wakes per band on a timeline semaphore, encrypts in place, computes parity, and issues one `sendmmsg` per band per path.

Overrun at frame level: if `sum(actual) > 1.15 B`, the pacer stretches the sends over the remaining frame period rather than dropping, and the next frame's `B` is reduced by the excess. WiVRn's pacing already does the stretch.

**Scene cuts.** When the analysis pass reports more than 50% of tiles above the intra threshold, the allocator applies a global QP offset so the frame fits 1.5 B (about 3 ms of extra transfer at 150 Mbit/s, absorbed by the band deadline) and quality recovers over the following frames through the rolling refresh, fovea first. HEVC IDR spikes of 4 to 8x the budget stall the current pipeline; a capped 1.5x burst at lower quality replaces them.

**Bitrate range.** The same loop runs from 20 Mbit/s to 1 Gbit/s. Below about 40 Mbit/s an average tile gets 12 to 25 bytes, so periphery tiles drop to the half-resolution base layer and static content goes to skip tiles while the fovea keeps full resolution. Above 400 Mbit/s the QP floor is reached and remaining bits go to lossless UI tiles and 4:4:4 fovea. No codec or profile switch along the way.

### 4.6.1 The degradation ladder: blur, never block

A design requirement, not a tuning detail: when the budget is short the picture must lose texture
before it loses structure. H.264 at low bitrate raises QP everywhere and the eye sees 8x8 and 16x16
blocks. This codec must instead look like a scene whose textures were blurred, or whose surfaces went
low-poly, while edges, outlines and text stay crisp. Almost all of the machinery already exists;
the ladder only fixes the order in which rate control spends it.

Per tile, the encoder classifies content once per frame from two cheap statistics it already
computes for rate control: the log-variance activity term (Section 5.2) and a gradient-coherence
measure (ratio of structure-tensor eigenvalues over the tile, one pass over the pixels). Four classes
come out: **text** (UI stencil or high coherence plus high contrast), **edge** (high coherence),
**texture** (high activity, low coherence), **flat** (low activity). Under budget pressure the
classes descend different ladders:

| Step | Texture tiles | Flat tiles | Edge tiles | Text tiles |
|---|---|---|---|---|
| 1 | Low-pass weighting matrix (Section 1.5) drops high-frequency AC first | Same | QP +2 only | Untouched |
| 2 | `res_level` 1/2, bilinear upsample | `res_level` 1/2 | Untouched | Untouched |
| 3 | `res_level` 1/4 | `res_level` 1/4 | Low-pass weighting, QP +4 | Untouched |
| 4 | DC-plane only: 64 block DCs, planar interpolation | DC-plane only | `res_level` 1/2 | QP +4 within the lossless-or-near class floor |

Each step is what produces the wanted look. Step 1 is blur, because zeroing high-frequency DCT
coefficients is a low-pass filter, and the weighting matrix makes it a smooth one instead of a
threshold. Steps 2 and 3 are blur with no blocking at all, because the tile is coded small and
resampled up. Step 4 is the low-poly look: a tile reduced to its block DCs and planar interpolation
is a smooth gradient field, the piecewise-planar surface the user described, not a mosaic. Edge and
text tiles hold their step until the others are exhausted, so outlines and glyphs survive the frame
that has turned to soft shapes around them.

Cost: the classification is one pass on the encoder GPU per tile and reuses the activity statistic.
The decoder pays nothing it does not already pay; `res_level` upsampling and DC-plane reconstruction
are existing v1 tools. No new syntax. The only new thing is the encoder's ordering, and the ladder is
expressed as the per-class QP floor and `res_level` cap that Section 4.6 feeds into the per-tile bit
allocation.

Two optional v2 refinements, each behind a tool bit: a 1-bit-per-8x8 edge mask carried in the tile
payload so the decoder's `res_level` upsampler picks a sharper 4-tap kernel on edge blocks and the
bilinear elsewhere (about 8 bytes per tile, decoder cost one branch per block); and a per-tile
"contour" mode that codes the tile as a DC plane plus one straight edge with two side values, which
is the true low-poly primitive at a few bytes per tile. Neither is needed for the look; both make it
cheaper.

What "cheaper than H.264" means here, stated plainly: on the Pico 4 the H.264 decoder is fixed
function and costs no GPU time at all, so this codec cannot beat it on headset GPU cycles. It beats
it on bits per frame during head motion, on latency by the row-band pipeline, on loss behaviour by
per-tile references, and on what a low bitrate looks like. Those four are the contest.

## 4.7 Decode-time governor

The client reports `decode_us` per band and per frame plus a GPU frequency state bit from the Adreno sysfs node. The target is decode at or under 40% of the frame period (4.4 ms at 90 Hz), leaving the reprojection pass and the runtime's own compositor their share.

Knobs, in order, each step applied server-side at a band boundary:

1. Drop enhancement layers on class C tiles.
2. Class C tiles to the half-resolution base layer.
3. Shrink the full-resolution fovea radius by 10%.
4. Drop enhancement layers on class B.
5. Refresh rate 90 to 72 Hz (a runtime call on the client, coordinated over the control channel).

Step down when 3 consecutive frames exceed 110% of target or any frame exceeds 150%; step up one knob after 180 consecutive frames (2 s) under 70% with no deadline misses. The asymmetry is deliberate: XR2 thermal throttling arrives in seconds and leaves in minutes, and a governor that hunts every 100 ms makes the periphery shimmer.

The governor never touches bits, only pixels-of-work; the bits it frees are redistributed by the allocator. The two loops run on different timescales (bitrate: 100 ms to 1 s; governor: 3 frames down, 2 s up) and different variables, so they do not fight. The coupling is entropy decode, which is proportional to bits (about 1 bit per lane-cycle with rANS): 4800 bits per tile per frame at 1 Gbit/s is comfortable on 64-lane workgroups, and the fifth knob exists for when it is not.

## 4.8 Multipath and multi-user

WiVRn NX already stripes USB and WiFi. The tile-run header carries `path_id` and a per-path sequence so loss and reordering are measured per path, and the datagram rather than the byte stream is the unit, so a stall on one path never blocks the other.

Striping policy: class A runs are sent on both paths when the combined bandwidth allows it (duplication costs at most 35% of bits and only when both paths are up); class B and C runs are split by weighted round robin on the measured delivery rate of each path (BBR's estimate, per path). Reordering window: tiles are position-addressed and land in the ring by (frame_id, tile) regardless of arrival order, so no reorder buffer exists for decoding; the only wait is the FEC group's, which is bounded by the band deadline. Per-path keys as in 4.1. When a path stalls (RTT sample above 3x its baseline, or 20 ms with no datagram received while the other path is flowing), its weight goes to zero within one band and probing resumes with class C runs only.

**Multicast, honestly.** 802.11 multicast goes out at a basic rate (6 Mbit/s or lower), unacknowledged and unretried; a 100 Mbit/s base layer cannot be multicast on any consumer access point, and APs that convert multicast to unicast just send N copies with more loss than the server would. Decision: no multicast. Multi-user shares the encode, not the air: one encode of the shared base tiles, N unicast sends, per-user fovea tiles per user. Wired multi-user through a switch could use IP multicast later.

## 4.9 Latency telemetry

Stamps, per band, in two clocks:

| Stamp | Clock | Carried in |
|---|---|---|
| pose sample time | client | pose history (via pose_seq) |
| render finish | server | frame metadata, first run of band 0 |
| encode finish | server | `enc_us` in header |
| tx | server | `tx_ts` in header |
| rx first / last | client | feedback |
| decode finish | client | feedback (`decode_us`) |
| compose submit, predicted display | client | local, feedback flags |

WiVRn's clock offset estimator (ping/pong, minimum-filtered) gives the offset to about 0.3 ms on USB and 1 ms on WiFi, estimated per path since the two have different asymmetries; one-way delay = rx - tx - offset. The HUD shows a stacked bar per stage at p50/p99 over the last second (render, encode, queue, air, decode, deadline wait, runtime), the staleness heatmap, per-path rate/loss/RTT, FEC recovered/failed, re-predicted tiles per frame, decode ms against the governor target, and a histogram of frame-ready-to-deadline margin, the one number that says whether the deadline should move.

Audio and pose/input are out of scope; two couplings: the client pose ring must outlive the longest `pose_seq` in flight (2 s), and audio runs on the server presentation clock, so it does not stall on partial frames.

## 4.10 What others do and what is different here

- **ALVR**: hardware H.264/HEVC/AV1, NAL units over UDP, no FEC in current versions, loss handled by requesting an IDR; foveation by rendering a warped image, not in the codec.
- **Meta Link / Air Link**: hardware codecs through MediaCodec; Meta's developer documentation describes sliced encoding so encode, transfer and decode overlap per slice, dynamic bitrate, and Phase Sync for frame timing. Slices of a standard codec, frame-level references.
- **Steam Link**: H.264/HEVC with Valve's own transport and dynamic bitrate/resolution; codec detail not public.
- **NVIDIA CloudXR**: sliced hardware encode with per-slice packetization, frame-level references.
- **PSVR2**: DisplayPort, no compression. It sets the latency bar this design aims to approach on WiFi.
- **Apple Vision Pro Mac Virtual Display**: reported as a proprietary low-latency foveated stream over WiFi 6E; details not public.

Differences: the loss unit is a run of self-contained tiles, not a slice whose reference is a whole frame; the reference is what the client acknowledged, so there is no IDR path; the pose is shared state indexed by sequence number, not transmitted; partial frames are presented on a deadline with deterministic concealment that remains a valid reference; feedback is per band; FEC strength follows the foveation map.

## 4.11 Failure modes and risks

- **WiFi bufferbloat.** APs and the headset driver hold hundreds of milliseconds of queue. Pacing and BBR keep our own queue shallow; another client saturating the AP shows as RTT growth within a band and is answered with bitrate, not buffering. It appears in the heatmap as bottom-band holes first.
- **Android receive path.** WiFi power save adds 100 ms and must stay off (WiVRn's high-performance lock); the receive interrupt sits on one core of the XR2 and saturates around 80k pps. Jumbo MTU on USB is the mitigation; 1 Gbit/s over WiFi is not a first-release target.
- **USB tether stalls.** RNDIS on Linux hosts stalls 50 to 200 ms at intervals; NCM is better. Multipath switches within one band; a single-path USB user gets one warped frame.
- **Socket buffers.** Android defaults to about 200 KB of `SO_RCVBUF`; a 1.5x burst at 400 Mbit/s is 800 KB. Request 8 MB, verify it was granted (vendor ROMs cap `rmem_max`), else lower the burst cap.
- **Clock offset error.** A 1 ms error shifts every one-way measurement; the deadline controller therefore works only on the client-clock margin, so telemetry error never steers presentation.
- **Reference distance on WiFi.** The N-2 cost is estimated, not measured; if it exceeds 15% the fix is feedback per half band at 0.8 Mbit/s uplink.
- **Thermal.** Compute decode heats the XR2 more than the hardware decoder; the Phase 0 gate must measure decode watts, not only milliseconds.
- **Adreno submit overhead and timeline semaphore latency** (assumed 50 to 100 us and under 200 us) are unverified; if worse, bands become 3 per frame and the floor grows by about 1.5 ms.

---

# 5. Perception, foveation, future tools and the competitive landscape

## 5.1 Foveation model

### 5.1.1 One map, three consumers

Decision: a single per-frame **foveation map** is the source of truth. It is a small R8 texture, one texel per codec tile (for 2160x2160 at 64x64 tiles: 34x34 texels), generated on the server from (gaze or lens center, lens model, head velocity, content class). Three consumers read it:

1. the app's render pass, as a `VK_KHR_fragment_shading_rate` attachment (VRS), handed over through the OpenXR foveation extension (`XR_FB_foveation` family on Meta; on Monado we add a vendor extension that exposes the same map),
2. the encoder: per-tile sample scale, QP offset, chroma mode,
3. the client reprojection shader: the existing WiVRn NX defoveation pass, which now reads per-tile scale instead of only the global remap parameters.

If the app renders periphery tiles at VRS 2x2, the render target already contains 2x2-replicated shading; the encoder's 1/2 downsample of that tile is then lossless, so foveation is paid once, not twice. If the app ignores VRS (most do), the encoder still downsamples; the only loss is wasted GPU shading on the PC, not quality.

### 5.1.2 The grid and the lens

The tile grid stays axis-aligned and uniform in the encoded image (workgroup mapping must be trivial: tile id = (x/64, y/64)). Foveation is expressed per tile as:

- sample scale `s` in {1, 1/2, 1/4} (tile of 64x64 render pixels coded as 64, 32 or 16 samples per side),
- chroma mode in {4:4:4, 4:2:0, 4:1:0-like (chroma at 1/4 both axes)},
- QP offset `dQ` in [-8, +12] on the codec's own step ladder (Section 3),
- refresh priority (feeds the transport FEC class).

"Lens space" enters through a density function, not through warped tiles. For a rectilinear render target with half-FOV `F` the pixels per degree at off-axis angle `theta` is `ppd_render(theta) = ppd_center / cos^2(theta)`; at 45 degrees a tan-projected image spends 2x the pixels per degree that it spends in the center. The eye needs `ppd_needed(e) = 60 / (1 + e/e2)` with `e2 = 2.3` degrees (the standard cortical magnification / minimum-angle-of-resolution model, Geisler and Perry 1998 use the same constant), `e` = eccentricity from gaze in degrees, 60 ppd = 30 cycles/degree = 20/20 acuity. The scale for a tile is

```
s_tile = clamp_to_ladder( margin * ppd_needed(e_tile) / ppd_render(theta_tile) )
```

with `margin = 1.5` (contrast-detection thresholds are measured with gratings; natural images need less, but 1.5x keeps us on the safe side of the studies that report "indistinguishable" rather than "acceptable").

Pico 4 numbers: 2160 px across roughly 100 degrees of horizontal FOV, about 21 ppd average, about 24 ppd in the center after distortion (WiVRn default 1.0x render scale gives ppd_center ~ 22). The panel is therefore below foveal acuity everywhere, which caps what foveation can win in the center: the fovea tile is coded at `s = 1` and there is no headroom above it.

| eccentricity from gaze | ppd_needed x1.5 | ppd_render at 22 center | raw ratio | ladder | notes |
|---|---|---|---|---|---|
| 0 to 8 deg | 90 to 20 | 22 | > 0.9 | 1 | fovea + pad |
| 8 to 18 deg | 20 to 10 | 23 | 0.9 to 0.45 | 1 then 1/2 at 14 deg | mid ring |
| 18 to 35 deg | 10 to 5.5 | 25 to 33 | 0.4 to 0.17 | 1/2 | |
| 35 to 50 deg | 5.5 to 4 | 33 to 53 | 0.17 to 0.08 | 1/4 | lens over-sampling helps here |

Sample count with eye tracking: roughly 8% of the image at s=1, 25% at 1/2, 67% at 1/4 gives 0.08 + 0.25/4 + 0.67/16 = 0.18 of the full-resolution samples. That is the win the bit budget in Section 3 assumes for the Pro profile.

### 5.1.3 Fixed foveation (Pico 4, no eye tracking)

Without gaze the map is centered on the lens axis and eccentricity is replaced by `e' = max(0, e - R_box)`, where `R_box` is the region the eye visits often. VR gaze statistics (Sitzmann et al. 2018, "Saliency in VR", and later head/eye datasets) put roughly 90% of fixations within about 15 degrees horizontally and 10 degrees vertically of the head direction; users turn their head rather than their eyes for larger offsets. Decision: `R_box = 20` degrees horizontally, 15 vertically, elliptical. Beyond that the same ladder applies. The Pico 4's pancake lenses are sharp to the edge, unlike Fresnel optics whose blur beyond about 30 degrees would have hidden a further step, so no extra lens-blur credit is taken.

Result for the Pico 4: about 40% of tiles at s=1, 35% at 1/2, 25% at 1/4: 0.40 + 0.09 + 0.016 = 0.50 of the samples. Fixed foveation halves the work; it does not give the 5x of eye tracking. This matches field experience with the current continuous remap in WiVRn (users tolerate roughly a 2x reduction and complain above it). A user "foveation strength" slider scales `R_box` and `margin` together; the defaults above are the conservative point.

### 5.1.4 Eye-tracked foveation: latency budget and padding

Gaze-to-photon budget on a Quest Pro / Pico 4 Enterprise class device:

| stage | ms |
|---|---|
| eye camera exposure + tracker inference (90 to 120 Hz) | 8 to 11 |
| headset to PC (piggybacked on the pose packet) | 2 to 4 |
| render of the next frame with the new map | 5 to 11 |
| encode + transport + decode (this codec, Section 4 target) | 12 to 20 |
| scanout | 4 to 11 |
| total | 31 to 57 |

Padding rule: the s=1 region must still contain the fovea when the frame lands. Saccades are covered by saccadic suppression (vision is degraded for about 50 ms around a saccade) and handled by prediction below; the padding is for smooth pursuit, whose comfortable ceiling is about 30 deg/s with bursts to 100 deg/s. Decision: `pad = 0.05 deg per ms of gaze-to-photon latency + 1 deg tracker error`, so 40 ms gives 3 degrees and the s=1 radius is 5 + 3 = 8 degrees, which is the "0 to 8" row above. Albert et al. 2017 ("Latency requirements for foveated rendering in virtual reality", ACM TAP) found 50 to 70 ms tolerable with a gentle falloff and detection around 20 to 40 ms with an aggressive one; our ladder is the gentle kind, so 57 ms worst case is inside the tolerated range, and the map generator widens the pad automatically from measured latency telemetry (Section 4 stamps).

Saccade prediction: the main sequence relates saccade amplitude to peak velocity and duration (duration about 2.2 ms per degree + 21 ms). From the first 15 to 20 ms of a saccade's velocity profile the landing point can be predicted to within 10 to 20% of amplitude (Arabadzhiyska et al. 2017, "Saccade landing position prediction for gaze-contingent rendering", SIGGRAPH). Decision: the client runs the predictor (it has the raw samples at full rate) and sends a predicted landing point with a confidence; the server places the s=1 region at the landing point one to two frames early. During the saccade the fovea tiles at the old position degrade to 1/2 without penalty. Refresh cost: a tile jumping from 1/4 to 1 has a low-resolution warped reference, so its residual is nearly intra-sized; the predictor gives the encoder two frames to spread that cost through the enhancement layer instead of one spike. Without prediction, expect a 15 to 25% bit spike on the frame after each saccade at 3 saccades/s; with prediction, half of that.

Open risk: WiVRn NX's current foveation is a continuous separable remap (the render itself is squeezed, the client unwarps). Keeping it in v1 reduces app render cost, which per-tile scale cannot. But pose-warped prediction (Section 2) then operates in the remapped domain: the reprojection of the reference must unwarp, rotate, rewarp. That is a per-pixel function evaluation, cheap, but Section 2 must accept the remap as an input rather than assuming a rectilinear reference. Cross-section conflict flagged. Long term the per-tile scale should replace the continuous remap for the encoded image, with VRS carrying the render-cost win instead.

## 5.2 Perceptual quantization

All perceptual terms collapse into a per-tile QP offset and a chroma mode. The encoder does not carry a full HVS model per coefficient; the tile is the unit.

```
dQ_tile = dQ_ecc(e) + dQ_motion(v_slip) + dQ_lum(Ybar) + dQ_act(sigma) + dQ_class
QP_tile = clamp(QP_base + dQ_tile, QP_min_class, QP_max)
```

**Eccentricity** (`dQ_ecc`): on top of the sample scale, +0 inside the fovea, +2 in the mid ring, +4 at 1/2 tiles, +6 at 1/4 tiles. The scale removes resolution the eye cannot see; the QP offset removes contrast it cannot see: peripheral contrast sensitivity at the surviving frequencies is 2 to 4x lower than foveal.

**Motion** (`dQ_motion`): head rotation alone does not blur the retinal image; the vestibulo-ocular reflex counter-rotates the eye and the world stays stable. What matters is retinal slip: content motion that the eye does not track. The codec knows it exactly: the per-tile residual motion after pose warp (Section 2's correction vectors) is the slip for a fixating user. The spatio-temporal contrast sensitivity surface (Kelly 1979) shows the high-spatial-frequency limb falling by roughly a factor of 3 between 0 and 30 deg/s and effectively vanishing above 15 cycles/deg at 100 deg/s. Decision: `dQ_motion = 0` below 10 deg/s slip, +2 at 30, +4 at 60, +6 above 100 deg/s, per tile, with two safeguards: it decays over one frame when motion stops (masking after motion offset lasts about 50 to 100 ms, so one 11 ms frame is well inside it), and during fast head rotation (> 120 deg/s yaw) an additional +2 applies globally because reprojection blur and pursuit errors dominate anyway. Intra-refresh tiles ignore `dQ_motion` (their job is to be a clean reference).

**Luminance** (`dQ_lum`): threshold contrast rises in dark regions on an LCD with a finite black level, and the display's EOTF spreads codes thin in the shadows. Using Watson's DCTune luminance term, threshold scales as `(Ybar/Y_ref)^0.65`: a tile at 12% of reference luminance tolerates a step 4x smaller in absolute terms but its coded values sit in the dark end of the transfer curve where quantization is coarser per nit. Net rule: `dQ_lum = -2` for tiles with mean luma below 16/255 (protect shadows, this is where banding lives), 0 in the mid range, +2 for tiles above 220/255 (bright, saturated highlights mask well).

**Activity / contrast masking** (`dQ_act`): the x264 adaptive-quantization rule survives because it works: `dQ_act = -strength * (log2(sigma^2_tile) - log2(sigma^2_avg))` with strength 1.0 and clamp [-4, +4]. Flat tiles get finer steps, busy tiles coarser; the Watson contrast-masking exponent of about 0.7 is what the log-variance rule approximates.

**Class** (`dQ_class`): from the UI stencil / quad-layer metadata: text panels lock to lossless (QP_min_class forces it), passthrough tiles get +2 (camera noise masks), skip tiles have no QP.

**Chroma per eccentricity**: chromatic contrast sensitivity cuts off around 10 to 12 cycles/deg for red-green and 5 to 6 for blue-yellow (Mullen 1985), a third to a quarter of luminance. Decision: 4:4:4 in the fovea (s=1 tiles; text fringes are what 4:2:0 breaks), 4:2:0 in s=1/2 tiles, and chroma at 1/4 in both axes for s=1/4 tiles (the 4:1:0-like mode; the tile still stores Co and Cg, at 4x4 samples for a 16x16 luma tile). Additionally, the Cg (roughly blue-yellow) plane takes +2 QP relative to Co everywhere.

**Banding**: 8-bit output with an sRGB-like curve steps 1/255 per code, above the Weber fraction in dark gradients (sky, fog, menus). Decisions: the internal pipeline is 10-bit luma always, 8-bit only as a wire-format choice for the Lite profile; the decoder's output stage adds 1 LSB blue-noise dither (a 64x64 precomputed tile, offset per frame for temporal decorrelation) when writing to an 8-bit storage image; the existing 10-bit toggle becomes "keep 10-bit through to the panel where the compositor supports it". Dither costs 2 ops per pixel.

## 5.3 Quality metrics for VR

PSNR is the wrong tool for four reasons: it weighs every pixel equally when 80% of them are in the periphery at 1/4 sampling by design; it measures the encoded image, not what is displayed after lens warp, reprojection and defoveation; it is blind to temporal artifacts (tile pop-in on saccades, refresh flicker, warped-reference hole fill) which are the artifacts this codec actually produces; and it cannot compare a 4:4:4 fovea against a 4:2:0 periphery.

Decisions:

- Primary objective metric: **FovVideoVDP** (Mantiuk et al. 2021, SIGGRAPH), which takes gaze, display geometry (ppd), luminance and temporal content and outputs a JOD score; ColorVideoVDP (2024) as the color-aware successor once its VR tuning is stable. It is run in **display space**: the PC simulator decodes bit-exactly, runs the real client reprojection shader with the recorded poses, and compares against the same shader run on the uncompressed frames. This is the only way the warped-reference concealment is charged for what it actually shows.
- Secondary, cheap, per-tile: eccentricity-weighted SSIM (in the spirit of FWQI, Wang and Bovik 2001, and the foveated SSIM variants used in the foveated-streaming literature) computed in the same display space, used inside the encoder's rate control loop where a VDP is too slow. VMAF is kept only as a sanity number for the base layer when compared with HEVC.
- Temporal: a dedicated "pop-in" metric: per-tile JOD delta between consecutive frames in the fovea ring after a scale change, thresholded; tracked as a distribution, not a mean.
- Latency is a quality metric: motion-to-photon measured with a photodiode on the panel and an IMU on the headset, reported alongside every quality number; a codec that gains 1 JOD by adding 8 ms has lost.

Subjective methodology: sessions recorded from real games (poses, eye tracking where available, render targets at full rate) and replayed through the simulator to produce candidate streams, then presented on the actual headset with the recorded head motion re-driven through the compositor (the viewer's own head is tracked for comfort, the content path is the recorded one). Paired comparison (2AFC, "which is sharper / has fewer artifacts") against the uncompressed replay and against HEVC at matched bitrate, ITU-R BT.500 DSIS for absolute impairment, 15 to 20 participants, three content classes (fast game, text panel, social scene), and two task conditions (free viewing, and a reading / tracking task that forces gaze to the periphery). Report DMOS with confidence intervals and the fraction of trials where participants saw refresh or pop-in artifacts. Fixed foveation defaults (`R_box`, `margin`) come from this study, not from the model.

## 5.4 Future tools (versioned, optional, capability-gated)

Rule: every learned tool is **out of loop** (a post-filter on the decoded image the reference never sees) unless both sides can run integer-exact inference and exchange a weights hash at connect. This keeps drift impossible and lets the client refuse a tool with no effect on the bitstream. Capability bits: `TOOL_LEARNED_UPSAMPLE_V1`, `TOOL_LEARNED_DEBLOCK_V1`, `TOOL_QUANT_TABLE_V1`, `TOOL_NEURAL_SR_V1`; the bitstream carries hint flags only.

| tool | where it runs | size | cost | gate |
|---|---|---|---|---|
| peripheral upsampler: 3 conv layers 3x3, 8 channels, FP16, input 1/4 tile + warped ref, output 1/2 | headset GPU, s=1/4 tiles only | ~1.5k params | ~1.1k MAC per output pixel; on the 1/4-scale ring at 1/2 output that is ~60 GMAC/s per eye at 90 Hz | Adreno 740 (XR2 Gen 2) and up; Adreno 650 fails the 5.2 budget |
| learned deblocking / ringing filter, 4 layers, 16 channels, fovea tiles only | PC-class or XR2 Gen 2 GPU | ~7k params | ~4k MAC/px on 8% of pixels | strong headsets, Pro profile |
| content-adaptive quant tables: 64 coefficient weights x 3 planes x 8 tile classes, learned offline per game from opt-in sessions | encoder, table sent at connect | 1.5 KB | zero decode cost | any client (decoder just reads the table) |
| neural SR enhancement layer: ESPCN-style 4 layers x 32 channels int8, 1/2 to 1, replaces the coded enhancement layer for the mid ring | Hexagon NPU on XR2 Gen 2 (int8 TOPS figure vendor-marketed; treat as 2 to 3 usable TOPS, uncertain) | ~30k params | ~10k MAC/px; restricted to the mid ring (~25% of pixels) at 60 Hz with warp filling the other frames | NPU present and a Vulkan-to-NPU zero-copy path (AHardwareBuffer) proven |

The in-loop variant of the SR layer (server encodes residual on top of the SR prediction) would be a large gain but requires bit-exact int8 inference on both a desktop GPU and the Hexagon; this is deferred until such parity is demonstrated, and marked as the highest-risk item in this section.

## 5.5 Content types

| content | dominant property | codec response |
|---|---|---|
| games | fast motion, full-frame change on head turn, specular noise | pose-warped prediction carries most of it; `dQ_motion`; depth stream helps parallax; skip tiles rare |
| desktop / overlay panels (quad layers) | text, hard edges, static for seconds | lossless mode (RLE + rANS on YCoCg-R residual against the previous frame, most tiles skip), 4:4:4, no foveation on the panel while it is in the fovea box |
| passthrough MR | camera noise, alpha, composite with rendered content | alpha plane first class, +2 QP on camera tiles, chroma 4:2:0 everywhere (camera content has no text); the camera composite is done on the client, only the rendered layer with alpha is streamed |
| video players | content already compressed, 24 to 60 fps in a static frame | pass-through mode: the player's HEVC/AV1 bitstream travels untouched to the hardware decoder as a quad layer, and the codec only carries the surrounding scene; the compositor composes; this avoids double compression and frees the compute budget |
| social VR (VRChat) | dozens of animated avatars, high entropy, particles, mirrors | the hard case: pose warp helps the world, not the avatars; skip is rare; relies on the rate controller and on the periphery ladder; expect this to set the bitrate floor for a given quality |

## 5.6 Competitive landscape

**Hardware H.264/HEVC/AV1 paths (ALVR, WiVRn, Virtual Desktop, Steam Link, Meta Air Link).** Mature, free in engineering effort, hardware-decoded at low power, with excellent inter prediction for camera-like motion. ALVR and WiVRn add a continuous foveated remap before encode; Virtual Desktop adds client-side synchronous spacewarp, 10-bit HEVC, AV1 on Quest 3 and a Snapdragon super-resolution upscale; Meta Link exposes a "distortion curvature" knob that is fixed foveated encoding, plus a sharpening pass. What none of them can do: take the head pose into the predictor, refresh a single lost tile without an IDR or a slice trick, code alpha, code 4:4:4 on a mobile decoder, present a partial frame, or exceed the decoder's fixed pixel rate (the XR2 Gen 1 decoder is the reason these systems encode below panel resolution). Their latency floor is frame-granular by construction.

**JPEG XS (ISO/IEC 21122) and VC-2 (SMPTE 2042).** Intra-only wavelet codecs with line-based latency (32 lines for XS), visually lossless at 6:1 to 10:1, and simple enough to run in compute. They prove the intra tile design point this codec's Phase 1 resembles. They cannot use temporal redundancy, so 4K90 stereo sits at 500 Mbit to 1 Gbit, USB only. JPEG XS is patent-licensed (intoPIX, Fraunhofer and others); VC-2 is royalty-free by the BBC's declaration and is the safer reference for wavelet tools.

**LCEVC (MPEG-5 Part 2, V-Nova).** Base codec plus a compute-decoded enhancement of small-transform residuals with temporal residual prediction. It is the closest existing shape to the hybrid decode path here, and the strongest patent overlap: V-Nova licenses it commercially. Our differentiator is that the enhancement is predicted from a pose-warped reference and carries foveation and tiles, which LCEVC's residual layer does not. The FTO review must examine the enhancement-over-hardware-base structure specifically.

**GPU texture and lossless codecs (Oodle Kraken/Mermaid, Oodle Texture, GDeflate, BCPack; ASTC/BC7 hardware formats).** They demonstrate lane-interleaved entropy decoding at tens of GB/s on GPUs (GDeflate's 32-way interleave is the model for the per-lane rANS substreams), and RDO texture encoding (Oodle Texture) is the model for rate-distortion decisions on fixed-rate blocks. They have no perceptual model and no temporal prediction. One idea is worth keeping: a zero-compute mode where each tile is an ASTC 8x8 block set (2 bpp fixed) decoded for free by the sampler. At 2160x2160x2x90 that is 1.7 Gbit/s, viable over USB 3 with foveation and useful as a fallback when the compute budget is exhausted.

**Apple Vision Pro Mac Virtual Display.** Publicly known: a direct link to the Mac, eye-tracked foveated streaming (Apple stated foveation is used in the visionOS 2 ultrawide mode), hardware codec on both ends (HEVC assumed, unconfirmed). Excellent text quality, latency figures unpublished, closed. It validates eye-tracked foveated streaming as a product.

**PSVR2.** Wired DisplayPort, no compression, eye tracking with in-engine foveated rendering. Zero codec latency, zero flexibility, a cable. Its existence is the argument that the codec's success is measured against "uncompressed over a wire", which is the near-lossless USB4 end of our bitrate range.

**Meta Quest Link / Air Link foveated encoding.** Fixed foveated encoding via the distortion-curvature warp, dynamic bitrate, Link sharpening on the client; whether eye-tracked encoding is used with Quest Pro over Link is not publicly documented (uncertain). Same hardware-codec ceilings as above.

**NVIDIA CloudXR.** HEVC/AV1 via NVENC with server-side pose prediction and a client SDK; NVIDIA-only on the server. Recent versions advertise foveated encoding (uncertain on mechanism). Its pose handling is prediction of the pose for rendering, not pose-based prediction inside the codec.

**Google split rendering.** Android XR's split-rendering work is not documented in detail; the lineage runs through Seurat (2018, offline light-field simplification) and the Daydream-era work rather than a codec. Nothing to reuse, but a likely future platform for the client.

**Academic work.** Three threads matter. (1) Pose-based prediction: Levoy 1995 ("Polygon-assisted JPEG and MPEG compression of synthetic images") is the original render-locally-stream-the-residual idea; MPEG-4 Part 2 global motion compensation (1999) is prior art for warped prediction; Furion (Lai et al., MobiCom 2017), "Cutting the Cord" (Liu et al., MobiSys 2018) and Coterie (Meng et al., ASPLOS 2020) are the mobile-VR systems that split or predict; Shading Atlas Streaming (Mueller et al., SIGGRAPH Asia 2018) and QuadStream (Hladky et al., SIGGRAPH Asia 2022) stream object-space shading instead of video and are the strongest alternative architecture (they need engine integration, which we refuse to require). (2) Foveated coding and rendering: Guenter et al. 2012, Patney et al. 2016, Kaplanyan et al. 2019 (DeepFovea, the learned peripheral reconstruction our 5.4 upsampler descends from), Illahi et al. 2020 (foveated video encoding for cloud gaming), Ryoo et al. 2016 (foveated streaming on commodity devices), Lungaro et al. 2018 (gaze-aware 360 streaming), Tursun et al. 2019 (luminance-contrast-aware foveation). (3) Perception models we lean on: Albert et al. 2017 (latency), Arabadzhiyska et al. 2017 (saccade landing), Krajancich et al. 2021 (eccentricity-dependent flicker fusion; relevant to per-tile refresh flicker), Denes et al. 2020 (motion quality vs refresh and resolution), Mantiuk et al. 2021 (FovVideoVDP). Years and venues are from memory and should be checked before publication.

## 5.7 Patent and royalty summary

Not legal advice; an engineering map of where the mines are.

- H.264: the bulk of Baseline/Main patents (2003 filings) have expired or expire by 2027; pools (Via LA) still license. Not used here except as a hybrid base through the device's own licensed decoder.
- HEVC: three pools (Access Advance, Via LA, Velos Media) plus unaffiliated holders; the messiest landscape in video. Same position: only ever used through the licensed hardware decoder.
- AV1: royalty-free under the AOM license with defensive termination; Sisvel asserts a pool against it. Not used in v1 (no decoder on the Pico 4).
- LCEVC: commercially licensed by V-Nova; our hybrid layer must be reviewed against its claims.
- JPEG XS: licensed. VC-2: royalty-free; wavelet lifting (5/3) predates it and is safe.
- Entropy coding: rANS (Duda 2013) is published without patent; note the controversial Microsoft patent on specific rANS variants (granted 2022) and steer the implementation to the plain published construction. CABAC is patented and avoided.
- Transforms: 4x4/8x8 integer DCTs of the H.264 style have expiring patents; Haar/5-3 are clear. YCoCg-R (Malvar and Sullivan 2003) had Microsoft patents from that era, now at or past expiry (verify).
- Pose-warped prediction: Levoy 1995 and MPEG-4 GMC are prior art for the concept; Meta and Microsoft (Holographic Remoting) hold patents around reprojection-based remoting details. Foveated encoding has patents from Meta, NVIDIA, Sony and Apple on specific mechanisms.

Decision: no FTO cost before Phase 2 (research code, no distribution). A proper FTO review is required before Phase 3 ships in a WiVRn NX release, scoped to: pose-warped prediction with per-tile corrections, per-tile foveated quantization driven by eye tracking, the enhancement-over-hardware-base structure, and the entropy coder. Keep a written record of the public-domain sources for every tool from day one.

## 5.8 Why this is the future

The hardware trends all move the same way. WiFi 7 (320 MHz channels, MLO, 4K QAM) puts 1 to 2 Gbit/s at a headset in practice; USB4 makes 10+ Gbit/s a cable choice. At those rates the required compression ratio for 4K-per-eye 120 Hz stereo (about 2 x 4096 x 4096 x 120 x 15 bits, roughly 60 Gbit/s raw) is 30:1 to 60:1 over WiFi 7 and under 10:1 over USB4. Intra wavelet codecs already do 10:1 visually lossless; add pose-warped prediction and foveation and 40:1 near-lossless is credible. The problem stops being "squeeze the bits" and becomes "spend the least latency and the least headset power per delivered perceptual quality", which is a scheduling and perception problem, not a transform problem.

Every headset SoC generation multiplies compute (XR2 Gen 2: about 2.5x the GPU of Gen 1 plus an NPU; the next one more) while hardware video decoders stay bound to a fixed pixel rate, a fixed format list (no 4:4:4, no alpha, no tiles-as-packets), and a fixed feature set that changes only with new silicon. The XR2 Gen 1 decoder already cannot decode two 2160x2160 streams at 90 Hz, which is why every hardware-codec streamer encodes below the panel. A compute codec's ceiling rises with every SoC and its tools change with a software update; the 5.4 tools are the proof: none of them is a wire-format change.

Eye tracking is spreading (Quest Pro, PSVR2, Vision Pro, Pico enterprise models) and it converts the periphery, 90% of the pixels, into a near-free region for whoever can put per-tile scale and quantization into the bitstream and refresh a region in one frame. Hardware codecs can approximate this only through a global warp of the input image. 4K per eye and 120 Hz double down: pixel counts grow 3.6x and frame counts 1.3x, but the fovea does not grow at all, so the foveated bit budget grows far slower than the panel.

Finally, the content is synthetic and the streamer owns the render. It has the pose, depth, engine motion vectors, layer composition and UI stencil, information a camera-video codec is built without. A codec that takes those as inputs is not competing with HEVC and AV1 on their turf; it is a different tool, and the only one whose design space expands rather than shrinks with the hardware roadmap. Fixed-function decoders will keep the base layer of the hybrid path on weak devices for years; the compute path is where every new capability lands.

---

# 6. Reconciliation: decisions across sections

The five sections were written in parallel from one shared brief. Where they disagreed, or where one
section overturned a line of the brief, this section records the decision that the rest of the paper
now follows.

## 6.1 The datagram is a tile run, not a tile

The brief said "tile = packet". Sections 3 and 4 independently rejected it by arithmetic: at 150 Mbit
and 90 Hz a 64x64 tile averages about 90 bytes, so one datagram per tile would double the bitrate in
headers and push the headset past its UDP receive ceiling.

| Quantity | Value |
|---|---|
| Average tile payload at 150 Mbit, 90 Hz, 64x64 | ~90 bytes |
| Datagrams per second if tile = packet | 208 k |
| XR2 Gen 1 practical UDP receive ceiling | 50 to 100 k pps |
| Datagrams per second with 1400-byte tile runs | ~13 k |

Decision: a datagram carries a run of contiguous tiles from one tile row, packed to a 1400-byte
budget, with a 4-byte per-tile directory. Tiles remain independent bitstreams. The datagram is the
loss unit, the tile is the concealment unit. Only lossless UI tiles may fragment, to at most four
datagrams, and only in the Pro profile. Every mention of tile-as-packet elsewhere reads as tile run.

## 6.2 Tile size is 64x64

Sections 1, 3 and 4 all arrived at 64x64 for v1, for the same reason: per-tile header and rANS flush
cost must stay under about 8 bytes against a 75 to 90 byte average payload. 32x32 stays reserved by a
stream-header bit for high-bitrate USB profiles where the payload per tile is large enough to carry it.

## 6.3 Entropy coding: eight rANS lanes per tile in v1

Section 1 proposed 1 to 32 substreams chosen per tile so each lane decodes about 128 symbols. Section
3 designed the decoder around a fixed eight lanes so eight tiles fill one 64-wide Adreno wave and the
same code runs on wave32, wave128 and lavapipe's width of eight. Decision: v1 fixes eight lanes. The
`nsub_log2` header field stays in the syntax so v2 can vary it under a tool bit. Probabilities are
10-bit, state is 32-bit, tables are static per frame with built-in defaults, exactly as Section 1
specifies. Fixed state and precision keep the construction inside Duda's published rANS and outside
the 2022 Microsoft claims, pending the review in 6.9.

## 6.4 Intra is DC-plane, not directional

Sections 1 and 3 agree: v1 intra tiles carry a second-level DCT over the 64 block DCs with planar
interpolation, and no directional modes, because directional prediction needs an in-tile wavefront
that serializes the workgroup. Directional intra is a v2 tool bit, promoted to v1 only if Phase 1
shows more than a 40 percent bit gap against x264 intra on VR captures.

## 6.5 One quarter-pel vector per tile, five modes

Sections 1 and 2 agree on one quarter-pel motion vector per tile in v1, coded as a delta from the same
tile's previous vector, and on five tile modes: WARP_SKIP, WARP_MV, STATIC_MV, STEREO, INTRA.
Four vectors per tile is a v2 tool bit. Depth never reaches the decoder; per-tile plane parallax
collapses into the vector.

## 6.6 References: newest fully-acknowledged neighbourhood, four-slot ring

Section 2 modelled a single previous-frame reference with an encoder-side shadow of the client.
Section 4 pointed out that the warp reads across tile borders, so a tile's reference is only safe if
its 3x3 neighbourhood in the reference frame was received. Decision, combining both:

- The client holds a four-slot reference ring in display format (Section 1's RGBA8 or RGB10A2 slots).
- A tile references the newest frame among N-1, N-2, N-3 whose 3x3 neighbourhood is fully
  acknowledged, signalled by a 2-bit `ref_delta`, where 3 means intra.
- Missing tiles are filled by the deterministic concealment warp, identical to WARP_SKIP with the
  last vector, and the encoder replays the same fill on its mirror ring, so the shadow stays exact.
- Per-band feedback carries the received-tile bitmap; the encoder keeps eight frames of history.
- Rolling intra refresh at 1/180 of tiles per frame stays as a safety net. The IDR ladder and the
  NVENC DPB workaround in WiVRn NX are retired when the codec is active.

The estimated cost of lower bands referencing N-2 on WiFi is 5 to 10 percent of bits. It is
unmeasured and is a Phase 3 number to collect.

## 6.7 Pose travels twice, cheaply

Section 1 put the 26-byte pose in the frame header. Section 4 sends only a `pose_seq` that indexes the
client's own two-second pose ring. Both are kept: the frame header, replicated in the first datagram of
every band, carries the pose so a client with a gap in its ring still decodes, and every datagram
header carries the 16-bit sequence so the warp delta is computed from shared history. The cost is
26 bytes per band, six bands per frame.

## 6.8 Foveation and the warp

Section 2 requires the homography to run in linear render space. Section 5 defines the per-frame
foveation map as one R8 texel per tile, an integer lookup that composes deterministically. Decision:

- Phase 2 runs unfoveated to prove the predictor.
- Foveation in the codec is per-tile resolution level and QP, from Section 5's map. The predictor is
  formed at the coded resolution and the client always holds a full-resolution reference, so the warp
  never sees a non-uniform grid.
- WiVRn NX's continuous foveation remap stays for the hardware codecs and is bypassed when the codec
  is active. The render-cost win moves to Variable Rate Shading driven by the same map.

## 6.9 Multicast is out, multi-user shares the encode

Section 4 rejects multicast outright: consumer 802.11 sends multicast at the basic rate,
unacknowledged. Multi-user therefore shares the encoder's work, not the air. Each headset gets its own
unicast stream, and the shared base tiles are encoded once and sent twice.

## 6.10 The compute budget verdict

Section 3's estimate for the full compute decoder on Adreno 650 is 4 to 6 ms at p50 for two eyes at
2048 squared. At 90 Hz that is the same band as the CAS pass that hurt in the field. The paper
therefore expects the Pico 4 to land in hybrid mode, with the hardware HEVC decoder carrying the base
layer and compute carrying the pose-warped enhancement, and pure compute becoming the default on the
next Adreno generation and on PC-class clients. Phase 0 decides this with numbers, not hope.

## 6.11 Motion smoothing hands over

Section 2 replaces WiVRn NX's server-side block matcher with the codec's own per-tile correction
vectors as the extrapolation field, and excludes STATIC_MV tiles from extrapolation. The client warp
stays. This resolves the server-smoothing jitter problem by construction rather than by tuning.

## 6.12 Freedom-to-operate review, scoped

Before Phase 3 ships, a formal review of exactly four items: pose delta as global motion parameters
for a streamed VR frame (nearest relatives are expired MPEG-4 GMC and royalty-free AV1 global motion,
but Meta, Qualcomm, NVIDIA and Microsoft filings must be searched), view synthesis prediction in
3D-HEVC against the STEREO mode, the LCEVC family against the enhancement layer, and the 2022
Microsoft rANS patents against Section 1's fixed-precision construction. H.264 and HEVC are only ever
touched through the device's licensed hardware decoder.

---

# 7. Conclusion and roadmap

## 7.1 What the paper claims

- A codec whose only structure is the independent 64x64 tile can run every stage as one GPU
  workgroup per tile, on both the PC and the headset, with no CPU in the hot path except datagram
  decryption.
- Pose-warped prediction turns most of a VR frame into skip tiles during head motion, which is
  exactly when hardware codecs spend the most bits and add the most latency. At rest the paper expects
  parity with HEVC, not victory, and says so.
- Per-tile reference tracking with a deterministic concealment warp removes the IDR entirely.
  Loss costs bits in the affected tiles for one round trip and nothing else.
- Row-band pipelining and deadline presentation cut the render-to-photon floor to 12 to 23 ms
  against about 100 ms measured today, with the compositor phase wait, not the network, as the
  remaining long pole.
- One bitstream serves a hybrid hardware-plus-compute decoder on today's Pico 4 and a pure compute
  decoder on stronger hardware, selected by capability bits and by a decode-time governor.

## 7.2 What it does not claim

It does not claim a compression win over HEVC on static scenes, fast object motion or mirrors. It does
not claim the Pico 4 can run the pure compute path at 90 Hz. It does not claim a gigabit over WiFi in
the first release. Each of those is a measured number in the roadmap, not an assumption.

## 7.3 Roadmap

| Phase | Weeks | Deliverable | Exit criteria |
|---|---|---|---|
| 0 | 3 | Adreno benchmark app, capability probe | K1 to K6 measured on Pico 4, pure or hybrid decision recorded |
| 1 | 8 | Intra-only codec, reference decoder, conformance, fuzzing, quality harness | Bit-exact on lavapipe, RADV and Adreno; within 1 dB of x264 intra; Pass B under 5 ms p50; 24 h fuzz clean |
| 2 | 10 | Pose-warped inter, skip, per-tile reference tracking | Cross-vendor determinism green; within 10 percent of x265 zerolatency at rest, 30 percent better on motion frames; no drift under 5 percent loss |
| 3 | 6 | WiVRn NX integration, hybrid mode, telemetry, governor | Glass-to-glass under 40 ms at 150 Mbit on WiFi 6; one hour on Pico 4 without a crash; FTO review done |
| 4 | open | Stereo, foveated tiles, 4:4:4 fovea, depth stream | 25 percent saving at equal FovVideoVDP on the fovea; decode time unchanged |

## 7.4 The first thing to build

The Phase 0 benchmark in Section 3.4. It is about a week of work, it reuses real kernels, and every
later decision in this paper hangs on its table. Nothing else should be written until that table
exists.
