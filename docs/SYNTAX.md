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
| 24 | `INTRA_CFL` | chroma predicted from the co-located reconstructed luma (section 7.7) |
| 25 | `CTX_V3` | the neighbour-conditioned entropy model (section 9.9) |
| 26 | `TAB_V2` | the compact transmitted table set (section 9.4.1) |
| 27 | `XFORM_LARGE` | tiles may set `xform_size != 0`: a 16x16 or 32x32 transform (section 6.7) |
| 28 | `NEAR_SKIP` | tile ROWS may carry near-skip corrections (sections 3.3 and 13.9) |
| 29 | `QUAD_MV` | tiles may set `quad_mv` (section 13.10) |
| 30 | `ENTROPY_LITE` | the table-free, fully parallel entropy tool (section 9.10) |

Bits 17, 21 and 22 are independent: any subset may be set. `ENTROPY_LITE`
(bit 30) is mutually exclusive with `SIGN_HIDE` (bit 22) and `CUSTOM_TABLES`
(bit 6), and constrains `nsub_log2` and `table_set`; see section 9.10.
`SIGN_HIDE` is
mutually exclusive with `LOSSLESS` (bit 5) -- hiding a sign spends one level
step, so the two cannot both be true; a stream setting both is `BITSTREAM`.

`XFORM_4X4_SPLIT` (bit 19) gates tile-header bit 28 and nothing else; it is
independent of every other tool. `INTRA_CFL` (bit 24) is **not** independent:
it adds a tenth mode to the chroma mode alphabet, so it requires `INTRA_DIR`
(the mode unit exists only with it) and `CTX_V2` (only the v2 mode symbol has
room for a tenth value), and it predicts samples, so it requires the replace
form -- frame `flags` bit 2 clear. A stream setting bit 24 without all three
is `BITSTREAM` (`r30`).

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

`CTX_V3` (bit 25) requires `CTX_V2` (bit 21) and `TAB_V2` (bit 26) requires
`CUSTOM_TABLES` (bit 6); either on its own is `BITSTREAM`. Both are otherwise
independent of every other bit -- in particular `CTX_V3` never reads the
transform size, and `TAB_V2` changes no shader work at all.

`XFORM_LARGE` (bit 27) gates the tile header's `xform_size` field and nothing
else. It is independent of every other bit with one exception, which is the
composition rule of 4.1: `split4x4` (bit 19, word1 bit 28) is meaningful only
where `xform_size` selects the 8x8 transform. A stream that never sets bit 27
decodes byte-identically to a syntax v1.4 stream, and a decoder that does not
implement it refuses the handshake rather than mis-decoding a tile.

`NEAR_SKIP` (bit 28) and `QUAD_MV` (bit 29) both require `INTER`, because both
describe tiles on modes only `INTER` allows. `NEAR_SKIP` is the only tool bit
in this version that gates a **tile-row header** structure rather than a
tile-header field: its correction records and the bitmap that names them are
in the row header (3.3), which is why it costs no word1 bit at all.

`ENTROPY_LITE` (bit 30) replaces the entropy layer rather than extending it,
so it is mutually exclusive with `SIGN_HIDE` (22) and `CUSTOM_TABLES` (6):
both are statements about an arithmetic coder it does not have. Its payload
also has no field for the 4x4 split flag and no room for a tenth chroma mode,
so it excludes `XFORM_4X4_SPLIT` (19) and `INTRA_CFL` (24) as well (section
9.10). It is otherwise independent of every other bit -- it changes how
coefficients are written, not which ones there are, so every other transform,
prediction and inter tool composes with it unchanged.

Bits 31-63 are reserved and must be zero. Capability negotiation is an
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
| 3 | u8 | bit 7 `dc_present`, bits 6:0 `tile_count` |
| 4 | u64 | `skip_bitmap` |
| 12 | u64 | `dc_bitmap`, **only when `dc_present`** |
| 20 | 9 x n | one near-skip correction per `dc_bitmap` bit, in column order |

followed by `tile_count` tile structures.

Bit *i* of `skip_bitmap` set means tile *i* of this row **of this eye** is
`WARP_SKIP` and is **not** transmitted. `tile_count` must equal the number of
tiles in the row that are not marked skipped, and bits at or above
`cols_per_eye` must be zero.

**`dc_present` and `dc_bitmap` (tool bit 28 `NEAR_SKIP`).** `dc_present` set
without tool bit 28 is `BITSTREAM`. When it is set, a `dc_bitmap` follows the
skip bitmap and then one nine-byte correction record per set bit, in ascending
column order, before the first tile structure. Constraints, each `BITSTREAM`:

* `dc_present` with an all-zero `dc_bitmap` -- that would be two encodings of
  one stream, and a parser must not accept the redundant one;
* a `dc_bitmap` bit at or above `cols_per_eye`;
* a `dc_bitmap` bit whose `skip_bitmap` bit is **clear**. Every corrected tile
  is a skipped tile: the correction replaces a skipped tile's flat block-mean
  field, and there is nothing for it to correct on a coded tile.

The record's contents and their reconstruction are section 13.9. `NEAR_SKIP`
is the only tool bit in this version that gates a tile-ROW header structure
rather than a tile-header field, which is why it costs no word1 bit and why
its nine bytes are spent only on the tiles the bitmap names. The bitmap therefore covers one row of one eye, so
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
| 28 | `split4x4` | this tile's residual blocks carry a 4x4 split flag (section 6.8). Gated by tool bit 19, and **must be 0 unless bits 29-30 select the 8x8 transform** |
| 29-30 | `xform_size` | transform edge of every plane of this tile: 0 = 8x8, 1 = 16x16, 2 = 32x32; 3 reserved (section 6.7). Gated by tool bit 27 |
| 31 | `quad_mv` | four quadrant vectors follow the tile vector (13.10). Gated by tool bit 29 |

The `split4x4` constraint is normative and is checked by the decoder: a tile
with `xform_size != 0` and `split4x4 == 1` is **`BITSTREAM`**. The two fields
act at different granularities -- one per tile, one per 8x8 block -- and are
kept separate rather than collapsed into a single size ladder, which would
conflate a tile-level choice with a block-level one (Appendix A decision 74).
When `split4x4` is 0 the payload codes no per-block split flag.

Then, in this order:

1. if `mv_present`:
   * `u16 disparity` if `mode == STEREO`
   * `i8 mv_x, i8 mv_y` otherwise
2. if `quad_mv`: four bytes of quadrant vector deltas (13.10)
3. `u8 alpha_value` if `alpha_mode == 1`
4. `payload_len` bytes of rANS payload

The near-skip correction is **not** here: it is in the tile-row header (3.3
and 13.9), because a near-skip tile is a skipped tile and has no tile
structure at all.

`mv_x` and `mv_y` are the tile's motion vector **itself**, in quarter samples
(Q.2), range `[-32, +31.75]` samples. They are **not** a delta from the tile's
stored previous vector. A delta would make parsing a tile header depend on the
decoder's per-tile state being correct -- exactly in the cases after a concealed
frame where the format most needs to recover -- and that destroys the property
the whole transport design rests on, that a tile is an independently decodable
bitstream and a datagram is only a loss unit. The stored vector exists for
`WARP_SKIP` and for concealment, never for parsing (section 13.5).

`split4x4` requires tool bit 19 `XFORM_4X4_SPLIT` and is mutually exclusive
with `tskip`, whose 64 coded values are samples in raster order and have no
sub-block structure; either violation is `BITSTREAM` (`r31`, `r32`).

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
`mode <= 4`; word0 bit 3 and word1 bits 30-31 zero; `xform_size != 3`;
`xform_size != 0` requires the `XFORM_LARGE` tool bit and requires
`tskip == 0`; `chroma444` may only be 1 if
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

`quad_mv` requires the `QUAD_MV` tool bit and `mode == WARP_MV` or
`mode == STATIC_MV`. The near-skip constraints are in 3.3, because they are
about the row header. Sections 13.9 and 13.10 say why each is a constraint
rather than a convention.

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

The 16-point and 32-point transforms of section 6.7 add two constant matrices,
on the **same 512 scale** as the seven constants above:

```
Odd16[n][j] = round(512 * cos(pi * (2n+1) * (2j+1) / 32))   n, j in 0..7
Odd32[n][j] = round(512 * cos(pi * (2n+1) * (2j+1) / 64))   n, j in 0..15
```

`Odd16`, the odd half of the 16-point transform:

```
    510,   490,   452,   396,   325,   241,   149,    50
    490,   325,    50,  -241,  -452,  -510,  -396,  -149
    452,    50,  -396,  -490,  -149,   325,   510,   241
    396,  -241,  -490,    50,   510,   149,  -452,  -325
    325,  -452,  -149,   510,   -50,  -490,   241,   396
    241,  -510,   325,   149,  -490,   396,    50,  -452
    149,  -396,   510,  -452,   241,    50,  -325,   490
     50,  -149,   241,  -325,   396,  -452,   490,  -510
```

`Odd32`, the odd half of the 32-point transform:

```
    511,   506,   497,   482,   463,   439,   411,   379,   344,   305,   263,   219,   172,   124,    75,    25
    506,   463,   379,   263,   124,   -25,  -172,  -305,  -411,  -482,  -511,  -497,  -439,  -344,  -219,   -75
    497,   379,   172,   -75,  -305,  -463,  -511,  -439,  -263,   -25,   219,   411,   506,   482,   344,   124
    482,   263,   -75,  -379,  -511,  -411,  -124,   219,   463,   497,   305,   -25,  -344,  -506,  -439,  -172
    463,   124,  -305,  -511,  -344,    75,   439,   482,   172,  -263,  -506,  -379,    25,   411,   497,   219
    439,   -25,  -463,  -411,    75,   482,   379,  -124,  -497,  -344,   172,   506,   305,  -219,  -511,  -263
    411,  -172,  -511,  -124,   439,   379,  -219,  -506,   -75,   463,   344,  -263,  -497,   -25,   482,   305
    379,  -305,  -439,   219,   482,  -124,  -506,    25,   511,    75,  -497,  -172,   463,   263,  -411,  -344
    344,  -411,  -263,   463,   172,  -497,   -75,   511,   -25,  -506,   124,   482,  -219,  -439,   305,   379
    305,  -482,   -25,   497,  -263,  -344,   463,    75,  -506,   219,   379,  -439,  -124,   511,  -172,  -411
    263,  -511,   219,   305,  -506,   172,   344,  -497,   124,   379,  -482,    75,   411,  -463,    25,   439
    219,  -497,   411,   -25,  -379,   506,  -263,  -172,   482,  -439,    75,   344,  -511,   305,   124,  -463
    172,  -439,   506,  -344,    25,   305,  -497,   463,  -219,  -124,   411,  -511,   379,   -75,  -263,   482
    124,  -344,   482,  -506,   411,  -219,   -25,   263,  -439,   511,  -463,   305,   -75,  -172,   379,  -497
     75,  -219,   344,  -439,   497,  -511,   482,  -411,   305,  -172,    25,   124,  -263,   379,  -463,   506
     25,   -75,   124,  -172,   219,  -263,   305,  -344,   379,  -411,   439,  -463,   482,  -497,   506,  -511
```

The largest absolute row sum is **2613** for `Odd16` and **5215** for `Odd32`;
both bounds appear in the range analysis of 6.3.

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

### 6.2.1 Inverse 1D transform of length 16 and 32 (normative)

A length-`2M` DCT-III is the length-`M` DCT-III of the even-indexed
coefficients plus a dense `M x M` rotation of the odd-indexed ones. Written
with the 512-scaled `Odd16` / `Odd32` of 6.1 the even half needs **no
rescaling at all**, which is the whole reason the constants are on that scale:

```
idct16(x[0..15]) -> y[0..15]:
    e[0..7] = idct8(x[0], x[2], x[4], x[6], x[8], x[10], x[12], x[14])
    for n in 0 .. 7:
        o = sum over j in 0 .. 7 of  x[2*j + 1] * Odd16[n][j]
        y[n]      = e[n] + o
        y[15 - n] = e[n] - o

idct32(x[0..31]) -> y[0..31]:
    e[0..15] = idct16(x[0], x[2], ..., x[30])
    for n in 0 .. 15:
        o = sum over j in 0 .. 15 of  x[2*j + 1] * Odd32[n][j]
        y[n]      = e[n] + o
        y[31 - n] = e[n] - o
```

The forward transform is the exact transpose: butterfly first, then the
shorter forward transform on the sums and the transposed rotation on the
differences.

```
fdct16(y[0..15]) -> x[0..15]:
    for n in 0 .. 7:  u[n] = y[n] + y[15 - n] ;  v[n] = y[n] - y[15 - n]
    x[0], x[2], ..., x[14] = fdct8(u[0..7])
    for j in 0 .. 7:
        x[2*j + 1] = sum over n in 0 .. 7 of  v[n] * Odd16[n][j]
```

and `fdct32` likewise from `fdct16` and `Odd32`.

**Gain.** Each doubling of the length multiplies the gain by `sqrt(2)`:

| length | gain per dimension | 2D gain | shifts must sum to |
|---|---|---|---|
| 8 | `2^10` | `2^20` | 20 |
| 16 | `2^10 * sqrt(2)` | `2^21` | 21 |
| 32 | `2^11` | `2^22` | 22 |

The 16-point gain is irrational and the **two-dimensional** gain is not: the
two passes multiply, so every size is exactly unit gain after its shift chain.
A coefficient of `n * 128` at position 0 of an `n x n` block reconstructs a
flat 128 at every `n`.

Neither of the two constant matrices is factorised. The multiply counts per
`n x n` inverse transform are

| n | multiplies per 1D transform | per sample of the block |
|---|---|---|
| 8 | 11 | 2.75 |
| 16 | 11 + 64 = 75 | 9.4 |
| 32 | 75 + 256 = 331 | 20.7 |

which is the price the decoder pays for the tool and is priced against its
rate saving in `ref/RESULTS-xform-a.md`. A further factorisation of `Odd16` /
`Odd32` is possible and would change no bit: the *result* is normative, the
spelling is not, exactly as for `mulC4` in 6.3.

### 6.3 Inverse 2D transform (normative)

`src[n*n]` are the dequantized coefficients in raster order inside the block,
index `u * n + v` with `u` the vertical and `v` the horizontal frequency, and
`n` the block edge of 6.7 (8, 16 or 32).

```
pass 1 (rows):    for each row r: idctN(src[r*n .. r*n+n-1]) -> out[0..n-1]
                  tmp[c*n + r] = clamp16((out[c] + (1 << (s1-1))) >> s1)
pass 2 (columns): for each row r of tmp: idctN(tmp[r*n ..]) -> out[0..n-1]
                  dst[c*n + r] = clamp16((out[c] + (1 << (s2-1))) >> s2)
```

Both passes write transposed, so `dst` comes out in spatial raster order
`y * n + x`. The two shifts sum to log2 of the size's 2D gain (6.2.1), so
every size is unit gain: a coefficient of `n * 128` at position 0 reconstructs
a flat 128.

**Shift chain and intermediate ranges** (copy these exactly; the GPU passes
must match bit for bit). The worst cases are the largest absolute row sum of
the exact 1D transform times the largest legal input, so each is attainable:

| stage | 8x8 | 16x16 | 32x32 |
|---|---|---|---|
| inverse pass 1 shift `s1` | `>> 7`, `+64` | `>> 7`, `+64` | `>> 8`, `+128` |
| inverse pass 2 shift `s2` | `>> 13`, `+4096` | `>> 14`, `+8192` | `>> 14`, `+8192` |
| forward pass 1 | `>> 6`, `+32` | `>> 7`, `+64` | `>> 8`, `+128` |
| forward pass 2 | `>> 14`, `+8192` | `>> 14`, `+8192` | `>> 14`, `+8192` |
| worst before either inverse pass | `8.9e7` | `1.7e8` | `3.5e8` |
| worst before forward pass 1 | `1.6e6` | `3.3e6` | `6.6e6` |
| worst before forward pass 2 | `1.1e8` | `2.1e8` | `4.2e8` |

Every one of those is inside `int32` (`2.1e9`) with at least six times the
margin. Both passes of both directions clamp their output to int16, so a GPU
may hold the transpose buffer in `int16` LDS at every size — the first-pass
shift grows by one per size for exactly that reason: the value entering it
doubles per size (one more butterfly level), so all three sizes leave the same
margin, and the forward first pass lands on `25 650` from a full-amplitude
residual at every size.

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

The 8-point flow graph has gain exactly `2^10` per dimension, so its two
inverse shifts must sum to 20 for unit gain; 7 + 13 and 8 + 12 both do. The reference
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

The encoder uses the exact transpose of the flow graph, with the shifts of the
table in 6.3 (the first pass clamped to int16), giving coefficients on the
orthonormal DCT-II scale at every size. The forward transform is not
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
  chroma matrix for planes 1 and 2. **Only the 8x8 matrix is ever transmitted
  or built in.** A block of edge `n > 8` reads it replicated: the weight of
  coefficient `(u, v)` of an `n x n` block is

  ```
  k    = log2(n) - 3                      // 0, 1 or 2
  w[i] = matrix[(u >> k) * 8 + (v >> k)]  // i = u * n + v
  ```

  so the roll-off covers the same fraction of the frequency plane at every
  transform size, `w` keeps its range `[1, 32]`, and no extra matrix, no extra
  syntax and no extra derivation rule exists. The custom matrices of
  `quant_matrix == 255` are 128 bytes at every size for the same reason. The tile's matrix is the frame's when
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

`tskip == 1` requires `xform_size == 0` (section 4.1). The coded values of a
transform-skip block are residual samples in raster order, which is a
statement about an 8x8 neighbourhood; a 1024-sample raster unit is a different
tool and would need its own measurement, so the syntax refuses the
combination rather than defining it untested.

**Lossless mode** is `tskip = 1` with a resolved QP of 0 on every plane, and
`res_level = 0`. `chroma444 = 1` (or a 4:2:0 source) is required for the picture
itself to be lossless; a 4:2:0 tile is lossless only with respect to its own
subsampled chroma.

### 6.7 Transform size (tool bit 27)

The tile header's `xform_size` names one transform edge for the whole tile.
The edge a **plane** actually uses is that edge capped by the plane's own
coded extent:

```
xform_edge  = 8 << xform_size                       // 8, 16, 32
bsize(p)    = min(xform_edge, coded_extent(p))      // 4.2
nb(p)       = coded_extent(p) / bsize(p)            // blocks per edge
```

The cap is what makes every combination legal without a constraint: a
`res_level` 2 tile (16x16 luma) with `xform_size == 2` codes one 16x16 luma
block, and the 4:2:0 chroma plane of a 64x64 tile with a 32x32 luma transform
codes one 32x32 chroma block. Both `coded_extent` and `xform_edge` are powers
of two and `coded_extent >= 8`, so `bsize` is always 8, 16 or 32 and `nb` is
always a power of two in `[1, 8]`.

Each cell is `nb x nb` blocks of `bsize x bsize`:

| tile | `xform_size` | luma | 4:2:0 chroma | 4:4:4 chroma |
|---|---|---|---|---|
| `res_level` 0 | 0 | 8x8 of 8 | 4x4 of 8 | 8x8 of 8 |
| `res_level` 0 | 1 | 4x4 of 16 | 2x2 of 16 | 4x4 of 16 |
| `res_level` 0 | 2 | 2x2 of 32 | 1x1 of 32 | 2x2 of 32 |
| `res_level` 1 | 2 | 1x1 of 32 | 1x1 of 16 | 1x1 of 32 |
| `res_level` 2 | 2 | 1x1 of 16 | 1x1 of 8 | 1x1 of 16 |

Everything downstream follows `bsize` and `nb` by the rules already stated:
the DC plane holds `nb * nb` values (7.1) and carries its second-level 8x8
transform only when `nb == 8`, which now happens exactly when `bsize == 8` on
a full-resolution plane; the planar interpolation reads block centres at
`bsize` spacing (7.2); the directional predictors are the same formulas with
the block edge left as `n` (7.4); the weighting matrix is replicated (6.5);
the scan is the zigzag of the block (9.2); and the LAST classes and the LEVEL
bands are scaled by the unit's size (9.3).

**Why per tile and not per 32x32 quadrant.** The DC plane is a per-plane
structure whose resolution *is* the transform grid: `nb * nb` block means for
the whole plane, interpolated across the whole plane. A per-quadrant size
would make that grid non-uniform, and the DC plane would have to be split into
four independently-interpolated pieces with a seam between them — which is the
one thing the DC-plane predictor exists to avoid. The tile is the smallest
unit at which the transform size can change without changing what the DC plane
*is*, and the tile is also the unit of the GPU's workgroup, so a uniform size
per tile is what keeps 7.6's schedule uniform.

**Interaction with the tools already defined.**

| tool | at 16x16 and 32x32 |
|---|---|
| `INTRA_DC_PLANE` (0) | unchanged; the DC plane is `nb * nb`, and at `nb < 8` it is coded flat exactly as a `res_level` tile's already is |
| `TRANSFORM_SKIP` (1) | mutually exclusive (6.6) |
| `RES_LEVEL` (2) | independent; the plane cap above resolves every combination |
| `LOSSLESS` (5) | requires `tskip`, so `xform_size == 0` |
| `WM_ID` (20) | unchanged; the built-in matrix is replicated like any other (6.5) |
| `INTRA_DIR` (17) | all nine modes are defined at every size (7.4); the mode unit holds `nb * nb` modes |
| `CTX_V2` (21), `SIGN_HIDE` (22) | unchanged; no new context and no new symbol (9.3) |
| `INTER` (10), `WARP` (11), `STEREO` (12) | independent: an inter tile codes the residual against its predictor with the same block structure |

### 6.7.1 What a 32x32 inverse transform costs a GPU decoder (note for Pass B)

The constraint is `docs/PAPER.md` design principle 2 and `docs/03-vulkan.md`
3.2.3: **one workgroup of 256 threads per 64x64 tile, no cross-tile state.**
Nothing here changes that; a larger transform changes how the 256 threads are
divided and what they hold, not the dispatch.

**The thread mapping.** At 8x8, Pass B gives each 8x8 block 4 threads and each
thread 2 rows. At 32x32 the natural unit is **one thread per 1D transform**:
the odd half is a dense 16 x 16 product over all sixteen odd coefficients, so
splitting one 1D transform across threads means either duplicating the
16-point even half or an unbalanced 3.4-to-1 split. One row per thread is
balanced, needs no cross-lane exchange inside a transform, and reads its 32
coefficients as one coalesced 64-byte load.

A `res_level` 0 4:2:0 tile with `xform_size == 2` therefore has

| plane | blocks | 1D transforms per pass | threads |
|---|---|---|---|
| Y (64x64) | 4 of 32x32 | 128 | 128 |
| Co (32x32) | 1 of 32x32 | 32 | 32 |
| Cg (32x32) | 1 of 32x32 | 32 | 32 |
| **total** | 6 | **192** | **192 of 256 = 75 %** |

and all three planes go through both passes together. A 4:4:4 tile has twelve
32x32 blocks, 384 transforms per pass, so it runs the same schedule in two
rounds (luma, then the two chroma planes).

**LDS.** The transpose buffer is a whole *plane*, not a block, so it is the
same size it is at 8x8: `64 x 64 x 2 B = 8192 B` for luma, `32 x 32 x 2 B =
2048 B` for a 4:2:0 chroma plane. The `clamp16` after pass 1 (6.3) is what
keeps it `int16` at every size.

| tile | LDS for the transpose | barriers for the inverse transform |
|---|---|---|
| 4:2:0, all three planes at once | `8192 + 2*2048 = 12 288 B` | **2** |
| 4:4:4, two rounds | `8192 B` (reused) or `24 576 B` in one round | 4, or 2 |
| 4:2:0 at `xform_size == 0`, for comparison | `12 288 B` | 6 (three rounds of 32 blocks) |

**Dependent steps.** Two per round: pass 1 writes the transpose buffer, one
barrier, pass 2 reads it. The 8x8 schedule needs the same two per round but
more rounds, because 256 threads at 4 per block cover 64 blocks and a
`res_level` 0 4:2:0 tile has 96 of them. **Large transforms reduce the number
of barriers per tile.**

**Registers.** A thread holding a whole 32-point transform has the sixteen odd
coefficients live while it accumulates sixteen outputs, plus the sixteen
results of the even half: about **48 int32** at the peak, against 8 + 8 for the
8x8 form. On hardware with a 64-VGPR full-occupancy budget that is the binding
constraint rather than LDS, and the fallback is to stage the coefficient
vector through a second LDS buffer.

**Arithmetic.** This is where the tool is expensive, and 6.2.1 states it: 2.75
multiplies per sample at 8x8, 9.4 at 16x16, **20.7 at 32x32** — 7.5x the
multiply count of the 8x8 form for a `res_level` 0 tile. Adds and shifts grow
with it. Against that, the tool removes barriers, removes rounds, and (7.6)
cuts the directional-intra wavefront from 22 steps to 4. Whether the trade is
worth taking on a given part is a Pass B measurement that does not exist yet,
which is why `xform_size` is per tile and behind a tool bit: a decoder that
does not want it refuses the bit, and an encoder that knows the decoder is
arithmetic-bound sends `xform_size == 0`.

**What it costs the rANS lane schedule, stated plainly.** A large block is
ONE coding unit of 256 or 1024 coefficients, not `(n/8)^2` units of 64, and
that is a real cost this section is not silent about. Units are distributed
round-robin over `2^nsub_log2` lanes (9.1), so at `res_level` 0 and 32x32 a
4:2:0 tile has **nine units over eight lanes**, one of which is a four-value
DC plane: one lane serialises 2048 coefficients while another does four. The
alternative -- partitioning a large block into 64-coefficient coding groups --
keeps the unit count and the lane balance identical to version 1 at every
transform size, and it needs no Pass A change at all, where this form needs
the two larger zigzags and `last_shift` in the shader's `LAST` class and LEVEL
band derivation.

That alternative was **not** taken, because the DC-plane re-grid and the
`n x n` predictors above are what win the rate and they are orthogonal to the
grouping question. Grafting the grouping onto this design is a measured
follow-up, not a taste one: it is worth taking if it costs less than about
3 BD-rate points of this package's measured win, and this note exists so that
the cost is on the record either way.

The reference Vulkan decoder does **not** implement tool bit 27 today
(`vk/decoder/nxvc_vkdec_parse.cpp`), so it refuses such a stream at the
handshake rather than mis-decoding it — the same forward-compatibility gate
every other unimplemented tool goes through.

The split flag of 6.8 is present and meaningful **only when this field selects
the 8x8 transform**. A tile that sets `xform_size != 0` and `split4x4` is
**malformed**: `BITSTREAM`. The two fields act at different granularities --
`xform_size` is per tile and chooses the transform, `split4x4` is per block
and subdivides an 8x8 one -- and they are deliberately kept separate rather
than collapsed into a single size ladder, which would conflate a tile-level
choice with a block-level one. This one constraint is the whole price of that
(Appendix A decision 74).

### 6.8 The 4x4 transform split (tool bit 19)

When tile-header bit 28 `split4x4` is set, each residual block whose `CBF` is
1 carries a one-bit `split` flag (section 9.8). With `split == 1` the block's
64 coded values are **four 4x4 sub-blocks** rather than one 8x8 transform.
The prediction is unchanged: it is still one 8x8 block from one intra mode, so
the split adds no dependency between blocks and no step to the section 7.6
wavefront.

**Layout.** Sub-block `sb = 2*sy + sx` (`sx`, `sy` in 0..1) covers samples
`x in [4*sx, 4*sx+3]`, `y in [4*sy, 4*sy+3]` of the block. Its 16
coefficients occupy the same quadrant of the block-local 8x8 coefficient
array: coefficient `(u, v)` of sub-block `(sx, sy)`, `u` vertical and `v`
horizontal, is at block-local index `(4*sy + u) * 8 + 4*sx + v`. The
coefficient array is therefore the same 64 values an unsplit block has,
permuted, and `CBF`, `LAST`, the level chain and the lane schedule are
untouched. The scan is section 9.2's `split` scan.

**Dequantization** is section 6.5 unchanged, with the weight

```
w4[u][v] = w[2*u][2*v]        // the tile's 8x8 matrix, subsampled by two
```

so no second weighting-matrix family and no second quantizer step table
exist. This is exact because the 4x4 transform below is built to the *same*
gain as the 8x8 one.

**Constants.** The 1D inverse matrix is
`M[n][k] = round(1024 * c_k * cos(pi*(2n+1)*k/8))` with `c_0 = 1/2` and
`c_k = 1/sqrt(2)` otherwise, which takes three distinct magnitudes:

| name | value | equals |
|---|---|---|
| `D0` | 512 | `1024 * 1/2` |
| `D1` | 669 | `round(1024 * cos(pi/8)  / sqrt(2))` |
| `D2` | 277 | `round(1024 * cos(3pi/8) / sqrt(2))` |

The four rows are

```
row0 = { D0,  D1,  D0,  D2}      row2 = { D0, -D2, -D0,  D1}
row1 = { D0,  D2, -D0, -D1}      row3 = { D0, -D1,  D0, -D2}
```

and their Gram matrix is exact where it can be. Every row has norm
`2*D0^2 + D1^2 + D2^2 = 1048578`, two above `2^20`. Four of the six row pairs
are **exactly** orthogonal, because their cross terms cancel identically
(`row0.row1 = D0^2 + D1*D2 - D0^2 - D2*D1 = 0`); the two pairs that oppose
`2*D0^2` against `D1^2 + D2^2` -- `row0.row3` and `row1.row2` -- come to
**-2**, the same two the norm is off by, since `2*512^2 = 1048576` and
`669^2 + 277^2 = 1048578`.

So the transform is orthogonal to two parts in `2^20` and its gain is `2^10`
per dimension to the same accuracy -- the gain the 8x8 flow graph has exactly.
That is the whole reason for choosing these constants over the natural
`512*sqrt(2)` scaling: it is what lets 6.5's dequantizer, 6.3's shift chain
and the tile's own weighting matrix serve both sizes with no second family of
anything (Appendix A decision 55). The residual error is four orders of
magnitude below one quantiser step at every legal QP.

**Inverse 1D transform (normative).** Input `x[0..3]` int32, output `y[0..3]`.

```
e0 = (x0 + x2) * D0
e1 = (x0 - x2) * D0
o0 =  x1 * D1 + x3 * D2
o1 =  x1 * D2 - x3 * D1
y0 = e0 + o0 ;  y3 = e0 - o0
y1 = e1 + o1 ;  y2 = e1 - o1
```

**Inverse 2D transform (normative).** `src[16]` are the dequantized
coefficients in raster order inside the sub-block, index `u * 4 + v`.

```
pass 1 (rows):    for each row r: idct4_1d(src[r*4 .. r*4+3]) -> out[0..3]
                  tmp[c*4 + r] = clamp16((out[c] + 64) >> 7)
pass 2 (columns): for each row r of tmp: idct4_1d(tmp[r*4 ..]) -> out[0..3]
                  dst[c*4 + r] = clamp16((out[c] + 4096) >> 13)
```

The transform is orthonormal, so coefficient-domain squared error is
sample-domain squared error and 6.5's step means the same thing at both sizes.
Note that "unit gain" is in that sense, not in absolute DC: a flat 4x4 block of
value `v` has DC `4v`, so a DC coefficient of 1024 reconstructs a flat **256**
where the 8x8's reconstructs a flat 128. The block has a quarter of the
samples.

**This is the same shift chain, the same rounding and the same `clamp16` as
the 8x8 inverse of 6.3**, which is the whole reason the constants were scaled
to gain `2^10`: a decoder implements one dequantizer, one clamp discipline and
one transpose convention for both sizes.

**Ranges.** Every product is a constant times a value already clamped to
int16, so unlike the 8x8's odd-part rotation there is no operand that needs an
exact wide product -- `mulC4` has no 4x4 counterpart.

| stage | shift | rounding | clamp | worst-case magnitude before the shift |
|---|---|---|---|---|
| inverse pass 1 | `>> 7` | `+64` | int16 | `32768 * (2*D0 + D1 + D2) = 6.5e7` |
| inverse pass 2 | `>> 13` | `+4096` | int16 | `6.5e7` |
| forward pass 1 | `>> 6` | `+32` | int16 | `511 * 1970 = 1.0e6` |
| forward pass 2 | `>> 14` | `+8192` | int16 | `32767 * 1970 = 6.5e7` |

All four are comfortably inside int32.

**Forward 2D transform (informative)**, as for 6.4: the exact transpose of the
flow graph, `>> 6` after the first pass clamped to int16 and `>> 14` after the
second.

**What it costs a GPU decoder.** Nothing structural. The split changes neither
the prediction nor its wavefront (7.6) and adds no coding unit, no barrier and
no LDS: the four sub-blocks of a block are independent of each other and of
every other block, and 8 four-point transforms are *less* arithmetic than 4
eight-point ones. The only new work in Pass A is one bypass bit per coded
block; the only new work in Pass B is a branch on that bit.
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

For a plane with `nb x nb` blocks (`nb` = 8, 4, 2 or 1; section 6.7), the first
coding unit of that plane holds `nb * nb` values.

**Decoder (normative):**

```
tdc  = (qstep[qp >> 1] * 16 + 8) >> 4                 // == qstep[qp >> 1]
for i in 0 .. nb*nb-1:  dc[i] = clamp16((coef[i] * tdc + 8) >> 4)

if nb == 8:  dc = idct8x8(dc)                          // second-level transform
// nb < 8: no second-level transform, dc holds the values directly

for i:  M[i] = clamp(dc_offset + dc[i], 0, maxval)
```

`M` is the `nb x nb` array of reconstructed block means. The second-level 8x8
DCT is applied only when `nb == 8`, which is a full-resolution plane with
`bsize == 8`. Smaller DC planes are coded flat: the transform would buy
nothing over 16, 4 or 1 value. A 16x16-transform tile therefore has a 4x4 flat
DC plane and a 32x32-transform tile a 2x2 one, by the rule that was already
there for `res_level` tiles rather than by a new one.

**Encoder (informative):** the block mean is
`(sum of the bsize^2 samples + (1 << (2*log2(bsize) - 1))) >> (2*log2(bsize))`,
rounded away from zero — at `bsize == 8` that is `(sum + 32) >> 6`;
the array of `mean - dc_offset` is transformed (when `nb == 8`), quantized with
step `tdc` and dead zone `tdc/3`, and the encoder then runs the decoder
reconstruction above so its prediction is exactly the decoder's.

### 7.2 Planar prediction

Block `(bx, by)`'s mean sits at the block centre, sample
`(bx*bs + (bs-1)/2, by*bs + (bs-1)/2)` with `bs = bsize(p)` (section 6.7). The
prediction at sample `(x, y)` is the bilinear interpolation of `M` at that
grid, with the source coordinate in Q4:

```
ux = (16 * x - 8 * (bs - 1) + (bs >> 1)) >> log2(bs)
uy = (16 * y - 8 * (bs - 1) + (bs >> 1)) >> log2(bs)
pred[y][x] = bilinear(M, nb, nb, ux, uy)      // section 8
```

The shift is arithmetic and the `+ (bs >> 1)` is the rounding term of clause
3.3; at `bs == 8` the numerator is an exact multiple of 8 and the expression
reduces to `2 * x - 7` for every `x`, which is the v1 formula unchanged. At
`bs` 16 and 32 the exact coordinate has a half-Q4 fraction (the block centre
falls between two Q4 positions) and the rounding term settles it the same way
in both directions.

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
before it. Everything below is written for a block of edge `n = bsize(p)`
(section 6.7); at `n == 8` it is the v1.3 derivation character for character.

| mode | name | prediction |
|---|---|---|
| 0 | `DC_PLANE` | `pred[y][x]` from 7.2 -- the v1 predictor |
| 1 | `DC` | mean of the `n` top and `n` left neighbours, below |
| 2 | `PLANAR` | HEVC-style, below |
| 3 | `H` | `L[j]` |
| 4 | `V` | `A[i]` |
| 5 | `DDL` | diagonal down-left, 45 deg |
| 6 | `DDR` | diagonal down-right, 45 deg |
| 7 | `VR` | vertical-right, 26.6 deg |
| 8 | `HD` | horizontal-down, 63.4 deg |

**Reference samples (normative).** For the block at `(bx, by)` with origin
`(x0, y0) = (n*bx, n*by)` and `b = log2(n)`, define

```
at(x, y):
    cx = clamp(x, 0, size-1);  cy = clamp(y, 0, size-1)
    if (cy>>b) < by  or  ((cy>>b) == by and (cx>>b) < bx):
        return recon[cy][cx]        // a block already reconstructed
    else:
        return base[cy][cx]         // section 7.5

TL   = at(x0-1, y0-1)
A[k] = at(x0+k, y0-1)   for k = 0..2n-1    A[-1] = TL
L[k] = at(x0-1, y0+k)   for k = 0..2n-1    L[-1] = TL
```

The reference arrays are `2n` long at every size, so `DDL` reaches `A[2n-1]`
and the above-right dependency of 7.6 is one block wide at every size.

Coordinates are clamped **into the tile**, and the fallback for anything not
yet reconstructed is `base`, which is derived from this tile's own DC plane.
A tile therefore never reads a neighbouring tile: **tiles stay independent**,
which is what the transport's per-tile loss recovery and the rate controller's
per-tile ladder both depend on. The top and left borders of a tile read the
DC-plane prediction, not a neighbour.

**Predictors.** `i` is the column and `j` the row inside the block, 0..7.

```
DC:     P[j][i] = (sum(A[0..n-1]) + sum(L[0..n-1]) + n) >> (b + 1)

PLANAR: P[j][i] = ((n-1-i)*L[j] + (i+1)*A[n]
                   + (n-1-j)*A[i] + (j+1)*L[n] + n) >> (b + 1)

DDL:    k = i + j
        P[j][i] = k == 2n-2 ? (A[2n-2] + 3*A[2n-1] + 2) >> 2
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

`i` and `j` run over `0..n-1`. `DDR`, `VR` and `HD` are written above without
an `n` in them because there is none: their index arithmetic depends only on
`i` and `j`, so the same three formulas serve every size.

Every mode but 0 is a weighted average of references that are already in
`[0, maxval]`, so no clamp is needed and none is applied. Every index used is
within `A[-1..2n-1]` / `L[-1..2n-1]`: `DDR` reaches `A[-1]` at `k == 1`, `VR`
reaches `L[-1]` at `q == 2`, and `DDL` reaches `A[2n-1]`.

**Which predictor applies at each size.** All nine, at all three sizes. Mode 0
is the DC-plane prediction at every size, so `INTRA_DIR` stays a strict
superset of the DC-plane predictor for every `xform_size`, and the mode
alphabet, the MPM derivation (9.6) and the mode context are the same nine
symbols however large the block is.

Restricting the set at the larger sizes was measured and is the wrong way
round. The modes chosen on `vr-mixed-1024` 4:4:4 at QP 16:

| block | `DC_PLANE` | `DC` | `PLANAR` | `H` | `V` | `DDL` | `DDR` | `VR` | `HD` |
|---|---|---|---|---|---|---|---|---|---|
| 8x8 | 76.1 % | 7.6 % | 2.9 % | 5.8 % | 5.0 % | 0.8 % | 0.6 % | 0.6 % | 0.7 % |
| 16x16 | 42.8 % | 19.6 % | 7.2 % | 15.7 % | 10.3 % | 1.3 % | 0.9 % | 0.8 % | 1.3 % |
| 32x32 | 54.7 % | 15.2 % | 5.7 % | 10.8 % | 8.7 % | 1.8 % | 0.8 % | 0.7 % | 1.6 % |

The directional predictors are used **more**, not less, as the block grows:
the DC plane at a 32x32 grid is four block means for a whole tile, so it has
almost no spatial detail left to give, and the neighbour-based predictors take
over. (`ref/RESULTS-xform-a.md` section 4.)

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

**`xform_size` moves this on its own.** With `nb` blocks per plane edge the
wavefront is `3*nb - 2` steps, and the fixed thread assignment gives each block
`256 / nb^2` threads:

| `xform_size` | `nb` (luma, `res_level` 0) | steps | threads per block | occupancy |
|---|---|---|---|---|
| 0 (8x8) | 8 | 22 | 4 | 4.5 % |
| 1 (16x16) | 4 | **10** | 16 | **10.0 %** |
| 2 (32x32) | 2 | **4** | 64 | **25.0 %** |

Those are the same numbers the restriction table below prices — `xform_size 1`
lands exactly on restriction **B**, and `xform_size 2` beats **B + C** — except
that the restrictions cost 1.6 to 1.8 % of rate to buy them and `xform_size`
*saves* rate on the content where it is chosen (`ref/RESULTS-xform-a.md`).
Barriers per 4:4:4 tile fall from 3 + 66 to 3 + 30 at 16x16 and 3 + 12 at
32x32.

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

### 7.7 Chroma from luma (tool bit 24)

With `INTRA_CFL`, the **chroma** planes of an `INTRA` tile gain a tenth intra
mode:

| mode | name | prediction |
|---|---|---|
| 9 | `CFL` | a linear function of the co-located reconstructed luma |

The luma plane keeps its nine-mode alphabet; only chroma mode units use ten
(section 9.6). Mode 9 in a luma mode unit is `BITSTREAM`.

**What it reads.** The tile's own **reconstructed luma plane**, complete. The
coding-unit order of 9.1 already decodes plane 0 before planes 1 and 2, so
this is a dependency on a plane the decoder has finished, never on a
neighbouring tile: **tiles stay independent.**

**Co-location.** Let `S` be the luma plane's coded edge and `Sc` the chroma
plane's, and `f = S / Sc`, which is 1 for a 4:4:4 tile and 2 for a 4:2:0 one
at every `res_level` (section 4.2). No other ratio occurs, and a stream in
which one would is `BITSTREAM`. The luma value co-located with chroma sample
`(cx, cy)` is

```
Lc(cx, cy):
   f == 1:  Y[clamp(cy, 0, S-1)][clamp(cx, 0, S-1)]

   f == 2:  x0 = clamp(2*cx,   0, S-1) ; x1 = clamp(2*cx+1, 0, S-1)
            y0 = clamp(2*cy,   0, S-1) ; y1 = clamp(2*cy+1, 0, S-1)
            (Y[y0][x0] + Y[y0][x1] + Y[y1][x0] + Y[y1][x1] + 2) >> 2
```

where `Y` is the reconstructed luma plane. The `f == 2` kernel is the rounded
2x2 average of 5.2, the same one chroma was subsampled with, so the two planes
are aligned by construction. Coordinates clamp into the tile, so `cy == -1`
and `cx == -1` are defined.

**Model derivation (normative).** For the chroma block at `(bx, by)` with
origin `(x0, y0) = (8*bx, 8*by)`, take the 16 neighbour pairs

```
for k in 0 .. 7:
    Cn[k]     = A[k]                      Ln[k]     = Lc(x0 + k, y0 - 1)
    Cn[8 + k] = L[k]                      Ln[8 + k] = Lc(x0 - 1, y0 + k)
```

with `A` and `L` exactly the reference arrays of 7.4 -- so the chroma side of
a pair falls back to `base` wherever the neighbour is not yet reconstructed,
and the tile stays independent.

Select four indices. `lo0` is the index of the smallest `Ln`, `lo1` the index
of the smallest `Ln` among the remaining fifteen, and `hi0`, `hi1` the same
for the largest; **every tie takes the lowest index**, which makes the
selection deterministic. Then

```
base_l = (Ln[lo0] + Ln[lo1] + 1) >> 1        base_c = (Cn[lo0] + Cn[lo1] + 1) >> 1
top_l  = (Ln[hi0] + Ln[hi1] + 1) >> 1        top_c  = (Cn[hi0] + Cn[hi1] + 1) >> 1
dl     = top_l - base_l                       // 0 .. 255

dl == 0:  alpha = 0
dl >  0:  alpha = clamp((((top_c - base_c) * kCflRecip[dl]) + 64) >> 7,
                        -2048, 2047)
```

`dl` is never negative -- each of the two largest neighbours is at least each
of the two smallest -- so `kCflRecip` is only ever indexed in `[1, 255]`.

`kCflRecip[d] = round(2^15 / d)` for `d` in 1..255 is a **256-entry u16
reciprocal table** -- the third and last place this format divides, and like
the other two it is a table lookup in the decoding process rather than a
division opcode (spec/03-conventions.md 3.4). `kCflRecip[0]` is never read.
**`round` is unambiguous here: `2^15 / d` is never exactly half-integral for
any `d` in `[1, 255]`**, so the table is fully determined and no rounding mode
has to be named. (`2^15 / d = k + 1/2` would need `2^16 = d(2k + 1)` with
`2k + 1` odd and greater than 1, which cannot divide a power of two.) A
decoder that rounds halves up, down, or to even therefore builds the same 256
entries.
The product is `(top_c - base_c) / dl` in Q15 and the `>> 7` brings it to Q8,
so `alpha` is the fitted slope in chroma units per luma unit, Q8, clamped to
`+-8.0`.

Both ends are the **mean of two** pairs rather than a single extremum, so one
noisy neighbour cannot set the slope. `|top_c - base_c| <= 511` and
`kCflRecip[dl] <= 32768`, so the product is at most `1.7e7` and the whole
derivation is int32.

**Prediction.**

```
P[j][i] = clamp(base_c + ((alpha * (Lc(x0 + i, y0 + j) - base_l) + 128) >> 8),
                0, maxval)
```

with `maxval` the **chroma** plane's (section 4.3). `|alpha| <= 2048` and
`|Lc - base_l| <= 255`, so the product is at most `5.2e5`. Unlike the modes of
7.4 this one is clamped, because a fitted slope can leave the sample domain.

Everything else -- 7.3 reconstruction, the mode unit, `res_level`, the
weighting matrix -- is unchanged.

**What it costs a GPU decoder.** One **extra dependent step per tile**: the
chroma planes may no longer be predicted concurrently with luma, so a tile
that ran luma and chroma in parallel now needs one barrier between them. It is
one barrier, not a wavefront: within the chroma plane the 7.4 schedule is
exactly as it was. No extra LDS beyond the reconstructed luma plane, which a
Pass B tile shader already holds (64x64 int16 = 8 KB). Per block the fit is a
16-element min/max reduction over two values each -- about 60 ops, once per
block, against the 64 samples it then predicts.

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
        unit: block (bx, by) of p    (bsize*bsize coefficients)
```

The **4x4 split flag is deliberately *not* in the mode unit** but inside each
block's own coefficient unit (9.8), for the same reason the modes are in one
unit: a unit's syntax may depend only on values its own lane has already
produced, and a plane's mode unit and its block units generally fall in
different lanes. Putting the flag in the mode unit would make a block's scan
order depend on another lane's output. In the block unit it is also free
wherever `CBF` is 0, which at the Phase 1 operating point is most blocks.

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
| block of edge `n`, `tskip == 0`, `split == 0` | `n x n` zigzag |
| 64-coefficient block, `split == 1` | the split scan, below. `split` exists only at `n == 8` (4.1) |
| 64-coefficient block, `tskip == 1` | raster (`scan[i] = i`) |
| DC plane, 64 values | 8x8 zigzag |
| DC plane, 16 values | 4x4 zigzag |
| DC plane, 4 values | `0, 1, 2, 3` |
| DC plane, 1 value | `0` |

**The zigzag of an `n x n` block**, for every `n` the format uses (2, 4, 8, 16,
32), is one rule:

```
p = 0
for s in 0 .. 2*(n-1):                     // the anti-diagonal
    ulo = max(s - n + 1, 0);  uhi = min(s, n - 1)
    if s is even:  for u = uhi down to ulo:  scan[p++] = u * n + (s - u)
    else:          for u = ulo up to uhi:    scan[p++] = u * n + (s - u)
```

with `u` the vertical and `s - u` the horizontal index. `tskip` replaces it
with the raster order, and `tskip` implies `n == 8` (6.6).

The 8x8 and 4x4 tables below are that rule's output, written out because the
conformance vectors pin them:

```
 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
```

4x4 zigzag: `0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15`.

**The split scan** (tool bit 19) is the four 4x4 sub-blocks of the block in
raster order, each in 4x4 zigzag, concatenated. It is generated by

```
scan[p] = (4*sy + z/4) * 8 + 4*sx + z%4
   with  sb = p >> 4 ;  sx = sb & 1 ;  sy = sb >> 1 ;  z = zigzag4[p & 15]
```

and is, in full:

```
 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 25, 18, 11, 19, 26, 27,
 4,  5, 12, 20, 13,  6,  7, 14, 21, 28, 29, 22, 15, 23, 30, 31,
32, 33, 40, 48, 41, 34, 35, 42, 49, 56, 57, 50, 43, 51, 58, 59,
36, 37, 44, 52, 45, 38, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
```

Concatenating the sub-blocks rather than interleaving them is what lets
`LAST` still truncate a tail: a block whose energy is in sub-block 0 -- the
common case, since the split is chosen exactly where the residual is local --
codes a `LAST` below 16 and pays nothing for the other three.

### 9.3 Contexts and alphabets

There are **12 contexts of 16 symbols** each, **16** when the stream sets tool
bit 21 `CTX_V2`, and **27** when it also sets bit 25 `CTX_V3` (section 9.9).
Every context's 16 frequencies are 10-bit, at least 1,
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

For a unit of at most 64 coefficients, `last = base[class] + raw`. A unit with
exactly one coefficient codes no `LAST` and has `last = 0`.

**Units larger than 64 coefficients** — a 16x16 block (256) or a 32x32 one
(1024) — reuse the same 16-symbol class table over the unit's 64 equal-sized
scan **groups**, with the position inside the group as extra raw bits:

```
last_shift = the smallest s >= 0 with (ncoef >> s) <= 64
             // 0 for every unit of at most 64 coefficients, 2 for 256, 4 for 1024

nraw = raw_bits[class] + last_shift
raw  = nraw > 0 ? bp(nraw) : 0          // no field is read when nraw == 0
last = (base[class] << last_shift) + raw
```

The widest raw field is therefore `4 + 4 = 8` bits, one bypass chunk, at every
size. The decoder must reject `class == 15`, a `base << last_shift` that is
`>= ncoef`, and a resulting `last >= ncoef`.

This is deliberately **not** a new class table. The classes are a coarse
logarithmic partition of the scan, and applying them to groups keeps that
partition covering the same *fraction* of the scan at every size, which is what
makes the same trained frequencies fit all three. The cost is `last_shift`
bypass bits — 2 on a 256-coefficient block, 4 on a 1024-coefficient one, once
per coded unit — against 256 or 1024 coded values.

**LEVEL**, for scan positions `last, last-1, ..., 0` (reverse scan order):
symbol `min(|q|, 15)`, where 15 is the escape. The context is

```
bp   = pos & 15  in a split block (tool bit 19), else pos
g    = bp >> last_shift                    // the scan group, as for LAST
band = 0 if g == 0, 1 if g in 1..3, 2 if g in 4..9, 3 if g >= 10
prev = 0 if the previously decoded level was 0, 1 if its magnitude was 1, else 2
        (prev = 0 at the first position of a unit)

               prev=0  prev=1  prev=2
   band 0        0       1       2
   band 1        3       4       2
   band 2        5       6       7
   band 3        5       6       7

context index = 4 + that value
```

The two mappings compose in that order and are mutually exclusive in practice:
`split` exists only at `xform_size == 8`, where `last_shift` is 0, and a unit
with a nonzero `last_shift` cannot be split. They are written as a composition
anyway, so that neither tool's rule depends on the other's absence.

`bp` exists because in a split block the 64 scan positions are four
concatenated 4x4 sub-blocks, so a position's *frequency* is its position
within its sub-block; without it every coefficient of sub-blocks 1 to 3 would
land in band 3 whatever its frequency. Nothing else about the LEVEL contexts
changes, and `LAST` classes are still taken over the whole 64.

`last_shift` is 0 for every unit of at most 64 coefficients, so this is the v1
banding unchanged there. At 16x16 and 32x32 the four bands cover the same
fraction of the scan as they do at 8x8 — positions 0-3 / 4-15 / 16-39 / 40+ at
16x16, four times that at 32x32 — which is why **no context is added at any
size**.

A per-size context family was measured before being rejected. Splitting the
frame's symbol histogram by transform size and coding each size with its own
perfectly trained table — an oracle, with no signalling cost at all — saves
**3.17 %** of the coefficient bits on `vr-mixed-1024` 4:4:4 at QP 16. Paying
for it means transmitting three families instead of one, which on the same
frame is 2 560 more bytes of `table_sets` against 2 337 bytes saved: a loss
before the split tables are penalised for being trained on a third of the
data. Reproduce with `-DNXVC_XFORM_CTX_EXPERIMENT`
(`ref/RESULTS-xform-a.md` section 4).

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
v1 context model, **160 bytes** (16 x 16 x 5) under `CTX_V2` and **220 bytes**
(22 x 16 x 5) under `CTX_V3`: MSB-first bit packing, contexts in index order,
symbols in symbol order. The built-in default it is a delta against is the same
set index of the same model's family. Each 5-bit value
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

#### 9.4.1 `TAB_V2`: the compact table set (tool bit 26)

Tool bit 26 `TAB_V2` requires `CUSTOM_TABLES` (bit 6); a stream setting it
without bit 6 is `BITSTREAM`. It changes two things about a **transmitted**
set, and nothing about the built-in defaults or about the decode step.

1. **A per-row flag.** Each context is preceded by one bit `row_coded`. When
   it is 1 the row's sixteen 5-bit deltas follow as above. When it is 0 the row
   **is** the built-in default row of that (set, context), byte for byte: no
   deltas follow and no normalization is performed (a built-in row already sums
   to 1024). A row therefore costs 1 bit instead of 81 when the frame's own
   statistics do not beat the default.

2. **A variable-length table area.** A set is `nctx + 80 * coded_rows` bits
   rather than a whole number of bytes, so the transmitted sets are one
   contiguous bit sequence -- sets in ascending index order over
   `tables_present`, contexts in index order inside each -- **zero-padded to a
   byte boundary once, after the last set**. The decoder recovers the length of
   the area as `ceil(bits_read / 8)`; a set that would read past the end of the
   frame is `TRUNCATED`.

Without the bit every row is coded, the flags are absent, and a set is exactly
`nctx * 16 * 5` bits, which is a whole number of bytes for every defined
`nctx` -- so a stream without bit 26 parses exactly as before.

An encoder that finds no row worth coding for a set **must** leave that set's
`tables_present` bit clear rather than transmit a set of all-zero flags; the
two are equivalent to the decoder, and the shorter one is normative for the
reference encoder only (`tables_present` is an encoder choice, so a decoder
accepts either).

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

**With `CTX_V2`** it is one symbol in context 15, alphabet `0 .. nmodes - 1`:

```
sym == 0 : mode = mpm
sym >= 1 : mode = nonmpm(mpm, sym - 1)
```

`nmodes` is 9, or **10 for a chroma plane's mode unit when tool bit 24
`INTRA_CFL` is set** (section 7.7). Symbols `nmodes .. 15` in context 15 are
illegal (`BITSTREAM`). `nonmpm(mpm, i)` is the `i`-th of the `nmodes - 1`
modes **other than** `mpm`, in ascending mode order.

Without `CTX_V2` the non-MPM index is a 3-bit field, which is why `INTRA_CFL`
requires `CTX_V2`: a tenth mode does not fit in it, and widening that field
would change the meaning of every existing v1.3 stream's bypass bits.

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

### 9.8 The 4x4 split flag (tool bit 19)

In a tile whose header sets `split4x4` (4.1), every **64-coefficient residual
block** unit codes one extra element:

| order | element | descriptor |
|---|---|---|
| 1 | `CBF` | `ae(ctx_cbf)` |
| 2 | `split` | `bp(1)`, **only when `CBF == 1`** |
| 3 | `LAST` | as 9.3 |
| ... | levels | as 9.3 |

`split` selects the unit's scan (9.2) and its LEVEL banding (9.3) and, in
reconstruction, the transform of 6.7. A unit with `CBF == 0` codes no flag and
its `split` is 0 -- which costs nothing, since a block with no coefficients
reconstructs the same residual either way.

DC-plane units and mode units never carry the flag; neither has sub-blocks.

A value of 1 is legal for any residual block of any coded plane, including
chroma and alpha, and for any `res_level`, because a block is 8x8 in every
one of them.

### 9.9 `CTX_V3`: the neighbour-conditioned model (tool bit 25)

Tool bit 25 `CTX_V3` requires `CTX_V2` (bit 21); a stream setting it without
bit 21 is `BITSTREAM`. It keeps everything v2 says about *which* symbols are
coded and changes only *which context* each one is coded in. There are **27
contexts of 16 symbols**, and a third built-in table family
(`kDefaultFreqV3`).

The model keeps v2's sixteen rows and adds eleven. A unit with nothing to
condition on codes in exactly the row v2 gave it, so the new rows only ever
see conditioned data.

**Unit class.** Every coefficient unit belongs to one of three classes,
derived from its position in the tile (9.1) and never transmitted:

| `ucls` | units |
|---|---|
| 0 | residual blocks of the luma plane, and of the alpha plane |
| 1 | residual blocks of a chroma plane |
| 2 | the DC-plane unit of any plane |

A mode unit (9.6) has no class and does not participate.

**The neighbour class.** Each rANS lane keeps two registers: a neighbour class
`nbr` and the *group* it belongs to. A **group** is one plane's run of block
units; a DC-plane unit is in no group. On beginning a unit whose group differs
from the one the lane holds, the lane sets its group to the new one and resets
`nbr` to 0. On finishing a coefficient unit that is in a group, the lane sets

| `nbr` | the unit just finished |
|---|---|
| 0 | there is none -- start of a group |
| 1 | `CBF == 0`, not coded |
| 2 | coded, `LAST < 4` (sparse) |
| 3 | coded, `LAST >= 4` (dense) |

Four values, not one bit. The sparse/dense split of a coded neighbour is a
large part of the model's measured gain and is what a one-bit neighbour
misses.

**This conditioning is per CODING UNIT -- the 8x8 coefficient group -- and
never per transform block.** A lane owns units `l, l+N, l+2N, ...` and decodes
them in that order (9.1), so the unit `nbr` describes is always one **this lane
has already finished**. The derivation is causal inside the lane: it needs no
cross-lane communication and no barrier beyond the one the schedule already
has, whatever the interleaved order does with the other lanes. It does depend
on `N`, which is `2^nsub_log2` from the tile header and is parsed before the
payload, so nothing about tile independence changes.

The context derivation **never reads the transform size**. A unit that sets
the 4x4 split flag of 6.8 is still one 64-coefficient coding unit, and the
only thing the split changes is the band a scan position falls in (9.3),
which is applied before a `LEVEL` context is chosen. Were a larger transform
ever to span several coding units, each of them would be conditioned on the
previous unit its own lane decoded, exactly as an 8x8 block's is, and nothing
in this section would notice they came from one transform.

For the ordinary tile -- `res_level` 0, `nsub_log2` 3, so `N = 8` and each
plane is 8x8 blocks -- a lane's units are exactly **one column of blocks**, and
`nbr` describes the block **directly above**.

**The context assignment.** Rows 0-15 are v2's, unchanged. `base_cbf[ucls]` is
v2's `CBF` row for the class (0 luma, 1 chroma, 12 DC) and `base_last[ucls]`
its `LAST` row (2, 3, 13).

| index | use |
|---|---|
| 0-15 | v2's sixteen rows, used whenever there is nothing to condition on |
| 16-18 | `CBF`, luma/alpha, `nbr` 1..3: `16 + (nbr - 1)` |
| 19-21 | `CBF`, chroma, `nbr` 1..3: `19 + (nbr - 1)` |
| 22 | `LAST`, luma/alpha, neighbour coded (`nbr >= 2`) |
| 23 | `LAST`, chroma, neighbour coded (`nbr >= 2`) |
| 24 | `LEVEL` at scan position 0 of a DC-plane unit |
| 25 | `LEVEL` at scan position `LAST`, band 0-1 |
| 26 | `LEVEL` at scan position `LAST`, band 2-3 |

So `CBF` uses `base_cbf[ucls]` when `nbr == 0` and one of 16-21 otherwise;
`LAST` uses `base_last[ucls]` when `nbr < 2` and 22 or 23 otherwise. `LAST`
does not split sparse from dense: that distinction says how likely a
coefficient is at all, which is `CBF`'s question, and by the time `LAST` is
coded the unit's own `CBF` has already answered it.

`LEVEL` is **not** conditioned on the neighbour -- the previously decoded level
inside the same unit already carries that information, and about this unit
rather than about the one before it. It does split two cases v2 shared:

* **the coefficient at scan position `LAST`**, which is nonzero by
  construction and therefore cannot share a context with positions that may be
  zero. Two rows, by band, using the band of the scan position after the 4x4
  split mapping of 6.8;
* **the DC term of a DC-plane unit** (scan position 0), which is a block mean
  rather than a residual. Other positions of a DC-plane unit keep v2's single
  un-banded row 14.

`band`, `prev` and `kLevelCtx` are exactly 9.3's, and the `MODE` symbol keeps
v2's row 15 -- a mode-symbol context split was built, retrained and measured
**worse**, and is not in this model.

Everything else is unchanged: the alphabets, the `LAST` classes, the escape
code, the sign, sign data hiding and the mode unit's binarisation all read
exactly as 9.3, 9.6 and 9.7 write them.

**What it costs a GPU decoder.** Nothing per symbol: the context index is one
compare and one add on registers the lane already holds, then the same table
lookup. Pass A's shared cumulative-frequency table grows with the context
count -- `s_cum[8][27][16]` is 13824 bytes against 8192 at 16 contexts -- so a
64-thread workgroup of 8 tiles needs about **15.5 KiB** of LDS including the
scan tables, against 10 KiB today and a 32 KiB budget. The per-lane state is
two registers: a 2-bit class and the group it belongs to.

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

### 9.10 ENTROPY_LITE (tool bit 30)

An alternative coefficient coding with **no arithmetic coder**: no rANS state,
no probability tables, no `table_set`, and no serial dependency of any kind
between one coded value and the next. It exists for links whose bitrate is
cheap and whose decode latency is not -- a headset whose Pass A is
latency-bound on the longest tile's serial symbol chain (`vk/decoder/passA`).
It costs bits; section 9.5 remains the default.

A stream setting bit 30 MUST NOT set `SIGN_HIDE` (bit 22) or `CUSTOM_TABLES`
(bit 6): the first needs a coder to spend a level step on, the second a table
to transmit. Two more tools do not fit the payload below and are likewise
excluded: it has no field for the per-block 4x4 split flag of 6.8, so a
stream setting bit 30 MUST NOT set `XFORM_4X4_SPLIT` (bit 19) -- a decoder
takes every Lite tile as unsplit -- and its non-MPM mode index is a 3-bit
field with room for the eight non-MPM values of the nine-mode alphabet only,
so it MUST NOT set `INTRA_CFL` (bit 24), for the reason 9.6 gives for
`INTRA_CFL` requiring `CTX_V2`. The reference encoder clears both tools when
it selects this one. Every tile of such a stream MUST carry
`nsub_log2 == 3`, and the tile header's `table_set` field is reinterpreted as
the **variant selector**:

| `table_set` | variant |
|---|---|
| 0 | `FIXED`: a per-unit magnitude class and fixed-width magnitude fields |
| 1 | `RICE`: a per-unit Exp-Golomb order and an explicit body length |
| 2-7 | reserved, illegal |

Every other part of the tile -- the header, the geometry, the unit list of
9.1, the scan orders of 9.2, the mode unit's MPM derivation of 9.6 -- is
unchanged.

**Sections.** The payload is five sections in this order, each **padded to a
byte boundary** after it, every field packed **MSB-first inside a byte**:

```
H0  ceil(nunits / 16) bits: bit g = "any unit of group g is coded"
H1  for each group g with H0 bit 1, in ascending g:
        min(16, nunits - 16g) bits, one "coded" bit per unit
P   for each coded COEFFICIENT unit, in unit order:
        last_bits(ncoef) bits  LAST
        3 bits                 magnitude class (FIXED) / EG order (RICE)
        12 bits                body length in bits          [RICE only]
S   for each coded unit, in unit order:
        mode unit  -> nb*nb bits, one is_mpm flag per block, raster order
        coef unit  -> LAST bits of significance, scan positions 0 .. LAST-1
B   for each coded unit, in unit order:
        mode unit  -> 3 bits of non-MPM index per block whose flag is 0
        coef unit  -> per nonzero scan position 0 .. LAST, ascending:
                      the magnitude field, then one sign bit (1 = negative)
```

`last_bits(n)` is the smallest `b` with `2^b >= n`: 0, 2, 4 and 6 for the
1-, 4-, 16- and 64-coefficient units of 9.1. A coefficient unit's `coded` bit
is 1 exactly when the unit has a nonzero coefficient, so a coded unit always
has one; the coefficient at scan position `LAST` is nonzero by construction
and its significance bit is **not** coded. A mode unit's `coded` bit is 1
whenever the plane has blocks, which in a conforming stream is always.

**The magnitude field, `FIXED`.** The 3-bit class selects a width:

```
mag_bits[8] = { 0, 1, 2, 3, 4, 6, 8, 16 }
```

and the field carries `|q| - 1` in that many bits, so a class covers
`1 <= |q| <= 2^mag_bits`. Class 0 is **zero bits wide**: a unit whose every
nonzero level is +-1 spends nothing at all on magnitudes, which above QP 16 is
most of them. Class 7 covers the whole int16 range, so `FIXED` has no escape
code and every coefficient of a unit sits at a **computable bit position**:
one thread can decode one coefficient. A decoder MUST reject a decoded
`|q| - 1` above 32766.

**The magnitude field, `RICE`.** The 3-bit parameter `k` is the order of the
Exp-Golomb code of 9.5 (`n = v + 2^k`, `b = floor(log2 n)`, `b - k` one-bits,
a zero, then the low `b` bits of `n`), applied to `|q| - 1`. The code is
variable length, so a unit's coefficients are no longer individually
addressable and the 12-bit body length in section P is what keeps the *unit*
addressable. A decoder MUST reject a unit whose body does not consume exactly
the coded length, a prefix longer than 20 one-bits, and a value above 32766.

**Why the sections are in this order.** Each section's start is computable
from the sections before it and the unit list alone: `H0`'s length is a
constant of `nunits`, `H1`'s is a popcount over `H0`, `P`'s is a sum of
per-unit constants over the coded units, `S`'s needs `LAST`, which is in `P`,
and each unit's slot in `B` is a prefix sum of per-unit body lengths, which
`P` and `S` determine. A decoder therefore reaches any unit with three prefix
sums over quantities it has already read, and no unit's decode depends on any
other unit's. This is the whole content of the tool.

The one dependency that remains is inside a mode unit, and it is the MPM
derivation of 9.6, not a coder chain: block `b` reads the already-resolved
modes of `b - 1` and `b - nb`, so a mode unit resolves as a raster wavefront of
`2*nb - 1` steps.


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
5. for each coded plane, in the order Y, Co, Cg [, A]:
     a. dequantize + inverse-transform the DC plane   -> means M
     b. planar-interpolate M                          -> planar
     c. pred = planar                     for mode == INTRA
        pred = clamp(W + planar - dc_offset, 0, maxval)   otherwise
     d. for each block: dequantize, inverse transform -- 8x8, or four 4x4
        sub-blocks when the block's `split` flag is set (6.8), or the residual
        taken directly for tskip -- add pred, clamp
     e. upsample the plane by its factor into the picture
   The plane order is normative when INTRA_CFL is in use: a chroma block in
   mode CFL reads the finished luma plane of the same tile (7.7).
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
chroma, a `CTX_V2` table set that overruns the tile rows, `LOSSLESS`
together with `SIGN_HIDE`, `INTRA_CFL` without the three tools it requires
(`r30`), tile-header `split4x4` without tool bit 19 (`r31`), `split4x4`
together with `tskip` (`r32`), `TAB_V2` without `CUSTOM_TABLES` (`r33`),
`CTX_V3` without `CTX_V2` (`r34`), a `CTX_V3` table set that overruns the tile
rows (`r35`), a reserved `xform_size` (`r36`), `xform_size` without its tool
bit (`r37`), `xform_size` on a transform-skip tile (`r38`), and `split4x4`
together with `xform_size != 0` (`r39`).

**The v2 intra tools.** `v36`-`v44` pin them: `INTRA_DIR` alone in 4:4:4 and
4:2:0, `INTRA_DIR` with `CTX_V2`, `CTX_V2` alone, the layered form, every v2
feature at once with transmitted 160-byte table sets, the combination with
`res_level` cycling and transform skip, `SIGN_HIDE` alone, and the reference
encoder's shipped default configuration. `v01`-`v35` are **byte-identical** to
the v1.2 set: all three tools are additive and off unless their bit is set.

**The v1.6 detail tools.** `v57`-`v61` pin them: the 4x4 transform split
alone in 4:4:4 and 4:2:0, chroma from luma alone in 4:4:4 (`f == 1`) and 4:2:0
(`f == 2`), and both together.

**The v1.6 entropy and context package.** `v62`-`v65` pin `TAB_V2` and
`CTX_V3` alone and together, and `v66`-`v67` pin them inside an INTER stream,
where the unit sequence is not the intra one.

**The v1.6 large transforms.** `v68`-`v73` pin tool bit 27: 16x16 and 32x32
alone in 4:2:0 and 4:4:4, the encoder's per-tile rate-distortion choice with
every v2 intra tool on, a 32x32 request inside a `res_level`-cycling tile grid
(where the plane cap of 6.7 produces three different block sizes in one
frame), and 16x16 with directional intra on 4:4:4 chroma.

`v01`-`v56` are **byte-identical** to the v1.4 set across all three packages:
every tool is additive and off unless its bit is set, which `nxv-vectors
--check` proves on every commit. The one rejection vector that moved is `r09`,
which pinned "tile word1 bit 28 is reserved" and now pins **bit 31**, because
28 is `split4x4` and 29-30 are `xform_size`; the streams that set those bits
without their tool bits are `r31` and `r37`, which must be refused for the new
reasons.

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

### 13.9 Near-skip: the correction record (tool bit 28)

A warped tile often drifts by a small, smooth amount -- a shading change, a
slow exposure ramp, the accumulated rounding of a resampling chain. The
encoder's only two answers were a `WARP_SKIP` that leaves the drift in the
reference and a fully coded tile that pays a rANS payload and a lane flush to
remove it. **Near-skip** is the answer in between: the tile's entire residual
is a per-plane block-mean field -- a DC level and a pair of ramps -- in nine
signed bytes.

**The record is in the TILE-ROW header, not in the tile** (3.3). A near-skip
tile is a *skipped* tile with a bias: its `skip_bitmap` bit is set, it has no
tile structure at all, it uses its stored `last_mv` like any skipped tile, and
it is named by the row's `dc_bitmap`. That placement is the whole design:

* it costs **no word1 bit**, which is what left room for `split4x4` and
  `xform_size` in the same header;
* it is nine bytes flat rather than a tile header plus a vector plus the
  correction;
* and it is the only form of the tool that can appear in a **warp-only
  chain**, which by definition contains no coded tiles. A form that made the
  tile coded cannot reach the case the tool most exists for.

The constraints are 3.3's, and they are about the row rather than the tile.

Each record is nine bytes: for each of the three colour planes in order
Y, Co, Cg, three signed bytes `c0`, `c1`, `c2`. Alpha is never corrected.

**All three terms are always present.** An earlier design made the two ramp
terms optional behind a second tile-header bit; its encoder then never chose
them, so three of the nine bytes and the `>> log2(nb)` arithmetic below were
exercised by no vector and no test. The ramps are also about three quarters of
what the correction recovers. One record size, always fitted.

**Decoding process.** Per plane, with `nb` the plane's blocks per edge and
`t_dc` the DC-plane quantiser step of 6.5 -- `dequant_step(qp >> 1, 16)`, the
same step a coded DC plane uses, because this is that DC plane written another
way:

```
d0 = dequant(c0, t_dc)
dh = dequant(c1, t_dc)
dv = dequant(c2, t_dc)

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
| 31 | `quad_mv` | four quadrant vectors follow the tile vector |

It requires tool bit 29 `QUAD_MV`, and it requires `mode == WARP_MV` or
`mode == STATIC_MV`, which are the modes that carry a tile vector to refine.
Word1 has no reserved bits left: 28 is `split4x4`, 29-30 `xform_size` and 31
`quad_mv`.  The reserved-bit rejection vector `r09` therefore moved to word0
bit 3, which is the last must-be-zero header bit (docs/TOOLBITS.md 4.1).

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

### 13.11 Sub-tile intra: measured, and NOT in this version

One 32x32 quadrant of an inter tile dropping the predictor was built and
measured, and the syntax is **withdrawn**. It gets no tool bit
(docs/TOOLBITS.md 2) and word1 bit 31 went to `quad_mv` instead.

The idea is sound: where something was occluded a frame ago and is not now, no
vector recovers it -- the samples are not in the reference at any displacement
-- and a rotation-only predictor meets this as a strip along a near-field
object's edge, in a tile that is otherwise perfectly predicted. `INTRA` on the
whole tile throws away three quadrants that did not need it.

It measured at **-0.50 and +0.59 BD-rate points** -- a loss on one of the two
bands -- and shipped off by default on the branch that built it. The reason is
that this corpus cannot produce the case: synthetic rotation-and-pan over a
band-limited picture has no disocclusion in it. That is a statement about the
material, not about the tool, and it is why the syntax is withheld rather than
rejected: it should be re-measured on a capture that contains near-field
parallax, and it costs nothing to leave the bit unallocated until then.

What a format should not do is spend its last reserved tile-header bit on a
tool that is disabled and unproven.

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
* reject `quad_mv` without its tool bit and `quad_mv` on any mode other than
  `WARP_MV` and `STATIC_MV`; reject a row header whose `dc_present` is set
  without tool bit 28, whose `dc_bitmap` is empty, whose `dc_bitmap` names a
  column beyond `cols_per_eye`, or whose `dc_bitmap` names a tile that
  `skip_bitmap` does not (3.3);
* reproduce every `decoded_md5` of the `v45`-`v75` vectors.

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

53. **The 4x4 split reuses the block's coding unit rather than making four.**
    The obvious shape -- four 16-coefficient units where there was one 64 --
    quadruples the unit count, changes the lane schedule, and pays four `CBF`
    symbols where one usually says "nothing here". Concatenating the four 4x4
    scans into the same 64-value unit leaves `CBF`, `LAST`, the level chain
    and the lane schedule *bit-for-bit unchanged* and turns the whole tool
    into one new scan table plus one branch in the inverse transform. It also
    keeps `LAST`'s tail truncation working, which the four-unit form loses:
    energy concentrated in sub-block 0 codes a `LAST` under 16 and pays
    nothing for the other three. The cost is that `LAST` cannot skip a leading
    empty sub-block; measured against the four-unit form's four extra `CBF`
    symbols per block, that is the better trade at every rate we can reach.

54. **The 4x4 split flag lives in the block's own coding unit, not in the
    mode unit.** The mode unit is where a per-block flag naturally belongs --
    it is already a per-block array in raster order -- but a plane's mode unit
    and its block units generally fall in *different rANS lanes*, and section
    9.1's contract is that a unit's syntax depends only on values its own lane
    has already produced. A split flag in the mode unit would make a block's
    scan order and LEVEL banding depend on another lane's output, which the
    interleaved schedule does not order. Putting it after `CBF` in the block's
    own unit is lane-local, and it is also strictly cheaper: a block with no
    coefficients codes no flag at all.

55. **The 4x4 transform is scaled to the 8x8's gain, so there is one
    dequantizer.** A 4-point DCT-III built the obvious way has 1D gain
    `512*sqrt(2)`, which is not a power of two and would need its own shift
    chain, its own quantiser scale and its own weighting-matrix family.
    Scaling the constants to `M[n][k] = round(1024 * c_k * cos(...))` gives
    gain exactly `2^10` per dimension -- the 8x8's -- so 6.5's dequantizer,
    6.3's `>> 7` / `>> 13` shift chain and the `clamp16` discipline all apply
    unchanged, and the 4x4 weights are the tile's own matrix subsampled by
    two. The rounding cost is two parts in `2^20` of row norm. One dequantizer
    and one clamp rule for both sizes is worth more than that.

56. **Chroma from luma is a tenth *mode*, not a flag.** A per-block flag
    orthogonal to the mode would cost a bit on every chroma block whether or
    not the tool ever fires -- 16 bytes per 4:4:4 tile, about 5 % of a QP 16
    tile. As a tenth symbol value in the trained `MODE` context it costs
    essentially nothing when it is not used and is cheaper than a flag when it
    is. The price is that it needs `CTX_V2`: without it the non-MPM index is a
    3-bit bypass field with room for exactly eight alternatives, and widening
    that field would reinterpret every existing v1.3 stream's bypass bits.

57. **The CFL model is fitted between two averaged extremes, not by least
    squares.** A least-squares fit over 16 pairs needs a division by a sum of
    squares whose range is far too wide for a reciprocal table, and a
    fixed-iteration divide would put a 32-round loop in the per-block path. The
    min/max fit divides only by a *luma difference*, which is bounded by 255
    and therefore a 256-entry table lookup. Averaging the two lowest and the
    two highest pairs rather than taking single extrema is what keeps one noisy
    neighbour from setting the slope; it costs one extra selection pass and no
    arithmetic that was not already there.

58. **CFL reads the reconstructed luma plane, which makes the plane order of
    9.1 normative.** It was previously a convention. The dependency is on a
    plane of the *same tile*, already fully decoded by the time chroma starts,
    so tiles stay independent and the transport's per-tile loss recovery is
    untouched. What it costs Pass B is one barrier between the luma and chroma
    planes of a tile -- a shader that predicted all three planes concurrently
    now cannot. That is one barrier against the 69 that directional intra
    already spends (7.6), and it buys the whole tool.

59. **The adaptive dead zone got a named table and no tool bit; the
    reconstruction offset got neither.** The encoder's `f = 1/3` was a magic
    number in three places; it is now `kDeadZoneDc` / `kDeadZoneAc`, indexed
    by the same frequency banding the LEVEL contexts use, in sixths of a step
    so that `2` reproduces the old value exactly. Measured, no other value and
    no shape beats it by more than 0.5 % of rate at any QP, because the RD
    trellis already does the job everywhere except the DC plane and the DC
    plane is insensitive to it. The *decoder-side* reconstruction offset was
    built and measured too and is worse in both rate and quality at every
    operating point -- the trellis picks levels against the real
    reconstruction point, so moving that point only adds error the trellis
    must then pay for. It therefore has no tool bit and no syntax; the
    measurement is in ref/RESULTS-detail-a.md section 3.
60. **The v3 model conditions on the lane's own previous unit, not on a
    geometric neighbour.** The natural thing to condition a `CBF` on is the
    block above it. That block is a *different lane's* unit for every plane
    geometry except the common one, and a lane cannot read another lane's
    result without a barrier the schedule does not have -- the two lanes are
    not at the same unit at the same time, and a unit spans many scheduling
    rounds. Conditioning on "the previous CODING UNIT this lane finished, in
    the same plane" is causal by construction under every schedule, and for the
    ordinary tile (`res_level` 0, `nsub_log2` 3, so 8 lanes over 8x8 blocks) it
    **is** the block above, because a lane owns exactly one column of blocks.
    The price is that the context derivation depends on `nsub_log2`; that field
    is in the tile header and is parsed before the payload, so nothing about
    tile independence changes.

    The unit of conditioning is the **8x8 coefficient group and nothing
    larger**. Were a transform ever to span several groups, each group would
    still be conditioned on the previous group its own lane decoded, and the
    context derivation would not know the transform existed. That keeps this
    model orthogonal to the transform-size tools and keeps 9.1's rule -- a
    unit's syntax may depend only on values its own lane has produced --
    exactly as it was.

61. **The neighbour is four classes on `CBF` and two on `LAST` --- and the
    measurement that said one bit was enough was measuring the wrong thing.**
    A layout sweep built and retrained six variants --- 2, 3 and 4 neighbour
    classes, each with and without a second `LEVEL` family --- and concluded
    that the **smallest won**, on rate as well as on size
    (`ref/RESULTS-ctx-b.md` 2). That sweep is honest and reproducible, and this
    model does not follow it, because the sweep was run against a *fixed-length*
    transmitted table set, where every added context costs 80 bits per set
    whether or not it earns them. Under `TAB_V2` (9.4.1) a row that stays at
    its default costs **one bit**, and the arithmetic that made a wide model
    lose no longer holds: the four-class neighbour plus the two `LEVEL` splits
    measure -5.42 / -3.95 / -9.93 BD-rate points against the narrow model's
    -0.53 / -0.66 / -3.52 on the same material and the same settings
    (`JUDGE-ctx.md` 2).

    What survives: a coded neighbour is split **sparse from dense** at
    `LAST < 4` on `CBF`, where it says how likely a coefficient is at all, and
    not on `LAST`, where the unit's own `CBF` has already answered that.
    `LEVEL` gets no neighbour family --- that part of the sweep stands, and it
    was worth 0.01 % over eight points.

    The general lesson is worth more than the entry: **a width decision
    measured under one table format is not a width decision.** The two tools
    had to be measured together, and separately they each pointed the wrong
    way.

62. **`CTX_V3` splits `LEVEL` at `LAST` and at the DC term, and nowhere else
    --- and it does not read the transform size.** The coefficient at scan
    position `LAST` is nonzero by construction, so sharing a context with
    positions that may be zero costs it the whole probability mass at symbol 0;
    two rows by band recover that. The DC term of a DC plane is a block mean
    rather than a residual and gets its own row. Beyond those two, the DC
    plane keeps one un-banded `LEVEL` context, exactly as v2 gave it;
    banding a dense low-frequency image by scan position was measured not to
    pay when v2 was built and the neighbour conditioning does not change that
    argument. Transform size is not an axis this syntax
    has: every residual unit is 64 coefficients and every DC-plane unit already
    has contexts of its own. Tool bit 19 `XFORM_4X4_SPLIT` **is** built (6.8),
    and it deliberately did not become one: a split unit is still 64
    coefficients in one coding unit, and all it changes is the *band* a scan
    position falls in, which 9.3's banding already handles before a context is
    chosen. The context model still never asks what transform produced the
    coefficients.

63. **A transmitted table row may say "use the default", and the delta stays a
    flat 5 bits.** Adding contexts is what a context model is *for*, and the
    thing that stops it is that a transmitted table set grows linearly with
    the context count -- 14.45 % of a QP 36 inter frame before this package,
    the largest single overhead in it. One bit per row meaning "the built-in
    default row, unchanged", plus dropping a set no row improves, takes that
    frame's table area from 480 bytes to 109 and a QP 24 one from 800 to 321,
    because most rows of most sets are already close to the cluster they were
    trained from. The obvious
    companion, Exp-Golomb coding the deltas around "no change", was implemented
    and **measured worse** than the flat 5-bit index: a trained row is not
    concentrated near its default, it is *shifted* from it, so the small-value
    code loses more on the shifted symbols than it gains on the unshifted ones.
    `ref/RESULTS-ctx-b.md` has the numbers.

64. **`LEVEL` at scan position `LAST` gets its own contexts, and that is where
    a large part of the model's gain is.** The value coded at `LAST` cannot be
    zero: `LAST` is defined as the highest scan position with a nonzero
    coefficient. Coding it in a context shared with positions that may be zero
    spends the whole probability mass at symbol 0 on an outcome that cannot
    happen, at every unit in the stream. Two rows, split by band, cost 160 bits
    per transmitted set -- one bit each under `TAB_V2` when they do not pay --
    and are among the cheapest coding gains in the whole entropy layer. The
    narrow model that lost this tournament never tested it.

65. **REJECTED: reassigning tiles to trained tables without retraining.** The
    encoder scored every tile against the *built-in* table sets even in the
    emitting pass, where the frame carries trained ones -- a real bug, found
    independently twice. The obvious fix, reassign each tile against the
    trained sets, is **worse than doing nothing**: measured at **-1.8 %** at
    4:4:4 QP 16, it makes the stream *larger*, because assignment and training
    have to agree and half a Lloyd step disagrees with both. It also has no
    tool bit and no off switch, so an encoder that ships it cannot reproduce a
    v1.4 stream at all, and it rewrites four shipped conformance vectors
    (`v34`, `v41`, `v42`, `v44`) to do it. The whole Lloyd step -- reassign,
    retrain, repeat -- is what shipped, as `nxvc_config::table_iters`. Recorded
    so nobody rebuilds the half.

66. **12-bit probabilities are the thing to take at the next version break,
    and only then.** Widening `kProbBits` from 10 to 12 was built and measured
    on both models: it is worth well under a percent (one measurement put it at
    0.23-1.23 %, another at a mean of -0.17 % with two of eight points the
    wrong way), and the reason it is small is that the binding precision is the
    **5-bit log-domain delta** of a transmitted row, not the 10-bit total. It
    changes every stream, so it cannot be additive and cannot have a tool bit;
    it belongs to a version break, with the delta precision widened at the same
    time or not at all. `NXVC_PROB_BITS` is the build knob that keeps the
    option open.

67. **16 rANS lanes were measured and rejected.** Doubling the lane count
    costs **+6 % to +33 %** in rate: each lane flushes four bytes at the end of
    a tile, and at the tile sizes this syntax uses the flush is already a
    visible fraction of a small tile. `nsub_log2` remains a per-tile field so a
    stream that wants the parallelism can pay for it; the default does not.
68. **The transform size is per tile, and everything downstream is derived
    from it rather than signalled.** The DC plane's resolution, the planar
    interpolation grid, the directional predictors' block edge, the weighting
    matrix, the scan, the LAST classes and the LEVEL bands all follow the
    block size by a rule; the bitstream gains exactly two bits per tile
    (section 6.7). Two alternatives were rejected. A per-32x32-quadrant size
    would split the DC plane, which is a per-plane structure whose resolution
    *is* the transform grid, into four independently interpolated pieces with
    a seam between them -- the one artefact the DC-plane predictor exists to
    avoid. A per-size family of transmitted probability tables was measured:
    an oracle split saves 3.17 % of the coefficient bits and costs more than
    that in transmitted tables (section 9.3), so no context was added.

69. **Only the 8x8 weighting matrix is ever transmitted.** A 16x16 or 32x32
    block reads it replicated by `u >> k`, `v >> k`. Transmitting a matrix per
    size would triple the 128-byte custom-matrix record and quadruple the
    built-in table for a roll-off whose shape is a function of *normalised*
    frequency, which replication already preserves.

70. **The LAST classes and the LEVEL bands scale with the unit, they do not
    multiply.** A 16x16 or 32x32 unit codes its LAST class over 64 scan groups
    and `last_shift` raw bits inside the group, and bands its LEVEL contexts by
    group. That reuses one trained set of frequencies at all three sizes,
    keeps the widest LAST raw field at 8 bits (one bypass chunk), and costs 2
    or 4 bypass bits per coded unit against 256 or 1024 coded values.

71. **`tskip` and `xform_size` are mutually exclusive.** Transform skip codes
    residual samples in raster order, which is a statement about an 8x8
    neighbourhood; a 1024-sample raster unit is a different tool with its own
    scan question and its own measurement, and defining it untested would put
    a shape in the format that no encoder emits and no decoder is exercised on.

72. **One transform family over four edges, and a test that measures it
    against floating point.** 4x4 came from the detail package and 16x16 and
    32x32 from the transform package; keeping two families would have meant
    two dequantiser scales, and the second one would have been wrong
    **silently** -- a factor of two shifts the effective QP by 6 and leaves
    every rate and PSNR number plausible. The invariant is that the quantiser
    sees **orthonormal** coefficients at every size, which is not the same as
    "the same 2D gain at every size": the unnormalized graph grows by sqrt(2)
    per doubling, so the internal 2D gains are 2^20, 2^20, 2^21 and 2^22 and
    the shift chains differ accordingly. `ref.transform_gain` checks the
    orthonormal scale at every size against a floating-point DCT-II, to within
    0.1 %, and exists so that the chain cannot drift.

73. **The inverse shift chain tracks the true per-size gain rather than a
    round number.** The ideal first-pass inverse shift grows by half a bit per
    doubling -- 7, 7.5, 8 -- and the chain rounds it to 7, 7, 8, keeping one
    more bit of the int16 transpose buffer in use. The competing rule "shift1
    grows by one per doubling" gives 7, 8, 9, which over-shifts by half a bit
    at 16x16 and a full bit at 32x32; modelled in exact fixed point over
    smooth-plus-noise residuals it is about 3 % worse in round-trip RMSE at
    both large sizes, for twice the headroom under a clamp that neither form
    reaches on real input.

74. **`split4x4` and `xform_size` stay separate fields, and pay one
    constraint for it.** They act at different granularities: `xform_size` is
    per tile and chooses the transform, `split4x4` is per 8x8 block and
    subdivides one. Collapsing them into a single size ladder would conflate a
    tile-level choice with a block-level one, and would lose the composition
    the measurement supports -- the split wins on flat 8x8 blocks and the large
    transforms win on smooth regions, so they were never competing for the same
    tiles. The price is the rule of 4.1: `split4x4` is meaningful only where
    `xform_size` selects the 8x8 transform, and a stream that sets both is
    `BITSTREAM` (`r39`). One constraint, pinned by one vector, against two
    fields that each keep their measured gain.

75. **The encoder's rate-distortion lambda is normative to nothing and is
    documented anyway.** Every encoder-side decision in `ref/` minimises
    `D + lambda*R` with `lambda = k * qstep^2`, **`k = 0.22`** fitted on the
    quality harness, and `lambda_sad = sqrt(lambda)` wherever the metric is
    first order (the motion search). A decoder must not care, and none of it
    sets a tool bit. It is recorded here because three separate expressions of
    that trade existed in the reference encoder at v1.4 and a reader
    reasonably assumed one of them was required by the syntax; none is.

    The per-tile mode decision uses the **same** lambda. It used to divide by
    `kRefPersist = 4` -- the number of frames a reconstruction is a reference
    for -- and that divisor is **removed**: the persistence factor is charged
    once already, on the skip candidate's excess, and charging it again here
    was a double charge. Removing it is worth **-3.4 BD-rate points**, the
    largest single item in the rate-distortion package.
    `ref/RESULTS-rdo-b.md` is the fit.

    Phase 1 prefers 0.22 and the Phase 2 kill test prefers 0.30; 0.22 ships
    because Phase 1 is the graded criterion, and the difference is under a
    point either way.

76. **Encoder effort is a named preset, and the preset is a LIBRARY concept.**
    `nxvc_config::preset` takes an `nxvc_preset` and the library resolves it;
    `nxv-enc --preset fast|medium|slow` sets that field rather than expanding
    the preset itself. A preset that exists only in the CLI is not available
    to anything embedding the encoder, which is most of what the encoder is
    for. It sets the trellis candidate set, the motion search stages, the
    number of directional intra modes RD-checked, and whether the per-tile QP
    search runs; it does **not** set `--wm auto`. Every stream any preset
    produces decodes through the identical path. `medium` is the default and
    is what `nxvc_config_default()` gives a caller that sets no fields.

77. **The chroma distortion weight defaults to 1.0, and is a perceptual knob
    rather than a coding gain.** `nxvc_config::chroma_weight_q8` weights
    chroma squared error in the encoder's distortion, scaled by the plane's
    sample density. Weighting chroma below 1.0 was measured at roughly half of
    one branch's PSNR-Y headline at 4:4:4 -- and it is fitted to the 6:1:1
    *reporting convention*, it lowers absolute chroma PSNR, it does nothing at
    4:2:0 where chroma already has a quarter of the samples, and it regresses
    SSIM there. Shipping it on by default would be tuning the encoder to the
    scoreboard. It ships at 1.0, documented, and anything quoted with it must
    be quoted on both metrics.
78. **`ENTROPY_LITE` reuses `table_set` as its variant
    selector rather than claiming a second tool bit.** PAPER 1.6 reserved bit
    16 `ENT_BITPLANE` for a Lite-profile fallback and described it as bit-plane
    significance plus Golomb. What is actually defined here (section 9.10) is
    not a bit-plane coder: it is one significance map per coding unit plus
    fixed-width magnitude fields, which is the same family but codes each
    coefficient once instead of once per plane, and is the version that makes
    a coefficient's bit position computable. It is given a bit of its own (30)
    rather than bit 16 so that a decoder that once implemented the bit-plane
    sketch cannot mistake one for the other. `table_set` names the variant
    because a stream with no probability tables has nothing else for the field
    to mean, and reusing it costs no header bit and no version bump.

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
