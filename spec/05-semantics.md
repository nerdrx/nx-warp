# 5. Semantics

One entry per syntax element declared in clause 4, in the order the elements
appear. Each entry begins with the element name in bold backticks; that form is
what `spec/tools/check_spec.py` matches against clause 4.

Unless an entry says otherwise, the source is `docs/SYNTAX.md` [R-18] at commit
`9083dd1`, and a value outside the stated range makes the stream malformed and
MUST be rejected (clause 3, "the decoder must reject").

## 5.1 Structural note

Two elements are renamed with respect to the source document, to keep every
identifier in this document set unique:

| Here | In `docs/SYNTAX.md` | Reason |
|---|---|---|
| `table_set_idx` | `table_set` (tile header field) | `table_set(k)` is also the name of the transmitted-table structure |
| `frame_flags`, `dg_flags` | `flags` | Three different structures each have a `flags` |

## 5.2 Stream header

**`magic`** identifies an NX Warp stream. Its value MUST be `0x3156584E`, which
is the byte sequence `N`, `X`, `V`, `1` read little endian.

**`version`** is the format version. Version 1 is specified here. A decoder MUST
reject any other value with a version error, distinct from a malformed-stream
error, so that an application can report it usefully.

**`profile`** names the tool subset the stream claims to use: 0 = Lite,
1 = Full, 2 = Pro (clause 8.2). `docs/SYNTAX.md` marks this element
*informative* — the enforceable statement of what a stream uses is `tools`.
That leaves the profile-dependent choice of interpolation filter unsignalled;
see clause 8.2 and Annex C issue C-7. [pending WARP.md]

**`level`** names the numeric limits the stream claims to stay within
(clause 8.3). Also marked informative in the source document, and no level
table has yet been defined anywhere. [pending SYNTAX.md]

**`tile_size`** selects the tile grid. Bit 0 is 0 for 64x64 and 1 for 32x32;
bits 1..7 are reserved and MUST be 0. Version 1 encoders always emit 64x64
[PAPER 6.2]; a decoder that implements only 64x64 rejects the 32x32 value.

**`width`**, **`height`** are the luma sample dimensions **per eye**. Both MUST
be in `[16, 4096]` and even. `ceil(width / 64)` MUST NOT exceed 64
(clause 4.2.1).

**`eyes`** is 1 or 2. With `eyes == 2` every tile carries an `eye` field
selecting its view.

**`bit_depth`** is 8 or 10. A value of 10 requires the `BITDEPTH10` tool bit.

**`num_layers`** is 1 to 4. `layer_desc[i]` for `i >= num_layers` MUST be zero.
More than one layer requires the `LAYERS` tool bit.

**`chroma_format`** is 0 for 4:2:0 or 1 for 4:4:4. It sets the picture-level
chroma geometry; individual tiles may still be 4:2:0 inside a 4:4:4 stream via
`chroma444 == 0` under the `PER_TILE_CHROMA` tool bit.

**`layer_desc`** describes each layer as three bit fields (`layer_type`,
`layer_scale`, `layer_flags`).

**`layer_type`** is 0 = NATIVE (this specification's tile format), 1 = HEVC_NAL,
2 = H264_NAL. Types 1 and 2 select the hybrid path, in which layer 0 is decoded
by an external hardware decoder and only the enhancement layers use this
format. The conversion of the external decoder's output into this format's
sample domain, and the bounds on the drift it introduces, are not specified.
[pending HYBRID.md]

**`layer_scale`** is 0 = 1/1, 1 = 1/2, 2 = 1/4 of the stream's `width` and
`height`. Value 3 is reserved. [pending HYBRID.md]

**`layer_flags`** are 26 reserved bits. Version 1 defines none; they MUST be 0.

**`tools`** is the 64-bit mandatory tool mask (clause 8.4). A decoder MUST
refuse a stream in which any set bit is one it does not implement. Bits 20..63
are reserved and MUST be zero. Capability negotiation is an intersection: the
sender may only set bits the receiver offered.

**`alpha_present`** is 1 if the stream carries a fourth (alpha) plane, else 0.
`alpha_mode != 0` in a tile requires `alpha_present == 1`.

**`color_transform`** is 0 for none or 1 for YCoCg-R. With
`color_transform == 1` the samples handed to the encoder and produced by the
decoder are R, G, B in planes 0, 1, 2, and the reversible lifting of clause 6.3
is applied internally; `chroma_format` MUST then be 1. With
`color_transform == 0` the planes are coded exactly as given, which is the path
used for ordinary YCbCr input.

**`color_space`** tells the sink what the coded planes *are*, so that a decoder
can hand them straight to a compositor [SYNTAX 2.2]:

| Value | Meaning |
|---|---|
| 0 | Unspecified: planes coded as given, range unstated |
| 1 | YCbCr BT.709, limited range |
| 2 | YCbCr BT.709, full range |
| 3 | RGB, which requires `color_transform == 1` (YCoCg-R) |

It is **descriptive only**: the transform, quantiser and entropy coder are byte
for byte identical for every value, so `color_space` changes no decoded sample.
Values 0 to 2 imply `color_transform == 0` and the planes reach the transform
stage untouched — the WiVRn Linux path, whose capture is already
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` and is coded as delivered
[INTEGRATION-DECISIONS 1, ERRATA]. The tie is normative in both directions:
`color_space == 3` **if and only if** `color_transform == 1`.

The two elements therefore do not duplicate each other: `color_transform` says
what the codec *does*, `color_space` says what the result *means*. Annex C
issue C-1 is closed by this definition.

**`stream_reserved`** is 19 bytes that MUST be zero.

**`ext_len`** is the byte length of the TLV extension area that follows the
64-byte header.

## 5.3 Extension area, frame header

**`tlv_type`** identifies an extension record. A decoder MUST skip records whose
type it does not recognise; types `0x8000`–`0xFFFF` are private. No TLV type is
mandatory in version 1, by construction: anything that must be understood is a
tool bit instead.

**`tlv_length`** is the payload length in bytes, not counting the 4-byte prefix
and not counting the padding.

**`tlv_payload`** is `tlv_length` bytes, uninterpreted by this specification.

**`tlv_pad`** is `(4 - (tlv_length & 3)) & 3` zero bytes aligning the next
record to 4 bytes.

**`frame_number`** identifies the frame and wraps at `2^16`. It is compared for
equality with `row_frame_number` in every tile-row header, which is what makes a
replicated row header self-checking.

**`pose`** is 26 bytes that are **opaque to the decoding process**: carried,
hashed and compared byte-wise, never interpreted, because the decoder performs
no floating-point arithmetic (clause 3.4). Their agreed meaning is
`u16 quat[4]` then `u16 angvel[3]` as `binary16` bit patterns, then
`u32 pos[3]` as `binary32` bit patterns in metres [SYNTAX 3.2, decision 5]. The
transport carries a *different* 26-byte pose structure with an incompatible
layout [TRANSPORT 3.3]; see Annex C issue C-6.

**`base_qp`** is the frame-level quantisation parameter, 0 to 63. Each tile's
quantiser is `clamp(base_qp + qp_delta, 0, 63)`.

**`chroma_qp_off`** is a signed offset added to the tile QP for planes 1 and 2,
the result clamped to `[0, 63]`.

**`alpha_qp_off`** is a signed offset added to the tile QP for plane 3, the
result clamped to `[0, 63]`.

**`quant_matrix`** selects the weighting matrices: 0 to 3 select the built-in
pairs of Annex A.2, and 255 means 128 bytes of custom matrices follow. Any
other value is malformed. Value 0 is flat for both luma and chroma; values 1 to
3 differ in the luma matrix only and all use the same built-in chroma matrix
[SYNTAX decision 13].

**`tables_present`** is a bitmask: bit `k` set means probability table set `k`
is transmitted for this frame, in ascending `k`, after any custom matrices. A
cleared bit means the built-in default set `k` is used, which is what makes
`tables_present == 0` a valid and loss-tolerant frame.

**`ref_slots`** identifies the reference slots this frame overwrites. Its
encoding is defined only as "reference slots this frame overwrites (Phase 2)";
neither the source document nor the paper says whether it is a bitmask of the
four slots or a slot index, nor how it interacts with the transport's
`frame_id mod 4` slot addressing [TRANSPORT 7.1]. Recorded as Annex C issue
C-8. [pending WARP.md]

**`frame_flags`** carries `tile_map_reset`, `stereo_enable` and six reserved
bits.

**`tile_map_reset`** set means this frame is a tile-map reset: the stream header
is repeated and no tile may reference an earlier frame. It is the format's
nearest equivalent to an IDR.

**`stereo_enable`** set means inter-view prediction is enabled for this frame,
so tiles of the second eye may use `mode == STEREO`. [pending STEREO.md]

**`frame_flags_reserved`** is 6 bits that MUST be 0.

**`frame_reserved`** is one byte at offset 35 that MUST be 0.

**`frame_bytes`** is the total byte length of the frame unit including its
header, which makes a frame self-delimiting so a stored file can be walked and
a truncated frame detected cheaply [SYNTAX decision 4]. It MUST be at least 40
and MUST NOT exceed the bytes available; after the last tile of the last row
exactly `frame_bytes` bytes MUST have been consumed.

**`homography`** is the quantised per-eye projective transform that the inter
predictor of clause 6.7 evaluates. **No syntax element carries it.** The
implementation in `warp/` takes nine `int32` plus an origin `(ox, oy)`, rows 0
and 1 in Q10.21, row 2 in Q2.29 with `h22 == 1 << 29`
[warp/include/nxvc/warp.h @ 9083dd1]; `docs/PAPER.md` 2.2 instead specifies
nine `int32` in Q8.24 with `h22 = 2^24`, 36 bytes per eye, carried in the frame
header. Neither is in the frame header as specified in clause 4.4. This is the
format's largest hole; see Annex C issue **C-4**. [pending WARP.md]

## 5.4 Tables, tile row, tile header

**`custom_matrix_luma`** is 64 bytes, raster order within the 8x8 block, Q4 with
16 meaning 1.0, applied to planes 0 and 3. Values are clamped into `[1, 32]` on
parse rather than rejected, so that the `int32` bound on dequantisation holds
for any byte value [SYNTAX decision 12].

**`custom_matrix_chroma`** is the following 64 bytes, same encoding, applied to
planes 1 and 2.

**`table_delta`** is a 5-bit index into `kDeltaMul` (Annex A.3) applied in the
log domain to the built-in default frequency of the *same set index*, same
context and same symbol, before the deterministic renormalisation of clause
6.6.2.

**`row_frame_number`** MUST equal `frame_number`. It exists so that a row header
replicated into a datagram is self-checking [SYNTAX 3.3].

**`row_index`** MUST equal the ordinal of the row, counting from 0.

**`tile_count`** is the number of tile structures that follow in this row. It
MUST equal the number of tiles of the row whose `skip_bitmap` bit is clear.

**`skip_bitmap`** is 64 bits; bit `i` set means tile `i` of this row is
`WARP_SKIP` and is **not** transmitted. This is the format's largest overhead
saving: a static periphery tile costs one bit rather than a header
[PAPER 1.2]. Its width is what caps a picture at 64 tile columns
(clause 4.2.1, Annex C issue C-3). In an intra-only Phase 1 stream a nonzero
`skip_bitmap` MUST be rejected, because a skip references a frame the decoder
cannot have [SYNTAX 12].

**`layer`** is the layer this tile belongs to, 0 to 3, and MUST be less than
`num_layers`.

**`eye`** is the view, 0 or 1, and MUST be 0 when `eyes == 1`.

**`tile_word0_reserved`** is 1 bit that MUST be 0.

**`tile_index`** is the index of the tile **inside its row**, 12 bits. It MUST
equal the tile's position in the row. Note that the transport addresses tiles by
a *linear* index `row * cols + col` [TRANSPORT 1]; the two indices are different
quantities.

**`payload_len`** is the length of the entropy-coded payload in bytes, 16 bits.
The transport tile directory instead carries a 12-bit `dir_len`, capping a
transported tile at 4095 bytes; see Annex C issue C-9.

**`mode`** is the tile's prediction mode:

| Value | Name | Predictor |
|---|---|---|
| 0 | `WARP_SKIP` | Warped reference, stored last vector, no residual |
| 1 | `STATIC_MV` | Reference at identity (no warp) plus the vector |
| 2 | `WARP_MV` | Warped reference plus the vector |
| 3 | `INTRA` | DC plane and planar interpolation, no reference |
| 4 | `STEREO` | The decoded first eye of the same frame |
| 5–7 | — | Reserved, MUST be rejected |

The field is 3 bits, resolving the paper's own disagreement between a 2-bit
four-mode field [PAPER 1.2] and a five-mode list [PAPER 6.5]
[SYNTAX decision 1]. Modes 0, 1, 2 and 4 require the corresponding tool bits
(`INTER`, `WARP`, `STEREO`). The numbering above is `docs/SYNTAX.md`'s and is
authoritative; note that [STEREO 9] refers to `STEREO` as "mode 3", counting
ordinally in the paper's mode table rather than by this field's value. The two
statements are compatible only under that reading. Annex C issue C-23.

**`res_level`** is the coded resolution: 0 = 64x64, 1 = 32x32, 2 = 16x16, and 3
is **reserved and MUST be rejected**. The tile is coded at `64 >> res_level` and
upsampled on reconstruction by the kernel of clause 6.5. A nonzero value
requires the `RES_LEVEL` tool bit. The transport directory's `dir_res_level`
assigns value 3 the meaning "DC-plane only"; see Annex C issue C-5.

**`chroma444`** set means this tile's chroma planes are coded at the full tile
resolution. It may be 1 only when `chroma_format == 1`; a 4:2:0 tile inside a
4:4:4 stream requires the `PER_TILE_CHROMA` tool bit and may produce a combined
upsampling factor of 8 [SYNTAX decision 26].

**`alpha_mode`** is 0 = opaque (the plane is filled with 255), 1 = constant
(`alpha_value` follows and fills the plane), 2 = coded as a fourth plane with
the luma tools at its own QP, 3 = reserved and rejected. Any value other than 0
requires `alpha_present == 1`.

**`qp_delta`** is a signed 6-bit two's complement offset in `[-32, 31]` applied
to `base_qp` before clamping to `[0, 63]`. It is the encoder's primary
perceptual control [PAPER 1.5].

**`table_set_idx`** selects which of the eight probability table sets this tile
decodes with, 0 to 7. Called `table_set` in `docs/SYNTAX.md`.

**`nsub_log2`** gives the lane count `N = 2^nsub_log2`, 0 to 5. Version 1
encoders emit 3, i.e. eight lanes, so that eight tiles fill one 64-wide wave
[PAPER 6.3]; any other value requires the `NSUB_VAR` tool bit, so a decoder
built for exactly eight lanes refuses the stream at the handshake rather than
at the tile [SYNTAX decision 24].

**`mv_present`** set means two signed bytes `mv_x`, `mv_y` follow the tile
header.

**`ref_sel`** selects the reference slot: 0 is the newest, 1 to 3 are older. It
is the same field the transport calls `ref_delta`, keeping the name of
[PAPER 1.2] and the meaning of [PAPER 6.6]; the paper's additional reading
"3 means intra" is dropped here because `mode` already says intra
[SYNTAX decisions 1, 2]. The transport header's `ref_delta` retains value 3 as
intra (clause 5.6), so the two fields do not have identical value sets.
[STEREO 9] further states that `ref_sel` is *not coded* for a `STEREO` tile,
which this syntax cannot express — the bits are always present in `word1`.
Annex C issue C-22.

**`tskip`** set means the whole tile skips the transform: the 64 coded values of
each block are residual samples in raster order (clause 6.4.4). It requires the
`TRANSFORM_SKIP` tool bit.

**`wgt`** is the enhancement-layer blend weight of the spatial hypothesis:
0, 1/4, 1/2, 3/4. The source document's table gives four values for a 2-bit
field, while [PAPER 1.7] lists five weights (0, 1/4, 1/2, 3/4, 1). No blending
process is specified anywhere. Recorded as Annex C issue C-10.
[pending HYBRID.md]

**`tile_word1_reserved`** is 6 bits that MUST be 0.

**`mv_x`**, **`mv_y`** are the tile's motion vector components, signed 8-bit, in
quarter samples (Q.2), range `[-32, +31.75]` samples. Present only when
`mv_present` is set. Two things about this element are unsettled:

* Whether the coded value is the vector itself or a delta from the tile's stored
  previous vector is stated by [PAPER 2.3] (a delta) but by no normative
  document. Annex C issue C-11. [pending WARP.md]
* For a `STEREO` tile the vector is a **disparity**, which [STEREO 2.3] decides
  to code as an unsigned Exp-Golomb value reaching about 512 samples — neither
  the encoding nor the range of this element. Annex C issue **C-21**.

**`alpha_value`** is the constant alpha that fills the plane when
`alpha_mode == 1`.

## 5.5 Payload elements

**`lane_init_state`** is the initial rANS state of one lane, little endian, 4
bytes, `active_lanes` of them at the start of the payload, lane 0 first. Each
MUST be at least `L = 2^16`; a smaller value is malformed
[SYNTAX 9.5, decision 23].

**`active_lanes`** is `min(1 << nsub_log2, unit_count)`. Lanes beyond the coding
unit count are never initialised and consume no bytes
[SYNTAX decision 25].

**`unit_count`** is the number of coding units in the tile, derived from
`res_level`, `chroma444`, `chroma_format` and `alpha_mode` by clause 6.6.3.

**`ncoef`** is the number of coefficients in the current coding unit: 64 for a
block, and `nb * nb` for a DC plane.

**`cbf`** is the coded block flag of a coding unit. Symbol 0 means every
coefficient of the unit is zero and the unit is finished; symbol 1 means
coefficients follow. Symbols 2 to 15 are illegal in this context and MUST be
rejected.

**`last_class`** is the class of the last significant scan position, 0 to 14.
Class 15 is reserved and MUST be rejected. It is coded only when the unit has
more than one coefficient; a one-coefficient unit has `last = 0`.

**`last_raw`** is `last_raw_bits` bypass bits refining the class into a scan
position.

**`last_base`**, **`last_raw_bits`** are `kLastBase[last_class]` and
`kLastRawBits[last_class]` from Annex A.5.

**`last`** is `last_base + last_raw`, the scan position of the last nonzero
coefficient. A decoder MUST reject `last_class == 15`, a `last_base` that is
`>= ncoef`, and a resulting `last >= ncoef`.

**`level`** is `min(abs(q), 15)` for one coefficient, decoded for scan positions
`last` down to 0 inclusive, in reverse scan order. Symbol 15 is the escape. The
level at position `last` MUST NOT be zero. Every position down to and including
0 is coded, including `last` itself, so that the lane state machine stays
branch-free at the unit boundary [SYNTAX decision 16].

**`level_escape`** is the order-3 Exp-Golomb suffix coding `abs(q) - 15`,
carried as bypass bits (clause 6.6.6). A prefix longer than 16 one-bits MUST be
rejected, as MUST a decoded `v > 32752`, which is what bounds `abs(q)` to 32767
and makes the `int32` dequantisation bound provable
[SYNTAX decision 20].

**`level_sign`** is one bypass bit following every nonzero level, after any
escape bits. 1 means negative.

**`ctx_cbf`**, **`ctx_last`**, **`ctx_level`** are the context indices of
clause 6.6.5: 0 or 1 for `cbf`, 2 or 3 for `last_class`, and `4 + kLevelCtx`
for `level`. A DC-plane unit uses the same contexts as its plane.

## 5.6 Transport elements (normative in [R-19])

These are defined here only to the depth a decoder needs; clause 7 states which
of them it must act on.

**`dg_version`** is `NXT_VERSION`, 1. A receiver MUST drop other versions.

**`dg_flags`** is four bits: `KEYFRAME_RUN` (every tile in the run is intra),
`PARTIAL_FRAME` (the sender already knows the frame is incomplete),
`LOSSLESS` (the run carries lossless tiles, and only then may a tile be
fragmented) and `LAST_RUN_OF_FRAME` (advisory; frame completion is driven by the
deadline, not by this bit).

**`stream_id`** identifies the WiVRn stream, one per quad layer or eye-pair
geometry.

**`frame_id`** is the transport's frame counter, 16 bits, wrapping every 12.1
minutes at 90 Hz. It addresses the reference ring as `frame_id mod 4`. It is
not the same element as the bitstream's `frame_number`, although the two are
expected to agree; nothing states that they must. See Annex C issue C-12.

**`tile_first`** is the **linear** tile index `row * cols + col` of the first
tile of the run.

**`dg_tile_count`** is the number of tiles carried, 1 to 255. **Zero marks a
parity datagram.**

**`layer_id`** is the layer, 0 = base.

**`ref_delta`** means the reference frame is `frame_id - 1 - ref_delta`, with
value 3 meaning intra. A run is homogeneous in this field, which is how a
per-datagram reference field is reconciled with per-tile reference choice
[TRANSPORT D3].

**`frag_idx`**, **`frag_count`** identify a fragment of an oversize tile;
`frag_count` is fragments minus one, so 0 means unfragmented. Fragmentation is
legal only with `LOSSLESS` and the `CAP_FRAGMENT` capability, and a tile with
any fragment missing is a lost tile.

**`tile_class`** is 0 = A (fovea and quad-layer base), 1 = B (mid eccentricity),
2 = C (periphery and enhancement layers). Value 3 is reserved and MUST NOT be
sent. It drives the FEC parity count and is on the wire so a receiver can
attribute FEC groups without decrypting first.

**`band`** is the row band, 0 to 6, with 7 meaning not band addressed.

**`pose_hdr`** set means the payload begins with the 26-byte frame/pose header.

**`caps`** echoes the negotiated capability set. A receiver MUST drop a datagram
whose `caps` contains a bit it did not negotiate (clause 8.4).

**`pose_seq`** indexes the client's own two-second pose ring: the render pose the
server used. The pose itself never travels downstream except in the replicated
header.

**`path_seq`**, **`path_id`** are the per-path sequence number (wrapping at
16384) and the sending path, 0 or 1 in version 1.

**`fec_group`**, **`fec_idx`**, **`fec_k`** identify the FEC group within
`(frame_id, band, tile_class)`, the index within it, and the number of data
datagrams. `fec_idx >= fec_k` marks a parity datagram; `fec_k == 0` means no FEC
on this datagram.

**`tx_ts`** is the server clock in microseconds, wrapping every 71.6 minutes.

**`dg_payload_len`** is the ciphertext length in bytes, excluding the 16-byte
tag. For GCM and ChaCha20-Poly1305 it equals the plaintext length.

**`enc_us`** is telemetry: encode-finish minus render-finish for this band.

**`dir_len`** is the tile bitstream length in bytes, 0 to 4095, where 0 means an
empty (skip) tile. The sum of all `dir_len`, plus `4 * dg_tile_count`, plus 26
if `pose_hdr`, MUST equal the plaintext length; a receiver that finds otherwise
MUST discard the whole datagram and count it as lost, because it cannot know
where the tile boundaries are.

**`dir_qp`** is the tile's resolved QP, 0 to 63 — an absolute value, where the
tile header carries `qp_delta` relative to `base_qp`. Nothing states which is
authoritative if they disagree; see Annex C issue C-13.

**`dir_mode`** repeats the tile's `mode` with the same numbering.

**`dir_res_level`** repeats `res_level`, except that value 3, reserved in the
tile header, here means "DC-plane only" [TRANSPORT 3.1, PAPER 4.6.1]. See
Annex C issue C-5.

**`dir_lossless`**, **`dir_chroma444`**, **`dir_alpha`** are one-bit summaries of
the tile for the receiver's benefit.

**`dir_reserved`** is 6 bits that MUST be 0, reserved for the version 2 edge
mask and contour mode.
