# 8. Profiles, levels and capabilities

Version 1 signals what a stream demands of a decoder in three independent
places, and it is important to keep them apart:

| Mechanism | Element | Force |
|---|---|---|
| Tool mask | `tools` | **Normative and enforceable.** A decoder MUST refuse a stream with a set bit it does not implement |
| Profile | `profile` | A name for a tool subset. Marked *informative* by [R-18] |
| Level | `level` | A name for a set of numeric limits. Marked *informative* by [R-18], and no limits have been defined |
| Transport capabilities | `caps` | Negotiated out of band, echoed per datagram, enforced per datagram [TRANSPORT 2.2] |

The design intent is that `tools` is the only forward-compatibility gate and
that profiles and levels are shorthand for humans [SYNTAX 2]. That intent has
a hole in it: at least one bit-exactness-critical choice — the interpolation
filter — is attached to the profile and to nothing else. See clause 8.2 and
Annex C issue C-7.

## 8.1 Capability negotiation

Negotiation is an **intersection**. The client sends its own `tools` mask and
its `profile` and `level` in the connect handshake; the server MUST only set
bits the client offered. A decoder that sees an unknown mandatory bit refuses
the stream rather than guessing. This is the same forward-compatibility scheme
as Vulkan feature bits, and it needs no version arithmetic
[PAPER 1.2, SYNTAX 2].

Transport capabilities are negotiated the same way and additionally enforced
per datagram, so a capability can be withdrawn mid-session — with one band of
skew — without a version bump [TRANSPORT 2.2].

## 8.2 Profiles

| Profile | Value | Intent [PAPER 1.2, 1.6, 2.2, 6.1] |
|---|---|---|
| Lite | 0 | Low-bitrate wireless. Bilinear interpolation in the warp. Bit-plane entropy fallback permitted. Stereo inter-view off. 64x64 tiles |
| Full | 1 | The default. Catmull-Rom interpolation in the warp. rANS entropy coding |
| Pro | 2 | High-bitrate USB / WiFi 7. The only profile in which a lossless tile may be fragmented across up to four datagrams [PAPER 1.8, 6.1] |

**Hybrid is not a profile.** A hybrid stream is one whose `layer_desc[0]` has
`layer_type` 1 (HEVC_NAL) or 2 (H264_NAL): layer 0 is decoded by the device's
licensed hardware decoder and the enhancement layers use this format unchanged.
Every profile can be hybrid or pure-compute; the client's `tools` mask and
`layer_type` are the only things that differ, which is the point — one format,
two decoders, no fork [PAPER 1.7]. What the hybrid path additionally requires
of a decoder is unspecified. [pending HYBRID.md]

**The unresolved part.** Bilinear versus Catmull-Rom changes every predicted
sample, so it is normative in the strongest sense: two decoders that disagree
about it produce different pictures from the same bitstream and neither can be
called conforming. Today the only thing that selects it is `profile`, which the
normative syntax document marks informative, and no tool bit distinguishes the
two filters. Either `profile` must become normative, or a tool bit
(`FILTER_CATMULL_ROM`, say) must be defined. Annex C issue C-7.
[pending WARP.md]

## 8.3 Levels

**No level definition exists.** [PAPER 1.2] describes `level` only as "max
tiles/frame class and max decode work", and neither [R-18] nor [R-19] defines
a single limit. This clause therefore states the *dimensions* a level must
constrain and the working points the design assumes, so that a level table can
be written against something. **None of the numbers below is normative.**
[pending SYNTAX.md]

### 8.3.1 Dimensions a level must constrain

1. **Maximum picture dimensions per eye.** Bounded above by the syntax at
   4096x4096 (`width`, `height` in `[16, 4096]`) and, more tightly, by the
   64-column limit that the 64-bit `skip_bitmap` imposes: 4096 luma samples of
   width (clause 4.2.1). Annex C issue C-3 is precisely that the transport's
   own configuration violates this.
2. **Maximum tiles per frame**, which with two eyes and up to four layers is
   not simply `cols * rows`.
3. **Maximum coded bytes per band**, since the band is the pipelining and
   feedback unit and the receiver's buffers are sized from it.
4. **Maximum coded bytes per tile.** Three different limits already exist and
   they do not agree: `payload_len` allows 65535, the transport directory's
   `dir_len` allows 4095, and the run budget yields
   `max_tile_bytes = 1400 - 24 - 4 = 1372` for an unfragmented tile
   [PAPER 4.1, TRANSPORT 3.4]. Annex C issue C-9.
5. **Maximum decode work**, which is what the field actually means and what no
   document has yet expressed in countable units. Coded symbols per frame,
   coefficients per frame, or predicted samples per second are the candidates.

### 8.3.2 Working points the design assumes (informative)

| Quantity | Value | Source |
|---|---|---|
| Per-eye picture, first target | 2048x2048 or 2160x2160 | [PAPER 2, 1.1] |
| Stereo picture | 4320x2160 | [TRANSPORT 1] |
| Tiles per eye at 2160x2160, 64x64 | 1156 | [PAPER 1.1] |
| Tiles per stereo frame | 2312 | [TRANSPORT 1] |
| Tile rows, bands, rows per band | 34, 6, 6 (last band 4) | [TRANSPORT 1] |
| Tiles in a band | 408 (last band 272) | [TRANSPORT 1] |
| Reference ring slots | 4 | [PAPER 6.6] |
| Encoder shadow history | 8 frames | [PAPER 2.6] |
| Rolling intra refresh period | 1/180 of tiles per frame | [PAPER 2.6] |
| Datagram budget, Ethernet path | 1400 bytes | [PAPER 4.1] |
| Average tile at 150 Mbit/s, 90 Hz | ~90 bytes | [PAPER 6.1] |
| Worst-case lossless 64x64 4:4:4 8-bit tile | ~12 KB | [PAPER 1.8] |
| Decode budget, first target hardware | 4 ms per frame at 90 Hz | [PAPER 2.11, 3.2.5] |

### 8.3.3 Constraints that already have normative force

These are syntax constraints, not level limits, and apply to every stream
regardless of `level`:

* `width`, `height` in `[16, 4096]` and even; `ceil(width / 64) <= 64`;
* `eyes` in `{1, 2}`; `bit_depth` in `{8, 10}`; `num_layers` in `[1, 4]`;
* `nsub_log2 <= 5`, so at most 32 lanes per tile;
* `dg_tile_count` in `[1, 255]`, so at most 255 tiles per run;
* `fec_k` in `[1, 10]`, and at most 4 fragments per oversize tile;
* `path_id` in `[0, 1]` in version 1, the field allowing 4.

## 8.4 Tool bits

The 64-bit `tools` mask [SYNTAX 2.2]. Bits 20 to 63 are reserved and MUST be
zero.

| Bit | Name | Meaning |
|---|---|---|
| 0 | `INTRA_DC_PLANE` | DC-plane intra (clause 6). **Mandatory in v1** |
| 1 | `TRANSFORM_SKIP` | Tiles may set `tskip` |
| 2 | `RES_LEVEL` | Tiles may set `res_level != 0` |
| 3 | `CHROMA444` | The stream or its tiles may be 4:4:4 |
| 4 | `ALPHA` | A fourth plane is present |
| 5 | `LOSSLESS` | QP 0 with transform skip is used |
| 6 | `CUSTOM_TABLES` | Frames may transmit probability tables |
| 7 | `NSUB_VAR` | Tiles may use `nsub_log2 != 3` |
| 8 | `PER_TILE_CHROMA` | 4:2:0 tiles inside a 4:4:4 stream |
| 9 | `YCOCGR` | The YCoCg-R colour transform is in use |
| 10 | `INTER` | Inter modes are used (Phase 2) |
| 11 | `WARP` | Pose-warped prediction (Phase 2) |
| 12 | `STEREO` | Inter-view prediction (Phase 2) |
| 13 | `LAYERS` | More than one layer |
| 14 | `BITDEPTH10` | 10-bit samples |
| 15 | `ENT_OFFSET_TABLE` | Per-substream offset table instead of one interleaved stream |
| 16 | `ENT_BITPLANE` | Bit-plane entropy fallback |
| 17 | `INTRA_DIR` | Directional intra |
| 18 | `XFORM_WAVELET` | 5/3 wavelet transform |
| 19 | `XFORM_4X4_SPLIT` | Per-block 4x4 transform split |

Bits 15 to 19 are declared but their behaviour is specified nowhere: they name
version 2 tools and the fallbacks of [PAPER 1.6]. A version 1 decoder refuses
any stream that sets them, which is the correct and sufficient behaviour, but
it means the mask contains five bits that no document defines. Annex C issue
C-17.

Bit 14 (`BITDEPTH10`) is different in kind: `bit_depth == 10` is a legal
stream-header value, so a decoder may be asked to accept it, yet no 10-bit
sample domain or quantiser scaling is specified (clause 6.2, Annex C issue
C-14).

## 8.5 The Phase 1 subset

A Phase 1 decoder implements everything except inter prediction. It MUST
[SYNTAX 12]:

* accept `mode == INTRA` and reject `WARP_SKIP`, `STATIC_MV`, `WARP_MV` and
  `STEREO` with an **unsupported** status, distinct from a malformed-bitstream
  status;
* reject a nonzero `skip_bitmap`, since a skip references a frame it cannot
  have;
* reject `eyes != 1`, `num_layers != 1`, `bit_depth != 8`, `layer != 0`,
  `eye != 0`, and any tool bit outside the supported set;
* **parse** `mv_present`, `ref_sel` and `wgt` correctly even though it cannot
  use them, so that a Phase 2 stream is cleanly refused rather than misparsed.

That last requirement is why clause 4 specifies the full v1 syntax including
the inter fields, rather than a Phase 1 subset.

## 8.6 Transport capability bits

Negotiated at connect and echoed in `caps` on every datagram
[TRANSPORT 2.2].

| Bit | Name | Meaning |
|---|---|---|
| 0 | `CAP_FEC` | RS FEC groups are in use; the payload budget is reduced |
| 1 | `CAP_MULTIPATH` | More than one path is active; `path_id` may be nonzero |
| 2 | `CAP_JUMBO` | The path MTU probe found more than 1500; the run budget is the probed value |
| 3 | `CAP_FRAGMENT` | Oversize lossless tiles may be fragmented |
| 4 | `CAP_POSE_HDR` | `pose_hdr` may be set |
| 5 | `CAP_RLE_FEEDBACK` | The feedback bitmap may use RLE mode |
| 6–7 | — | Reserved, MUST be 0 |

There is no defined relationship between a transport capability and a tool bit,
even where they describe the same thing — `CAP_FRAGMENT` and the Pro profile's
fragmentation rule, for instance. Annex C issue C-18.
