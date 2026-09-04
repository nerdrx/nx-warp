# 4. Bitstream syntax

Syntax elements are declared in ` ```syntax ` blocks, one element per line,
with the descriptors of clause 3.6. Every identifier declared here has a
semantics entry in clause 5; `spec/tools/check_spec.py` enforces that.

The normative source for this clause is `docs/SYNTAX.md` [R-18], transcribed at
commit `9083dd1`. Byte offsets given in the tables are the offsets in that
document and are part of the format. Where a field is not yet fixed, the line
carries a `[pending ...]` marker and clause 5 says what is unknown.

## 4.1 Structure

```
stream()  {
    stream_header()
    extension_area()
    while (more_data())
        frame()
}

frame()  {
    frame_header()
    if (quant_matrix == 255)
        quant_matrices()
    for (k = 0; k < 8; k++)
        if (tables_present & (1 << k))
            table_set(k)
    for (row = 0; row < num_tile_rows; row++)
        tile_row(row)
}

tile_row(row)  {
    tile_row_header()
    for (i = 0; i < tile_count; i++)
        tile()
}

tile()  {
    tile_header()
    if (mv_present)     { mv_x; mv_y }
    if (alpha_mode == 1)  alpha_value
    tile_payload()
}
```

[SYNTAX 11]

## 4.2 Stream header

Fixed 64 bytes, sent once at the start of a stream and repeated on every
tile-map reset [SYNTAX 2].

```syntax
stream_header()                                        Descriptor
    magic                                              f(32)
    version                                            f(8)
    profile                                            f(8)
    level                                              f(8)
    tile_size                                          f(8)
    width                                              f(16)
    height                                             f(16)
    eyes                                               f(8)
    bit_depth                                          f(8)
    num_layers                                         f(8)
    chroma_format                                      f(8)
    layer_desc[0..3]                                   f(32)
    tools                                              f(64)
    alpha_present                                      f(8)
    color_transform                                    f(8)
    color_space                                        f(8)     [pending SYNTAX.md]
    stream_reserved                                    b(19)
    ext_len                                            f(16)
```

| Offset | Size | Element |
|---|---|---|
| 0 | 4 | `magic` |
| 4 | 1 | `version` |
| 5 | 1 | `profile` |
| 6 | 1 | `level` |
| 7 | 1 | `tile_size` |
| 8 | 2 | `width` |
| 10 | 2 | `height` |
| 12 | 1 | `eyes` |
| 13 | 1 | `bit_depth` |
| 14 | 1 | `num_layers` |
| 15 | 1 | `chroma_format` |
| 16 | 16 | `layer_desc[0..3]`, four `f(32)` |
| 32 | 8 | `tools` |
| 40 | 1 | `alpha_present` |
| 41 | 1 | `color_transform` |
| 42 | 1 | `color_space` **[pending SYNTAX.md]** |
| 43 | 19 | `stream_reserved`, MUST be zero |
| 62 | 2 | `ext_len` |

**`color_space` is provisional.** At the time of writing, `docs/SYNTAX.md`
places 20 reserved bytes at offset 42 and defines no `color_space`. The `ref/`
agent is adding a `color_space` element (0 = YCoCg-R, 1 = YCbCr passthrough
with a range flag) because the WiVRn source frames are already 4:2:0 YCbCr.
This clause reserves offset 42 for it and shortens `stream_reserved` to 19
bytes so the header stays 64 bytes and `ext_len` stays at offset 62. The exact
encoding of the range flag — a second byte, or a bit of this one — is not yet
decided, and its interaction with `color_transform` (clause 5.2.14) must be
settled. Recorded as Annex C issue C-1. [pending SYNTAX.md]

**Bit fields of `layer_desc[i]`**, LSB first [SYNTAX 2]:

```syntax
layer_desc(i)                                          Descriptor
    layer_type                                         f(4)
    layer_scale                                        f(2)
    layer_flags                                        f(26)
```

### 4.2.1 Constraints

A decoder MUST reject a stream header that violates any of the following
[SYNTAX 2]:

* `magic != 0x3156584E` or `version != 1` — reject with a version error.
* `tile_size` bits 1..7 nonzero.
* `width` or `height` outside `[16, 4096]`, or odd.
* `ceil(width / 64) > 64`. The tile-row `skip_bitmap` is 64 bits wide, so a
  picture may not exceed 64 tile columns. **This constraint conflicts with the
  transport configuration**, which uses `cols = 68` for a 4320-wide stereo
  picture [TRANSPORT 1]. Recorded as Annex C issue C-3.
* `chroma_format`, `color_transform` or `alpha_present` out of range.
* `color_transform == 1` with `chroma_format != 1`.
* Any bit set in `tools` that the decoder does not implement (clause 8.4).
* `layer_desc[i] != 0` for any `i >= num_layers`.

## 4.3 Extension area

`ext_len` bytes of TLV records immediately follow the stream header
[SYNTAX 2.1].

```syntax
extension_area()                                       Descriptor
    while (bytes_consumed < ext_len)
        tlv_record()

tlv_record()                                           Descriptor
    tlv_type                                           f(16)
    tlv_length                                         f(16)
    tlv_payload                                        b(tlv_length)
    tlv_pad                                            b((4 - (tlv_length & 3)) & 3)
```

A decoder MUST skip every `tlv_type` it does not recognise. A record that would
run past `ext_len` makes the stream malformed. Types `0x8000`–`0xFFFF` are
private. Version 1 defines no mandatory TLV type: anything that must be
understood is signalled in `tools` instead.

## 4.4 Frame header

Fixed 40 bytes [SYNTAX 3.1].

```syntax
frame_header()                                         Descriptor
    frame_number                                       f(16)
    pose                                               b(26)
    base_qp                                            f(8)
    chroma_qp_off                                      s(8)
    alpha_qp_off                                       s(8)
    quant_matrix                                       f(8)
    tables_present                                     f(8)
    ref_slots                                          f(8)
    frame_flags                                        f(8)
    frame_reserved                                     f(8)
    frame_bytes                                        f(32)
    homography                                         [pending WARP.md]
```

| Offset | Size | Element |
|---|---|---|
| 0 | 2 | `frame_number` |
| 2 | 26 | `pose` |
| 28 | 1 | `base_qp` |
| 29 | 1 | `chroma_qp_off` |
| 30 | 1 | `alpha_qp_off` |
| 31 | 1 | `quant_matrix` |
| 32 | 1 | `tables_present` |
| 33 | 1 | `ref_slots` |
| 34 | 1 | `frame_flags` |
| 35 | 1 | `frame_reserved`, MUST be zero |
| 36 | 4 | `frame_bytes` |

**Bit fields of `frame_flags`**, LSB first:

```syntax
frame_flags()                                          Descriptor
    tile_map_reset                                     f(1)
    stereo_enable                                      f(1)
    frame_flags_reserved                               f(6)
```

**`homography` has no home in the syntax.** The inter predictor requires the
quantised per-eye homography — nine `int32` per eye [PAPER 2.2], or nine
`int32` plus an origin in the `warp/` implementation — and **no element of the
frame header carries it.** The 40-byte frame header is full, and the 26 pose
bytes are explicitly opaque and explicitly not interpreted by the decoding
process [SYNTAX 3.2, decision 5]. A Phase 2 stream therefore cannot presently
be decoded from the syntax as specified. This is the single largest gap in the
format. Recorded as Annex C issue **C-4**, the highest-priority open issue.
[pending WARP.md]

### 4.4.1 Constraints

`base_qp <= 63`; `quant_matrix <= 3` or `quant_matrix == 255`;
`frame_bytes >= 40` and not beyond the available bytes; after the last tile of
the last row exactly `frame_bytes` bytes MUST have been consumed
[SYNTAX 3.1].

## 4.5 Quantisation matrices and probability tables

Present, in this order, immediately after the frame header [SYNTAX 3.1].

```syntax
quant_matrices()                                       Descriptor
    custom_matrix_luma[0..63]                          f(8)
    custom_matrix_chroma[0..63]                        f(8)

table_set(k)                                           Descriptor
    for (c = 0; c < 12; c++)
        for (s = 0; s < 16; s++)
            table_delta[c][s]                          f(5)
```

`quant_matrices()` is 128 bytes: 64 for luma and alpha, then 64 for chroma,
each in raster order within the 8x8 block, Q4 with 16 meaning 1.0. Values are
clamped to `[1, 32]` on parse rather than rejected [SYNTAX 3.1, decision 12].

`table_set(k)` is exactly 120 bytes: 12 contexts x 16 symbols x 5 bits, packed
MSB first (clause 3.1). It is present if and only if bit `k` of
`tables_present` is set, and sets appear in ascending `k`.

## 4.6 Tile-row header

One per tile row, 12 bytes [SYNTAX 3.3]. In transport this structure is
replicated in every datagram of the row (clause 7.1).

```syntax
tile_row_header()                                      Descriptor
    row_frame_number                                   f(16)
    row_index                                          f(8)
    tile_count                                         f(8)
    skip_bitmap                                        f(64)
```

`row_frame_number` MUST equal the frame header's `frame_number`, and
`row_index` MUST equal the row's ordinal. `tile_count` MUST equal the number of
tiles in the row whose bit in `skip_bitmap` is clear.

## 4.7 Tile header

Two little-endian `u32` words, 8 bytes, followed by up to 3 optional bytes
[SYNTAX 4.1].

```syntax
tile_header()                                          Descriptor
    layer                                              f(2)
    eye                                                f(1)
    tile_word0_reserved                                f(1)
    tile_index                                         f(12)
    payload_len                                        f(16)
    mode                                               f(3)
    res_level                                          f(2)
    chroma444                                          f(1)
    alpha_mode                                         f(2)
    qp_delta                                           s(6)
    table_set_idx                                      f(3)
    nsub_log2                                          f(3)
    mv_present                                         f(1)
    ref_sel                                            f(2)
    tskip                                              f(1)
    wgt                                                f(2)
    tile_word1_reserved                                f(6)
```

**word0**, LSB first:

| Bits | Element |
|---|---|
| 0–1 | `layer` |
| 2 | `eye` |
| 3 | `tile_word0_reserved`, MUST be 0 |
| 4–15 | `tile_index` |
| 16–31 | `payload_len` |

**word1**, LSB first:

| Bits | Element |
|---|---|
| 0–2 | `mode` |
| 3–4 | `res_level` |
| 5 | `chroma444` |
| 6–7 | `alpha_mode` |
| 8–13 | `qp_delta` (signed) |
| 14–16 | `table_set_idx` |
| 17–19 | `nsub_log2` |
| 20 | `mv_present` |
| 21–22 | `ref_sel` |
| 23 | `tskip` |
| 24–25 | `wgt` |
| 26–31 | `tile_word1_reserved`, MUST be 0 |

Then, in order:

```syntax
tile_optional()                                        Descriptor
    if (mv_present) {
        mv_x                                           s(8)
        mv_y                                           s(8)
    }
    if (alpha_mode == 1)
        alpha_value                                    f(8)
```

### 4.7.1 Constraints

`res_level != 3`; `alpha_mode != 3`; `nsub_log2 <= 5`; `mode <= 4`;
`tile_word0_reserved` and `tile_word1_reserved` zero; `chroma444 == 1` only if
`chroma_format == 1`; `alpha_mode != 0` requires `alpha_present`; `tile_index`
equals the tile's position in the row [SYNTAX 4.1].

Note that `res_level == 3` is reserved and MUST be rejected here, while the
transport tile directory assigns it the meaning "DC-plane only" (clause 4.9).
Recorded as Annex C issue C-5.

## 4.8 Tile payload

`payload_len` bytes. The payload is not byte-addressable syntax: it is the
interleaved rANS stream of clause 6.6, and the elements below are read only
through the schedule of clause 6.6.4 [SYNTAX 9].

```syntax
tile_payload()                                         Descriptor
    for (l = 0; l < active_lanes; l++)
        lane_init_state[l]                             f(32)
    ... interleaved renormalisation bytes ...

coding_unit(u)                                         Descriptor
    cbf                                                ae(ctx_cbf)
    if (cbf == 1) {
        if (ncoef > 1)
            last_class                                 ae(ctx_last)
        if (last_raw_bits > 0)
            last_raw                                   bp(last_raw_bits)
        for (pos = last; pos >= 0; pos--) {
            level                                      ae(ctx_level)
            if (level == 15)
                level_escape                           eg3(v)
            if (level != 0)
                level_sign                             bp(1)
        }
    }
```

Derived elements referenced above:

```syntax
derived()                                              Descriptor
    active_lanes    = min(1 << nsub_log2, unit_count)
    unit_count      = (number of coding units, clause 6.6.3)
    ncoef           = (coefficients in this coding unit, clause 6.6.3)
    last            = last_base + last_raw
    last_base       = kLastBase[last_class]
    last_raw_bits   = kLastRawBits[last_class]
    ctx_cbf         = (0 for Y and A planes, 1 for Co and Cg)
    ctx_last        = (2 for Y and A planes, 3 for Co and Cg)
    ctx_level       = 4 + kLevelCtx[band][prev]
```

The payload begins with `4 * active_lanes` bytes of initial lane states, lane 0
first, each little endian. The remaining bytes are interleaved renormalisation
pairs consumed in schedule order [SYNTAX 9.5].

## 4.9 Transport encapsulation (informative here, normative in [R-19])

The bitstream above is what a stored file contains. On the wire, tiles are
carried in *tile runs*; the elements a decoder must understand are listed here
so that clause 5 can define them and clause 7 can use them. The full and
normative definition is [TRANSPORT 2, 3].

```syntax
datagram_header()                                      Descriptor
    dg_version                                         f(4)
    dg_flags                                           f(4)
    stream_id                                          f(8)
    frame_id                                           f(16)
    tile_first                                         f(16)
    dg_tile_count                                      f(8)
    layer_id                                           f(4)
    ref_delta                                          f(2)
    frag_idx                                           f(2)
    frag_count                                         f(2)
    tile_class                                         f(2)
    band                                               f(3)
    pose_hdr                                           f(1)
    caps                                               f(8)
    pose_seq                                           f(16)
    path_seq                                           f(14)
    path_id                                            f(2)
    fec_group                                          f(8)
    fec_idx                                            f(4)
    fec_k                                              f(4)
    tx_ts                                              f(32)
    dg_payload_len                                     f(16)
    enc_us                                             f(16)

tile_directory_entry()                                 Descriptor
    dir_len                                            f(12)
    dir_qp                                             f(6)
    dir_mode                                           f(3)
    dir_res_level                                      f(2)
    dir_lossless                                       f(1)
    dir_chroma444                                      f(1)
    dir_alpha                                          f(1)
    dir_reserved                                       f(6)
```

The datagram header is 24 bytes, cleartext, and is the complete associated data
of the AEAD that protects the payload [TRANSPORT 2]. The plaintext payload is
an optional 26-byte frame/pose header, then `dg_tile_count` four-byte directory
entries, then the concatenated tile bitstreams [TRANSPORT 3].

**The transport's 26-byte frame/pose header is not the bitstream's `pose`.**
Both are 26 bytes; their layouts are different and incompatible
[SYNTAX 3.2 versus TRANSPORT 3.3]. Recorded as Annex C issue C-6.
