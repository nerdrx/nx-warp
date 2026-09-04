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
