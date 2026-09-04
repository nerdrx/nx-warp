# 2. Terms, definitions, abbreviations and symbols

Only terms actually used by this document set and by the component documents
are listed. Where a term has a specific meaning that differs from its ordinary
use in video coding, the difference is stated.

## 2.1 Terms and definitions

**active lane** — one of the first `min(N, unit_count)` rANS lanes of a tile
payload. Lanes beyond the coding-unit count are never initialised and consume
no bytes [SYNTAX 9.1, decision 25].

**band** (also *row band*) — a group of `band_rows` consecutive tile rows,
six rows in the v1 configuration, used as the unit of pipelining, of feedback,
and of frame-header replication [TRANSPORT 1]. A band is a transport concept:
the bitstream itself has no band structure.

**block** — 8x8 samples of one plane; the transform unit. A 64x64 tile has 64
luma blocks [SYNTAX 1].

**bypass bits** — raw, uniformly distributed bits coded directly on the rANS
state with an implicit uniform context, rather than through a probability
table (clause 6.6.5).

**coding unit** — one entropy-coded object: either a DC plane or an 8x8 block.
A tile payload is an ordered list of coding units [SYNTAX 1, 9.1].

**concealment** — reconstruction of a tile position for which no bitstream
arrived, by running the prediction process with the tile's stored last motion
vector and no residual. Concealment is *identical* to a `WARP_SKIP` tile, which
is what allows an encoder to replay it exactly (clause 6.11).

**conformance vector** — a bitstream in `tests/vectors/` together with pinned
MD5 digests of the bitstream and of the decoded planes (clause 9.3).

**DC plane** — the `nb x nb` array of reconstructed block means of one plane of
an intra tile, coded as the first coding unit of that plane and used to build
the planar intra prediction [SYNTAX 7.1].

**datagram** — one transport protocol data unit, carrying a *tile run*. The
datagram is the loss unit; the tile is the concealment unit [PAPER 6.1].

**eye** — one of the (up to two) views of a stereo stream. Views are coded in
the same picture geometry and identified by the `eye` field of the tile header.

**exact** (of a tile position in a reference frame) — the sender knows from
feedback that the receiver holds bit-identical samples there. Defined
recursively over concealment in clause 7.3 [TRANSPORT 9].

**frame** — one self-delimiting coded unit: a frame header, optional tables,
and one tile-row structure per tile row [SYNTAX 3].

**lane** — one rANS state within a tile payload. `N = 2^nsub_log2` lanes exist;
`active` of them are used. Also called a *substream* in [I-1].

**layer** — one of up to four coded representations of the same content, layer
0 being the base. Layers above 0 predict from the layer below (the *spatial
hypothesis*) and from their own previous reconstruction (the *temporal
hypothesis*) [PAPER 1.7]. [pending HYBRID.md]

**level** — a set of numeric limits on what a bitstream may demand of a decoder
(clause 8.3).

**picture** — the decoded sample array of one eye of one layer of one frame.

**plane** — one component array: 0 = Y, 1 = Co, 2 = Cg, 3 = A [SYNTAX 1]. With
`color_transform == 0` the planes carry whatever the application put in them
(commonly Y, Cb, Cr).

**pose** — the 26 opaque bytes in the frame header that identify the head pose
the frame was rendered for. Opaque to the decoding process (clause 5.3.2).

**profile** — a named subset of the coding tools (clause 8.2).

**reference epoch** — the state of the reference ring that both the encoder's
shadow and the receiver agree on. See *exact*.

**res_level** — the per-tile coded resolution: the tile is coded as a
`64 >> res_level` square image and upsampled on reconstruction [SYNTAX 4.2].

**run** — see *tile run*.

**shadow** (also *client shadow*) — the encoder's mirror of the receiver's
reference ring, reconstructed from feedback rather than assumed. Encoder-side;
named here because reference eligibility is defined in terms of it
[TRANSPORT 9].

**skip bitmap** — the 64-bit field of a tile-row header marking the tiles of
that row that are `WARP_SKIP` and are therefore not transmitted [SYNTAX 3.3].

**slot** — one of the four entries of the reference ring, addressed by
`frame_id mod 4` [TRANSPORT 7.1].

**stale** (of a decoded tile) — decoded, but from an earlier frame; `age > 0`
[TRANSPORT 7.5].

**stream** — a stream header, an extension area, and a sequence of frames.

**tile** — 64x64 luma samples, coded as an independent bitstream. Fixed at 64
in version 1; 32 is reserved by a stream-header bit [SYNTAX 2, PAPER 6.2].

**tile run** — a contiguous, ascending sequence of tiles from one tile row,
homogeneous in the keys of clause 7.1, packed into one datagram
[TRANSPORT 3.2].

**tool bit** — one bit of the 64-bit `tools` mask. A decoder that does not
implement a set bit MUST refuse the stream (clause 8.4).

**transform skip** — a per-tile mode in which the coded values are residual
samples rather than transform coefficients [SYNTAX 6.6].

**warp** — the pose-derived homography prediction of clause 6.7 [R-26].

## 2.2 Abbreviations

| Abbreviation | Expansion |
|---|---|
| AEAD | Authenticated Encryption with Associated Data [R-11] |
| CBF | Coded block flag |
| DCT | Discrete cosine transform |
| FEC | Forward error correction |
| IDCT | Inverse discrete cosine transform |
| LDS | Local data share (GPU workgroup-local memory) |
| LSB / MSB | Least / most significant bit |
| MTU | Maximum transmission unit |
| MV | Motion vector |
| QP | Quantisation parameter |
| rANS | Range variant of asymmetric numeral systems [R-8] |
| RS | Reed-Solomon [R-15] |
| SATD | Sum of absolute transformed differences (encoder only) |
| TLV | Type-length-value record |

## 2.3 Symbols and mathematical conventions

| Symbol | Meaning |
|---|---|
| `L` | rANS lower bound, `2^16` [SYNTAX 9.5] |
| `M` | rANS probability precision, `2^10` [SYNTAX 9.5] |
| `N` | Number of rANS lanes of a tile, `2^nsub_log2` |
| `nb` | Blocks per edge of a coded plane, one of 8, 4, 2, 1 [SYNTAX 4.2] |
| `maxval` | Largest legal sample value of a plane (clause 5.4.4) |
| `dc_offset` | Sample-domain offset of a plane (clause 5.4.4) |
| `qstep[qp]` | Quantiser step table, Q4 (Annex A.2) |
| `w[i]` | Weighting-matrix entry at raster position `i`, Q4, in `[1, 32]` |
| `coded_size` | `64 >> res_level`, the coded luma extent of a tile |
| `cols`, `rows` | Tile columns and rows of a picture |
| `H` | The quantised per-eye homography, `h00`..`h22` of `warp_ext()` (clause 4.4.2) |
| `ox`, `oy` | The picture centre, `(width >> 1, height >> 1)`; derived, never transmitted |
| `cols_per_eye` | `ceil(width / 64)`; the transport's `cols` is `eyes * cols_per_eye` (Annex D **D-3**) |

The mathematical conventions — operator precedence, shift semantics,
Q-formats, clipping — are specified in clause 3, not here.
