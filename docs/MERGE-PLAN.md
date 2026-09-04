# Tournament merge plan

Companion to `docs/TOOLBITS.md`, which owns the bit allocation. This file owns
the **order**, the **conflict matrix** and the **specific manual resolutions**.

**Two normative decisions are fixed** and the merge carries them rather than
re-litigating them: the single transform family and its gain invariant
(section 4.4) and `CTX_V3`'s per-coding-unit conditioning (section 4.5).

Everything here was measured, not guessed: every merge below was actually run
with `git merge --no-commit --no-ff` on a throwaway branch off `merge-main` and
then aborted. Baseline for all of it is `merge-main` = `e311de9`.

---

## 1. Branch survey

All ten tournament branches fork from **`e4e85af`**. `merge-main` has moved on
since (the band-limited corpus v2 and `tourney/metric`), which is why a plain
`git diff merge-main tourney/<x>` looks like each branch deletes half the
quality harness -- it does not; that is the diff reading `merge-main`'s newer
files as absent from the older fork point. Every number below comes from
three-way merges, not from `git diff`.

| branch | commits since fork | new tool bits | word1 bits | rewrites existing vectors | Appendix A |
|---|---|---|---|---|---|
| `xform-a` | 12 | 1 (24 `XFORM_LARGE`) | 28-29 | `r09` | 53-56 |
| `xform-b` | 11 | 1 (24 `XFORM_LARGE`) | 28-29 | none | 53-54 |
| `detail-a` | 12 | 19 + 24 `INTRA_CFL` | 28 | `r09` | 53-59 |
| `detail-b` | 18 | 19 + 24 `INTRA_CFL` | none | none | 53-59 |
| `ctx-a` | 7 | 24 `CTX_V3`, 25 `VEC_ENT` | none | `v34 v41 v42 v44` | 53-60 |
| `ctx-b` | 12 | 24 `TAB_V2`, 25 `CTX_V3` | none | none | 53-56 |
| `inter-a` | 12 | 24 `NEAR_SKIP`, 25 `QUAD_MV`, 26 `SUBTILE_INTRA` | 28,29,30,31 | none | **none** |
| `inter-b` | 11 | 24 `WARP_DC`, 25 `MV_QUAD` | 28 | none | 53-56 |
| `rdo-a` | 10 | **none** | none | **all 56** | 53 |
| `rdo-b` | 13 | **none** | none | **all 56** | 53-54 |

`tourney/percept` (`8322708`) is **already an ancestor of `merge-main`** --
`git merge-base --is-ancestor` confirms it. Merging it is a guaranteed no-op
and the merge script skips it. `tourney/sparse` sits on the same commit.

## 2. Every branch merges cleanly on its own

Merged one at a time onto `merge-main`, ten of the eleven are **completely
clean**:

| branch | conflicts merging alone onto `merge-main` |
|---|---|
| `xform-a`, `xform-b`, `inter-a`, `inter-b`, `detail-a`, `detail-b`, `ctx-a`, `rdo-a`, `rdo-b`, `percept` | none |
| `ctx-b` | `tools/quality/compare.py` (2 hunks) |

The single first-merge conflict is worth stating because it is not a tool
collision at all. `merge-main` split the encode/decode timing in
`run_codec()` and added `**(extra or {})` to the `measure()` call;
`tourney/ctx-b` independently fixed a **real measurement bug** in the same
function -- `--frames N` on a longer sequence must truncate the source, or the
reported bitrate is `N/seq.frames` of the true value. The two edits are
disjoint in intent, so the resolution is their union, and it is scripted:
`scripts/resolve-compare-py.py`. That fix is worth keeping **whichever ctx
branch wins**; if `ctx-a` is the winner, the hunk should be cherry-picked from
`ctx-b` anyway.

## 3. The conflict matrix

Every cross-category pair, first branch merged and committed, second branch
merged `--no-commit`. `V` marks conflicts confined to `tests/vectors/`
(regenerable, never hand-merged -- section 4.3); the count is conflicted files.

| | detail-a | detail-b | ctx-a | ctx-b | inter-a | inter-b | rdo-a | rdo-b |
|---|---|---|---|---|---|---|---|---|
| **xform-a** | 15 (+V) | 11 (+V) | 12 (+V) | 14 (+V) | 5 | 6 | 6 (+V) | 8 (+V) |
| **xform-b** | 14 (+V) | 13 (+V) | 11 (+V) | 11 (+V) | 4 | 5 | 5 (+V) | 6 (+V) |
| **detail-a** | -- | -- | 12 (+V) | 13 (+V) | 4 | 6 | 7 (+V) | 6 (+V) |
| **detail-b** | -- | -- | 15 (+V) | 16 (+V) | **2** | 3 | 6 (+V) | 7 (+V) |
| **ctx-a** | -- | -- | -- | -- | 6 (+V) | 7 (+V) | 10 (+V) | 9 (+V) |
| **ctx-b** | -- | -- | -- | -- | 1 | 1 | 1 | 1 |
| **inter-a** | -- | -- | -- | -- | -- | -- | 3 (+V) | 4 (+V) |
| **inter-b** | -- | -- | -- | -- | -- | -- | 5 (+V) | 6 (+V) |

The shape of it:

* **The intra trio fight each other.** `xform` x `detail` x `ctx` are the
  expensive pairs (11-16 files) because all three edit the same regions of
  `ref/src/entropy.{h,cpp}`, `ref/src/common.h`, `ref/src/codec_impl.inc` and
  the same three tables in `docs/SYNTAX.md`.
* **`inter` is nearly disjoint from the intra tools** (2-6 files). Its spec
  lives in section 13, its code in `ref/src/inter.*` and the inter half of
  `codec_impl.inc`. `detail-b + inter-a` is the cheapest cross-category pair in
  the whole tournament at two files.
* **`rdo` conflicts are almost entirely vectors** and therefore free once
  regeneration is in the pipeline.
* The `ctx-b` row reads `1` only because `ctx-b` merged *first* leaves its own
  `compare.py` conflict unresolved, which blocks the second merge. Read that
  row as "resolve `compare.py`, then see the corresponding column".

## 4. The four conflict classes, and how each is resolved

### 4.1 `docs/SYNTAX.md` -- three colliding tables plus section numbering

Every syntax branch edits the same three places, so this file conflicts in
every multi-branch stack (up to 10 hunks per step).

* **Section 2.3 tool-bit table.** All six branches add a row `| 24 | ... |`.
  Resolution: keep every row, renumber per `docs/TOOLBITS.md` section 2.
* **Section 4.1 word1 table.** `xform` takes 28-29, `detail-a` takes 28,
  `inter-a` takes 28-31, `inter-b` takes 28. Resolution: the single layout in
  `docs/TOOLBITS.md` section 4, and the `28-31 reserved` row is deleted.
* **Section numbering.** `xform-b` and `detail-a` **both call their new
  section 6.7**: `xform-b` "Transform size (tool bit 24)" and `detail-a`'s 4x4
  split. Resolution: `6.7` = transform size (`XFORM_LARGE`), `6.8` = the 4x4
  split, and `detail-a`'s CfL stays at `7.7`. Every cross-reference to "6.7"
  inside `detail-a`'s prose must be repointed to 6.8 -- this is prose, and
  `sed` on the section number alone will corrupt `xform`'s references. Do it
  by hand.
* **Appendix A.** Every branch numbers from 53. Renumber in merge order:
  for the trial stack, **xform-b 53-54, detail-a 55-61, ctx-b 62-65, inter-a
  none, rdo-b 66-67**. `inter-a` contributes no appendix entry and should be
  asked for one (TOURNEY-RULES criterion 4).

### 4.2 `include/nxvc/nxvc.h` and the python mirrors

Three conflicting regions, all mechanical:

1. the `#define NXVC_TOOL_<NAME> (1ull << N)` block -- every branch appends at
   24; keep all, renumber;
2. `NXVC_TOOLS_SUPPORTED` -- every branch appends its own tools to the mask;
   the resolution is always the **union**, never one side;
3. `struct nxvc_encode_opts` / the tile struct -- each branch appends its own
   fields under an "additive since syntax v1.5" comment; keep all fields,
   collapse to one comment block reading v1.6.

**This is why the renumbering is cheap.** Both the C reference and the python
package refer to tools by *name* everywhere except their definition sites, so
changing a bit number touches exactly four files:
`include/nxvc/nxvc.h`, `python/src/nxvc/_ffi.py`, `python/src/nxvc/bitstream.py`
and `docs/SYNTAX.md`. `ref/src/*` needs no edit at all for a renumber.

`python/src/nxvc/_ffi.py` has three things that must move together and are easy
to miss: `Tool.<NAME> = 1 << N`, the `(1 << N, "NAME")` row in the names table,
and **`Tool.RESERVED_FROM`**, which is the first reserved bit and is what
`bitstream.py` uses to reject reserved bits. Leave `RESERVED_FROM` at 24 and
every new tool is rejected as reserved; every branch bumps it and every stack
conflicts on it. It becomes **31** (option B) or **32** (option A).
`NXVC_BITSTREAM_MINOR` appears in both `nxvc.h` and `_ffi.py` and becomes 6.

### 4.3 `tests/vectors/` -- regenerate, never merge

`vectors.md5`, `rejects.md5` and the `.nxv` blobs conflict in almost every
stack, and **none of them should ever be hand-resolved**:

* `rdo-a` and `rdo-b` rewrite all 56 existing vectors and all 100 lines of
  `vectors.md5`, because the encoder's decisions changed.
* `ctx-a` rewrites `v34`, `v41`, `v42`, `v44`.
* `xform-a` and `detail-a` rewrite `r09_reserved_tile_bit.nxv` (both need
  word1 bit 28, so the "reserved bits are zero" vector had to move).
* Every syntax branch appends `v57`+ and `r30`+ rows.

Resolution, always: `git checkout --theirs` the blobs merely to clear the
index, then **`nxv-vectors --generate tests/vectors`** and commit whatever it
writes. The generator is the authority; the committed blobs are its output.
`ref.vectors` then re-checks them from scratch. The one thing that does need
review is the *set* of vector names, since two branches may add different
`v57`, and `tests/ref/vectors.cpp` -- which is the generator's source -- must
keep both, renumbered so no name repeats. That file conflicts in every stack
and is the one place in this class needing a human.

#### 4.3.1 Reject vectors do not survive a renumber

Found by running the pipeline, not by reading it. The reject vectors build
malformed headers by poking raw bytes into the stream:

```
b[32 + 3] |= 0x01;            // tool bit 24 NEAR_SKIP
```

Byte 3 of the u64 `tools` field at offset 32 **is** bit 24, spelled as a
literal. After the renumbering pass that line still sets bit 24, which is now
`INTRA_CFL` or nothing at all, so the decoder answers `VERSION` ("unsupported
tool") where the vector expects `BITSTREAM`. On the ctx-b + inter-a trial that
broke five vectors:

```
r30_tab_v2_no_tables:   decoder returned VERSION, expected BITSTREAM
r31_ctx_v3_no_v2:       decoder returned VERSION, expected BITSTREAM
r33_near_skip_on_intra: decoder returned VERSION, expected BITSTREAM
r35_near_skip_payload:  decoder returned VERSION, expected BITSTREAM
r36_quad_mv_on_intra:   decoder returned VERSION, expected BITSTREAM
```

`scripts/retool-bits.py` reports every such line (`check_literal_pokes()`) but
deliberately does not rewrite them: which constant a poke should become depends
on what the vector means to test, and guessing would turn a failing vector into
a passing one that tests nothing. Each has to be rewritten against
`NXVC_TOOL_<NAME>` by hand -- ten sites today, at `tests/ref/vectors.cpp` lines
734, 776, 788, 793, 883, 901, 906, 911, 914 and 915.

This also corrects section 4.2's claim: the renumber touches four files *plus*
`tests/ref/vectors.cpp`, which is the one place tool bits are written as
numbers rather than names.

#### 4.3.2 Reject vectors collide by number, like the `v57`s

`ctx-b` adds `r30_tab_v2_no_tables` / `r31_ctx_v3_no_v2`; `inter-a` adds
`r30_near_skip_no_tool` / `r31_quad_mv_no_tool` and `r32`-`r36`. The filenames
differ so nothing overwrites, but the tree ends with two `r30`s and two `r31`s
and a `rejects.md5` whose numbering no longer means anything. Renumber the same
way as the `v` set: in merge order, one contiguous sequence.

### 4.4 The genuinely semantic conflict: `ref/src/transform.{h,cpp}`

This is the only conflict in the tournament that a careful merge cannot
resolve textually, and it must be flagged before the merge starts.

`xform-b` **generalises the transform**: it replaces `fdct8x8(const i32[64],
i16[64])` / `idct8x8` with `fdct_2d(int n, const i32*, i16*)` /
`idct_2d(int n, ...)` for `n` in {8, 16, 32}, with the shifts parameterised as
`>> (3 + log2 n)` then `>> 14`.

`detail-a` **keeps `fdct8x8`/`idct8x8`** and adds a *fourth* size downward: new
constants `kD0 = 512`, `kD1 = 669`, `kD2 = 277` for a 4x4 transform.

Taking either side loses the other's tool outright.

**DECISION (fixed by the coordinator, 2026-09-04). One transform family.**
There is exactly one implementation, `fdct_2d(n)` / `idct_2d(n)` for `n` in
{4, 8, 16, 32}, built on `xform-b`'s matrix-family recursion. `detail-a`'s
4x4 becomes the `n == 4` row of that family and its call sites are rewritten
onto it. The normative invariant the family must satisfy:

> **The quantiser sees orthonormal coefficients at the v1 reference scale.**
> The 2D gain is exactly `2^20` at every size; there is **one** qstep table;
> and there is one weighting rule, `w_N[u][v] = w_8[u >> s][v >> s]` where
> `s = log2(N) - 3`.

Each size gets whatever forward/inverse shift split achieves that, and each
split ships with a documented int32/int16 range proof in `docs/SYNTAX.md`
section 6. `detail-a`'s constants `kD0 = 512`, `kD1 = 669`, `kD2 = 277` stay
**if** they already meet the invariant; otherwise they are rescaled to.

**This is a correctness trap, not a tidiness rule.** The detail judge showed a
scale that is off by 2x does not fail loudly: it silently shifts the effective
QP by 6, so every rate and PSNR number stays plausible and every comparison is
wrong. So the decision carries a mandatory test:

> A new ctest checks the measured 2D gain of **every** size in {4, 8, 16, 32}
> against a floating-point DCT and requires agreement to within **0.1 %**.

Write it as `ref.transform_gain` (or fold it into `ref.transform`), make it run
under the `asan-ubsan` preset like the rest, and add an Appendix A entry
recording the invariant and why the test exists. Do not merge `xform` without
it: it is the only thing standing between a 2x scale slip and a tournament's
worth of invalid measurements.

*Still open, and separate from the decision above:* what `split4x4 == 1` means
when `xform != 0`. My recommendation is mutual exclusion -- **`split4x4 == 1`
requires `xform == 0`**, a `BITSTREAM` error otherwise, with a reject vector --
matching the existing `tskip`/`xform` exclusion, since `detail-a`'s split wins
on flat 8x8 blocks and `xform`'s large transforms win on smooth regions and the
two were never competing for the same tiles. Note that once the family is one
ladder over {4, 8, 16, 32}, `split4x4` and `xform_size` are arguably the same
field seen from two ends, and the `xform` and `detail` judges may prefer to
collapse them into a single size selector rather than add an exclusion rule.
That is a design call for them, not for the integration pass.

### 4.5 The second semantic conflict: `ctx` x `xform` in the context derivation

Found while resolving the trial merge, and **not** visible in the file counts of
section 3, because it is two branches rewriting the same function in
incompatible ways rather than the same lines.

`ctx-b` adds a `UnitCtx` to every coefficient unit carrying `ucls`
(luma/chroma/DC class) and `ctx_v3`, and routes the LEVEL context through
`uc.level(scan_pos, prev_class)`.

`xform-b` adds `band_min` to every unit -- the LEVEL band floor for an 8x8
coefficient group inside a larger transform block, 0 for the group holding the
block's DC and 3 for the rest -- and routes the same context through
`level_ctx(scan_pos, prev_class, band_min)`.

Both rewrite `level_rate()`'s signature and both rewrite the unit-emission loop
in `ref/src/codec.cpp` (10 conflicting hunks). Taking either side silently
drops the other tool's context model. The merge is
`uc.level(scan_pos, prev_class, band_min)`, with `UnitCtx` carrying `ucls`,
`ctx_v3` **and** `band_min`.

That much is mechanical. The normative question underneath it -- does
`CTX_V3`'s lane-state conditioning apply per 8x8 coefficient group inside a
32x32 block, or per transform block? -- is now settled:

**DECISION (fixed by the coordinator, 2026-09-04). CTX_V3 conditions per
CODING UNIT, never per transform block.** The coding unit is the 8x8
coefficient group. A large block's units are conditioned on the previous unit
**the same rANS lane decoded**, exactly as 8x8 blocks are. A 32x32 block is
sixteen coding units for this purpose and nothing about the context derivation
notices that they came from one transform.

This keeps `CTX_V3` orthogonal to `XFORM_LARGE` -- the context model never
reads the transform size -- and it keeps the existing rule of section 9.1, that
a unit's syntax may only depend on values its own lane has already produced.
Write it into `docs/SYNTAX.md` section 9 as normative text with its own
Appendix A entry.

### 4.6 The collision git will not show you

The most dangerous case in the tournament produces **no conflict where it
matters**. Merging `tourney/ctx-b` then `tourney/inter-a` does conflict, but
only on `NXVC_TOOLS_SUPPORTED`, `docs/SYNTAX.md` and `tests/ref/vectors.cpp`.
The `#define` block itself merges **silently and cleanly**, because the two
branches appended their lines at different points in it, giving a header
containing:

```
#define NXVC_TOOL_TAB_V2          (1ull << 24)
#define NXVC_TOOL_CTX_V3          (1ull << 25)
#define NXVC_TOOL_NEAR_SKIP       (1ull << 24)   <-- same bit
#define NXVC_TOOL_QUAD_MV         (1ull << 25)   <-- same bit
```

Git had no overlapping hunk to complain about, so resolving the three files it
*did* flag leaves this in place. The result compiles. It produces streams in
which `TAB_V2` and `NEAR_SKIP` are the same bit.

This is the whole reason `scripts/retool-bits.py` runs unconditionally after
the merges rather than only when a conflict was seen, and the reason
`docs/TOOLBITS.md` is a single global table rather than a per-branch note. Any
merge of two tournament branches that does **not** run the renumbering pass is
wrong even when git reported success.

## 5. Recommended merge order

Eight orders were measured end to end, counting conflicted files outside
`tests/vectors/`:

| order | total |
|---|---|
| `ctx-b, xform-b, detail-a, inter-a, rdo-b` | **30** |
| `detail-a, xform-b, ctx-b, inter-a, rdo-b` | 31 |
| `xform-b, detail-a, ctx-b, inter-a, rdo-b` | 32 |
| `ctx-b, detail-a, xform-b, inter-a, rdo-b` | 32 |
| `rdo-b, xform-b, detail-a, ctx-b, inter-a` | 32 |
| `inter-a, ctx-b, detail-a, xform-b, rdo-b` | 32 |
| `ctx-b, detail-a, inter-a, xform-b, rdo-b` | 33 |
| `inter-a, xform-b, detail-a, ctx-b, rdo-b` | 34 |

The spread is 30 to 34 out of ~32, so **order buys almost nothing**; the
resolutions in section 4 are the whole cost. Order is therefore chosen for the
*shape* of the work rather than the file count:

> **`detail-a` -> ctx -> xform -> inter -> `rdo`**

1. **`detail-a` first.** `JUDGE-detail.md` reached its verdict first and fixes
   detail's bits (19 `XFORM_4X4_SPLIT`, 24 `INTRA_CFL`) and its word1 bit 28,
   so every later package renumbers around them rather than the other way
   round. It also carries the largest merge-time obligation list
   (`docs/TOOLBITS.md` 6.1), which is better done against a clean tree than on
   top of three other packages.
2. **ctx second.** It is the entropy layer everything else codes through, so
   landing it early means `xform` resolves its entropy hunks against the final
   context model once, instead of against `CTX_V2` and then again. `ctx-b`'s
   `compare.py` conflict is scripted.
3. **xform third.** It owns the transform generalisation (4.4), and by now it
   is the package that adapts -- folding `detail-a`'s 4x4 into `fdct_2d` and
   moving its own `xform_size` down to word1 29-30.
4. **inter fourth.** Nearly disjoint from the three above, so it lands late
   and cheaply; it is also the branch whose word1 demand decides whether the
   extension byte exists, and by this point the intra bits are fixed.
5. **rdo last, always.** Encoder-only, and it rewrites every vector, so it must
   be the final input to a single regeneration pass. Putting it anywhere else
   means regenerating vectors twice.

This supersedes the file-count ranking above, which put `ctx-b` first: the
spread was 30 to 34 out of ~32, so the ordering is worth almost nothing in
conflicts and everything in whose bits are already frozen.

Regenerate vectors **once, after the last merge**, never between steps.

## 6. Per-step manual resolutions

In the merge order of section 5 (**detail, ctx, xform, inter, rdo**), with the
`a`/`b` picks shown as placeholders where the judge has not ruled. Appendix A
renumbering is cumulative in merge order: detail-a 53-59, then ctx, then xform,
then inter, then rdo.

| step | file | resolution |
|---|---|---|
| **detail-a** | `include/nxvc/nxvc.h` | `XFORM_4X4_SPLIT` 19 and `INTRA_CFL` 24 both **stay** (judge-fixed); add both to the supported mask |
| | struct layout | **judge item 1** -- move detail-a's new `nxvc_config`/`nxvc_tile_info` fields to the END of their structs, `detail-b`-style; mid-struct insertion silently moves every later offset |
| | from `detail-b` | **judge item 2** -- encode-stats counters and `--stats`, its four extra vectors, its finer rejection vectors, its CLI validation, `quantize_unit()` |
| | `docs/SYNTAX.md` | **judge item 3** -- pin that `round(2^15 / d)` has no ties for `d` in [1, 255]; Appendix A 53-59 |
| | `ref/RESULTS-detail-a.md` | **judge item 4** -- regenerate rate columns and gate sentences on the fixed harness (absolute bitrates were ~6x high) |
| **ctx** | `tools/quality/compare.py` | `scripts/resolve-compare-py.py` (union) -- ctx-b only, but cherry-pick it even if ctx-a wins |
| | `include/nxvc/nxvc.h`, `python/src/nxvc/_ffi.py` | `CTX_V3` -> 25, second tool (`VEC_ENT`/`TAB_V2`) -> 26; union the mask; `RESERVED_FROM` |
| | `ref/src/codec.cpp`, `ref/src/entropy.{h,cpp}` | **4.5, decided** -- `uc.level(scan_pos, prev_class, band_min)`; `UnitCtx` carries `ucls`, `ctx_v3` and `band_min`; `CTX_V3` conditions per coding unit, never per transform block |
| | `ref/src/entropy.h` | keep both: `band_min` (ctx) and `split_present`/`split_out` (detail) |
| **xform** | `include/nxvc/nxvc.h` | `XFORM_LARGE` 24 -> **27** |
| | `ref/src/transform.{h,cpp}` | **4.4, decided** -- one family `fdct_2d(n)`/`idct_2d(n)` over {4,8,16,32}; 2D gain exactly `2^20` at every size; one qstep table; `w_N[u][v] = w_8[u>>s][v>>s]`; documented int32/int16 range proof per size |
| | `tests/ref/test_transform.cpp` | **4.4, mandatory** -- `ref.transform_gain`: every size's 2D gain vs a float DCT, within 0.1 %. A 2x slip is silent and shifts effective QP by 6 |
| | `docs/SYNTAX.md` | xform keeps 6.7, detail's split becomes **6.8**; word1 `xform_size` moves to **29-30** (`split4x4` keeps 28) |
| | `split4x4` x `xform` | **still open** -- recommend `split4x4 == 1` requires `xform == 0` with a reject vector, or collapse the two into one size selector (4.4) |
| **inter** | `include/nxvc/nxvc.h` | `NEAR_SKIP`/`QUAD_MV`/`SUBTILE_INTRA` -> 28/29/30; `TILE_EXT` 31 if inter-a; rename inter-b's `WARP_DC`/`MV_QUAD` onto the shared names |
| | `docs/SYNTAX.md` | inter-a: move its four word1 flags into the extension byte (TOOLBITS 4, option A). inter-b: `quad_mv` takes word1 31, no extension byte |
| | `tests/ref/vectors.cpp` | renumber `v`/`r` vectors onto one sequence (4.3.2); rewrite the raw tool-bit pokes against constants (4.3.1) |
| | Appendix A | inter-a contributes **no** entry; ask for one (rules criterion 4) |
| | **if inter-b wins** | `JUDGE-inter.md` 1.1: its tools-off encoder is **not** byte-identical to v1.4 (+11.5 %). The rolling refresh seeds its phase from `refresh_max_age` (720) instead of `intra_period` (180), so ~3/4 of tiles start past the threshold and are forced INTRA on frame 1. Fix the seed, then **re-measure**: that 11.5 % is the denominator of every delta in `ref/RESULTS-inter-b.md` section 3 |
| **rdo** | `ref/src/codec.cpp`, `codec_impl.inc`, `ref/tools/nxv-enc.cpp` | encoder-side; take rdo where it only reorders search, keep the other branches' new syntax emission |
| | all of `tests/vectors/` | regenerate, once, after this step |
| **final** | `r09_reserved_tile_bit` | word1 is full; move it to word0 bit 3 (TOOLBITS 4.1) and re-hash |
| | every `RESULTS-*.md` | absolute bitrates measured before the `--frames` fix are ~6x high; ratios survive, absolutes do not |

## 7. Running it

`scripts/tourney-merge.sh <winner-list>` performs the above: it orders the
winners by section 5's rule, merges each, applies the bit renumbering, resolves
the scripted conflicts, regenerates vectors and rejects, then builds and runs
`ref.*` and `fuzz.*` under the `asan-ubsan` preset, stopping at the first
failure. It refuses to run on `merge-main` itself.

Baseline to beat, measured on `merge-main` in this worktree:
**17/17 tests pass in 61 s** under `asan-ubsan`.

**Status: the real merge has NOT started, and must not, until
`JUDGE-xform.md`, `JUDGE-ctx.md`, `JUDGE-inter.md` and `JUDGE-rdo.md` all
exist.** Only `JUDGE-detail.md` has landed (merge `detail-a`).
`JUDGE-inter.md` exists but its verdict and merge checklist are still
`PLACEHOLDER`, so the inter winner -- and with it whether the tile extension
byte of `docs/TOOLBITS.md` 4 option A is needed at all -- is **not** decided.
Its section 1.1 is already final and is recorded in section 6 above, because it
is a merge obligation whichever way the verdict goes. What has been
done is a dry run of the machinery on `ctx-b` + `inter-a`, on the throwaway
branch `integ-scratch`, to prove the pipeline builds, renumbers, regenerates
and tests green -- it is not a claim about who should win. `merge-main` is
untouched at `e311de9` and nothing has been pushed.

The two decisions in 4.4 and 4.5 are fixed inputs to that merge whenever it
happens, and `scripts/tourney-merge.sh` prints both at the point of conflict so
they cannot be re-litigated at the keyboard.
