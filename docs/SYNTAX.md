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
* `bit_depth` must be 8. **`bit_depth == 10` is reserved in v1 and must be
  rejected with an "unsupported" status**, as must the `BITDEPTH10` tool bit.
  v1 does not define the 10-bit sample domain, the `qstep` scaling or the
  clamps, so a 10-bit stream has no defined meaning here; specifying it is a
  v2 item and it is listed in the spec's open issues. The rejection vectors
  `r03_bit_depth_10` and `r02_unknown_tool` pin both refusals.
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
| 20 | `WM_ID` | tiles may set `wm_id != 0` (section 4.1) |
| 21 | `CTX_V2` | the 16-context entropy model (section 9.3) |
| 22 | `SIGN_HIDE` | sign data hiding (section 9.7) |
| 23 | `FILTER_CATMULL_ROM` | Catmull-Rom interpolation in the warp instead of bilinear. **Not defined for version 1** |
| 24 | `NEAR_SKIP` | tiles may set `near_skip` (section 13.9) |
| 25 | `QUAD_MV` | tiles may set `quad_mv` (section 13.10) |
| 26 | `SUBTILE_INTRA` | tiles may set `sub_intra` (section 13.11) |

Bits 17, 21 and 22 are independent: any subset may be set. `SIGN_HIDE` is
mutually exclusive with `LOSSLESS` (bit 5) -- hiding a sign spends one level
step, so the two cannot both be true; a stream setting both is `BITSTREAM`.

`INTER` (bit 10) gates every tile mode other than `INTRA`. `WARP` (bit 11)
gates `warp_present` and the two warped modes and requires `INTER`; a stream
setting `WARP` without `INTER` is `BITSTREAM`. `STEREO` (bit 12) requires
`eyes == 2`.

**Bits 14 and 23 are reject-in-v1.** `BITDEPTH10` has no defined sample
domain, quantiser scaling or clamp in version 1, and `FILTER_CATMULL_ROM` would
make the interpolation filter a decoder-visible choice; a version 1 decoder
MUST refuse a stream that sets either, with a `VERSION` status. Because bit 23
is refused, **every conforming version 1 stream is bilinear**, in every
profile, and `profile` selects nothing (section 13.4).

`NEAR_SKIP`, `QUAD_MV` and `SUBTILE_INTRA` all require `INTER`, because all
three are per-tile bits on modes only `INTER` allows. Bits 27-63 are reserved
and must be zero. Capability negotiation is an
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
| 33 | u8 | `ref_slots` | bitmask of the reference-ring slots this frame overwrites, bit *s* for slot *s* (section 13.2) |
| 34 | u8 | `flags` | bit 0: tile-map reset, bit 1: stereo inter-view, bit 2: layered directional intra (section 7.5), bit 3: `warp_present` (section 3.1.1), bits 4-7 reserved and must be 0 |
| 35 | u8 | reserved | must be 0 |
| 36 | u32 | `frame_bytes` | total byte length of this frame unit, header included |

Then, in this order:

1. If `flags` bit 3 (`warp_present`) is set: **`36 * eyes` bytes** of
   `warp_ext()`, section 3.1.1.
2. If `quant_matrix == 255`: **128 bytes** of custom weighting matrices, 64 for
   luma and alpha followed by 64 for chroma, each in raster order inside the
   8x8 block, Q4 (16 == 1.0). Values are clamped to `[1, 32]` on parse.
3. For *k* = 0..7 in ascending order, if bit *k* of `tables_present` is set:
   **120 bytes** of probability table deltas for set *k*, or **160 bytes** when
   the stream sets `CTX_V2` (section 9.4).

Constraints: `base_qp <= 63`; `quant_matrix <= 3 || quant_matrix == 255`;
`flags` bit 2 requires tool bit 17 `INTRA_DIR` (`BITSTREAM` otherwise);
`flags` bit 3 requires tool bit 11 `WARP`; `flags` bits 4-7 must be zero;
`frame_bytes >= 40` and `frame_bytes` must not exceed the bytes available. After
the last tile of the last row, exactly `frame_bytes` bytes must have been
consumed, `warp_ext()` included.

`frame_number` and the transport's `frame_id` are the same 16-bit counter with
the same wrap and **must be equal**; a datagram whose `frame_id` disagrees with
the frame it carries is inconsistent and must be discarded
(`docs/TRANSPORT.md` 7.2).

`ref_slots` names the ring slots this frame overwrites. On a stream that sets
`INTER` it must equal `1 << (frame_number mod 4)` and any other value is
`BITSTREAM`. On a stream with no inter tools there is no reference ring for the
field to describe, it is inert, and 0 is the value every version 1.1 to 1.3
encoder writes; it is not checked there. The mask form is kept rather than an
index so that a version 2 frame writing more than one slot needs no new
element.

### 3.1.1 `warp_ext()`

Present if and only if `flags` bit 3 (`warp_present`) is set, immediately after
the 40-byte frame header and before the custom matrices. It is one 36-byte
record per eye in ascending eye order, each nine little-endian **signed** 32-bit
integers: the quantised per-eye homography the warped modes predict through.

| off | size | element | format |
|---|---|---|---|
| 0 | 4 | `h00` | Q10.21 |
| 4 | 4 | `h01` | Q10.21 |
| 8 | 4 | `h02` | Q10.21 |
| 12 | 4 | `h10` | Q10.21 |
| 16 | 4 | `h11` | Q10.21 |
| 20 | 4 | `h12` | Q10.21 |
| 24 | 4 | `h20` | Q2.29 |
| 28 | 4 | `h21` | Q2.29 |
| 32 | 4 | `h22` | Q2.29, must be `0x20000000` |

Rows 0 and 1 are Q10.21 (range +-1024.0, resolution 2^-21); row 2 is Q2.29
(range +-4.0, resolution 2^-29). A single format cannot hold both ends: the
translation terms need ten integer bits at any streamed width, and the
perspective row's entries are of order 5e-5 and need ten *significant* bits
below it (`docs/WARP.md` 3).

The matrix maps centred integer sample indices of this frame to centred source
indices of the reference: `(x - ox, y - oy) -> (x_src - ox, y_src - oy)`. The
origin is **not a field**; it is derived, per eye, as

```
ox = width  >> 1
oy = height >> 1
```

in luma samples. The half-sample convention and the centring are folded into
the matrix by the encoder, so the decoder subtracts the origin, runs the
matrix, adds it back, and needs no other geometric parameter.

`h22` is normalised and not free. It is transmitted anyway so that the record
is nine `int32` and a decoder loads the matrix with one uniform copy.

**A decoder MUST reject** (`BITSTREAM`) a stream in which any of the following
does not hold, for any eye:

1. `h22 == 0x20000000`;
2. every entry satisfies `-2^30 <= h <= 2^30` (`kEntryMax`, `docs/WARP.md` 3);
3. at each of the four picture corners `(cx, cy)` with `cx` in `{-ox, width - ox}`
   and `cy` in `{-oy, height - oy}`, the denominator
   `den = h20*cx + h21*cy + h22`, accumulated in 64 bits, fits `int32` and
   satisfies `2^28 <= den < 2^30`;
4. `warp_ext()` is present exactly when `warp_present` is set, and
   `frame_bytes` accounts for its `36 * eyes` bytes.

Condition 3 bounds the whole picture, because `den` is affine in `(cx, cy)`.
It is what licenses the fixed 32-iteration restoring divide of the corner
derivation to use a `uint32` remainder. A decoder that accepted a matrix
violating it would still produce defined, identical output on every
implementation -- the warp saturates -- but the stream is malformed and
rejection is the specified behaviour.

Cost: 72 bytes per stereo frame, 6.5 kB/s at 90 Hz. Against a 150 Mbit/s
working point that is 0.00003 % of the stream, and it stays negligible after
the transport replicates it into the first datagram of each band.

Why not somewhere else: widening the frame header costs 72 bytes on every
Phase 1 and every all-intra frame and moves every offset in this section; the
TLV area of section 2.1 is per *stream*, not per frame, and version 1 defines
no mandatory TLV type; the tile-row header would replicate the matrix
`eyes * rows` times -- 68 copies, 4.9 kB per frame at the v1 configuration --
to buy loss tolerance the transport already provides; and deriving it from the
pose bytes is impossible for a decoder that does no floating-point arithmetic
(section 1), which is the whole reason the quantised matrix and not the pose is
the thing on the wire.

### 3.2 Pose

The 26 pose bytes are **opaque to the codec**: they are carried, hashed and
compared byte-wise but never interpreted by the normative decode path (the codec
does no floating-point arithmetic).

Their layout is the transport's `pose_header`, which
[`docs/TRANSPORT.md` 3.3](TRANSPORT.md) **owns**; this document references it
and does not restate it. There is exactly one 26-byte pose layout in the
format and it is the integer one: `pose_seq` (u16), four s16 Q15 quaternion
components, three s32 positions in millimetres x 256, and a u32
`render_finish_ts`. The `pose` field of a stored frame header and the
frame/pose header the transport replicates into the first datagram of each band
are byte-identical, which is what makes that replication argument true rather
than merely plausible.

The earlier reading of these bytes as 7 x `binary16` plus 3 x `binary32` is
**superseded**. It put IEEE bit patterns in the one structure this document
and the transport share, in a format whose normative paths carry no floating
point, and it carried neither `pose_seq` -- which the client needs to identify
the pose in its own ring -- nor `render_finish_ts`. The three half-precision
angular-velocity components it carried are dropped: angular velocity is a
client-side quantity, recovered by differentiating the client's own pose ring,
which `pose_seq` indexes, and nothing in the decoding process reads it.

### 3.3 Tile-row structure

**A picture is one eye.** `width` and `height` are per eye, and a stereo frame
contains `eyes` pictures rather than one picture of double width. The
transport's tile grid spans the eye pair:

```
cols_per_eye = ceil(width  / 64)
rows         = ceil(height / 64)
cols         = eyes * cols_per_eye
tile_first   = row * cols + eye * cols_per_eye + tile_index
```

and inversely, from a linear index `n`: `row = n / cols`, `col = n % cols`,
`eye = col / cols_per_eye`, `tile_index = col % cols_per_eye`. `tile_index` is
this document's in-row index; `tile_first` is the transport's linear index
(`docs/TRANSPORT.md` 1). A decoder must reject a configuration whose transport
`cols` is not `eyes * cols_per_eye` or whose `rows` is not `ceil(height / 64)`.
At the v1 target configuration -- 2160 per eye, two eyes -- that is 34 columns
per eye, `cols = 68`, `rows = 34`, 2312 tiles, which is exactly the transport's
headline table. Nothing needed widening; this sentence was what was missing.

A frame contains **`eyes * rows`** tile-row structures, ordered **row-major,
eye-minor**:

```
for (row = 0; row < rows; row++)
    for (eye = 0; eye < eyes; eye++)
        tile_row_header()   then its tiles
```

`row_index` must equal `row`; **the eye is positional and is not a field of the
row header**. This order is load-bearing twice: it makes a run of tiles a
contiguous range of linear indices *and* of bitstream bytes, and it puts the
whole left-eye row ahead of the right-eye row of the same index, which is what
lets a `STEREO` tile's dependency on up to three left-eye tiles of the same row
be satisfied (section 13.6).

For each tile row `row = 0 .. rows - 1` and each eye, in that order:

| offset | size | field |
|---|---|---|
| 0 | u16 | `frame_number` (must equal the frame header's) |
| 2 | u8 | `row_index` (must equal `row`) |
| 3 | u8 | `tile_count` |
| 4 | u64 | `skip_bitmap` |

followed by `tile_count` tile structures.

Bit *i* of `skip_bitmap` set means tile *i* of this row **of this eye** is
`WARP_SKIP` and is **not** transmitted. `tile_count` must equal the number of
tiles in the row that are not marked skipped, and bits at or above
`cols_per_eye` must be zero. The bitmap therefore covers one row of one eye, so
the structural bound `ceil(width / 64) <= 64` binds `cols_per_eye`, not `cols`,
and is satisfied for every legal `width` up to the syntax maximum of 4096.

A skipped tile has no tile structure at all, so every parameter of it is
derived rather than coded: `res_level` 0, chroma at the stream's own
resolution, `alpha_mode` 0, `ref_sel` 0, and the vector is the tile's stored
`last_mv` (section 13.5). Skip requires `INTER` and `warp_present`.

`skip_bitmap` is **authoritative**. The transport states the same fact a second
time, as a directory entry with `dir_len == 0` and `dir_mode == WARP_SKIP`, and
the two must agree: if a bitmap bit is set no directory entry for that tile may
carry `dir_len != 0`, and if a directory entry has `dir_len == 0` the bit must
be set. On disagreement the decoding process follows `skip_bitmap` and the
receiver marks the tile UNDECODABLE. The redundancy is worth keeping: the
bitmap is what makes a skipped tile cost one bit, and the directory entry is
what lets a receiver account for the tile without holding the row header. In a stored file the row header is written once; in
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
| 26-27 | `wm_id` | per-tile weighting matrix: 0 = the frame's, 1-3 = built-in matrix `wm_id` |
| 28 | `near_skip` | the residual is a correction field, not a payload (13.9) |
| 29 | `near_skip_ac` | that field carries the two ramps as well (13.9) |
| 30 | `quad_mv` | four quadrant vectors follow the tile vector (13.10) |
| 31 | `sub_intra` | one quadrant drops the predictor (13.11) |

Then, in this order:

1. if `mv_present`:
   * `u16 disparity` if `mode == STEREO`
   * `i8 mv_x, i8 mv_y` otherwise
2. if `quad_mv`: four bytes of quadrant vector deltas (13.10)
3. if `sub_intra`: one byte, bits 1:0 the quadrant, bits 7:2 zero (13.11)
4. `u8 alpha_value` if `alpha_mode == 1`
5. if `near_skip`: `3 * (near_skip_ac ? 3 : 1)` correction bytes (13.9)
6. `payload_len` bytes of rANS payload

`mv_x` and `mv_y` are the tile's motion vector **itself**, in quarter samples
(Q.2), range `[-32, +31.75]` samples. They are **not** a delta from the tile's
stored previous vector. A delta would make parsing a tile header depend on the
decoder's per-tile state being correct -- exactly in the cases after a concealed
frame where the format most needs to recover -- and that destroys the property
the whole transport design rests on, that a tile is an independently decodable
bitstream and a datagram is only a loss unit. The stored vector exists for
`WARP_SKIP` and for concealment, never for parsing (section 13.5).

`disparity` is one little-endian `u16`:

| bits | meaning |
|---|---|
| 11:0 | unsigned horizontal disparity in quarter samples, 0 to 4095 (0 to 1023.75 samples) |
| 15:12 | reserved, must be 0 |

The source position in the decoded first eye lies `disparity` quarter samples
to the **right** of the tile's own position. There is deliberately **no
vertical field**: a downward component is what would break a row-pipelined
decoder's three-left-tile dependency bound, and removing the field makes that
rule structural rather than a constraint a malformed stream could violate. The
four reserved bits are where a version 2 signed non-positive `dy` goes. Twelve
bits is five times the worst case measured on real content (`f * IPD / z` is
about 200 samples at 30 cm) and covers `z` down to about 6 cm, inside the near
clip plane of every headset. The field is two bytes, exactly as
`mv_x` + `mv_y`, so no structure changes size.

Constraints: `res_level != 3`; `alpha_mode != 3`; `nsub_log2 <= 5`;
`mode <= 4`; word0 bit 3 and word1 bits 28-31 zero; `chroma444` may only be 1 if
`chroma_format == 1`; `alpha_mode != 0` requires `alpha_present`; `tile_index`
must equal the tile's position in the row; `eye` must equal the eye derived from
the tile's position in the frame (section 3.3); `wm_id != 0` requires the
`WM_ID` tool bit and is illegal when the frame carries custom matrices
(`quant_matrix == 255`), because the two would silently disagree about which
weights apply.

Inter constraints: `mode != INTRA` requires the `INTER` tool bit;
`mode == WARP_SKIP` or `WARP_MV` requires `warp_present` in the containing
frame; `mode == STEREO` requires the `STEREO` tool bit, `eye == 1` and
`mv_present == 1`, and its `disparity` bits 15:12 must be zero.
`ref_sel == 3` is reserved. For `mode == INTRA` and `mode == STEREO`, `ref_sel`
must be 0 and **is ignored** by the decoding process.

`near_skip` requires the `NEAR_SKIP` tool bit, `mode != INTRA`,
`payload_len == 0`, `res_level == 0` and `alpha_mode != 2`; `near_skip_ac`
requires `near_skip`. `quad_mv` requires the `QUAD_MV` tool bit and
`mode == WARP_MV` or `mode == STATIC_MV`. `sub_intra` requires the
`SUBTILE_INTRA` tool bit, `mode != INTRA` and `near_skip == 0`. Sections 13.9
to 13.11 say why each of those is a constraint rather than a convention.

`ref_sel` is **authoritative**; the transport's `ref_delta` is an advisory copy
of it with one extra value, 3, meaning "no temporal reference", which is what
an `INTRA` or `STEREO` tile must carry there. On disagreement the decoding
process uses `ref_sel` and the receiver marks the tile UNDECODABLE, because it
cannot account for the tile in its reference model.

The tile's quantiser is `clamp(base_qp + qp_delta, 0, 63)` **from the
bitstream**. The transport's `dir_qp` must equal that value and is advisory: it
exists so a receiver can act on a tile's quality -- retransmit priority,
telemetry, concealment choice -- before parsing the tile. On disagreement the
decoding process uses `qp_delta` and the receiver counts an integrity fault
without discarding the tile, since `dir_qp` never affects decoded samples.

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
O1 = mulC4(P + Q)          // see 6.3: an exact product, not an int32 one
O2 = mulC4(P - Q)

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
| inverse pass 1 | `>> 7` | `+64` | int16 | `1.1e8` at the outputs `y0..y7` |
| inverse pass 2 | `>> 13` | `+4096` | int16 | `1.1e8` |
| forward pass 1 | `>> 6` | `+32` | int16 | `511 * 4096 = 2.1e6` |
| forward pass 2 | `>> 14` | `+8192` | int16 | `32767 * 4096 = 1.3e8` |

**`mulC4` is an exact product, and it is the one place the flow graph leaves
int32 if written naively.** Dequantized coefficients are clamped to int16
(section 6.5), which bounds the even part and the odd-part terms

```
|A|, |B| <= 32768 * (A1 + A7) = 32768 * 602 = 1.98e7
|C|, |D| <= 32768 * (A3 + A5) = 32768 * 710 = 2.33e7
|P|, |Q| <= 4.31e7          so   |P +- Q| <= 8.62e7
```

and `8.62e7 * 362 = 3.12e10`, which is **outside int32**. The value
`(s * C4 + 256) >> 9` is nonetheless well defined, and a decoder must produce
it exactly. Split `s` into `hi = s >> 9` (arithmetic) and `lo = s & 511`:

```
mulC4(s):  hi = s >> 9 ; lo = s & 511
           return hi * C4 + ((lo * C4 + 256) >> 9)
```

This is exact for every int32 `s`, because `512 * hi * C4` is a multiple of
512 and the shift therefore distributes; both partial products are small
(`|hi * C4| <= 6.1e7`, `lo * C4 <= 1.9e5`). An implementation with a cheap
64-bit multiply may use one instead; the *result* is normative, not the
spelling. With `mulC4` bounded this way, `|O1|, |O2| <= 6.1e7` and the pass
outputs `y = e +- O` stay under `1.1e8`, comfortably inside int32.

The same identity applies to the forward transform's `P`/`Q` rotation, whose
operands are far smaller; it is used there only so that one routine serves
both directions.

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
with legal (if pathological) coefficient values. It is also what makes the
range analysis above apply unchanged to pass 2.

The **dequantizer** has its own bound: the largest step is QP 63 with the
largest legal weight, `t = (23170 * 32 + 8) >> 4 = 46340`, and the largest
legal level is 32767, so `q * t = 1.52e9` — inside int32 with room to spare.

Conformance vector `v35_saturate420` carries a tile whose every dequantized
coefficient saturates the int16 clamp in the sign pattern that maximises
`|P + Q|`; `ctest -R ref.saturate` decodes it and sweeps the arithmetic at its
bounds, and is meant to be run under `-fsanitize=undefined`
(`cmake --preset asan-ubsan`).

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
* Normal blocks: the tile's weighting matrix, luma matrix for planes 0 and 3,
  chroma matrix for planes 1 and 2. The tile's matrix is the frame's when
  `wm_id == 0`; when `wm_id` is 1, 2 or 3 it is built-in matrix `wm_id` for
  planes 0 and 3 and the built-in chroma matrix (index 3's formula) for planes
  1 and 2, exactly as the frame-level rule below. `wm_id != 0` is illegal in a
  frame with custom matrices.

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

## 7. Intra prediction

Every plane of an `INTRA` tile is predicted from a low-resolution image of its
own block means -- the **DC plane** -- coded first, then interpolated (7.1-7.3).
That is the whole of v1, and it stays the base of everything: it is fully
parallel, needs no wavefront and no cross-block dependency.

When tool bit 17 `INTRA_DIR` is set, each 8x8 block additionally carries an
**intra mode** (7.4) that may replace the DC-plane prediction with one of eight
directional predictors built from the block's reconstructed neighbours. Mode 0
*is* the DC-plane prediction, so `INTRA_DIR` is a strict superset: a tile that
codes mode 0 everywhere reconstructs exactly as it would without the tool.

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

### 7.4 Directional intra (tool bit 17)

With `INTRA_DIR`, each coded plane carries one **mode unit** (section 9.1)
holding `nb * nb` intra modes in raster order, and the blocks of that plane are
reconstructed **in raster order**, each seeing the reconstruction of the ones
before it.

| mode | name | prediction |
|---|---|---|
| 0 | `DC_PLANE` | `pred[y][x]` from 7.2 -- the v1 predictor |
| 1 | `DC` | `(sum(A[0..7]) + sum(L[0..7]) + 8) >> 4` |
| 2 | `PLANAR` | HEVC-style, below |
| 3 | `H` | `L[j]` |
| 4 | `V` | `A[i]` |
| 5 | `DDL` | diagonal down-left, 45 deg |
| 6 | `DDR` | diagonal down-right, 45 deg |
| 7 | `VR` | vertical-right, 26.6 deg |
| 8 | `HD` | horizontal-down, 63.4 deg |

**Reference samples (normative).** For the block at `(bx, by)` with origin
`(x0, y0) = (8*bx, 8*by)`, define

```
at(x, y):
    cx = clamp(x, 0, size-1);  cy = clamp(y, 0, size-1)
    if (cy>>3) < by  or  ((cy>>3) == by and (cx>>3) < bx):
        return recon[cy][cx]        // a block already reconstructed
    else:
        return base[cy][cx]         // section 7.5

TL   = at(x0-1, y0-1)
A[k] = at(x0+k, y0-1)   for k = 0..15      A[-1] = TL
L[k] = at(x0-1, y0+k)   for k = 0..15      L[-1] = TL
```

Coordinates are clamped **into the tile**, and the fallback for anything not
yet reconstructed is `base`, which is derived from this tile's own DC plane.
A tile therefore never reads a neighbouring tile: **tiles stay independent**,
which is what the transport's per-tile loss recovery and the rate controller's
per-tile ladder both depend on. The top and left borders of a tile read the
DC-plane prediction, not a neighbour.

**Predictors.** `i` is the column and `j` the row inside the block, 0..7.

```
PLANAR: P[j][i] = ((7-i)*L[j] + (i+1)*A[8] + (7-j)*A[i] + (j+1)*L[8] + 8) >> 4

DDL:    k = i + j
        P[j][i] = k == 14 ? (A[14] + 3*A[15] + 2) >> 2
                          : (A[k] + 2*A[k+1] + A[k+2] + 2) >> 2

DDR:    i > j:  k = i - j;  P = (A[k-2] + 2*A[k-1] + A[k] + 2) >> 2
        i < j:  k = j - i;  P = (L[k-2] + 2*L[k-1] + L[k] + 2) >> 2
        i == j:             P = (A[0]   + 2*TL     + L[0] + 2) >> 2

VR:     z = 2*i - j;   k = i - (j >> 1)
        z >= 0, even:  P = (A[k-1] + A[k] + 1) >> 1
        z >= 0, odd:   P = (A[k-2] + 2*A[k-1] + A[k] + 2) >> 2
        z == -1:       P = (L[0]   + 2*TL     + A[0] + 2) >> 2
        z <  -1:  q = j - 2*i;  P = (L[q-1] + 2*L[q-2] + L[q-3] + 2) >> 2

HD:     z = 2*j - i;   k = j - (i >> 1)
        z >= 0, even:  P = (L[k-1] + L[k] + 1) >> 1
        z >= 0, odd:   P = (L[k-2] + 2*L[k-1] + L[k] + 2) >> 2
        z == -1:       P = (A[0]   + 2*TL     + L[0] + 2) >> 2
        z <  -1:  q = i - 2*j;  P = (A[q-1] + 2*A[q-2] + A[q-3] + 2) >> 2
```

Every mode but 0 is a weighted average of references that are already in
`[0, maxval]`, so no clamp is needed and none is applied. Every index used is
within `A[-1..15]` / `L[-1..15]`.

**Reconstruction** is 7.3 with `pred[y][x]` replaced by `P[j][i]`, and the
result is written to `recon` as well as to the output plane, because the next
block in raster order reads it.

### 7.5 The two forms: replace and layer

`base` in 7.4 -- both the reference fallback and what mode 0 predicts -- is
selected by **frame-header `flags` bit 2**:

| bit 2 | form | `base` | mode 0 predicts | what the modes predict |
|---|---|---|---|---|
| 0 | replace | `pred` (7.2) | `pred[y][x]` | the samples |
| 1 | layer | all-zero | 0 | the DC-plane residual |

In the layered form the block's reconstruction is
`clamp(pred[y][x] + P[j][i] + res, 0, maxval)` and what is stored into `recon`
for later blocks is that value **minus** `pred[y][x]`, i.e. the reconstructed
DC-plane residual.

Both forms make mode 0 identical to v1. The replace form is what the reference
encoder emits by default: measured on the quality harness it is better at every
operating point (ref/RESULTS-intra.md), because on this content the DC plane's
smooth interpolation is exactly what a directional predictor wants to be rid
of, not something it wants to correct. The layered form is kept in the syntax
because it is one flag bit and it is the better shape for content whose
low-frequency structure the DC plane already captures well.

`flags` bit 2 without tool bit 17 is a `BITSTREAM` error (`r14`).

### 7.6 What this costs a GPU decoder (note for Pass B)

7.1-7.3 are fully parallel: one barrier per plane, every block predicted at
once. 7.4 is a **wavefront**, and the numbers below are the schedule the Pass B
shader has to implement. They assume the module's own layout: **256 threads per
64x64 tile, 4 threads per 8x8 block**.

Block `(bx, by)` reads its left neighbour `(bx-1, by)`, its above neighbour
`(bx, by-1)` and its **above-right** neighbour `(bx+1, by-1)` (mode 5 `DDL`
reaches `A[15]`). With an above-right dependency the independent set is not the
anti-diagonal `bx + by` but `2*by + bx`, so the luma plane of a `res_level` 0
tile is a **22-step** wavefront, not the 15 steps an anti-diagonal would give.

| plane | steps | blocks per step, mean | threads active, mean |
|---|---|---|---|
| luma, `res_level` 0 (8x8 blocks) | 22 | 2.91 | 11.6 / 256 = **4.5 %** |
| 4:4:4 chroma, `res_level` 0 | 22 | 2.91 | 4.5 % |
| 4:2:0 chroma (4x4 blocks) | 10 | 1.60 | 2.5 % |

Barriers per tile, against one per plane for the DC plane alone:

| | v1 (7.1-7.3) | with `INTRA_DIR` |
|---|---|---|
| 4:4:4, `res_level` 0 | 3 | 3 + 66 = **69** |
| 4:2:0, `res_level` 0 | 3 | 3 + 42 = **45** |

The arithmetic does not grow -- a directional predictor is a handful of adds
and shifts per sample, less than the DC plane's bilinear interpolation. What
grows is **serialization**: 22 rounds of it where there was one, at 4.5 %
occupancy. This is exactly the cost PAPER design principle 2 exists to refuse,
and it is why 7.4 is a tool bit rather than a mandatory part of v1.

**Three restrictions that reduce it, with what each costs in rate.** Measured
on the harness sequence at 2048x1024 4:4:4, one frame, QP 8/16/24, against the
7.4 derivation as written (build with `-DNXVC_DIR_SCHED_EXPERIMENT` and set
`NXVC_DIR_SCHED` to reproduce; ref/RESULTS-intra.md section 5 has the table):

| restriction | steps | occupancy | rate cost |
|---|---|---|---|
| as written | 22 | 4.5 % | -- |
| **A**: drop the above-right reference (clamp `A[k]`, `k >= 8`, to `base`) | 15 | 6.7 % | **+0.24 %** |
| **B**: confine the dependency to 32x32 sub-tiles | 10 | 10.0 % | +1.6 % |
| **C**: 16x16 super-blocks, their 4 blocks predicted in parallel from the super-block border | 10 | 10.0 % | not measured |
| **A + B** | 7 | 14.3 % | +1.8 % |
| **B + C** | 4 | 25.0 % | not measured |
| **A + B + C** | 3 | 33.3 % | not measured |

Restriction **A** is essentially free -- a quarter of a percent for a third of
the barriers -- because `DDL` is a minority mode and its above-right samples
are the ones furthest from the block anyway. **B** is a one-line change to the
`at()` test in 7.4 (the "already reconstructed" predicate gains a same-sub-tile
term) and is the same idea as tile independence applied one level down, so it
needs no new concept and no new syntax field. **A + B** brings the luma plane
to 7 steps and 14.3 % occupancy -- a 3.1x reduction in barriers and a 3.2x
increase in occupancy -- for 1.8 % of rate, which against directional intra's
own 22.5 points is a good trade.

**7.4 as written is the normative derivation.** It is deliberately the
best-rate variant, because the right time to pay rate for barriers is against a
*measured* Pass B barrier cost on the target part, and that number does not
exist yet. When it does, A and A+B are the two candidates, and adopting either
is a `SYNTAX.md` change and a conformance-vector regeneration, not a new tool
bit: it narrows what `INTRA_DIR` means rather than adding to it.

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
    if INTRA_DIR:
        unit: the mode unit of p     (nb*nb intra modes, section 9.6)
    for by in 0..nb-1, bx in 0..nb-1 (raster):
        unit: block (bx, by) of p    (64 coefficients)
```

The mode unit is a whole unit rather than a symbol attached to each block on
purpose. A block's mode is coded relative to the modes of its left and above
neighbours (9.6), and those live in the *same* unit, so the derivation only
ever reads values the same lane has already produced -- whatever the
interleaved lane schedule does with the other units. Attaching the mode to the
block unit would make the prediction depend on the decode order *between*
lanes, which the schedule does not define.

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

There are **12 contexts of 16 symbols** each, or **16** when the stream sets
tool bit 21 `CTX_V2`. Every context's 16 frequencies are 10-bit, at least 1,
and sum to exactly 1024, so every symbol is always decodable and a hostile
stream cannot produce an undefined symbol.

| index | use | model |
|---|---|---|
| 0 | `CBF`, luma and alpha planes | both |
| 1 | `CBF`, chroma planes | both |
| 2 | `LAST`, luma and alpha planes | both |
| 3 | `LAST`, chroma planes | both |
| 4-11 | `LEVEL`, band x previous-level class | both |
| 12 | `CBF`, DC-plane units | v2 only |
| 13 | `LAST`, DC-plane units | v2 only |
| 14 | `LEVEL`, DC-plane units (one context, no banding) | v2 only |
| 15 | `MODE`, the intra mode symbol (9.6) | v2 only |

In the **v1 model** a DC-plane unit uses the same contexts as its plane, and
the intra mode symbol is bypass-coded (9.6).

In the **v2 model** the DC plane gets contexts 12-14 of its own. It is a dense,
low-frequency image of block means and an AC block is a sparse high-frequency
residual; sharing a context made each of them pay for the other's statistics.
Contexts 0-11 keep their *meaning* in both models but not their *statistics*,
because in v2 they no longer see the DC plane at all -- so the two models have
**separate built-in table families** (`kDefaultFreq` and `kDefaultFreqV2` in
`ref/src/default_tables.inc`), both pinned by the conformance vectors.

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

A transmitted set is **120 bytes** (12 contexts x 16 symbols x 5 bits) in the
v1 context model and **160 bytes** (16 x 16 x 5) under `CTX_V2`: MSB-first bit
packing, contexts in index order, symbols in symbol order. The built-in default
it is a delta against is the same set index of the same model's family. Each 5-bit value
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
procedure (the one place a decoder divides; it runs 12 x 8 -- or 16 x 8 --
times per frame, not per symbol):

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

### 9.6 The mode unit (tool bit 17)

A mode unit carries the `nb * nb` intra modes of one plane, in raster order.
For block `b` (raster index), the **most probable mode** is

```
left  = (b % nb) > 0  ? modes[b - 1]  : DC_PLANE
above = (b / nb) > 0  ? modes[b - nb] : DC_PLANE
mpm   = left == above ? left : min(left, above)
```

so `mpm` is `DC_PLANE` for the first block of a plane, and for any block whose
neighbours are outside the plane.

**Without `CTX_V2`** the mode is two bypass fields:

| field | bits | meaning |
|---|---|---|
| `is_mpm` | 1 | 1: `mode = mpm`, and nothing follows |
| `idx` | 3 | present when `is_mpm == 0`: `mode = nonmpm(mpm, idx)` |

**With `CTX_V2`** it is one symbol in context 15, alphabet 0..8:

```
sym == 0 : mode = mpm
sym >= 1 : mode = nonmpm(mpm, sym - 1)
```

Symbols 9..15 in context 15 are illegal (`BITSTREAM`). `nonmpm(mpm, i)` is the
`i`-th of the eight modes **other than** `mpm`, in ascending mode order.

The unit is coded in full before its lane moves on, which is what lets the MPM
read `modes[b-1]` and `modes[b-nb]` without any assumption about the order the
lanes interleave in (section 9.1).

### 9.7 Sign data hiding (tool bit 22)

For a coding unit with `CBF == 1` whose `LAST` is at scan position **4 or
higher**, the sign at scan position `LAST` is **not coded**. It is

```
sum = sum of |level| over scan positions 0 .. LAST
sign(level at LAST) = (sum & 1) ? negative : positive
```

Everything else is unchanged: every other nonzero level still carries its own
sign bit immediately after the level. The hidden position is `LAST` rather than
the lowest nonzero because `LAST` is known to the decoder the moment the `LAST`
symbol is decoded and is nonzero by construction, so no lookahead is needed --
the decoder stores the magnitude, finishes the unit, and applies the parity.

The threshold of 4 exists so the encoder always has several coefficients to
spend the parity on: it makes the parity agree by moving exactly one level by
one step, choosing the move with the smallest squared error. That is an
encoder decision and is not normative.

`SIGN_HIDE` applies to DC-plane units as well as residual blocks.

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
2. build the coding-unit list from res_level, chroma444, alpha_mode, mode
3. run the interleaved rANS schedule -> int16 coefficients, one array per unit
   (this is GPU Pass A: output is a dense int16 coefficient buffer plus the
   tile record)
4. if mode != INTRA: build the inter predictor W from the reference picture
   ref_sel selects, the frame's warp_ext matrix and the tile's vector
   (section 13.3).  If the tile is skipped or concealed, W is the whole
   reconstruction and steps 5 and 6 do not run.
5. for each coded plane:
     a. dequantize + inverse-transform the DC plane   -> means M
     b. planar-interpolate M                          -> planar
     c. pred = planar                     for mode == INTRA
        pred = clamp(W + planar - dc_offset, 0, maxval)   otherwise
     d. for each block: dequantize, inverse transform (or take the residual
        directly for tskip), add pred, clamp
     e. upsample the plane by its factor into the picture
   (this is GPU Pass B: one workgroup per 64x64 tile)
6. if color_transform == 1, convert planes 0..2 back to RGB after upsampling
7. store the reconstruction into the reference-ring slot ref_slots names, in
   the CODED sample domain -- before step 6 -- and apply the prediction-state
   update of section 13.5
```

**Tile independence.** Within one eye and one frame, no tile's reconstruction
depends on any other tile. The single exception is `mode == STEREO`, where a
right-eye tile reads up to three left-eye tiles of the same row of the same
frame, which the row order of section 3.3 makes available. There is no other
intra-frame tile dependency in the format, no deblocking filter and no loop
filter in v1. Inter tiles depend on *previous* frames, which is a different
thing: the reference picture is complete and immutable while the frame that
reads it decodes.

---

## 11. Byte-layout cheat sheet

```
file  := stream_header ext_area frame*
frame := frame_header [warp_ext] [custom_matrices] [table_set]* tile_row*
tile_row := row_header tile*                 // eyes * rows of them
tile  := tile_header [mv | disparity] [alpha] payload
```

| structure | fixed size |
|---|---|
| stream header | 64 + `ext_len` |
| frame header | 40 (+`36 * eyes` if `warp_present`, +128 if custom matrices, +120 per transmitted table set, or +160 per set when `CTX_V2` is set) |
| tile-row header | 12, `eyes * rows` of them, row-major eye-minor |
| tile header | 8 (+2 if `mv_present`, +1 if `alpha_mode == 1`); a skipped tile has none |
| tile payload | `payload_len`, at least `4 * active_lanes` |

---

## 12. Phase 1 conformance (intra-only)

A Phase 1 decoder implements everything above except inter prediction. It must:

* accept `mode == INTRA` and reject `WARP_SKIP`, `STATIC_MV`, `WARP_MV` and
  `STEREO` with an "unsupported" status (not a malformed-bitstream status);
* reject a nonzero `skip_bitmap` (a skip references a frame it cannot have),
  and a skip bit at or above `cols_per_eye` as `BITSTREAM` in every profile;
* reject `eyes != 1`, `num_layers != 1`, `bit_depth != 8`, `layer != 0`,
  `eye != 0`, and any tool bit outside the supported set;
* parse `mv_present`, `ref_sel` and `wgt` correctly even though it cannot use
  them, so that a Phase 2 stream is refused rather than misparsed.

The conformance vectors in `tests/vectors/` pin the MD5 of each bitstream and of
its decoded planes. `nxv-vectors --check tests/vectors` verifies both, i.e. that
the decoder still produces the same pixels **and** that the encoder still
produces the same bytes. The Vulkan decoder is conformant when it reproduces
every `decoded_md5` in `tests/vectors/vectors.md5`.

**Rejection vectors.** `tests/vectors/rejects.md5` lists streams that must be
*refused*, each with the exact status the decoder has to return. Producing no
output is not enough: the status carries meaning, and confusing the two failure
families is a real interoperability bug.

| status | meaning |
|---|---|
| `VERSION` | the magic, version or `tools` mask is not something this decoder speaks |
| `UNSUPPORTED` | legal v1 syntax that this profile does not implement (a Phase 2 tile mode or a nonzero `skip_bitmap` on a decoder without `INTER`, `bit_depth` 10, 32x32 tiles) |
| `BITSTREAM` | this cannot be a legal stream at all (a reserved value, a field that disagrees with its position) |
| `TRUNCATED` | a length ran past the buffer |

A decoder is conformant on this suite when it returns exactly the listed status
for every vector and writes no samples. The suite covers a bad magic, an
unimplemented mandatory tool bit, `bit_depth` 10, the 32x32 tile profile, a
`payload_len` past the frame, `res_level` 3, a truncated rANS payload, a skip
bit for a column beyond the picture, a reserved tile-header bit, an `INTER`
tile, `wm_id` without its tool bit, a wrong `row_index`, a short
`frame_bytes`, `flags` bit 2 without `INTRA_DIR`, YCoCg-R declared with 4:2:0
chroma, a `CTX_V2` table set that overruns the tile rows, and `LOSSLESS`
together with `SIGN_HIDE`.

**The v2 intra tools.** `v36`-`v44` pin them: `INTRA_DIR` alone in 4:4:4 and
4:2:0, `INTRA_DIR` with `CTX_V2`, `CTX_V2` alone, the layered form, every v2
feature at once with transmitted 160-byte table sets, the combination with
`res_level` cycling and transform skip, `SIGN_HIDE` alone, and the reference
encoder's shipped default configuration. `v01`-`v35` are **byte-identical** to
the v1.2 set: all three tools are additive and off unless their bit is set.

`v23_custom_tables420` and `v34_wm_id420_tables` pin the probability-table
normalization of section 9.4 — the one place a decoder divides — so a decoder
that rounds it differently fails on a committed bitstream rather than on
customer content.

---

## 13. Inter prediction (Phase 2)

Everything in sections 6 to 9 is unchanged by inter prediction: the transform,
the quantiser, the scan, the contexts and the rANS schedule are the same for
every mode. What changes is what the residual is measured against, and that is
the whole of this section.

### 13.1 Modes

| `mode` | name | reference | vector | residual |
|---|---|---|---|---|
| 0 | `WARP_SKIP` | `warp(ref[N-1])` | the stored `last_mv` | none |
| 1 | `STATIC_MV` | `ref[N-1-ref_sel]`, unwarped | coded `mv` | coded |
| 2 | `WARP_MV` | `warp(ref[N-1-ref_sel])` | coded `mv` | coded |
| 3 | `INTRA` | none | none | coded |
| 4 | `STEREO` | the decoded first eye of **this** frame | coded `disparity` | coded |

`STATIC_MV` exists for head-locked content -- menus, HUDs, laser pointers, a
transport overlay -- where the warp is exactly wrong and the identity predictor
is exactly right. It does not read `warp_ext()` at all, and neither does
`STEREO`; a frame carrying either needs no `warp_present`.

### 13.2 The reference ring

Four slots, addressed by `frame_number mod 4`. A frame writes the slot its own
number names, which is what `ref_slots` states (section 3.1), and reads the
slot `(frame_number - 1 - ref_sel) mod 4`, whose stored frame number must be
`frame_number - 1 - ref_sel`; if it is not, the stream is malformed. A slot
holds the whole reconstructed picture of every eye **in the coded sample
domain** -- Y/Co/Cg, before the inverse colour transform -- because that is the
domain the predictor predicts in.

`ref_sel == 3` is reserved, so the reachable references are `N-1`, `N-2` and
`N-3`; the fourth slot is the one being written. `warp_ext()` describes the
transform between the picture the frame's tiles reference and this frame, so an
encoder that uses `ref_sel > 0` derives its matrix against that frame.

### 13.3 The predictor

The predictor is `nxvc_warp_ref` and nothing else: `docs/WARP.md` is normative
for it and `warp/` is its executable form. Per tile and per plane:

1. The plane's matrix is the frame's, conjugated by the plane's subsampling
   factor `sub` (1, or 2 for 4:2:0 chroma):

   ```
   h02' = round_half_away(h02 / sub)      h20' = h20 * sub
   h12' = round_half_away(h12 / sub)      h21' = h21 * sub
   ```

   with every other entry unchanged, and the origin
   `(ox, oy) = (plane_width >> 1, plane_height >> 1)` in that plane's samples.
   The halving rounds to nearest, ties away from zero, so it is symmetric about
   zero and identical everywhere; the doubling is exact, because a legal
   `h20`/`h21` is of order 2^15.

2. The plane's vector is `mv >> 1` for `sub == 2` and `mv` otherwise, an
   arithmetic shift, in quarter plane-samples.

3. `warp_tile()` produces the block: four corner source coordinates from two
   64-bit accumulations and a fixed 32-iteration restoring divide each, then
   integer bilinear interpolation of those corners across the block, then the
   vector added in Q.6, rounded to Q.4, and one **bilinear** sample with
   clamp-to-edge per tap. `STATIC_MV` and `STEREO` take the identity corners
   and never touch the matrix.

4. A tile whose `res_level` is nonzero predicts at full extent and box-averages
   the predictor down to the coded extent, with the same kernel the encoder
   uses on the source, so the residual is measured in one domain throughout.

The prediction the residual is added to is then

```
pred = clamp(W + planar(M) - dc_offset, 0, maxval)
```

where `W` is the predictor above and `planar(M)` is the DC plane of section 7.2
built, for an inter tile, from the block means of the **residual**. The
coding-unit list is therefore identical in every mode -- one DC unit and
`nb * nb` block units per plane -- and the DC plane doubles as the per-block DC
correction the warp needs. On a well-predicted tile every DC-plane coefficient
is zero and the whole structure costs one CBF symbol. An inter tile's block
means are `dc_offset + a residual mean`, whose range is wider than the sample
domain on both sides, so they are **not** clamped to it; `dequant` has already
bounded them.

The **mode unit** of section 9.6 is present only for `mode == INTRA`. An inter
tile's prediction is the warp, and nine ways of saying nothing is not a tool.

### 13.4 Interpolation filter

Version 1 is **bilinear**, in every profile. The filter is selected by tool bit
23 `FILTER_CATMULL_ROM`, which is not defined for version 1 and which a version
1 decoder must refuse (section 2.3), so there is exactly one legal predicted
sample for every conforming v1 bitstream and `profile` selects nothing.

The evidence is that Catmull-Rom is within **0.05 dB** of bilinear on a single
step and buys about 2 dB only on long warp chains -- which a higher per-tile
refresh rate shortens anyway -- set against 16 taps per sample rather than 4 on
a 4 ms decode budget. The tap table stays normative for the version 2 bit so
that it has a defined meaning the day it is enabled.

### 13.5 Per-tile prediction state

Six bytes per tile position **per eye**. It is a running history, which is what
makes it a different object from the transport's four-byte per-slot receiver
record; the two were compared as if they were one, and they are not.

| off | size | field |
|---|---|---|
| 0 | 2 | `last_mv_x`, s16 quarter samples |
| 2 | 2 | `last_mv_y`, s16 quarter samples |
| 4 | 2 | `last_disp`, u16 quarter samples, 12 bits used |

13.9 kB at the v1 configuration. Applied **after** a tile is reconstructed:

| `mode` | `last_mv` | `last_disp` |
|---|---|---|
| `WARP_MV` | `= (mv_x, mv_y)` | unchanged |
| `WARP_SKIP` | unchanged (it consumed it) | unchanged |
| `STATIC_MV` | unchanged | unchanged |
| `INTRA` | `= (0, 0)` | `= 0` |
| `STEREO` | unchanged | `= disparity` |

`STATIC_MV` does not update `last_mv` because its vector displaces an
*unwarped* reference while `WARP_SKIP` and concealment apply the stored vector
*after* the warp; storing it would conceal from the wrong place. `INTRA` clears
the state because after a refresh there is no motion history and a stale vector
is worse than zero. The two fields are separate because a tile may alternate
between `STEREO` and `WARP_MV`, and a 60-sample disparity is not a motion
vector.

The whole state is cleared to zero when `tile_map_reset` is set.

### 13.6 Concealment

A tile the client did not receive is reconstructed by running the `WARP_SKIP`
predictor with the tile's stored `last_mv` and no residual -- exactly a
legitimately skipped tile, which is why there is no separate concealment path
to test and why the encoder can replay it. It is deterministic, so an encoder
holding the same reference and the same state produces the same samples bit for
bit, which is what lets it predict the next frame from what the client actually
shows rather than from what it wished it had sent. A concealed tile's
prediction state does not advance.

`last_disp` is never used for concealment: a concealed right-eye tile conceals
temporally, from its own eye's previous frame, because the left eye of the
*current* frame may itself be missing.


### 13.7 What this reference does not do

* **`STEREO` under loss.** Concealing a left-eye tile changes what a `STEREO`
  tile of the same row was predicted from, and re-deriving that needs a
  full-frame replay. `nxvc_encoder_set_received_tiles` returns
  `UNSUPPORTED` for that combination rather than diverging silently.
* **Chroma tiles are predicted through the 64x64 kernel.** `warp_tile()`
  produces a fixed 64x64 block, so a 4:2:0 chroma tile, whose extent is 32,
  takes the top-left quarter of the 64x64 block at the same origin. The
  samples are the library's, bit for bit, and both sides of the codec do the
  same thing, so it is exact; what differs from a hypothetical 32x32 kernel is
  only the span the in-tile corner interpolation is fitted over. A GPU Pass B
  doing chroma natively at 32x32 must be given the same corner basis.

### 13.8 Refresh (encoder side, no syntax)

`WARP_SKIP` puts the raw prediction error straight into the reference and
leaves it there until the tile is next coded, so something has to bound how
long a tile position may go uncorrected. The bound is a **rolling intra
refresh**: a forced `INTRA` on every tile position within a period `T`,
PAPER.md 2.6's 2 s at 90 Hz.

This is entirely an encoder decision. The bitstream is the same either way,
which is why this subsection describes two schemes and normatively requires
neither.

**The fixed scheme.** Every frame, `1/T` of the tile positions are forced
`INTRA`, chosen by a fixed pseudo-random permutation of the tile index rather
than a column sweep, so there is no visible refresh wave.

**The drift-driven scheme.** The encoder holds a bit-exact shadow of what the
client displays (13.6), so it can *measure* the thing the fixed scheme can
only assume. Two rules replace the one:

* **The hard cap.** A tile position may go at most `T` frames without an
  `INTRA`. This is the same loss-recovery bound, stated as a ceiling rather
  than a period, so a refresh is never *early*. The clocks are staggered by
  the same permutation the fixed scheme uses, so the caps of a picture do not
  all fall due on one frame.
* **The drift gate.** A tile position whose shadow has drifted further from
  the source than `g * qstep^2 / 12` per sample, measured on the frame just
  encoded, may not take `WARP_SKIP`. It is *not* forced to `INTRA`: the drift
  is measured against the very reference the tile would predict from, and a
  coded `WARP_MV` residual usually corrects it for fewer bits than a fresh
  intra tile. The gate removes the free option; the ordinary
  rate-distortion decision then picks the mode.

`qstep^2 / 12` is the quantiser's own noise floor, and the gate is stated as a
multiple of it for the same reason the `WARP_SKIP` early-out is (`ref/`'s
`skip_thresh`): it is the error a *coded* tile would have left behind anyway,
so drift below it is not worth correcting, at any QP. That is the
rate-distortion content of the threshold. The rate of the correction is folded
into the multiplier `g`, because the encoder cannot know it without coding the
tile; `g` is the swept constant and `ref/RESULTS-inter-a.md` is the sweep.

### 13.9 Near-skip: the DC-correction tile form (tool bit 24)

A warped tile often drifts by a small, smooth amount -- a shading change, a
slow exposure ramp, the accumulated rounding of a resampling chain. The
encoder's only two answers were a `WARP_SKIP` that leaves the drift in the
reference and a fully coded tile that pays a rANS payload and a lane flush to
remove it. **Near-skip** is the answer in between: the tile's entire residual
is a per-plane block-mean field, flat or a pair of ramps, in three or nine
signed bytes.

Tile-header word1 gains two bits:

| bit | field | notes |
|---|---|---|
| 28 | `near_skip` | the tile carries a correction field and no payload |
| 29 | `near_skip_ac` | the correction field carries the two ramps as well |

Both require tool bit 24 `NEAR_SKIP`. Constraints, each a MUST-reject:

* `near_skip_ac` requires `near_skip`;
* `near_skip` requires `mode != INTRA` -- a picture built from nine bytes is
  not a prediction, and the form exists to correct one;
* `near_skip` requires `payload_len == 0`, `res_level == 0` and
  `alpha_mode != 2`. A payload would contradict the bit; a `res_level` would
  put the correction in a domain the reconstruction then stretches; a coded
  alpha plane has no correction defined for it.

The correction field follows the constant-alpha byte and is
`3 * (near_skip_ac ? 3 : 1)` bytes: for each of the three colour planes in
order Y, Co, Cg, one signed byte `c0`, then under `near_skip_ac` two more,
`c1` and `c2`. Alpha is never corrected.

**Decoding process.** Per plane, with `nb` the plane's blocks per edge and
`t_dc` the DC-plane quantiser step of 6.5 -- `dequant_step(qp >> 1, 16)`, the
same step a coded DC plane uses, because this is that DC plane written another
way:

```
d0 = dequant(c0, t_dc)
dh = near_skip_ac ? dequant(c1, t_dc) : 0
dv = near_skip_ac ? dequant(c2, t_dc) : 0

for by in 0 .. nb-1, bx in 0 .. nb-1:
    means[by][bx] = dc_offset + d0
                  + ((dh * (2*bx - nb + 1)) >> log2(nb))
                  + ((dv * (2*by - nb + 1)) >> log2(nb))
```

`>>` is arithmetic. `2*bx - nb + 1` runs over `+-(nb-1)` and the shift divides
it by `nb`, so a corner block sits one step short of the full amplitude --
the same convention the DC plane's own bilinear interpolation already uses.

From `means` the tile is finished by **exactly** the path 13.3 already
defines: the planar interpolation of 7.2 over the block centres, then
`pred = clamp(W + planar(M) - dc_offset, 0, maxval)`. There is no residual to
add, so the tile's samples *are* `pred`. No new arithmetic is introduced by
this tool; it introduces a second way of writing a `means` field and nothing
else.

**What it costs a GPU decoder.** Less than any other tile. No entropy decode,
no rANS lane flush, no inverse transform: one `warp_tile()`, `nb*nb` means
from three multiply-shift-adds each, the planar interpolation the DC path
already runs, and one add per sample. It is strictly cheaper than the coded
tile it replaces and strictly more expensive than the skip it replaces, by one
bilinear field.

### 13.10 Quadrant vectors (tool bit 25)

One vector per 64x64 tile is one vector for four thousand samples. Where a
tile straddles the boundary of an object moving against the background -- a
hand, a menu edge, a disc -- one half of it wants a different vector from the
other, and the residual pays for the disagreement over the whole tile.

Tile-header word1 gains one bit:

| bit | field | notes |
|---|---|---|
| 30 | `quad_mv` | four quadrant vectors follow the tile vector |

It requires tool bit 25 `QUAD_MV`, and it requires `mode == WARP_MV` or
`mode == STATIC_MV`, which are the modes that carry a tile vector to refine.
Word1 bit 31 remains reserved and must be zero.

Four bytes follow the tile's `mv_x`/`mv_y`, before the constant-alpha byte:
one byte per quadrant in raster order (top-left, top-right, bottom-left,
bottom-right), bits 3:0 the `x` delta and bits 7:4 the `y` delta, each a
**signed nibble** in quarter samples, range -8 to +7. The quadrant's vector is

```
mv_q = (mv_x + dx_q, mv_y + dy_q)
```

and it is a delta from the tile vector, not a vector of its own: four bytes
rather than eight, and a tile header still parses without any decoder state
(4.1).

**The corner basis is the tile's.** This is the whole of the rule, and 13.7's
caveat is why it has to be said. `warp_tile()` derives four source corner
coordinates for the tile, interpolates them across the block, and *then* adds
the motion vector, per sample, in Q.6. A quadrant therefore changes the vector
and nothing else: it does **not** re-derive corners at 32x32, and an
implementation that did would produce different samples. Two implementations
are consequently equivalent and both are conforming:

* run the tile's predictor four times, once per quadrant vector, and keep each
  quadrant's own 32x32 region -- which is what `ref/` does, because it is
  obviously correct;
* run it once and add `mv_q` inside the sample loop, selecting `q` from the
  sample's position -- which is what a GPU does, at **no cost at all** over a
  single-vector tile.

They are equivalent because the vector enters as a per-sample constant offset
after the corner interpolation, so nothing that depends on it is shared
between quadrants.

For a plane subsampled by `sub`, the quadrant vector is halved by the same
rule the tile vector is (13.3 step 2), applied to the sum: `mv_q >> 1`. The
quadrant boundary is the plane's own half extent, 32 luma samples and 16
chroma samples for 4:2:0.

`last_mv` (13.5) stores the **tile** vector, not a quadrant's. A concealed
tile has no quadrant structure to conceal with, and the tile vector is the one
statement about the tile as a whole.

**What it costs a GPU decoder.** Four bytes of traffic and one extra select
per sample. No new dependency, no cross-tile state, no extra pass.

### 13.11 Sub-tile intra (tool bit 26)

Where something was occluded a frame ago and is not now, no vector recovers
it: the samples are not in the reference at any displacement. A rotation-only
predictor meets this as a strip along a near-field object's edge, and the tile
containing it is otherwise perfectly predicted. `INTRA` on the whole tile
throws away three quadrants that did not need it.

Tile-header word1's last free bit becomes:

| bit | field | notes |
|---|---|---|
| 31 | `sub_intra` | one 32x32 quadrant of this tile is predicted intra |

It requires tool bit 26 `SUBTILE_INTRA`, `mode != INTRA` -- an intra tile has
no predictor to drop -- and `near_skip == 0`, because a near-skip tile has no
residual and the quadrant would then be a flat field.

One byte follows the quadrant deltas: bits 1:0 the quadrant index in raster
order (top-left, top-right, bottom-left, bottom-right), bits 7:2 reserved and
MUST be zero. A whole byte carries two bits because word1 has no room left,
and it is one byte on the tiles that carry a strip and none anywhere else.

**Decoding process.** In the named quadrant, and in every plane, the predictor
`W` of 13.3 is replaced by that plane's `dc_offset`. Nothing else changes. So
13.3's

```
pred = clamp(W + planar(M) - dc_offset, 0, maxval)
```

collapses in that quadrant to `clamp(planar(M), 0, maxval)`, which is exactly
the intra reconstruction of 7.3: the DC plane and the residual carry the
quadrant on their own, with the same coding units, the same contexts and the
same quantiser the rest of the tile uses. The quadrant boundary is the
plane's own half extent -- 32 luma samples, 16 chroma samples at 4:2:0 -- and
is block-aligned at every legal `res_level`.

The tile's mode is unchanged, so `last_mv` and the prediction state follow
13.5 as if the quadrant were predicted normally. A concealed tile has no
`sub_intra`, because a concealed tile has no header; concealment is the
`WARP_SKIP` predictor over the whole tile, as 13.6 says.

**What it costs a GPU decoder.** One byte of traffic and one select per
sample, in the same place 13.10's select already is. There is no second
prediction path: "intra" here means "the predictor contributes the DC offset",
which is a constant.

---

## 14. Phase 2 conformance

A Phase 2 decoder implements section 13 in addition to everything a Phase 1
decoder implements. It must:

* accept every value of `mode`, and reject `mode` 5 to 7;
* reject `warp_ext()` violating any of the four conditions of section 3.1.1;
* reject a warped mode in a frame with `warp_present == 0`, `mode == STEREO`
  on the left eye or without the `STEREO` tool bit, `ref_sel == 3`, a nonzero
  `ref_sel` on an `INTRA` or `STEREO` tile, a `disparity` with bits 15:12 set,
  a `ref_slots` other than `1 << (frame_number mod 4)` on an inter stream, a
  skip bit for a column beyond `cols_per_eye`, and a skip or an inter mode on a
  stream without the `INTER` tool bit;
* reject tool bits 14 and 23 with a `VERSION` status, and the `WARP` bit
  without `INTER` with a `BITSTREAM` one;
* reject `near_skip`, `quad_mv` or `sub_intra` without its tool bit,
  `near_skip_ac` without `near_skip`, `near_skip` on an `INTRA` tile or with a
  nonzero `payload_len`, `res_level` or `alpha_mode == 2`, `quad_mv` on any
  mode other than `WARP_MV` and `STATIC_MV`, `sub_intra` on an `INTRA` tile or
  together with `near_skip`, and a sub-intra byte with bits 7:2 set;
* reproduce every `decoded_md5` of the `v45`-`v61` vectors.

`v45`-`v56` are the twelve entries Annex D D-21 asks for: an identity warp with
a repeated picture (every tile `WARP_SKIP`, a bit-exact copy), a warped
moving-object sequence, a still picture under a 12 deg/frame matrix (where
`STATIC_MV` must win), a matrix sweep across the accepted envelope, tiles
straddling the picture edges, the skip and prediction-state rules over four
frames, `ref_sel` 1 and 2 against the four-slot ring, stereo with real
disparity, its `STATIC_MV` counterpart, a 4:2:0 inter sequence exercising the
conjugated chroma matrix, and rolling intra refresh at a period of 4.

Where D-21 states a *property* rather than a stream -- "`STATIC_MV` must not
change one output bit when the matrix does", "an integer vector under identity
equals a shifted tile" -- the property is asserted directly, in
`tests/ref/test_inter.cpp` and in `warp/`'s own suite, and the vector here pins
the bitstream and the reconstruction it holds for. A digest cannot express
"changing this field must not change the output"; an assertion can.

`v57`-`v61` are the syntax v1.5 additions: a near-skip stream at a coarse
quantiser on slowly drifting content, a quadrant-vector stream on an object
moving against a pan, near-skip on 4:2:0 (where `nb` is 4 and the ramp shift
differs), one stream with the drift refresh and both of those on at once, and
a sub-tile-intra stream on fast object motion. Each tool is behind its own
bit, so the same encoder with the bits off reproduces `v45`-`v56` byte for
byte -- which is the compatibility claim, stated as a test rather than as a
promise.

`r18`-`r36` are the rejection vectors, and they matter more than the positive
ones: sections 3 and 4 impose roughly forty MUST-reject conditions, and a
decoder that accepted every malformed stream would otherwise pass the suite
completely.

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

34. **The odd-part rotation is specified as an exact product, not as int32
    arithmetic.** Writing `((P + Q) * C4 + 256) >> 9` in int32 is undefined
    behaviour on legal input: `|P + Q|` reaches `8.6e7` and the product is
    `3.1e10`. The value was always well defined; what was missing was a
    statement of how to compute it. Section 6.3 gives the two-word split,
    which is bit-identical to the mathematical value, so **no conformance
    vector changed** when the reference implementation was corrected. This is
    a specification fix, not a bitstream change. `ctest -R ref.saturate` under
    `-fsanitize=undefined` is the regression test.

35. **The inverse shift chain is 7 then 13 with `clamp16` after pass 1.**
    Restated here because the errata found the pair quoted inconsistently
    elsewhere: pass 1 is `clamp16((out + 64) >> 7)`, pass 2 is
    `clamp16((out + 4096) >> 13)`, the two shifts sum to 20 for unit gain, and
    the pass-1 clamp is normative rather than an implementation convenience
    (item 11). 8 + 12 also sums to 20 and is **not** conformant.

36. **`wm_id` is a per-tile override, and 0 means "the frame's matrix".**
    PAPER 4.6.1's degradation ladder needs to drop a *single* tile onto a
    low-pass weighting matrix without touching the frame, and rate control has
    no other way to spend step 1. Two of the tile header's reserved bits carry
    it. Encoding 0 as "inherit" rather than as "matrix 0" is what makes every
    stream written before this field existed still byte-identical, and it
    keeps the common case free. The ladder's step-1 matrix is **`wm_id = 2`**,
    `min(32, 16 + 2s)`, the strongest roll-off the `[1, 32]` weight range can
    express; it reaches the cap at `s = 8`, so half the coefficient positions
    are quantized twice as coarsely. A frame with custom matrices refuses
    `wm_id != 0` rather than defining a precedence rule nobody would remember.

37. **`bit_depth` 10 is reserved and rejected, not tolerated.** The stream
    header has the field and `tools` has the bit, but v1 defines neither the
    10-bit sample domain nor the `qstep` scaling nor the clamps for it, so
    there is nothing for a decoder to be conformant *to*. Accepting the field
    and guessing would produce two incompatible "10-bit" decoders. Both the
    field and the tool bit are refused, and `r02`/`r03` pin it.

38. **Rejection is part of conformance and the status is normative.**
    Appendix A item 30 said the decoder rejects rather than conceals; it did
    not say *how*. `tests/vectors/rejects.md5` now pins the exact status for
    thirteen malformed streams. The distinction that matters in practice is
    `UNSUPPORTED` (a legal stream this build cannot decode — the transport
    should fall back) against `BITSTREAM` (corruption — the transport should
    re-request), and a decoder that swaps them makes the wrong recovery
    decision.

39. **Directional intra keeps the DC plane, and mode 0 is the DC plane.**
    The alternative was to replace the DC plane with coded edge samples, the
    way H.264 and HEVC do. Keeping it costs 3-7 % of a frame (Appendix B) and
    buys three things that are worth more than that: a **tile-independent**
    fallback for the tile's top and left borders, so no tile ever reads
    another tile and per-tile loss recovery and the rate controller's per-tile
    ladder both keep working; a per-block escape hatch that makes the tool a
    strict superset of v1, so it can never be worse than v1 on a block; and a
    predictor that is still correct when a block's neighbours were themselves
    predicted badly. Measured, the tool is worth **-22.5 points of BD-rate**
    against x264 intra on the harness 4:4:4 sequence (+65.8 % to +43.3 %) and
    -16.6 on 4:2:0.

40. **Replace, not layer.** Two forms were implemented and measured
    (section 7.5). Predicting the samples directly beats predicting the
    DC-plane residual at every operating point tested, by 2-3 % of rate. The
    reason is specific to what the DC plane is: it is a smooth bilinear
    interpolation of block means, and on piecewise-constant content that
    smoothness is the error a directional predictor exists to remove, not a
    base it wants to correct. The layered form is one `flags` bit and is kept
    in the syntax because the argument reverses on content whose
    low-frequency structure the DC plane captures well; nothing in v1.3 emits
    it by default.

41. **The intra mode is a whole coding unit, not a field on the block unit.**
    The MPM reads the left and above modes. Units are distributed round-robin
    over the rANS lanes and the schedule defines the order of *operations*,
    not the order in which two different lanes reach their `k`-th unit -- so a
    mode attached to a block unit could be predicted from a mode that had not
    been decoded yet. Collecting a plane's modes into one unit makes the
    prediction read only values the *same lane* has already produced, which is
    true under every schedule. It costs one extra unit per plane.

42. **The v2 context model splits the DC plane out, and the two models have
    separate table families.** The DC plane is a dense low-frequency image of
    block means; an AC block is a sparse high-frequency residual. Sharing
    `CBF`, `LAST` and the eight banded `LEVEL` contexts made each pay for the
    other's statistics. v2 gives the DC plane contexts 12-14 and the intra
    mode context 15. Because contexts 0-11 no longer see the DC plane, their
    statistics change, so `kDefaultFreqV2` is a separate k-means run rather
    than the v1 family with four rows appended -- and a stream without bit 21
    keeps the v1 family, byte for byte. Measured at a further **-2.3 points**
    of BD-rate on 4:4:4 on top of directional intra, most of it the mode
    context.

43. **Sign data hiding hides the sign at `LAST`, not at the lowest nonzero.**
    HEVC hides the sign of the first coefficient of a 4x4 group, which its
    decoder can do because it decodes a whole group before reconstructing it.
    This syntax codes each sign immediately after its level, so hiding a sign
    the decoder has not reached yet would need lookahead. `LAST` is known as
    soon as the `LAST` symbol is decoded, is nonzero by construction, and its
    sign can simply be applied at the end of the unit. Measured at **-0.6 %**
    BD-rate: the byte count goes slightly *up* (the parity adjustment tends to
    raise a level) and the PSNR goes up more.

44. **The homography travels in a conditional structure after the frame
    header, not in the header and not in a TLV.** The 40-byte header has every
    byte assigned and one reserved; growing it to 112 costs 72 bytes on every
    frame that cannot use them and moves every offset in section 3.1. The TLV
    area is per *stream*. A conditional `warp_ext()` leaves every Phase 1 frame
    byte-identical to what it was, which is why `v01`-`v44` still hash the same
    after this section was written.

45. **Rows 0 and 1 are Q10.21 and row 2 is Q2.29.** A single Q8.24 format, as
    the draft paper had it, overflows `int32` by about seven bits on the
    translation terms at any streamed width; a single Q10.21 cannot resolve the
    perspective row, whose entries are of order 5e-5 and carry ten significant
    bits below that. The split-by-row format is the only one of the three that
    holds both ends, and it is the format `warp/` already implements.

46. **Motion vectors are absolute.** See section 4.1. The saving from a
    temporal delta is under 2 % of a tile's bits and the cost is that a tile
    header can no longer be parsed without the decoder's per-tile state being
    correct -- in exactly the frames after a loss where the format most needs
    to recover.

47. **The STEREO disparity is a 12-bit field in the tile's optional area, not
    an Exp-Golomb symbol in the payload.** Putting it in the payload would add
    a mode-conditional bypass symbol at the head of a tile's coding-unit list,
    which changes the interleaved lane schedule -- the single most delicate
    part of the format and the only part with a lane-order dependency -- to
    save under a tenth of a percent of a tile. Keeping it in the header also
    preserves the property that a tile header parses without starting the
    entropy decoder.

48. **A picture is one eye, and the transport's tile grid is the eye pair.**
    The two documents were measuring different things and neither was wrong;
    the missing statement was section 3.3's first sentence. Nothing needed
    widening: `skip_bitmap` stays 64 bits because it covers one row of one eye,
    and the transport's 68 columns are 34 per eye.

49. **The inter residual is coded against `warp + DC plane`, so the
    coding-unit list is mode-independent.** The alternative -- dropping the DC
    unit for inter tiles -- changes the unit list, and therefore the lane
    schedule, for a structure that costs one CBF symbol when it is not needed.
    Keeping it also gives the warp a free per-block DC correction, which is the
    one thing a rotation-only predictor most often needs.

50. **Tile corners are derived, never transmitted.** Four corner displacements
    per tile as Q4 `int16` -- the paper's Pass A alternative -- holds neither
    the range nor the resolution: the corner coordinate is Q.6 with a clamp at
    +-8192 pel and needs 20 bits. Widening it to Q6 `int32` would put 32 bytes
    per tile, 74 kB per stereo frame, in the bitstream to save eight divides
    per tile on a path that amortises them over 4096 pixels. The matrix costs
    72 bytes.

51. **`ref_slots` is enforced only on an inter stream.** Annex D D-10 states
    the rule for version 1 as a whole. Every syntax v1.1 to v1.3 encoder writes
    0 there, the field describes a reference ring those streams do not have,
    and making them retroactively malformed would invalidate the whole
    committed vector set to enforce a constraint on a value nothing reads.
    The rule is normative wherever it can mean anything, which is where `INTER`
    is set.

52. **`warp_present` is frame-flags bit 3 and `FILTER_CATMULL_ROM` is tool bit
    23.** Annex D D-1 and D-5 name bit 2 and bit 20 respectively. Both were
    already taken when Annex D was written -- bit 2 by the layered form of
    directional intra (decision 40) and bit 20 by `WM_ID` -- and both of those
    are shipped with conformance vectors behind them. The substance of D-1 and
    D-5 is unchanged; only the bit numbers move, to the first bits that are
    actually free. This is an erratum against Annex D, recorded here because
    this document is where the bit numbers live.

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
