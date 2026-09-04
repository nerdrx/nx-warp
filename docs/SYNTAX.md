# NX Warp v1 bitstream syntax

Normative specification of the NX Warp video bitstream, version 1.

This document and the CPU reference codec in `ref/` are the specification. Where
this document and `ref/` disagree, it is a bug in one of them and must be fixed;
neither may be "interpreted". The Vulkan decoder must reproduce the reference
decoder's output bit for bit on every conformance vector in `tests/vectors/`.

`docs/PAPER.md` is the design rationale. It is not normative. Where the paper
left a choice open, this document makes it and records it in
[Appendix A: decisions taken](#appendix-a-decisions-taken).

**Scope of this document.** The full v1 syntax is specified here, including the
inter-prediction fields, so that a Phase 1 decoder parses a Phase 2 stream's
headers and refuses it cleanly rather than misparsing it. The Phase 1
(intra-only) profile is called out in
[section 12](#12-phase-1-conformance-intra-only).

---

## 1. Conventions

* All multi-byte header fields are **little endian**.
* Bit fields inside a `u32` header word are listed **LSB first**.
* `x >> n` is an **arithmetic** right shift on signed values (rounding toward
  negative infinity) and a logical shift on unsigned values.
* `clamp(v, lo, hi)` returns `lo` if `v < lo`, `hi` if `v > hi`, else `v`.
* `clamp16(v) = clamp(v, -32768, 32767)`.
* All arithmetic in the normative decode path is **int32**. There is no
  floating point, no int64, and no division anywhere a decoder executes per
  coefficient or per pixel. The single exception is probability-table
  construction (section 9.4), which runs once per frame and may divide; it is
  called out explicitly there.
* "The decoder must reject" means the decode fails with an error. A conforming
  decoder never produces output from a stream it must reject, and never reads
  outside the buffer it was given.

Sample geometry:

| term | meaning |
|---|---|
| tile | 64x64 luma samples, fixed in v1 |
| block | 8x8 samples, the transform unit |
| plane | 0 = Y, 1 = Co, 2 = Cg, 3 = A |
| coding unit | one entropy-coded block: a DC plane or an 8x8 block |

---

## 2. Stream header

Sent once at the start of a stream, and repeated on every tile-map reset. Fixed
size **64 bytes**, followed by a TLV extension area of `ext_len` bytes.

| offset | size | field | notes |
|---|---|---|---|
| 0 | u32 | `magic` | `0x3156584E` (the bytes `N X V 1`) |
| 4 | u8 | `version` | 1 |
| 5 | u8 | `profile` | 0 = Lite, 1 = Full, 2 = Pro (informative) |
| 6 | u8 | `level` | informative |
| 7 | u8 | `tile_size` | bit 0: 0 = 64x64, 1 = 32x32. Bits 1-7 reserved, must be 0 |
| 8 | u16 | `width` | luma samples per eye |
| 10 | u16 | `height` | luma samples per eye |
| 12 | u8 | `eyes` | 1 or 2 |
| 13 | u8 | `bit_depth` | 8 or 10 |
| 14 | u8 | `num_layers` | 1..4 |
| 15 | u8 | `chroma_format` | 0 = 4:2:0, 1 = 4:4:4 |
| 16 | u32 x4 | `layer_desc[4]` | `type(4) : scale(2) : flags(26)`, LSB first. Entries above `num_layers` must be 0 |
| 32 | u64 | `tools` | mandatory tool bitmask, section 2.3 |
| 40 | u8 | `alpha_present` | 0 or 1: the stream carries a 4th plane |
| 41 | u8 | `color_transform` | 0 = none, 1 = YCoCg-R |
| 42 | u8 | `color_space` | what the coded planes mean, section 2.3 |
| 43 | u8 x19 | reserved | must be 0 |
| 62 | u16 | `ext_len` | byte length of the TLV area that follows |

Constraints a decoder must check:

* `magic` and `version` must match, else reject with a version error.
* `tile_size` bits 1-7 must be 0.
* `width` and `height` must be in `[16, 4096]` and even.
* `ceil(width / 64) <= 64` (the tile-row skip bitmap is 64 bits wide).
* `chroma_format`, `color_transform` and `alpha_present` must be in range.
* `color_transform == 1` requires `chroma_format == 1`.
* `color_space == 3` if and only if `color_transform == 1`.
* If `tools` has any bit set that the decoder does not implement, the stream
  must be refused. This is the only forward-compatibility gate; unknown
  *optional* information travels in TLVs instead.

### 2.1 TLV extension area

Immediately after the 64-byte header, `ext_len` bytes of TLV records:

```
u16 type
u16 length          // payload bytes, not counting this 4-byte prefix
u8  payload[length]
u8  pad[(4 - (length & 3)) & 3]     // zero
```

Records are walked until exactly `ext_len` bytes are consumed; a record that
would run past the end makes the stream malformed. **A decoder must skip every
type it does not recognise.** Types `0x8000`-`0xFFFF` are private. v1 defines no
mandatory TLV type; anything that must be understood goes in `tools` instead.

### 2.2 Colour space

`color_space` is **descriptive only**: the transform, quantizer and entropy
coder are byte for byte identical for every value. It tells the sink what the
coded planes are, so a decoder can hand them straight to a compositor.

| value | meaning |
|---|---|
| 0 | unspecified: planes are coded as given, range unstated |
| 1 | YCbCr BT.709, limited range |
| 2 | YCbCr BT.709, full range |
| 3 | RGB (requires `color_transform == 1`, i.e. YCoCg-R) |

Values 0-2 imply `color_transform == 0`: the planes reach the transform stage
untouched. This is the WiVRn path on Linux, whose capture is already
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`, so a YCbCr 4:2:0 source is coded as-is
with no colour conversion in either direction; only an RGB source (the Windows
helper) pays for YCoCg-R. `nxv-dec --nv12` writes such a stream back as Y plus
interleaved UV.

### 2.3 Tool bits

| bit | name | meaning |
|---|---|---|
| 0 | `INTRA_DC_PLANE` | DC-plane intra (section 7). Mandatory in v1 |
| 1 | `TRANSFORM_SKIP` | tiles may set `tskip` |
| 2 | `RES_LEVEL` | tiles may set `res_level != 0` |
| 3 | `CHROMA444` | the stream or its tiles may be 4:4:4 |
| 4 | `ALPHA` | a 4th plane is present |
| 5 | `LOSSLESS` | QP 0 + transform skip is used |
| 6 | `CUSTOM_TABLES` | frames may transmit probability tables |
| 7 | `NSUB_VAR` | tiles may use `nsub_log2 != 3` |
| 8 | `PER_TILE_CHROMA` | 4:2:0 tiles inside a 4:4:4 stream |
| 9 | `YCOCGR` | the YCoCg-R colour transform is in use |
| 10 | `INTER` | inter modes are used (Phase 2) |
| 11 | `WARP` | pose-warped prediction (Phase 2) |
| 12 | `STEREO` | inter-view prediction (Phase 2) |
| 13 | `LAYERS` | more than one layer |
| 14 | `BITDEPTH10` | 10-bit samples |
| 15 | `ENT_OFFSET_TABLE` | per-substream offset table instead of one stream |
| 16 | `ENT_BITPLANE` | bit-plane entropy fallback |
| 17 | `INTRA_DIR` | directional intra |
| 18 | `XFORM_WAVELET` | 5/3 wavelet transform |
| 19 | `XFORM_4X4_SPLIT` | per-block 4x4 transform split |

Bits 20-63 are reserved and must be zero. Capability negotiation is an
intersection: the sender only sets bits the receiver offered.

---

## 3. Frame

A frame is a self-delimiting unit: a 40-byte frame header, optional quantization
matrices and probability tables, then one tile-row structure per tile row.
`frame_bytes` in the header covers the whole unit, so a decoder can skip a frame
it does not want, and a file is simply a stream header followed by concatenated
frames.

### 3.1 Frame header (40 bytes)

| offset | size | field | notes |
|---|---|---|---|
| 0 | u16 | `frame_number` | wraps at 2^16 |
| 2 | u8 x26 | `pose` | opaque to the codec, section 3.2 |
| 28 | u8 | `base_qp` | 0..63 |
| 29 | i8 | `chroma_qp_off` | added to the tile QP for Co and Cg |
| 30 | i8 | `alpha_qp_off` | added to the tile QP for A |
| 31 | u8 | `quant_matrix` | 0..3 built in, 255 = custom (128 bytes follow) |
| 32 | u8 | `tables_present` | bit *k*: probability table set *k* is transmitted |
| 33 | u8 | `ref_slots` | reference slots this frame overwrites (Phase 2) |
| 34 | u8 | `flags` | bit 0: tile-map reset, bit 1: stereo inter-view, rest reserved |
| 35 | u8 | reserved | must be 0 |
| 36 | u32 | `frame_bytes` | total byte length of this frame unit, header included |

Then, in this order:

1. If `quant_matrix == 255`: **128 bytes** of custom weighting matrices, 64 for
   luma and alpha followed by 64 for chroma, each in raster order inside the
   8x8 block, Q4 (16 == 1.0). Values are clamped to `[1, 32]` on parse.
2. For *k* = 0..7 in ascending order, if bit *k* of `tables_present` is set:
   **120 bytes** of probability table deltas for set *k* (section 9.4).

Constraints: `base_qp <= 63`; `quant_matrix <= 3 || quant_matrix == 255`;
`frame_bytes >= 40` and `frame_bytes` must not exceed the bytes available. After
the last tile of the last row, exactly `frame_bytes` bytes must have been
consumed.

### 3.2 Pose

The 26 pose bytes are **opaque to the codec**: they are carried, hashed and
compared byte-wise but never interpreted by the normative decode path (the codec
does no floating-point arithmetic). Their agreed meaning is

```
u16 quat[4]     IEEE-754 binary16 bit patterns, x y z w
u16 angvel[3]   IEEE-754 binary16 bit patterns, rad/s
u32 pos[3]      IEEE-754 binary32 bit patterns, metres
```

= 7 half floats + 3 floats = 26 bytes, matching PAPER 1.2. The transport layer
also carries a 16-bit `pose_seq` per datagram (PAPER 6.7); that lives in the
transport header, not here.

### 3.3 Tile-row structure

For each tile row `row = 0 .. ceil(height/64) - 1`, in order:

| offset | size | field |
|---|---|---|
| 0 | u16 | `frame_number` (must equal the frame header's) |
| 2 | u8 | `row_index` (must equal `row`) |
| 3 | u8 | `tile_count` |
| 4 | u64 | `skip_bitmap` |

followed by `tile_count` tile structures.

Bit *i* of `skip_bitmap` set means tile *i* of this row is `WARP_SKIP` and is
**not** transmitted. `tile_count` must equal the number of tiles in the row that
are not marked skipped. In a stored file the row header is written once; in
transport it is replicated in every datagram of the row, together with the frame
header (PAPER 6.7), which is why both are small and self-checking.

---

## 4. Tile

### 4.1 Tile header (8 bytes, up to 3 optional)

Two little-endian u32 words. Bits are listed LSB first.

**word0**

| bits | field | notes |
|---|---|---|
| 0-1 | `layer` | 0..3 |
| 2 | `eye` | 0 or 1 |
| 3 | reserved | must be 0 |
| 4-15 | `tile_index` | index of the tile inside its row |
| 16-31 | `payload_len` | entropy-coded payload length in bytes |

**word1**

| bits | field | notes |
|---|---|---|
| 0-2 | `mode` | 0 `WARP_SKIP`, 1 `STATIC_MV`, 2 `WARP_MV`, 3 `INTRA`, 4 `STEREO`, 5-7 reserved |
| 3-4 | `res_level` | 0 = 64x64, 1 = 32x32, 2 = 16x16 coded; 3 reserved |
| 5 | `chroma444` | 1 = this tile's chroma is coded at full tile resolution |
| 6-7 | `alpha_mode` | 0 opaque, 1 constant (byte follows), 2 coded, 3 reserved |
| 8-13 | `qp_delta` | signed 6-bit two's complement, -32..+31 |
| 14-16 | `table_set` | probability table set 0..7 |
| 17-19 | `nsub_log2` | rANS lane count is `2^nsub_log2`, 0..5 |
| 20 | `mv_present` | one quarter-pel MV (2 x i8) follows |
| 21-22 | `ref_sel` | reference slot: 0 newest, 1..3 older |
| 23 | `tskip` | the whole tile skips the transform |
| 24-25 | `wgt` | enhancement-layer blend weight: 0, 1/4, 1/2, 3/4 (of the spatial hypothesis) |
| 26-31 | reserved | must be 0 |

Then, in this order:

1. `i8 mv_x, i8 mv_y` if `mv_present`
2. `u8 alpha_value` if `alpha_mode == 1`
3. `payload_len` bytes of rANS payload

Constraints: `res_level != 3`; `alpha_mode != 3`; `nsub_log2 <= 5`;
`mode <= 4`; word0 bit 3 and word1 bits 26-31 zero; `chroma444` may only be 1 if
`chroma_format == 1`; `alpha_mode != 0` requires `alpha_present`; `tile_index`
must equal the tile's position in the row.

### 4.2 Tile geometry

```
coded_size   = 64 >> res_level                       // luma and alpha
chroma_size  = (chroma444 ? 64 : 32) >> res_level    // per tile
nb_luma      = coded_size / 8                        // blocks per edge
nb_chroma    = chroma_size / 8
```

| plane | full extent in the picture | coded extent |
|---|---|---|
| Y, A | 64 | `coded_size` (64, 32, 16) |
| Co, Cg, 4:2:0 stream | 32 | `chroma_size` (32, 16, 8) |
| Co, Cg, 4:4:4 stream | 64 | `chroma_size` (64, 32, 16 with `chroma444`; 32, 16, 8 without) |

The per-plane **upsampling factor** is `full_extent / coded_extent` and is one
of 1, 2, 4, 8 (8 only for a 4:2:0 tile at `res_level` 2 inside a 4:4:4 stream).
Reconstruction upsamples in **one** bilinear step by that factor; it never
cascades two steps.

Tiles at the right and bottom edge of a picture whose dimensions are not
multiples of 64 are coded as full tiles; the samples outside the picture are
reconstructed and then discarded.

### 4.3 Sample domains

| | plane 0 (Y) | planes 1, 2 (Co, Cg) | plane 3 (A) |
|---|---|---|---|
| `color_transform == 0` | `[0, 255]`, offset 128 | `[0, 255]`, offset 128 | `[0, 255]`, offset 128 |
| `color_transform == 1` | `[0, 255]`, offset 128 | `[0, 511]`, offset 256 | `[0, 255]`, offset 128 |

`maxval` is the top of the range and `dc_offset` the offset in the table above.
Both appear in the DC-plane and reconstruction formulas below.

---

## 5. Colour

### 5.1 YCoCg-R

When `color_transform == 1` the picture handed to the encoder and produced by
the decoder is **R, G, B in planes 0, 1, 2** and the codec applies the
exactly-reversible YCoCg-R lifting (Malvar and Sullivan, 2003):

```
forward:  Co = R - B
          t  = B + (Co >> 1)
          Cg = G - t
          Y  = t + (Cg >> 1)
          plane0 = Y ; plane1 = Co + 256 ; plane2 = Cg + 256

inverse:  Co = plane1 - 256 ; Cg = plane2 - 256
          t  = Y - (Cg >> 1)
          G  = Cg + t
          B  = t - (Co >> 1)
          R  = B + Co
```

`>>` is arithmetic. The chroma planes are 9-bit, biased by 256. The transform is
applied before subsampling and after upsampling, so a 4:2:0 tile in a YCoCg-R
stream subsamples in the YCoCg-R domain.

When `color_transform == 0` (the normal YUV path used by `nxv-enc --pix
yuv420p|yuv444p`) planes are coded exactly as given.

### 5.2 Chroma subsampling

Downsampling (encoder, informative) is a rounded 2x2 average
`(a + b + c + d + 2) >> 2`. Upsampling (decoder, **normative**) is the fixed
half-phase bilinear of section 8, which for factor 2 produces exactly the
weights 3/4 and 1/4.

---

## 6. Transform

### 6.1 Constants

Nine-bit constants, `round(512 * cos/sin)`:

| name | value | equals |
|---|---|---|
| `C4` | 362 | `512 cos(pi/4)` |
| `C2` | 473 | `512 cos(pi/8)` |
| `S2` | 196 | `512 sin(pi/8)` |
| `A1` | 502 | `512 cos(pi/16)` |
| `A3` | 426 | `512 cos(3pi/16)` |
| `A5` | 284 | `512 sin(3pi/16)` |
| `A7` | 100 | `512 sin(pi/16)` |

These are our own constants, not HEVC's or AV1's matrices. The flow graph is the
Loeffler-Ligtenberg-Moschytz factorization (1989, expired): 11 multiplies and 29
adds per 8-point 1D transform.

### 6.2 Inverse 1D transform (normative)

Input `x[0..7]` int32, output `y[0..7]` int32. Gain is exactly `2^10` relative to
the orthonormal DCT-III.

```
// even part
t0 = (x0 + x4) * C4
t1 = (x0 - x4) * C4
t2 =  x2 * S2 - x6 * C2
t3 =  x2 * C2 + x6 * S2
e0 = t0 + t3 ;  e3 = t0 - t3
e1 = t1 + t2 ;  e2 = t1 - t2

// odd part
A = x1 * A1 + x7 * A7
B = x1 * A7 - x7 * A1
C = x3 * A3 + x5 * A5
D = x3 * A5 - x5 * A3
O0 = A + C
O3 = B - D
P  = A - C
Q  = B + D
O1 = ((P + Q) * C4 + 256) >> 9
O2 = ((P - Q) * C4 + 256) >> 9

y0 = e0 + O0 ;  y7 = e0 - O0
y1 = e1 + O1 ;  y6 = e1 - O1
y2 = e2 + O2 ;  y5 = e2 - O2
y3 = e3 + O3 ;  y4 = e3 - O3
```

### 6.3 Inverse 2D transform (normative)

`src[64]` are the dequantized coefficients in raster order inside the block,
index `u * 8 + v` with `u` the vertical and `v` the horizontal frequency.

```
pass 1 (rows):    for each row r: idct8_1d(src[r*8 .. r*8+7]) -> out[0..7]
                  tmp[c*8 + r] = clamp16((out[c] + 64) >> 7)
pass 2 (columns): for each row r of tmp: idct8_1d(tmp[r*8 ..]) -> out[0..7]
                  dst[c*8 + r] = clamp16((out[c] + 4096) >> 13)
```

Both passes write transposed, so `dst` comes out in spatial raster order
`y * 8 + x`. Total shift is 20 and total gain 1, so a coefficient of 1024 at
position 0 reconstructs a flat 128.

**Shift chain and intermediate ranges** (copy these exactly; the GPU passes
must match bit for bit):

| stage | shift | rounding | clamp | worst-case magnitude before the shift |
|---|---|---|---|---|
| inverse pass 1 | `>> 7` | `+64` | int16 | `32767 * 2703 = 8.9e7` (fits int32) |
| inverse pass 2 | `>> 13` | `+4096` | int16 | `32767 * 2703 = 8.9e7` |
| forward pass 1 | `>> 6` | `+32` | int16 | `511 * 4096 = 2.1e6` |
| forward pass 2 | `>> 14` | `+8192` | int16 | `32767 * 4096 = 1.3e8` |

The 1D flow graph has gain exactly `2^10` per dimension, so the two inverse
shifts must sum to 20 for unit gain; 7 + 13 and 8 + 12 both do. The reference
uses **7 and 13** because keeping the extra fractional bit through the
transpose measures better: against a float IDCT on unclamped input the maximum
error is 0.699 with 7/13 versus 0.784 with 8/12, and the forward-inverse round
trip on random +-255 residuals has an RMS error of 0.347 versus 0.357. Both
are within a maximum of 2 LSB, so the choice is precision, not correctness --
but it is a bitstream choice and 8/12 is *not* conformant.
(PAPER 1.4's "7 after the first dimension, 12 after the second" sums to 19 and
would leave a residual gain of 2; see Appendix A item 10.)

The `clamp16` after pass 1 is **normative**: it bounds the intermediate to 16
bits so a GPU may keep the transpose buffer in `int16` LDS, and it is reachable
with legal (if pathological) coefficient values. Dequantized coefficients are
themselves clamped to `int16` (section 6.5), which bounds every product in the
transform to about `8.9e7`, comfortably inside int32.

### 6.4 Forward 2D transform (informative)

The encoder uses the exact transpose of the flow graph, with shifts `>> 6` after
the first pass (clamped to int16) and `>> 14` after the second, giving
coefficients on the orthonormal DCT-II scale. The forward transform is not
normative: any encoder may produce any coefficients it likes. It is specified in
`ref/src/transform.cpp` so results are reproducible.

### 6.5 Quantization

The quantizer step table is Q4 fixed point, `qstep[qp] = round(16 * 2^(qp/6))`,
so `qstep[0] = 16` is a step of exactly 1.0:

```
   16,    18,    20,    23,    25,    29,    32,    36,
   40,    45,    51,    57,    64,    72,    81,    91,
  102,   114,   128,   144,   161,   181,   203,   228,
  256,   287,   323,   362,   406,   456,   512,   575,
  645,   724,   813,   912,  1024,  1149,  1290,  1448,
 1625,  1825,  2048,  2299,  2580,  2896,  3251,  3649,
 4096,  4598,  5161,  5793,  6502,  7298,  8192,  9195,
10321, 11585, 13004, 14596, 16384, 18390, 20643, 23170
```

Per-plane QP:

```
qp_tile   = clamp(base_qp + qp_delta, 0, 63)
qp(Y)     = qp_tile
qp(Co,Cg) = clamp(qp_tile + chroma_qp_off, 0, 63)
qp(A)     = clamp(qp_tile + alpha_qp_off,  0, 63)
qp(DC plane of a plane p) = qp(p) >> 1
```

**Dequantization (normative)**, per coefficient at raster position `i`:

```
t = (qstep[qp] * w[i] + 8) >> 4          // Q4 step, at most 46340
c = clamp16((q * t + 8) >> 4)
```

with the weight `w[i]`:

* DC-plane coding units: `w[i] = 16` (flat) at every position.
* Transform-skip blocks: `w[i] = 16` (flat).
* Normal blocks: the frame's weighting matrix, luma matrix for planes 0 and 3,
  chroma matrix for planes 1 and 2.

Weights are in `[1, 32]` and quantized levels in `[-32767, 32767]`, so
`q * t` never exceeds `1.52e9` and the whole path is int32-safe with no
division and no saturation logic beyond `clamp16`.

Built-in weighting matrices, with `s = u + v` (`u` vertical, `v` horizontal):

| `quant_matrix` | luma / alpha | chroma |
|---|---|---|
| 0 | `16` (flat) | `16` (flat) |
| 1 | `min(32, 16 + s)` | `min(32, 16 + s + (s >> 1))` |
| 2 | `min(32, 16 + 2s)` | `min(32, 16 + s + (s >> 1))` |
| 3 | `min(32, 16 + s + (s >> 1))` | `min(32, 16 + s + (s >> 1))` |
| 255 | custom bytes 0..63 | custom bytes 64..127 |

(That is: matrix 0 is flat everywhere; matrices 1-3 use the built-in chroma
matrix, index 3's formula, for the chroma planes.)

**Quantization (encoder, informative)** is a dead-zone quantizer:
`q = sign(c) * ((|c| * 16 + t/3) / t)`, i.e. `floor(|c| / step + 1/3)`, the
classic `f = 1/3` for intra.

### 6.6 Transform skip

When `tskip == 1`, no transform is applied in either direction. The 64 coded
values of a block are the **residual samples** in raster order inside the block,
quantized with a flat weight, and the scan order is raster instead of zigzag
(section 9.2). At QP 0 the dequantizer is the identity (`t = 16`, `c = q`), so
`tskip` + QP 0 is mathematically lossless: the residual is `source - prediction`
and reconstruction is `clamp(prediction + residual)`.

**Lossless mode** is `tskip = 1` with a resolved QP of 0 on every plane, and
`res_level = 0`. `chroma444 = 1` (or a 4:2:0 source) is required for the picture
itself to be lossless; a 4:2:0 tile is lossless only with respect to its own
subsampled chroma.

---

## 7. Intra prediction: the DC plane

v1 has no directional intra. Every plane of an `INTRA` tile is predicted from a
low-resolution image of its own block means, coded first, then interpolated.

### 7.1 The DC plane

For a plane with `nb x nb` blocks (`nb` = 8, 4, 2 or 1), the first coding unit
of that plane holds `nb * nb` values.

**Decoder (normative):**

```
tdc  = (qstep[qp >> 1] * 16 + 8) >> 4                 // == qstep[qp >> 1]
for i in 0 .. nb*nb-1:  dc[i] = clamp16((coef[i] * tdc + 8) >> 4)

if nb == 8:  dc = idct8x8(dc)                          // second-level transform
// nb < 8: no second-level transform, dc holds the values directly

for i:  M[i] = clamp(dc_offset + dc[i], 0, maxval)
```

`M` is the `nb x nb` array of reconstructed block means. The second-level 8x8
DCT is applied only when `nb == 8`, which is the 64-block luma plane of a
`res_level` 0 tile and the chroma plane of a `res_level` 0 4:4:4 tile. Smaller
DC planes are coded flat: the transform would buy nothing over 16, 4 or 1 value.

**Encoder (informative):** the block mean is `(sum of the 64 samples + 32) >> 6`;
the array of `mean - dc_offset` is transformed (when `nb == 8`), quantized with
step `tdc` and dead zone `tdc/3`, and the encoder then runs the decoder
reconstruction above so its prediction is exactly the decoder's.

### 7.2 Planar prediction

Block `(bx, by)`'s mean sits at the block centre, sample `(bx*8 + 3.5,
by*8 + 3.5)`. The prediction at sample `(x, y)` is the bilinear interpolation of
`M` at that grid, with the source coordinate in Q4:

```
ux = 2 * x - 7        // = ((x - 3.5) / 8) * 16
uy = 2 * y - 7
pred[y][x] = bilinear(M, nb, nb, ux, uy)      // section 8
```

Sample positions outside the outermost block centres clamp to the edge, which is
what makes this "planar-like" rather than a true plane fit: the interpolation is
flat in the outer half-block border and linear in between. It is fully parallel,
needs no wavefront and no cross-block dependency.

### 7.3 Reconstruction

For each block, the 64 coded coefficients are dequantized and inverse
transformed (or taken directly, for `tskip`) into a residual `res[8][8]`, and

```
recon[y][x] = clamp(pred[y][x] + res[y - by*8][x - bx*8], 0, maxval)
```

The plane is then upsampled by its factor (section 4.2) into the picture,
using the same bilinear kernel, and samples outside the picture are dropped.

For a plane whose `alpha_mode` is 0 or 1 (alpha only) no coefficients are
coded and the whole tile area is filled with 255 or `alpha_value`.

---

## 8. Resampling kernel

One kernel serves chroma upsampling, `res_level` upsampling and the DC-plane
planar prediction. `sx`, `sy` are Q4 source coordinates.

```
x0 = sx >> 4 ; fx = sx & 15 ; x1 = x0 + 1
y0 = sy >> 4 ; fy = sy & 15 ; y1 = y0 + 1
clamp x0, x1 to [0, w-1] and y0, y1 to [0, h-1]
out = ( p[y0][x0]*(16-fx)*(16-fy) + p[y0][x1]*fx*(16-fy)
      + p[y1][x0]*(16-fx)*fy      + p[y1][x1]*fx*fy      + 128 ) >> 8
```

`>>` on `sx` is arithmetic, so negative coordinates floor correctly. The whole
2D interpolation is one rounding step; a decoder must not implement it as two
separable passes with intermediate rounding.

For an upsample by `factor` (1, 2, 4 or 8) the mapping from output sample `x` to
Q4 source coordinate is

```
mul = 16 / factor
off = mul / 2 - 8
sx  = mul * x + off
```

which is the half-phase alignment `source = (x + 0.5)/factor - 0.5`. At factor 2
this yields fractions 12 and 4, i.e. the 3/4 and 1/4 weights PAPER 1.3 requires.
At factor 1 the sample is copied.

---

## 9. Entropy coding

### 9.1 Coding units and lane assignment

A tile's payload is a list of **coding units**, in this order:

```
for each coded plane p in (Y, Co, Cg [, A if alpha_mode == 2]):
    unit: the DC plane of p          (nb*nb coefficients)
    for by in 0..nb-1, bx in 0..nb-1 (raster):
        unit: block (bx, by) of p    (64 coefficients)
```

The number of lanes is `N = 2^nsub_log2`. The number of **active** lanes is
`min(N, unit_count)`. Lane `l` owns units `l, l+N, l+2N, ...` and decodes them
in that order. A lane with no units is not initialized and consumes no bytes.

At each step of the schedule, lanes `0, 1, ..., active-1` in that order each
consume **one** operation, skipping lanes that have finished. Because the number
of operations a unit needs is determined causally by the values already decoded,
every lane knows when it is done without any side information. This is exactly
the loop the GPU Pass A shader runs, with the 8 lanes of a tile in one
subgroup cluster.

### 9.2 Scan order

| unit | scan |
|---|---|
| 64-coefficient block, `tskip == 0` | 8x8 zigzag |
| 64-coefficient block, `tskip == 1` | raster (`scan[i] = i`) |
| DC plane, 64 values | 8x8 zigzag |
| DC plane, 16 values | 4x4 zigzag |
| DC plane, 4 values | `0, 1, 2, 3` |
| DC plane, 1 value | `0` |

8x8 zigzag:

```
 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
```

4x4 zigzag: `0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15`.

### 9.3 Contexts and alphabets

There are **12 contexts of 16 symbols** each. Every context's 16 frequencies are
10-bit, at least 1, and sum to exactly 1024, so every symbol is always decodable
and a hostile stream cannot produce an undefined symbol.

| index | use |
|---|---|
| 0 | `CBF`, luma and alpha planes |
| 1 | `CBF`, chroma planes |
| 2 | `LAST`, luma and alpha planes |
| 3 | `LAST`, chroma planes |
| 4-11 | `LEVEL`, band x previous-level class |

A DC-plane unit uses the same contexts as its plane.

**CBF** (coded block flag), one per unit: symbol 0 means every coefficient of
the unit is zero and the unit is finished; symbol 1 means coefficients follow.
Symbols 2-15 are illegal and must be rejected.

**LAST**, coded when the unit has more than one coefficient: a 4-bit class plus
raw bits.

| class | base | raw bits | covers scan positions |
|---|---|---|---|
| 0..7 | 0..7 | 0 | 0..7 |
| 8 | 8 | 1 | 8..9 |
| 9 | 10 | 1 | 10..11 |
| 10 | 12 | 2 | 12..15 |
| 11 | 16 | 3 | 16..23 |
| 12 | 24 | 3 | 24..31 |
| 13 | 32 | 4 | 32..47 |
| 14 | 48 | 4 | 48..63 |
| 15 | - | - | reserved, illegal |

`last = base[class] + raw`. The decoder must reject `class == 15`, a `base`
that is `>= ncoef`, and a resulting `last >= ncoef`. A unit with exactly one
coefficient codes no `LAST` and has `last = 0`.

**LEVEL**, for scan positions `last, last-1, ..., 0` (reverse scan order):
symbol `min(|q|, 15)`, where 15 is the escape. The context is

```
band = 0 if pos == 0, 1 if pos in 1..3, 2 if pos in 4..9, 3 if pos >= 10
prev = 0 if the previously decoded level was 0, 1 if its magnitude was 1, else 2
        (prev = 0 at the first position of a unit)

               prev=0  prev=1  prev=2
   band 0        0       1       2
   band 1        3       4       2
   band 2        5       6       7
   band 3        5       6       7

context index = 4 + that value
```

The level at position `last` must be nonzero; a decoder must reject a zero
there.

**Escape**: symbol 15 is followed by an Exp-Golomb order-3 code of `|q| - 15`,
in bypass bits:

```
n = v + 8 ; b = floor(log2(n)) ; j = b - 3
emit j one-bits, then a zero bit, then the low b bits of n (that is n - 2^b)
```

Decoding counts one-bits until a zero (`j`, at most 16, else reject), reads
`j + 3` bits `r`, and forms `v = (2^(j+3) - 8) + r`. `v > 32752` must be
rejected (`|q|` is bounded by 32767).

**Sign**: one bypass bit after every nonzero level (after the escape bits, if
any). 1 means negative.

### 9.4 Probability tables

Eight table sets exist per frame. Set *k* is either the built-in default *k* (in
`ref/src/default_tables.inc`, reproduced by `nxv-vectors` and pinned by the
conformance vectors) or, if bit *k* of `tables_present` is set, a transmitted
table.

A transmitted set is **120 bytes**: 12 contexts x 16 symbols x 5 bits, MSB-first
bit packing, contexts in index order, symbols in symbol order. Each 5-bit value
`d` is an index into a log-domain multiplier table applied to the built-in
default of the *same set index*:

```
kDeltaMul[32] (Q8, 256 == 1.0; kDeltaMul[i] = round(256 * 2^((i-16)/4))) =
     16,   19,   23,   27,   32,   38,   45,   54,
     64,   76,   91,  108,  128,  152,  181,  215,
    256,  304,  362,  431,  512,  609,  724,  861,
   1024, 1218, 1448, 1722, 2048, 2435, 2896, 3444

f[s] = clamp((default[s] * kDeltaMul[d] + 128) >> 8, 1, 32767)
```

and then the row is normalized to sum exactly 1024 by this deterministic
procedure (the one place a decoder divides; it runs 12 x 8 times per frame, not
per symbol):

```
sum = sum(f)
if sum == 1024: done
for each s:  g[s] = clamp((f[s] * 1024) / sum, 1, 1009)     // truncating divide
total = sum(g)
while total < 1024:  add 1 to the largest g[s] (ties: lowest index);  total++
while total > 1024:  subtract 1 from the largest g[s] with g[s] > 1;   total--
```

The decoder then builds `cum[s]` (prefix sums) and the 1024-entry
slot-to-symbol table used by the decode step.

### 9.5 rANS

Fixed parameters, chosen so the construction stays inside Duda's published rANS
and outside the 2022 Microsoft claims (PAPER 1.9): **one** state width, **one**
probability precision, no adaptivity.

| parameter | value |
|---|---|
| state | 32-bit unsigned |
| `L` (lower bound) | `2^16` |
| renormalization | 16 bits at a time |
| probability precision `M` | `2^10` |
| lanes per tile | `2^nsub_log2`, 8 in v1 |

**Payload layout.** The payload begins with `4 * active_lanes` bytes: the
initial state of lane 0, then lane 1, and so on, each as a little-endian u32.
The remaining bytes are the interleaved renormalization pairs, consumed in
schedule order.

**Decode of one symbol** in context `t`:

```
slot = x & 1023
s    = t.slot2sym[slot]
x    = t.freq[s] * (x >> 10) + slot - t.cum[s]
if x < 2^16:  x = (x << 16) | (buf[p] << 8 | buf[p+1]) ;  p += 2
```

**Decode of `k` bypass bits** (`1 <= k <= 8`) is the same step with an implicit
uniform context:

```
slot = x & 1023
v    = slot >> (10 - k)
x    = (1 << (10 - k)) * (x >> 10) + slot - (v << (10 - k))
renormalize as above
```

A bypass value wider than 8 bits is split into chunks of at most 8 bits, **most
significant chunk first**; the first chunk carries `nbits - 8 * (chunks - 1)`
bits and every later chunk 8. (Only the escape suffix, up to 19 bits, is wider
than 8.)

Renormalization reads two bytes as `hi, lo` in that order — the pair is
big-endian inside a little-endian header format, because it is produced by an
encoder writing backwards; this is a wire fact, not a preference.

**Encoding** (informative but exact, since the encoder must be invertible): the
encoder walks the same schedule forward to record the global operation order,
then encodes the operations in **reverse**, writing bytes backwards from the end
of the buffer, with all lane states initialized to `L`:

```
if x >= (freq << 22):  write16(x & 0xffff) ;  x >>= 16
x = ((x / freq) << 10) + (x % freq) + cum
```

and finally writes the 4-byte state of lane `active-1` down to lane 0, so that
lane 0's state ends up first in the stream. `x < freq << 22` guarantees
`x_new < 2^32` for every legal `freq <= 1023`.

A decoder must reject a payload shorter than `4 * active_lanes`, an initial
state below `L`, and any renormalization that would read past the payload.

---

## 10. Reconstruction summary (decoder, per tile)

```
1. parse the tile header
2. build the coding-unit list from res_level, chroma444, alpha_mode
3. run the interleaved rANS schedule -> int16 coefficients, one array per unit
   (this is GPU Pass A: output is a dense int16 coefficient buffer plus the
   tile record)
4. for each coded plane:
     a. dequantize + inverse-transform the DC plane   -> means M
     b. planar-interpolate M                          -> pred
     c. for each block: dequantize, inverse transform (or take the residual
        directly for tskip), add pred, clamp
     d. upsample the plane by its factor into the picture
   (this is GPU Pass B: one workgroup per 64x64 tile)
5. if color_transform == 1, convert planes 0..2 back to RGB after upsampling
```

Nothing in step 4 or 5 depends on any other tile. There is no deblocking filter
and no loop filter in v1.

---

## 11. Byte-layout cheat sheet

```
file  := stream_header ext_area frame*
frame := frame_header [custom_matrices] [table_set]* tile_row*
tile_row := row_header tile*
tile  := tile_header [mv] [alpha] payload
```

| structure | fixed size |
|---|---|
| stream header | 64 + `ext_len` |
| frame header | 40 (+128 if custom matrices, +120 per transmitted table set) |
| tile-row header | 12 |
| tile header | 8 (+2 if `mv_present`, +1 if `alpha_mode == 1`) |
| tile payload | `payload_len`, at least `4 * active_lanes` |

---

## 12. Phase 1 conformance (intra-only)

A Phase 1 decoder implements everything above except inter prediction. It must:

* accept `mode == INTRA` and reject `WARP_SKIP`, `STATIC_MV`, `WARP_MV` and
  `STEREO` with an "unsupported" status (not a malformed-bitstream status);
* reject a nonzero `skip_bitmap` (a skip references a frame it cannot have);
* reject `eyes != 1`, `num_layers != 1`, `bit_depth != 8`, `layer != 0`,
  `eye != 0`, and any tool bit outside the supported set;
* parse `mv_present`, `ref_sel` and `wgt` correctly even though it cannot use
  them, so that a Phase 2 stream is refused rather than misparsed.

The conformance vectors in `tests/vectors/` pin the MD5 of each bitstream and of
its decoded planes. `nxv-vectors --check tests/vectors` verifies both, i.e. that
the decoder still produces the same pixels **and** that the encoder still
produces the same bytes. The Vulkan decoder is conformant when it reproduces
every `decoded_md5` in `tests/vectors/vectors.md5`.

---

## Appendix A: decisions taken

Choices this document makes where PAPER.md was silent, ambiguous, or internally
inconsistent. Each is a decision, not an interpretation.

32. **`table_set` is chosen by cost, not by QP, and the eight built-in sets are
    statistical clusters.** The field is a free 3-bit index; nothing requires it
    to track the QP. The eight defaults are k-means centroids of real tile
    symbol histograms (the distance being the bits a tile would cost under that
    set), and the reference encoder scores all eight per tile and picks the
    cheapest. Worth about 30% at QP 24 over keying `table_set = qp >> 3`.

33. **The rANS lane count is chosen per tile by cost.** Every lane costs a
    4-byte flush, and lanes buy only parallelism, never bits. The reference
    encoder takes the largest `N` in {1, 2, 4, 8} whose flush stays under a
    tenth of the tile payload. On a QP 38 frame the flush fell from 33% of the
    frame to under 10%. `N` is capped at 8 so a tile never needs more than one
    subgroup cluster.

1. **Five modes in a 3-bit field.** PAPER 1.2 gives `mode(2)` with four modes;
   PAPER 6.5 lists five (`WARP_SKIP`, `WARP_MV`, `STATIC_MV`, `STEREO`,
   `INTRA`). The field is 3 bits and the reserved field at the top of word1
   shrinks from 7 to 6 bits. `ref_sel` keeps the name from 1.2 and the meaning
   of `ref_delta` from 6.6 (0 = newest, 3 = ...); 6.6's "3 means intra" is
   dropped because `mode` already says intra.

2. **`ref_sel`, not `ref_delta`.** Same field, section 1.2's name kept.

3. **The stream header carries `chroma_format`, `alpha_present` and
   `color_transform`.** PAPER 1.2 left the stream-level chroma and alpha
   signalling implicit. Without them a decoder cannot allocate planes.

4. **Frame headers are self-delimiting.** A `u32 frame_bytes` was added (the
   paper's frame header is a transport structure and had no length). Without it
   a stored file cannot be walked, and a truncated frame cannot be detected
   cheaply.

5. **Pose is opaque.** 26 bytes, interpreted as 7 binary16 + 3 binary32 by
   whoever cares; the codec never does float arithmetic on it. PAPER 1.2 says
   "7 x f16 (quat)", which is one value too many for a quaternion; the extra
   three halves are assigned to angular velocity.

6. **The DC plane replaces the tile mean.** PAPER 1.4 mentions both "predicted
   from the tile mean (coded once, 8 bits)" and a second-level DCT over the 64
   block DCs. Both would be redundant: the DC coefficient of the second-level
   transform *is* the tile mean. There is no separate tile-mean field.

7. **The DC plane codes block means in the sample domain, not block DC
   coefficients.** PAPER 3.2.4 asks for "bilinear interpolation between the four
   nearest block DCs" as the predictor. Interpolating transform-domain DCs is
   not defined; interpolating means is. The prediction is therefore built from
   reconstructed means, and each block codes the full 64-coefficient DCT of
   `samples - prediction`, so the block syntax is uniform and will not change
   when inter arrives.

8. **The second-level transform applies only when `nb == 8`.** Smaller DC planes
   (16, 4 or 1 value, from `res_level` and 4:2:0) are coded flat.

9. **The DC plane is quantized at half the QP index** (`qp >> 1`) with a flat
   weighting matrix. The DC plane is 1/64 of the coefficients but carries the
   entire intra predictor: coarse block means make the planar interpolation
   blocky, and the AC residual then pays for the error in every block. Measured
   on a synthetic render frame this is worth +3.0 dB *and* -10% bits at QP 38
   against the `qp - 6` rule first tried, and it is never worse at any QP.

10. **Two-stage inverse shifts are 7 and 13.** PAPER 1.4 says "7 bits after the
    first dimension, 12 after the second"; with the flow graph of section 6.2
    the per-pass gain is `2^10`, so 7 + 12 leaves a residual gain of 2. Any pair
    summing to 20 is unity-gain; 7 + 13 is chosen over 8 + 12 on measured
    precision (section 6.3).

11. **`clamp16` after the first inverse pass is normative**, so the GPU may keep
    the transpose buffer in int16 as PAPER 1.4 intends. Without it the pass-1
    result can reach 19 bits on legal input.

12. **Quantizer steps are a Q4 integer table** and dequantization is
    `((q * ((qstep*w + 8) >> 4)) + 8) >> 4`. Weights are constrained to
    `[1, 32]` so no product leaves int32; a custom matrix is clamped into that
    range on parse rather than being rejected.

13. **`quant_matrix` selects the luma matrix; chroma uses the built-in chroma
    matrix** (index 3's formula) unless the frame is flat (index 0) or custom.
    A custom matrix is 128 bytes, luma then chroma, not 64.

14. **Weighting matrices are defined by formula**, not by a table of magic
    numbers, so they can be checked by inspection.

15. **Zigzag for transform blocks, raster for transform-skip blocks.** The paper
    does not say what the scan is when there is no transform; a residual sample
    field has no frequency ordering, so the natural scan is raster.

16. **LEVEL codes every position from `last` down to 0, including `last`.**
    The level at `last` is known nonzero and could be coded as `|q| - 1`; it is
    not, because the uniform rule keeps the lane state machine (and the shader)
    branch-free at the unit boundary. The cost is a fraction of a bit per block.

17. **The LAST class table** (section 9.3) is ours; the paper only said "4-bit
    class plus raw bits for classes above 8".

18. **The band x previous-level collapse to 8 contexts** (section 9.3) is ours;
    the paper specified the inputs and the count but not the mapping.

19. **Bypass bits ride the rANS state with an implicit uniform context** of
    `2^(10-k)`, chunked to at most 8 bits, MSB chunk first. This keeps a single
    stream and a single decode step shape, as PAPER 1.6 requires, and avoids a
    second code path for raw bits.

20. **Escape is Exp-Golomb order 3 with the prefix capped at 16 ones**, bounding
    `|q|` to 32767, which is what makes the int32 dequantizer bound provable.

21. **All 16 frequencies of every context are at least 1.** Contexts that use
    only two symbols (CBF) still carry 14 frequencies of 1. This costs about
    1.4% of the probability mass in those contexts and buys a decoder that
    cannot be driven to an undefined symbol by a corrupt stream.

22. **Custom tables are 5-bit log-domain deltas from the built-in defaults**, as
    PAPER 1.6 says, plus an explicit deterministic normalization procedure
    (section 9.4) that the paper did not give. Without a fixed normalization the
    tables are not reproducible.

23. **Initial rANS states cost 4 bytes per lane.** PAPER 1.6 hopes for "about 2
    bytes per substream" by folding the first symbols into the initial state.
    No construction does that without either a variable-length state or a
    terminating symbol, and the paper's own fallback ("4 bytes per substream
    naively") is what is specified. At 8 lanes this is 32 bytes per tile, which
    at QP 36 on 2048x2048 is about a third of the payload; see
    [Appendix B](#appendix-b-known-costs).

24. **`nsub_log2` is honoured, but v1 encoders emit 3.** PAPER 1.6 wants 1..32
    substreams chosen per tile, PAPER 6.3 fixes 8. Both are kept: the syntax and
    both reference implementations handle `N` in `{1, 2, 4, 8, 16, 32}`, and a
    stream that uses anything but 8 must set the `NSUB_VAR` tool bit, so a GPU
    decoder built for exactly 8 lanes refuses it at the handshake instead of at
    the tile.

25. **Lanes beyond the unit count are not initialized.** A tile with 6 coding
    units and `N = 8` pays for 6 states, not 8.

26. **Per-tile 4:2:0 inside a 4:4:4 stream is allowed and the combined
    upsampling factor may be 8.** Reconstruction always upsamples in one
    bilinear step.

27. **`color_space` is a separate, descriptive field from `color_transform`.**
    The coordinator's integration finding is that WiVRn's Linux capture is
    already YCbCr 4:2:0, so the common path must code planes as-is; only the
    Windows helper has RGB. `color_transform` says whether the codec converts,
    `color_space` says what the planes are. They are tied (`RGB` iff YCoCg-R)
    but separate, so a later colour space can be added without touching the
    transform stage.

28. **The colour transform is signalled per stream and the API is RGB in, RGB
    out for YCoCg-R.** The 9-bit chroma of YCoCg-R cannot be handed through an
    8-bit plane API, so the transform is internal: `--rgb`/`NXVC_CT_YCOCGR`
    means "planes 0..2 are R, G, B". The plain YUV path (`--pix yuv420p`) codes
    the given planes directly and is what the quality harness uses.

29. **Edge tiles are coded full-size with edge replication** and the excess is
    discarded on output. The alternative (partial tiles) would make every tile
    geometry variable for the sake of at most one tile row and column.

30. **The decoder rejects, rather than conceals, a malformed tile.** Concealment
    is a transport-layer decision (PAPER 2.7) and needs a reference frame, which
    Phase 1 does not have.

31. **`nxv-dec` writes alpha as a fourth plane** after V when the stream carries
    one. Raw planar files have no place to say so; `nxv-info` does.

## Appendix B: where the bits go

Measured on a 2048x2048 4:2:0 synthetic textured frame, 1024 tiles, default
encoder settings (per-tile lane count, per-frame probability tables, matrix 1),
using `nxv-enc --stats`:

| | QP 20 | QP 28 | QP 36 |
|---|---|---|---|
| bytes | 546076 | 108430 | 59786 |
| bpp | 1.042 | 0.207 | 0.114 |
| luma PSNR | 35.45 dB | 31.25 dB | 30.23 dB |
| frame header + tables | 0.03% | 0.15% | 0.47% |
| tile-row headers | 0.07% | 0.35% | 0.64% |
| tile headers (8 B each) | 1.5% | 7.6% | 13.7% |
| rANS flush (4 B per lane) | 6.0% | 4.6% | 6.9% |
| DC planes | 8.3% | 30.6% | 42.4% |
| luma blocks | 61.4% | 34.7% | 18.8% |
| chroma blocks | 1.2% | 3.2% | 0.5% |
| mean lanes per tile | 8.00 | 1.22 | 1.00 |

Three things this says to whoever works on v2:

1. **The 8-byte tile header is the floor.** At QP 36 it is 13.7% of the frame
   and there is nothing the encoder can do about it: it is fixed syntax. A
   `SKIP` bit or a shorter header form for tiles with a small payload is the
   obvious v2 lever, and it needs a syntax change, not an encoder change.
2. **The DC plane dominates at low rate** (42% at QP 36). It is the intra
   predictor, so its bits buy quality across the whole tile, but it is coded
   with the same contexts as ordinary blocks even though its statistics are
   quite different. Dedicated DC-plane contexts (the 12-context budget has no
   room; 16 would) are the cheapest remaining coding-efficiency win.
3. **The rANS flush is now under 7%** because the encoder spends lanes only
   when the payload can carry them. A 2-byte initial state would roughly halve
   what is left, at the cost of a variable-length state invariant.
