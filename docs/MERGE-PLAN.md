# Tournament merge plan

Companion to `docs/TOOLBITS.md`, which owns the bit allocation. This file owns
the **order**, the **conflict matrix** and the **specific manual resolutions**.

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

### 4.4 The genuinely semantic conflict: `ref/src/transform.{h,cpp}`

This is the only conflict in the tournament that a careful merge cannot
resolve textually, and it must be flagged before the merge starts.

`xform-b` **generalises the transform**: it replaces `fdct8x8(const i32[64],
i16[64])` / `idct8x8` with `fdct_2d(int n, const i32*, i16*)` /
`idct_2d(int n, ...)` for `n` in {8, 16, 32}, with the shifts parameterised as
`>> (3 + log2 n)` then `>> 14`.

`detail-a` **keeps `fdct8x8`/`idct8x8`** and adds a *fourth* size downward: new
constants `kD0 = 512`, `kD1 = 669`, `kD2 = 277` for a 4x4 transform.

Taking either side loses the other's tool outright. The resolution is to
finish `xform-b`'s generalisation one step further -- `fdct_2d` / `idct_2d`
over `n` in {**4**, 8, 16, 32}, with `detail-a`'s 4x4 constants folded in as
the `n == 4` row of the same matrix family -- and then rewrite `detail-a`'s
call sites onto it. `xform-b`'s Appendix A decision 53 already says the 16x16
and 32x32 transforms are "one matrix family with the 8x8", so extending the
family down to 4x4 is in the spirit of the branch, but it is **reference-codec
work, not merge work**, and it needs whoever owns `ref/src/transform.cpp`.

There is also a normative question the merge cannot settle by itself: what
`split4x4 == 1` means when `xform == 2` (a 32x32 transform). The cheapest
correct answer, and the one matching the existing `tskip`/`xform` exclusion, is
to make them mutually exclusive: **`split4x4 == 1` requires `xform == 0`**, a
`BITSTREAM` error otherwise, with a new reject vector. That is a one-line
constraint in section 4.1 and it keeps both tools' measured gains, since
`detail-a`'s split wins on flat 8x8 blocks and `xform`'s large transforms win
on smooth regions -- they were never competing for the same tiles.

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
`ctx_v3` **and** `band_min` -- but that is not a textual resolution, because it
raises a normative question neither branch answered:

> does `CTX_V3`'s lane-state conditioning apply per 8x8 coefficient group
> inside a 32x32 block, or per transform block?

The two readings give different bitstreams. This needs the `ctx` and `xform`
authors to agree, and the answer belongs in section 9 of `docs/SYNTAX.md` with
its own Appendix A entry. **The integration pass must not invent it.**

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

> **`ctx-b` -> `xform-b` -> `detail-a` -> `inter-a` -> `rdo-b`**

1. **`ctx-b` first.** It is the entropy layer everything else codes through,
   so landing it first means `xform` and `detail` resolve their entropy hunks
   against the final context model once, instead of against `CTX_V2` and then
   again. Its own `compare.py` conflict is scripted.
2. **`xform-b` second.** It owns the transform generalisation (4.4); every
   later branch adapts to `fdct_2d`, not the reverse.
3. **`detail-a` third**, folding its 4x4 into `xform-b`'s family.
4. **`inter-a` fourth.** Nearly disjoint from the three above, so it lands
   late and cheaply; it is also the branch whose word1 demand decides whether
   the extension byte exists, and by this point the intra bits are fixed.
5. **`rdo-b` last, always.** It is encoder-only and rewrites every vector, so
   it must be the final input to a single regeneration pass. Putting it
   anywhere else means regenerating vectors twice.

Regenerate vectors **once, after the last merge**, never between steps.

## 6. Per-step manual resolutions, trial stack

| step | file | resolution |
|---|---|---|
| ctx-b | `tools/quality/compare.py` | `scripts/resolve-compare-py.py` (union) |
| xform-b | `include/nxvc/nxvc.h` | `XFORM_LARGE` 24 -> 25; union the supported mask |
| | `python/src/nxvc/_ffi.py` | same; `RESERVED_FROM` -> 28 |
| | `docs/SYNTAX.md` | 2.3 row -> 25; word1 28-29; Appendix A 53-54 |
| detail-a | `include/nxvc/nxvc.h` | `INTRA_CFL` stays 24; add `XFORM_4X4_SPLIT` to mask |
| | `ref/src/transform.{h,cpp}` | **section 4.4 -- fold 4x4 into `fdct_2d`/`idct_2d`** |
| | `ref/src/entropy.h` | keep both: `band_min` (ctx) and `split_present`/`split_out` (detail) |
| | `docs/SYNTAX.md` | detail's 6.7 -> **6.8**; word1 `split4x4` -> bit 30; add the `split4x4` requires `xform == 0` constraint; Appendix A 55-61 |
| inter-a | `include/nxvc/nxvc.h` | `NEAR_SKIP`/`QUAD_MV`/`SUBTILE_INTRA` -> 28/29/30; `TILE_EXT` 31 |
| | `docs/SYNTAX.md` | move the four flags into the extension byte (TOOLBITS 4, option A) |
| | `tests/ref/vectors.cpp` | renumber `v57`+ so no name collides |
| rdo-b | `ref/src/codec.cpp`, `codec_impl.inc`, `ref/tools/nxv-enc.cpp` | encoder-side; take `rdo-b` where it only reorders search, keep the other branches' new syntax emission |
| | all of `tests/vectors/` | regenerate |
| final | `r09_reserved_tile_bit` | move to word0 bit 3 (TOOLBITS 4.1) and re-hash |

## 7. Running it

`scripts/tourney-merge.sh <winner-list>` performs the above: it orders the
winners by section 5's rule, merges each, applies the bit renumbering, resolves
the scripted conflicts, regenerates vectors and rejects, then builds and runs
`ref.*` and `fuzz.*` under the `asan-ubsan` preset, stopping at the first
failure. It refuses to run on `merge-main` itself.

Baseline to beat, measured on `merge-main` in this worktree:
**17/17 tests pass in 61 s** under `asan-ubsan`.
