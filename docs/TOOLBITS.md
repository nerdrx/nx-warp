# Tool-bit and tile-header allocation, bitstream minor 6

Status: **proposal**, written by the integration pass before the judges'
verdicts were all in. It is the single allocation table the tournament merge
uses. Nothing here changes a stream that does not set one of the new bits:
every entry below is additive and off by default, per TOURNEY-RULES.md.

Baseline is `merge-main` (`e311de9`), which is syntax v1.4 and whose
`NXVC_BITSTREAM_MINOR` is 4. The merged result is **v1.6**: the tournament
packages together are one minor bump, and 5 is skipped because eight of the ten
branches independently set `NXVC_BITSTREAM_MINOR 5` for their own package and a
merged stream is none of those eight streams.

The allocation principle is **one slot per tool, not one slot per branch**.
Where the two branches in a category ship the same tool under different names
(`inter-a`'s `NEAR_SKIP` and `inter-b`'s `WARP_DC`; `inter-a`'s `QUAD_MV` and
`inter-b`'s `MV_QUAD`) the slot is allocated once and the surviving branch is
renamed onto it. That makes the table below independent of the judges' choice
in four of the five categories, so the merge script does not need to know the
winners before the bit map is frozen.

---

## 1. Bits 0-23: the existing allocation (unchanged)

From `docs/SYNTAX.md` section 2.3 on `merge-main`. No tournament branch
reassigns any of these.

| bit | name | meaning | state on merge-main |
|---|---|---|---|
| 0 | `INTRA_DC_PLANE` | DC-plane intra (section 7). Mandatory in v1 | supported |
| 1 | `TRANSFORM_SKIP` | tiles may set `tskip` | supported |
| 2 | `RES_LEVEL` | tiles may set `res_level != 0` | supported |
| 3 | `CHROMA444` | the stream or its tiles may be 4:4:4 | supported |
| 4 | `ALPHA` | a 4th plane is present | supported |
| 5 | `LOSSLESS` | QP 0 + transform skip is used | supported |
| 6 | `CUSTOM_TABLES` | frames may transmit probability tables | supported |
| 7 | `NSUB_VAR` | tiles may use `nsub_log2 != 3` | supported |
| 8 | `PER_TILE_CHROMA` | 4:2:0 tiles inside a 4:4:4 stream | supported |
| 9 | `YCOCGR` | the YCoCg-R colour transform is in use | supported |
| 10 | `INTER` | inter modes are used (Phase 2) | supported |
| 11 | `WARP` | pose-warped prediction (Phase 2) | supported |
| 12 | `STEREO` | inter-view prediction (Phase 2) | supported |
| 13 | `LAYERS` | more than one layer | supported |
| 14 | `BITDEPTH10` | 10-bit samples | **reject, `VERSION`** |
| 15 | `ENT_OFFSET_TABLE` | per-substream offset table | declared, not implemented |
| 16 | `ENT_BITPLANE` | bit-plane entropy fallback | declared, not implemented |
| 17 | `INTRA_DIR` | directional intra | supported |
| 18 | `XFORM_WAVELET` | 5/3 wavelet transform | declared, not implemented |
| 19 | `XFORM_4X4_SPLIT` | per-block 4x4 transform split | **name pre-declared, implemented by `detail`** |
| 20 | `WM_ID` | tiles may set `wm_id != 0` | supported |
| 21 | `CTX_V2` | the 16-context entropy model | supported |
| 22 | `SIGN_HIDE` | sign data hiding | supported |
| 23 | `FILTER_CATMULL_ROM` | Catmull-Rom warp interpolation | **reject, `VERSION`** |

Bit 19 is the one pre-declared name a tournament branch fills in. `merge-main`
already has `#define NXVC_TOOL_XFORM_4X4_SPLIT (1ull << 19)` in
`include/nxvc/nxvc.h` and the row in section 2.3, but the constant is **absent
from `NXVC_TOOLS_SUPPORTED`**, so a stream setting it is refused today. Both
`detail-a` and `detail-b` implement exactly that bit and add it to the
supported mask. No renumbering is needed for it in either case.

## 2. Bits 24-31: the tournament allocation

**`JUDGE-detail.md` landed first and fixes `detail-a`'s two bits**, so detail
merges first and every later package renumbers around 19 and 24.
**`JUDGE-ctx.md` then merged a combination** -- ctx-a's `CTX_V3` plus ctx-b's
`TAB_V2` -- and **dropped `VEC_ENT`**, which frees a slot and settles what the
ctx package's second tool is. The rest follows the one-slot-per-tool rule.

`JUDGE-ctx.md` proposed a slightly different map (25 `XFORM_LARGE`,
26 `CTX_V3`, 27 `TAB_V2`, 28 `VEC_ENT` reserved), explicitly "offered for
`JUDGE-xform.md` to adopt or amend". The table below is the coordinator's
amendment and is the one the merge uses: `VEC_ENT` gets no reservation at all,
so a dropped tool leaves no bit with a history for a later package to inherit.

| bit | name | tool | branches claiming it | claimed | action |
|---|---|---|---|---|---|
| 19 | `XFORM_4X4_SPLIT` | per-block 4x4 split | `detail-a`, `detail-b` | 19 | **fixed by the judge**, no change |
| 24 | `INTRA_CFL` | chroma-from-luma | `detail-a`, `detail-b` | 24 | **fixed by the judge**, no change |
| 25 | `CTX_V3` | the third context model, **ctx-a's 27-context layout** | `ctx-a` (24), `ctx-b` (25) | 24 / 25 | **move to 25** |
| 26 | `TAB_V2` | variable-length table sets, **ctx-b's format** | `ctx-b` (24) | 24 | **move to 26** |
| -- | ~~`VEC_ENT`~~ | vector entropy coding | `ctx-a` (25) | 25 | **DROPPED by `JUDGE-ctx.md`**, no bit allocated |
| 27 | `XFORM_LARGE` | 16x16 and 32x32 transforms | `xform-a`, `xform-b` | 24 | **move 24 -> 27** |
| 28 | `NEAR_SKIP` | near-skip DC/ramp correction | `inter-a` `NEAR_SKIP` (24), `inter-b` `WARP_DC` (24) | 24 | **move to 28**; `inter-b` renamed onto `NEAR_SKIP` |
| 29 | `QUAD_MV` | four quadrant vectors per tile | `inter-a` `QUAD_MV` (25), `inter-b` `MV_QUAD` (25) | 25 | **move to 29**; `inter-b` renamed onto `QUAD_MV` |
| 30 | `SUBTILE_INTRA` | one quadrant drops the predictor | `inter-a` only (26) | 26 | **move to 30**; unallocated if `inter-b` wins |
| 31 | `TILE_EXT` | the tile extension byte | none -- new here | -- | section 4, option A only |

`rdo-a` and `rdo-b` allocate **no tool bit at all**: both are pure encoder
packages (rate-distortion search, lambda, effort presets) and change no
normative syntax. They are the only two branches whose merge cannot collide on
this table. They collide violently on the conformance vectors instead --
section 5.

Bits 32-63 stay reserved and must be zero.

### 2.1 The collision, stated plainly

**Six of the ten branches put their first new tool on bit 24** and four of them
put a second tool on bit 25:

| bit | claimed by |
|---|---|
| 24 | `xform-a` `XFORM_LARGE`, `xform-b` `XFORM_LARGE`, `detail-a` `INTRA_CFL`, `detail-b` `INTRA_CFL`, `ctx-a` `CTX_V3`, `ctx-b` `TAB_V2`, `inter-a` `NEAR_SKIP`, `inter-b` `WARP_DC` |
| 25 | `ctx-a` `VEC_ENT` (dropped), `ctx-b` `CTX_V3`, `inter-a` `QUAD_MV`, `inter-b` `MV_QUAD` |
| 26 | `inter-a` `SUBTILE_INTRA` |

Every branch was correct in isolation -- 24 was the next free bit on `main` for
all of them -- and so every stack of two or more branches needs the
renumbering in the table above. Only `detail`'s bit 24 survives untouched,
because `JUDGE-detail.md` reached its verdict first and pinned it.

Bits 24-63 were reserved-must-be-zero on `merge-main`, so a v1.4 decoder
refuses any of the seven new bits with an "unsupported tool" status regardless
of where they land. The renumbering is therefore invisible to conformance: it
only has to be internally consistent.

---

## 3. Tile header word1: the real contention

`merge-main`'s tile header is two u32 words, 8 bytes, plus up to 3 optional
bytes (`mv_x`/`mv_y` or `disparity`, then `alpha_value`). word1's high end
today:

| bits | field | source |
|---|---|---|
| 24-25 | `wgt` | existing |
| 26-27 | `wm_id` | existing, gated by tool bit 20 |
| 28-31 | reserved, must be 0 | **the only free space: 4 bits** |

Demand on those 4 bits:

| branch | fields wanted | bits |
|---|---|---|
| `xform-a` | `xform_size` (28-29) | 2 |
| `xform-b` | `xform` (28-29) | 2 |
| `detail-a` | `split4x4` (28) | 1 |
| `detail-b` | none -- the split flag is coded in the payload | 0 |
| `inter-a` | `near_skip` (28), `near_skip_ac` (29), `quad_mv` (30), `sub_intra` (31) | 4 |
| `inter-b` | `mv_quad` (28); its DC correction is a `dc_bitmap` in the **tile-row** header, not here | 1 |
| `ctx-a`, `ctx-b`, `rdo-a`, `rdo-b` | none | 0 |

`wm_id` at 26-27 is **not** actually contested: no branch moves or reuses it.
The contention is entirely over 28-31, and it is real:

| stack | demand | free | verdict |
|---|---|---|---|
| xform + detail-b + inter-b | 2 + 0 + 1 = 3 | 4 | fits, 1 spare |
| xform + detail-a + inter-b | 2 + 1 + 1 = 4 | 4 | **fits exactly, 0 spare** |
| xform + detail-b + inter-a | 2 + 0 + 4 = 6 | 4 | **2 bits over** |
| xform + detail-a + inter-a | 2 + 1 + 4 = 7 | 4 | **3 bits over** |

`JUDGE-detail.md` has already chosen `detail-a`, so the `split4x4` bit is
spent -- and because detail merges first, it keeps word1 bit 28 exactly as
authored and `xform` is the package that moves. The layout therefore turns
entirely on the inter verdict.

## 4. Proposed word1 layout

### Option A -- `inter-a` wins: one optional extension byte

| bits | field | gated by |
|---|---|---|
| 24-25 | `wgt` | existing |
| 26-27 | `wm_id` | tool bit 20 `WM_ID` |
| 28 | `split4x4` | tool bit 19 `XFORM_4X4_SPLIT` -- **as `detail-a` authored it; detail merges first, so this bit does not move**. Meaningful only when `xform_size == 8`; see 4.2 |
| 29-30 | `xform_size` | tool bit 27 `XFORM_LARGE` -- moved down from `xform`'s 28-29 |
| 31 | `tile_ext` | tool bit 31 `TILE_EXT` -- one extension byte follows the tile header |

`inter-a`'s four flags move out of word1 and into the extension byte, which is
read **after** word1 and **before** the `mv`/`disparity` and `alpha_value`
bytes, so the existing optional-byte order is preserved and every field keeps a
fixed offset once `tile_ext` is known:

| ext bit | field |
|---|---|
| 0 | `near_skip` |
| 1 | `near_skip_ac` |
| 2 | `quad_mv` |
| 3 | `sub_intra` |
| 4-7 | reserved, must be 0 |

Constraints: `tile_ext == 1` requires tool bit 31 `TILE_EXT`; an ext byte whose
bits 4-7 are nonzero is `BITSTREAM`; `near_skip_ac` requires `near_skip`;
`near_skip`, `quad_mv` and `sub_intra` each require their own tool bit (28, 29,
30) and `mode != INTRA`. A tile that needs none of the four must set
`tile_ext == 0` and send no byte, so the encoder cannot pad.

**Byte cost.** One byte, on the tiles that set `tile_ext` only. Zero on every
intra stream, zero on any stream that does not set `TILE_EXT`, and zero on an
inter tile using none of the four tools. The honest worst case is the one that
matters: Appendix A decision 1 records that the 8-byte tile header is 13.7% of
the frame at QP 36, so a ninth byte is **+12.5% of the header, about +1.7% of
the frame** on a tile that takes it -- and near-skip tiles are exactly the
small-payload tiles where a header byte is proportionally worst. That is the
price of `inter-a`'s fourth flag, and it should be weighed against `inter-a`'s
BD-rate margin over `inter-b` rather than waved through.

*Option A-2, if `inter-a`'s author confirms exclusivity.* If `near_skip` and
`quad_mv` cannot both be set on one tile, and `sub_intra` cannot combine with
`near_skip`, the four flags collapse to a 2-bit enum
(0 none, 1 `near_skip`, 2 `near_skip` + ac, 3 `quad_mv`) plus one bit for
`sub_intra` -- 3 bits, which does **not** fit either once `xform_size` and
`split4x4` are placed. It only fits if `split4x4` also moves. Not recommended:
it buys one byte at the cost of making three independent tools one field, which
is exactly the "one implementation per idea" the rules ask for the opposite of.

### Option B -- `inter-b` wins: no extension byte, no cost

| bits | field | gated by |
|---|---|---|
| 24-25 | `wgt` | existing |
| 26-27 | `wm_id` | tool bit 20 |
| 28 | `split4x4` | tool bit 19 `XFORM_4X4_SPLIT` -- **as `detail-a` authored it**. Meaningful only when `xform_size == 8`; see 4.2 |
| 29-30 | `xform_size` | tool bit 27 `XFORM_LARGE` -- moved down from `xform`'s 28-29 |
| 31 | `quad_mv` | tool bit 29 `QUAD_MV` (`inter-b`'s `mv_quad`, renamed) |

Word1 is then **exactly full**, tool bit 31 `TILE_EXT` is not allocated, and
the byte cost of the whole tournament is zero. `inter-b`'s near-skip
correction costs nothing here either: it lives in the tile-row header as
`dc_bitmap` plus a nine-byte record per corrected tile, which no other branch
touches.

This is the layout the merge should prefer unless `inter-a` wins on BD-rate by
a margin that clearly beats the extension byte.

### 4.2 `split4x4` and `xform_size` compose, they do not merge

Decided: the two stay **separate fields**, because they act at different
granularities -- `xform_size` is per tile and selects 8, 16 or 32, while
`split4x4` is per 8x8 block and splits it to 4x4. Collapsing them into one
size ladder would conflate a tile-level choice with a block-level one.

The rule that lets both be set on the same stream:

> `split4x4` is present and meaningful **only when the tile's
> `xform_size == 8`**. A tile with `xform_size != 8` and `split4x4 == 1` is
> **`BITSTREAM`**.

In word1 terms: bit 28 must be zero unless bits 29-30 select the 8x8
transform, and when bit 28 is zero the payload codes no per-block split flag.
It needs a rejection vector of its own and an Appendix A entry; see
`docs/MERGE-PLAN.md` 4.4.

### 4.3 The reserved-tile-bit reject vector

`tests/vectors/r09_reserved_tile_bit.nxv` pins "word1 bits 28-31 must be zero".
Every layout above consumes those bits, so the vector must move to the highest
still-reserved bit and be re-hashed. Under option B word1 is full, so **r09
must move to word0 bit 3**, the only remaining must-be-zero header bit, and its
name should change to `r09_reserved_tile_bit` on word0. Three branches already
rewrite this vector independently (`detail-a` moved it to bit 29, `xform-a`
rewrote it, `inter-b` adds `r32_tile_reserved_29`), which is why it appears in
almost every pairwise conflict in `docs/MERGE-PLAN.md`.

---

## 5. What does not fit in a bit table

Two categories change no syntax and allocate no bit, but are the loudest
merge conflicts:

* **`rdo-a` / `rdo-b`** rewrite the encoder's decisions, so **every one of the
  56 existing conformance vectors changes bytes** and all 100 lines of
  `tests/vectors/vectors.md5` are rewritten. Any stack containing an rdo branch
  must regenerate vectors rather than merge them.
* **`ctx-a`** additionally rewrites four existing vectors (`v34`, `v41`, `v42`,
  `v44`) because the context model changes the coded bytes of streams that
  already existed. `ctx-b` does not.

Both are handled by regeneration, not by hand-merging: see
`docs/MERGE-PLAN.md` section 4.

## 6. Appendix A numbering

`merge-main`'s decisions appendix ends at **52**. Every branch numbers its new
entries from 53, so every stack collides:

| branch | new entries | count |
|---|---|---|
| `xform-a` | 53-56 | 4 |
| `xform-b` | 53-54 | 2 |
| `detail-a` | 53-59 | 7 |
| `detail-b` | 53-59 | 7 |
| `ctx-a` | 53-60 | 8 |
| `ctx-b` | 53-56 | 4 |
| `inter-a` | **none** | 0 |
| `inter-b` | 53-56 | 4 |
| `rdo-a` | 53 | 1 |
| `rdo-b` | 53-54 | 2 |

`inter-a` adds no decisions appendix entry at all, which TOURNEY-RULES.md
criterion (4) asks for. Worth raising with its judge; it does not block the
merge.

The renumbering rule the merge order in `docs/MERGE-PLAN.md` implies is: each
branch's entries are renumbered to start after the running total, in merge
order. For the trial stack (xform-b, detail-a, ctx-b, inter-a, rdo-b) that is
xform-b 53-54, detail-a 55-61, ctx-b 62-65, inter-a none, rdo-b 66-67.

## 6.1 The `detail-a` merge checklist

`JUDGE-detail.md` merges `tourney/detail-a` **with four defects fixed and five
items taken from `detail-b`**. These are merge-time obligations, not follow-ups,
and detail merges first, so they are the first work in the whole tournament:

1. **Move `detail-a`'s new `nxvc_config` / `nxvc_tile_info` fields to the END
   of their structs.** The ABI is additive; `detail-b`'s appended layout is the
   model. `detail-a` inserted its fields mid-struct, which silently changes the
   offset of everything after them.
2. **Take from `detail-b`:** the encode-stats counters and `--stats`, its four
   extra conformance vectors, its finer rejection vectors, its CLI validation,
   and `quantize_unit()`.
3. **Add the sentence pinning that `round(2^15 / d)` has no ties for
   `d` in [1, 255]**, so the reciprocal table is well defined rather than
   dependent on a rounding mode.
4. **Regenerate the rate columns and gate sentences in `RESULTS-detail-a.md`
   with the fixed harness.** Both branches' absolute bitrates were inflated
   roughly 6x by `--frames` -- the same measurement bug `ctx-b` fixed in
   `run_codec()` and that `scripts/resolve-compare-py.py` carries into the
   merge (section 2 of `docs/MERGE-PLAN.md`). The BD-rate comparisons are
   ratios and survive; the absolute numbers do not.

Item 4 is why the `compare.py` resolution matters beyond `ctx-b`: **every
branch's absolute bitrate figure is suspect** until re-measured on the fixed
harness.

## 7. Summary: the merged supported mask

Assuming the trial stack, `NXVC_TOOLS_SUPPORTED` gains, over `merge-main`:

```
NXVC_TOOL_XFORM_4X4_SPLIT   (1ull << 19)   detail-a   (judge-fixed)
NXVC_TOOL_INTRA_CFL         (1ull << 24)   detail-a   (judge-fixed)
NXVC_TOOL_CTX_V3            (1ull << 25)   ctx-a's model  (ships OFF by default)
NXVC_TOOL_TAB_V2            (1ull << 26)   ctx-b's format (ships OFF by default)
NXVC_TOOL_XFORM_LARGE       (1ull << 27)   xform
NXVC_TOOL_NEAR_SKIP         (1ull << 28)   inter
NXVC_TOOL_QUAD_MV           (1ull << 29)   inter
NXVC_TOOL_SUBTILE_INTRA     (1ull << 30)   inter-a only
NXVC_TOOL_TILE_EXT          (1ull << 31)   inter-a only, option A
```

and `NXVC_BITSTREAM_MINOR` becomes 6. (`JUDGE-ctx.md` step 13 says v1.5,
correctly for the ctx package on its own; the tournament merges every package
under **one** minor bump, and 5 is skipped because eight of the ten branches
each set 5 for their own package.)

**`CTX_V3` and `TAB_V2` ship OFF by default**, per `JUDGE-ctx.md` step 6 and
ctx-a's convention: `vk/decoder/passA` does not implement bit 25 yet, so both
stay out of `kToolsSupported` in `nxvc_vkdec_parse.cpp` and the Vulkan decoder
refuses them with `VERSION`. That is what keeps the reference encoder's
**default** output decodable by the Vulkan decoder, which is the property the
whole tournament's tool-bit discipline exists to protect.
