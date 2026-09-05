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

## 2. Bits 24-29: the tournament allocation

**`JUDGE-detail.md` landed first and fixes `detail-a`'s two bits**, so detail
merges first and every later package renumbers around 19 and 24.
**`JUDGE-inter.md` then merged another combination -- `inter-a` as the base with
`inter-b`'s tile-row-header syntax for the near-skip correction -- and
**dropped sub-tile intra**, which together removed the word1 pressure that the
tile extension byte existed to relieve (section 4).
`JUDGE-ctx.md` merged a combination** -- ctx-a's `CTX_V3` plus ctx-b's
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
| 28 | `NEAR_SKIP` | near-skip DC correction, **in `inter-b`'s tile-row header form** | `inter-a` `NEAR_SKIP` (24), `inter-b` `WARP_DC` (24) | 24 | **move to 28**; the name is `inter-a`'s, the **syntax is `inter-b`'s** |
| 29 | `QUAD_MV` | four quadrant vectors per tile | `inter-a` `QUAD_MV` (25), `inter-b` `MV_QUAD` (25) | 25 | **move to 29**; `inter-b` renamed onto `QUAD_MV` |
| -- | ~~`SUBTILE_INTRA`~~ | one quadrant drops the predictor | `inter-a` (26) | 26 | **DROPPED by `JUDGE-inter.md`**, no bit allocated |
| -- | ~~`TILE_EXT`~~ | the tile extension byte | -- | -- | **not needed** -- see section 4 |
| 30 | `ENTROPY_LITE` | the table-free, fully parallel entropy coding | `exp/entropy-lite` (24) | 24 | **move to 30**, the first bit the tournament left free |

`rdo-a` and `rdo-b` allocate **no tool bit at all**: both are pure encoder
packages (rate-distortion search, lambda, effort presets) and change no
normative syntax. They are the only two branches whose merge cannot collide on
this table. They collide violently on the conformance vectors instead --
section 5.

**Bits 31-63 stay reserved and must be zero.** The tournament allocates seven
bits, 19 and 24-29, and stops there: `JUDGE-ctx.md` dropped `VEC_ENT` and
`JUDGE-inter.md` dropped sub-tile intra, and neither leaves a reservation
behind.

**Bit 30 is `ENTROPY_LITE`**, allocated after the tournament from
`exp/entropy-lite`, which had developed it on bit 24 like everything else.
It is not a tournament package and did not compete: it is a *decoder-side*
lever, and it is the only one in the format that changes the bitstream in
order to buy Pass A time. It ships **off** and is negotiated -- see section 8.

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

## 3. Tile header word1: the contention, and how it dissolved

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

`JUDGE-detail.md` chose `detail-a`, so the `split4x4` bit is spent -- and
because detail merges first, it keeps word1 bit 28 exactly as authored and
`xform` is the package that moves.

**The inter verdict then dissolved the overflow rather than packing around
it.** `JUDGE-inter.md` took `inter-b`'s tile-row-header form for the near-skip
correction and dropped sub-tile intra, so `inter-a`'s demand of four word1 bits
became at most one (`quad_mv`, and even that only if the row bitmap does not
already name the tiles). Demand is now 1 + 2 + 1 = 4 against 4 free. The
extension byte of the earlier draft is not needed; section 4 is the settled
layout.

## 4. The word1 layout (settled)

**There is one layout, and no extension byte.** `JUDGE-inter.md` moved the
near-skip correction out of the tile header into the **tile-row header**
(`inter-b`'s form: nine bytes per corrected tile, gated by a second per-row
bitmap) and dropped sub-tile intra. That removed three of the four word1 bits
`inter-a` wanted, and what remains fits exactly:

| bits | field | gated by |
|---|---|---|
| 24-25 | `wgt` | existing |
| 26-27 | `wm_id` | tool bit 20 `WM_ID` |
| 28 | `split4x4` | tool bit 19 `XFORM_4X4_SPLIT` -- **as `detail-a` authored it; detail merges first, so this bit does not move**. Meaningful only when `xform_size == 8`; see 4.2 |
| 29-30 | `xform_size` | tool bit 27 `XFORM_LARGE` -- moved down from `xform`'s 28-29 |
| 31 | `quad_mv` | tool bit 29 `QUAD_MV` -- **only if the flag is needed in the tile header at all**; if the tile-row bitmap already names the tiles carrying quadrant vectors, word1 bit 31 stays reserved |

Word1 is full or has one bit spare, and **the byte cost of the whole
tournament is zero**. The tile header stays 8 bytes plus its existing up-to-3
optional bytes.

This is worth stating plainly because the earlier draft of this file argued at
length about a ninth header byte. The judge's decision made the argument moot,
and by the better route: `inter-b`'s form is warp-only and buys **+2.10 dB on
the chain**, which is why it was taken over `inter-a`'s word1-bit tile
structure. The near-skip correction now costs no tile-header bit at all, and
its nine bytes are spent only on the tiles a row bitmap actually names.

`NEAR_SKIP` (bit 28) therefore gates a **tile-row header** structure, not a
word1 bit -- the only tool in the tournament that does. Its normative text
belongs in section 3.3, not 4.1.

### 4.1 The reserved-tile-bit reject vector

`tests/vectors/r09_reserved_tile_bit.nxv` pins "word1 bits 28-31 must be zero".
The layout above consumes 28-30 and possibly 31, so the vector must move and be
re-hashed. If word1 bit 31 stays reserved it can stay in word1; if `quad_mv`
takes it, **r09 moves to word0 bit 3**, the only remaining must-be-zero header
bit, and its name should say so. Three branches already rewrite this vector
independently (`detail-a` moved it to bit 29, `xform-a` rewrote it, `inter-b`
adds `r32_tile_reserved_29`), which is why it appears in almost every pairwise
conflict in `docs/MERGE-PLAN.md`.

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
NXVC_TOOL_NEAR_SKIP         (1ull << 28)   inter-a's name, inter-b's syntax
NXVC_TOOL_QUAD_MV           (1ull << 29)   inter
NXVC_TOOL_ENTROPY_LITE      (1ull << 30)   exp/entropy-lite (ships OFF)
```

and `NXVC_BITSTREAM_MINOR` becomes 6. (`JUDGE-ctx.md` step 13 says v1.5,
correctly for the ctx package on its own; the tournament merges every package
under **one** minor bump, and 5 is skipped because eight of the ten branches
each set 5 for their own package.)

**`CTX_V3` and `TAB_V2` ship OFF by default**, per `JUDGE-ctx.md` step 6 and
ctx-a's convention. That was originally also a decoder constraint --
`vk/decoder/passA` did not implement bit 25, so both stayed out of
`kToolsSupported` in `nxvc_vkdec_parse.cpp` and the Vulkan decoder refused
them with `VERSION`. **It no longer is**: the GPU decoder implements bits 19,
24, 25 and 26 and decodes all four with zero mismatching samples on lavapipe,
RADV and an Adreno 650 (`vk/decoder/README.md`, "Realigning to bitstream minor
version 6"). They stay off because they are negotiated tools whose value the
encoder cannot judge, not because anything refuses them.

The property that discipline exists to protect -- that the reference encoder's
**default** output is decodable by the Vulkan decoder -- is what forced the
realignment rather than what the realignment threatened. The merged default
sets `XFORM_4X4_SPLIT` and `INTRA_CFL`, so on `merge-main` the GPU decoder
refused 60 of its own conformance streams and failed 85 more at the handshake;
it accepts them now.

**The Phase 2 bits are in as well.** The GPU decoder implements bits 10
`INTER`, 11 `WARP`, 12 `STEREO`, 28 `NEAR_SKIP` and 29 `QUAD_MV`, and decodes
the sixteen Phase 2 vectors and refuses the sixteen Phase 2 rejection vectors
with zero mismatching samples on lavapipe, RADV and an Adreno 650
(`vk/decoder/README.md`, "The inter path"). It also implements the decoder
half of clause 6.11 concealment, `nxvc_vk_decoder_mark_missing()`.
**Bit 27 `XFORM_LARGE` is in too**, as Pass B build variants: exact against
`ref/` on lavapipe and RADV over all 228 conformance streams, but the Adreno
650 mis-decodes the streams that reach the large module (the private-memory
failure mode of `docs/ADRENO-RULES.md`; open issue in `vk/decoder/README.md`).
An encoder must not select `xform_size != 0` for an Adreno client until that
is fixed. **`ENTROPY_LITE` (bit 30) is the one still refused**: the kernel
exists and the decoder does not offer the bit. Both are off by default, so
the property holds.

---

## 8. Bit 30: `ENTROPY_LITE`, and why a tool can ship off and still matter

Every other tool in this table is a rate tool: it is on when it pays in
BD-rate and off when it does not, and the encoder decides. `ENTROPY_LITE` is
not that, and the difference is worth stating because the table above cannot
express it.

It **costs** rate -- **+43.0 %** of the payload at 4:4:4 and **+31.9 %** at
4:2:0, QP 24, measured on the merged encoder over the full clip -- and it
**buys**
decode time, by removing the serial rANS chain that Pass A is latency-bound
on: a tile's payload becomes five byte-aligned sections whose per-unit offsets
follow from three prefix sums over quantities already read, so every coding
unit decodes independently, and in the `FIXED` variant every coefficient's bit
position is computable, so one thread can decode one coefficient.

Measured on a Pico 4: **Pass A 138.5 ms -> 18.4 ms, 7.5x.** It is the only
bitstream-side lever that reaches the Adreno frame budget at all. On a desktop
RADV, where Pass A already fits inside its budget, the same tool is 4.1x
faster for bits nobody needed to spend.

That is why it is **negotiated rather than defaulted**. Whether the trade is
worth making depends on the decoder's own measured Pass A time, which the
encoder cannot know and must not guess. So:

* `nxvc_config::entropy_lite` is 0 by default and the tool bit is unset;
* a decoder that wants it says so at the handshake, exactly as it says which
  other tools it implements, and the encoder obliges;
* everything else in the format composes with it unchanged, because it changes
  how coefficients are written and not which ones there are.

Two variants are defined. **`FIXED` is the one this merge ships**: fixed-width
magnitude fields, which is what makes a coefficient's bit position computable
and therefore what makes the per-coefficient parallelism possible. **`RICE`
stays in the syntax and stays off**: it is worth 1-4 % of rate above about
140 Mbit/s and gives up exactly the property the tool exists for. Keeping it
defined costs a `table_set` value and documents the alternative that was
measured; shipping it would mean shipping the variant that does not buy the
thing.

The variant selector is the tile header's existing `table_set` field, which a
stream with no probability tables has nothing else to mean -- so the tool
costs no header bit beyond its tool bit.
