# Tournament merge plan

Companion to `docs/TOOLBITS.md`, which owns the bit allocation. This file owns
the **order**, the **conflict matrix** and the **specific manual resolutions**.

**Four normative decisions are fixed** and the merge carries them rather than
re-litigating them: the single transform family and its gain invariant
(section 4.4), `split4x4` and `xform_size` staying separate fields with
`split4x4` meaningful only at `xform_size == 8` (also 4.4), and `CTX_V3`'s
per-coding-unit conditioning (section 4.5), and the ctx package being a
combination of both branches with `VEC_ENT` and ctx-a's table reassignment
dropped (section 4.7).

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

`tourney/sparse` (`8322708`) is **already an ancestor of `merge-main`** --
`git merge-base --is-ancestor` confirms it -- so merging it is a guaranteed
no-op and the merge script skips it. **`tourney/percept` was on that commit
too and has since moved to `00e1f4c`**: it is now a real merge of five commits
and lands at step 7, after `rdo`. See section 4.8.

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

**DECISION (fixed by the coordinator, 2026-09-04). One transform family, and
`JUDGE-xform.md` picks `tourney/xform-a` to provide it.**

Merge **`xform-a`**: the DC plane re-gridded onto the transform, `n x n` intra
predictors, and **its inverse shift chain, which tracks the true per-size
gain**. `detail-a`'s 4x4 becomes the `n == 4` row of that family.

The transform-family invariant is read as **"the quantiser sees orthonormal
coefficients at every size"**, which `xform-a`'s chain satisfies. The `2^20`
figure in the earlier statement of the invariant **applies at 8x8**; it is not
a constant the other sizes must also hit.

**Take from `xform-b`:** the fuzz corpus seeds, the `plane_xform` / `weight_at`
tests, and the lane-balance statement into `docs/SYNTAX.md` 6.7.

**Do NOT take from `xform-b`:** its `INTRA_DIR` exclusivity, or its shift
chain.

*Post-merge measured step, not blocking the merge:* graft `xform-b`'s 8x8
coefficient-group entropy mapping onto `xform-a`'s design and accept it if it
costs **under 3 of `xform-a`'s 29 points**. It keeps Pass A unchanged, which is
why it is worth trying at all.

**This is a correctness trap, not a tidiness rule.** The detail judge showed a
scale that is off by 2x does not fail loudly: it silently shifts the effective
QP by 6, so every rate and PSNR number stays plausible and every comparison is
wrong. So the decision carries a mandatory test:

> A new ctest checks that the dequantized output of **every** size in
> {4, 8, 16, 32} is orthonormal at the v1 reference scale -- measured against a
> floating-point DCT, to within **0.1 %**. `xform-a`'s shift chain is what makes
> this hold per size; the test is what stops it drifting.

Write it as `ref.transform_gain` (or fold it into `ref.transform`), make it run
under the `asan-ubsan` preset like the rest, and add an Appendix A entry
recording the invariant and why the test exists. Do not merge `xform` without
it: it is the only thing standing between a 2x scale slip and a tournament's
worth of invalid measurements.

**DECISION (fixed by the coordinator, 2026-09-04). `split4x4` and
`xform_size` stay separate fields.** They act at different granularities --
`xform_size` is per tile and selects 8, 16 or 32; `split4x4` is per 8x8 block
and splits it to 4x4 -- so collapsing them into one size selector would
conflate a tile-level choice with a block-level one. They keep their own
fields, their own tool bits (27 `XFORM_LARGE`, 19 `XFORM_4X4_SPLIT`) and their
own word1 bits (29-30 and 28).

The normative rule that makes them compose:

> `split4x4` is present and meaningful **only when the tile's
> `xform_size == 8`**. A stream whose tile sets `xform_size != 8` *and* sets
> the split flag is **malformed**: `BITSTREAM`.

So word1 bit 28 must be zero unless word1 bits 29-30 select the 8x8 transform,
and when bit 28 is zero no per-block split flag is coded in the payload. This
needs:

* the constraint written into `docs/SYNTAX.md` 4.1 alongside the existing
  `tskip`/`xform` exclusion, and into 6.8;
* a **new rejection vector** pinning it -- a tile with `xform_size == 2` and
  `split4x4 == 1` must be refused `BITSTREAM`. Its `r`-number is assigned at
  merge time by the sequence renumbering of 4.3.2;
* an Appendix A entry recording that the two were deliberately kept separate
  and why.

The two tools keep their measured gains under this rule: `detail-a`'s split
wins on flat 8x8 blocks and `xform`'s large transforms win on smooth regions,
so they were never competing for the same tiles in the first place.

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

### 4.7 ctx is a combination, not a branch

`JUDGE-ctx.md` merges **neither branch whole**: ctx-a's `CTX_V3` (27 contexts,
the `LEVEL`-at-`LAST` split, the DC-term row) plus ctx-b's `TAB_V2`
(variable-length table sets with per-row default flags) and its `table_iters`
Lloyd refinement. Two things are **dropped**:

* **`VEC_ENT`** -- structural decoder cost for nothing measurable. It gets no
  tool bit at all (`docs/TOOLBITS.md` 2), and `scripts/retool-bits.py` fails
  the run if the name survives into the merged header.
* **ctx-a's unconditional table reassignment** -- an encoder-only change with
  no tool bit that rewrites four shipped conformance vectors and measures
  *worse than nothing* (ctx-b put it at -1.8 %).

That second drop **removes a conflict class from section 5**: the `ctx-a`
rewrites of `v34`, `v41`, `v42` and `v44` were entirely this change. Step 7 of
the checklist is the check -- those four must come back byte-identical to the
fork point, and if they do not, something else from ctx-a leaked into the
tools-off path.

The 13 steps, in order, from `JUDGE-ctx.md`:

| # | step |
|---|---|
| 1 | **ctx-b is the base**: `kProbBits`/`kProbTotal`/`kProbMax`/`kRansShift` naming, the `i64` widening in `normalize_freqs`, `quantize_row`, `row_cost`, `TableSetPlan`/`plan_table_set`/`serialize_table_set`, `parse_table_set(tab_v2)`, the variable-length table area and its `TRUNCATED` check |
| 2 | Replace ctx-b's 22 contexts with **ctx-a's 27**: neighbour class at 4 values with the plane-boundary reset, `LEVEL`-at-`LAST` split at two bands, the DC-term row -- re-expressed in ctx-b's accessor shape (`ctx_cbf`/`ctx_last`/`ctx_level` the only places a context is chosen), not ctx-a's `if` ladder |
| 3 | Retrain the built-in v3 family with `nxv-gentables v3` and regenerate `default_tables.inc`. **Neither branch's `kDefaultFreqV3` survives** |
| 4 | Take ctx-b's `table_iters` Lloyd refinement; drop ctx-a's `select_set(..., fp.tabs)`. Fix the API wart: `table_iters` means what it says, `0` = off, default `kDefaultTableIters`, delete the `255` sentinel from `nxvc_config` and `nxv-enc` |
| 5 | Delete ctx-a's whole-set drop test -- subsumed by ctx-b's `!pl.any` at row granularity |
| 6 | **Both bits OFF by default**; keep them out of `kToolsSupported` in `nxvc_vkdec_parse.cpp` so the Vulkan decoder refuses with `VERSION`. Carry ctx-b's `syntax_constants.h` staging at the 27-context stride (`s_cum` 13 824 B, ~15.5 KiB LDS) |
| 7 | **Verify `v34`, `v41`, `v42`, `v44` come back byte-identical** to `e4e85af`; `cmp` the tools-off encoder at 4:4:4 and 4:2:0, QP 16/24/32 |
| 8 | Regenerate the new vectors, renumber to `v57`+, take **both** branches' rejects renumbered (ctx-b's `tab_v2`-without-`CUSTOM_TABLES` and `ctx_v3`-without-`CTX_V2`, plus ctx-a's short-table truncation, which the variable-length area needs more than the fixed one did) |
| 9 | Take ctx-a's `test_codec.cpp` 6-axis sweep adapted to the merged layout; drop `test_inter.cpp`'s `VEC_ENT` tests with the tool |
| 10 | Merge the appendices -- they collide, both used 53-56. Keep ctx-a's 53 and 55, ctx-b's 54 (**amended**: the merged model keeps four classes, and the measurement that overturned it belongs in the entry) and 56, ctx-a's 59 and 60. Record ctx-a's reassignment as a **rejected** decision with ctx-b's -1.8 %, so nobody rebuilds it |
| 11 | Fix the three stale comments in `JUDGE-ctx.md` section 3 and ctx-b's `TAB_ROW_SKIP` naming drift |
| 12 | Re-run the section 2 measurement grid on the merged encoder, and **add band B** of the kill test, which the judge could not run and where `TAB_V2` is worth the most |
| 13 | Minor bump, `ctest --preset asan-ubsan -R 'ref\.|^fuzz\.'` green, fuzz replay green, and re-run ctx-b's libFuzzer campaign over the new variable-length table parser |

Step 13 says v1.5 for the package on its own; the tournament ships **one**
minor bump for all packages, so it is v1.6 (`docs/TOOLBITS.md` 7).

Step 2 is where decision 4.5 applies: the merged model's conditioning is **per
coding unit**, the 8x8 coefficient group, never per transform block.

### 4.8 percept merges last, and not for the thing it set out to prove

`tourney/percept` (`00e1f4c`) is **no longer the no-op** section 1 recorded.
It was `8322708` -- the fork point, already an ancestor of `merge-main` -- and
has since landed five commits of real content:

* **rc-to-encoder wiring**: `nxrc::EncDriver` (`rc/src/encdrive.cpp`) and the
  `nxv-enc --rc` option family, with `tests/rcenc/test_encdrive.cpp`;
* **two additive ABI items**: `nxvc_encoder_set_wm_map()` and
  `nxvc_tile_info::warp_mad_q8` (plus `NXVC_WARP_MAD_UNMEASURED`);
* **two real `rc/` fixes**: the `update_model` zero-bit-tile fix
  (`rc/src/allocate.cpp`) and the refresh-scheduler foveal-floor fix
  (`rc/src/refresh.cpp`);
* `ref/RESULTS-percept.md` and the `tools/quality/percept_*` scripts.

**Its measured result is negative for the spatial ladder**: the periphery is
over-degraded and every foveated metric loses. So it merges for the wiring,
the ABI and the two `rc/` fixes -- which stand on their own -- and **not** for
the ladder. Consequences:

* **`--rc` stays OFF by default.** The ladder is available to measure, not to
  ship. This is the same discipline as the tool bits: a package whose measured
  result is negative does not become the default path.
* **It merges at step 7, after `rdo`.** It is the only package that touches
  `rc/`, so nothing later has to resolve against it, and putting it after the
  single vector-regeneration pass keeps that pass unaffected -- percept changes
  no normative syntax and allocates no tool bit.
* **Verify both ABI items are APPENDED.** `warp_mad_q8` is correctly last in
  `nxvc_tile_info` on the branch, but `detail-a` also appends fields to that
  struct (`docs/TOOLBITS.md` 6.1, judge item 1) and merges first, so re-check
  after the merge that the two sets of appends are ordered and neither was
  interleaved into the middle.

#### The two conflicts, both scripted

`scripts/resolve-percept.py` handles them; `scripts/tourney-merge.sh` calls it
automatically on the percept step.

* **`tools/quality/nxq/fvvdp.py` is an add/add conflict.** percept forked
  before `tourney/metric` landed and carries a **verbatim copy** of metric's
  file, so its RESULTS numbers came from the same code. `merge-main` already
  merged `tourney/metric`, so **main's copy is kept and the branch's is
  dropped**. The branch's own header says exactly this: *"If the two branches
  are merged this file is a duplicate and the `tourney/metric` copy is the one
  to keep."* Taking the branch side instead would silently revert whatever
  metric has changed since the copy was taken.
* **`tools/quality/README.md` conflicts twice, both purely additive** -- the
  tool table gains percept's three scripts alongside main's `fvvdp`/`popin`/
  `latency` rows, and percept's `percept_run.py` section sits after main's
  `--metric` section. Resolution is the union, main's side first.

Verified by dry run: percept merges onto `merge-main` with exactly these two
conflicts, the resolver clears both, and the result configures and builds
clean under `asan-ubsan`.

#### OPEN ISSUE: rc spatial ladder calibration

> **rc spatial ladder calibration: periphery over-degradation.** The spatial
> ladder degrades the periphery further than the acuity model justifies, and
> every foveated metric loses as a result. Needs (a) **2160-px material** --
> the current corpus tops out too low for the periphery of the ladder to be
> exercised at a realistic eccentricity, so the measurement is not testing what
> the ladder is for; and (b) **the encoder's skip decision fed to the allocator
> before allocation**, so the allocator stops spending bits against tiles the
> encoder is about to skip. Until both are in place the ladder cannot be
> calibrated, and `--rc` stays off by default.

### 4.9 inter is a combination too, and it is where the byte cost went away

`JUDGE-inter.md`: **merge `inter-a` as the base** -- drift-driven refresh,
quad vectors, near-skip -- and take four things from `inter-b`.

**Take from `inter-b`:**

1. **The near-skip DC correction placement.** `inter-b`'s `WARP_DC` syntax --
   nine bytes in the **tile-row header**, gated by a second per-row bitmap --
   **instead of** `inter-a`'s word1-bit tile-structure form. `inter-b`'s form
   stays warp-only and buys **+2.10 dB on the chain**; `inter-a`'s does not.
   **Re-measure the package after the swap**: `inter-a`'s numbers were taken
   with its own form.
2. **`inter-b`'s refresh off-by-one fix.** `inter-a` ships the bug on its
   default drift path.
3. **`inter-b`'s single predictor loop** for the quad vectors, with the
   `warp.quad` equivalence test and the shader note.

**Drop from `inter-a`:**

4. **Sub-tile intra, entirely.** Bit 26 is gone and gets **no allocation**
   (`docs/TOOLBITS.md` 2).
5. **The ramp form of near-skip.** Nothing exercises it. **DC only.**

**Fix in `inter-a`:**

6. **`nxvc_config` fields must be APPENDED** (ABI). Same defect class as
   `detail-a`'s (`docs/TOOLBITS.md` 6.1, item 1) and `percept`'s appended ABI
   items (4.8): three packages append to the same structs, so after the merge
   check that all three sets of appends are ordered and none was interleaved.
7. **The drift gate must measure drift against the SHADOW** -- the loss-aware
   reconstruction -- **not against `inter-a`'s own reconstruction.** A gate
   that measures against its own output cannot see the drift that loss causes,
   which is the only drift the gate exists to catch.

#### What this does to the tile header

This is the decision that **removed the byte cost of the entire tournament.**
`inter-a` wanted four word1 bits (`near_skip`, `near_skip_ac`, `quad_mv`,
`sub_intra`) against four free, on top of `detail-a`'s one and `xform`'s two --
a three-bit overflow that `docs/TOOLBITS.md` was going to absorb with a ninth
tile-header byte gated by a `TILE_EXT` tool bit.

Moving the correction to the tile-row header and dropping sub-tile intra
reduces that demand to **at most one bit**, and the ramp drop removes
`near_skip_ac` as well. Word1 is now `split4x4` 28, `xform_size` 29-30,
`quad_mv` 31 *if it is needed there at all* -- the tile-row bitmap may already
name the tiles carrying quadrant vectors, in which case bit 31 stays reserved.
**No extension byte, no `TILE_EXT` bit, tile header stays 8 bytes.**

`NEAR_SKIP` is consequently the one tool in the tournament whose tool bit gates
a **tile-row header** structure rather than a tile-header field, so its
normative text belongs in `docs/SYNTAX.md` 3.3, not 4.1.

### 4.10 rdo: `rdo-b` is the base, five things port from `rdo-a`

`JUDGE-rdo.md`: **merge `tourney/rdo-b`** -- the rate model with bypass bits,
the unified lambda at **k = 0.22**, the trellis with its `LAST` bound and SDH
rebate, the DC plane through the trellis, the double-charge removal, the
hierarchical MV search with SATD, the cheap QP search, and the presets.

**Port from `rdo-a`:**

1. **Preset as a *library* concept** -- `nxvc_preset` in `nxvc_config`, not a
   CLI-only flag. A preset that only exists in `nxv-enc` is not available to
   anything embedding the encoder.
2. **The chroma distortion weight made explicit**, default-off at 1.0, with
   `rdo-a`'s per-sample-density scaling.
3. **`rdo-a`'s cheaper per-tile QP search** -- `requant_params` plus bounded
   descent.
4. **`RateCost::zero_cheapest`**, as a check of the trellis-truncation
   identity.
5. **`rdo-a`'s luma SSIM result (-25 %) as an OPEN ITEM to chase, not to merge
   blindly.**

#### Required fixes at merge: the docs contradict the code

`rdo-b`'s `ref/README.md` and its `docs/SYNTAX.md` decision 53 describe an
encoder it does not ship:

| claim in rdo-b's docs | what the code does |
|---|---|
| lambda `0.30` | ships **0.22** |
| a `kRefPersist` divisor | the divisor was **removed** |
| preset `slow` credits `--wm auto` | the code does **not** set it |

**Correct all three to the code**, and add the `CHANGELOG` entry `rdo-b` omits.
This is the same defect class as `JUDGE-detail.md` item 4 and
`JUDGE-inter.md` 1.1: the branch's own record of what it did is wrong, and the
number in it is the one someone will quote later.

### 4.11 Regenerating the vectors makes the conformance suite blind

A property of this pipeline that has to be said out loud before anyone reads a
green test run as evidence the merge is right.

`nxv-vectors --generate` rebuilds `tests/vectors/*` **from the merged
encoder**, and `ref.vectors` then checks the committed blobs against that same
encoder. After a regeneration the suite proves the encoder agrees with itself.
It cannot prove the merge preserved either branch's semantics: a resolution
that silently drops one branch's context split, or takes the wrong shift chain,
produces different bytes, gets those bytes blessed as the new vectors, and goes
green.

So **green `ref.*` after a regeneration is a necessary condition, not evidence
of a correct merge.** The checks that actually bite are:

1. **The tools-off byte-identity check against `e4e85af`.** Every judge ran it
   and it is the one test that catches a resolution leaking into the default
   path -- it is how `JUDGE-inter.md` found inter-b's +11.5 % refresh
   regression and how `JUDGE-ctx.md` pinned ctx-a's table reassignment. Run it
   after **every** step, not just at the end: with all new tools off, the
   merged encoder must reproduce `e4e85af` byte for byte at 4:4:4 and 4:2:0,
   QP 16/24/32.
2. **The per-package measurement grid**, re-run on the merged encoder. A merge
   that loses a tool's gain still passes every test in the repository.
3. For the transform specifically, the **2D-gain ctest of 4.4**, which is the
   only thing standing between a wrong shift chain and a silent QP shift of 6.

`ref.vectors` catches transcription mistakes and decoder/encoder disagreement.
It does not catch a semantically wrong merge. Budget the byte-identity check
into every step.

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

> **`detail-a` -> `ctx-b` -> `ctx-a` -> xform -> inter -> `rdo` -> `percept`**

1. **`detail-a` first.** `JUDGE-detail.md` reached its verdict first and fixes
   detail's bits (19 `XFORM_4X4_SPLIT`, 24 `INTRA_CFL`) and its word1 bit 28,
   so every later package renumbers around them rather than the other way
   round. It also carries the largest merge-time obligation list
   (`docs/TOOLBITS.md` 6.1), which is better done against a clean tree than on
   top of three other packages.
2. **ctx second, as a combination of both branches (4.7).** It is the entropy
   layer everything else codes through, so landing it early means `xform`
   resolves its entropy hunks against the final context model once, instead of
   against `CTX_V2` and then again. **ctx-b goes first** because
   `JUDGE-ctx.md` makes it the base -- naming, `quantize_row`, the
   variable-length table area -- and ctx-a's 27-context model then replaces
   ctx-b's 22 on top of it. `ctx-b`'s `compare.py` conflict is scripted.
3. **xform third.** It owns the transform generalisation (4.4), and by now it
   is the package that adapts -- folding `detail-a`'s 4x4 into `fdct_2d` and
   moving its own `xform_size` down to word1 29-30.
4. **inter fourth, as a combination of both branches (4.9).** Nearly disjoint
   from the three above, so it lands late and cheaply. Its word1 demand was
   what decided whether a ninth tile-header byte existed -- and the judge
   dissolved the question by moving the near-skip correction into the tile-row
   header and dropping sub-tile intra, so the answer is no.
5. **rdo sixth, as a combination (4.10).** Encoder-only, and it rewrites every vector, so it must be the
   final input to the single vector-regeneration pass. Putting it anywhere
   else means regenerating vectors twice.
6. **percept seventh, after rdo (4.8).** It is the only package touching
   `rc/`, allocates no tool bit and changes no normative syntax, so it cannot
   disturb the regeneration pass and nothing later resolves against it. It
   merges for the rc-to-encoder wiring, the two ABI items and the two `rc/`
   fixes -- **not** for the spatial ladder, whose measured result is negative.
   `--rc` stays off by default.

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
| **ctx** (combination) | -- | **13 steps, section 4.7.** ctx-b is the base; ctx-a's 27-context model replaces ctx-b's 22. Merge ctx-b first, then ctx-a |
| | `tools/quality/compare.py` | `scripts/resolve-compare-py.py` (union) |
| | `include/nxvc/nxvc.h`, `python/src/nxvc/_ffi.py` | `CTX_V3` -> 25, `TAB_V2` -> 26; **`VEC_ENT` deleted, not renumbered**; both bits OFF by default; union the mask; `RESERVED_FROM` |
| | `ref/src/default_tables.inc` | **retrain** with `nxv-gentables v3`; neither branch's `kDefaultFreqV3` survives |
| | `ref/src/codec.cpp`, `ref/src/entropy.{h,cpp}` | **4.5, decided** -- `uc.level(scan_pos, prev_class, band_min)`; `UnitCtx` carries `ucls`, `ctx_v3` and `band_min`; `CTX_V3` conditions per coding unit, never per transform block |
| | `ref/src/entropy.h` | keep both: `band_min` (ctx) and `split_present`/`split_out` (detail) |
| **xform** | `include/nxvc/nxvc.h` | `XFORM_LARGE` 24 -> **27** |
| | `ref/src/transform.{h,cpp}` | **4.4, decided** -- one family `fdct_2d(n)`/`idct_2d(n)` over {4,8,16,32}; 2D gain exactly `2^20` at every size; one qstep table; `w_N[u][v] = w_8[u>>s][v>>s]`; documented int32/int16 range proof per size |
| | `tests/ref/test_transform.cpp` | **4.4, mandatory** -- `ref.transform_gain`: every size's 2D gain vs a float DCT, within 0.1 %. A 2x slip is silent and shifts effective QP by 6 |
| | `docs/SYNTAX.md` | xform keeps 6.7, detail's split becomes **6.8**; word1 `xform_size` moves to **29-30** (`split4x4` keeps 28) |
| | `split4x4` x `xform_size` | **4.4, decided** -- kept as separate fields (different granularities). `split4x4` is meaningful only when `xform_size == 8`; `xform_size != 8` with the split flag set is `BITSTREAM`. Add the constraint to SYNTAX 4.1/6.8, a rejection vector, and an Appendix A entry |
| **inter** (combination) | -- | **section 4.9.** `inter-a` is the base; take four things from `inter-b`, drop two from `inter-a`, fix two in it |
| | `include/nxvc/nxvc.h` | `NEAR_SKIP` -> 28, `QUAD_MV` -> 29; **`SUBTILE_INTRA` deleted, not renumbered**; **`nxvc_config` fields APPENDED** |
| | `docs/SYNTAX.md` | near-skip is a **tile-row header** structure (3.3), `inter-b`'s 9-byte form gated by a second per-row bitmap -- **not** a word1 bit. `quad_mv` in word1 31 only if the row bitmap does not already name the tiles. **DC only, no ramp** |
| | `ref/src/inter.*` | `inter-b`'s refresh off-by-one fix and its single predictor loop, with the `warp.quad` equivalence test and the shader note |
| | drift gate | measure against the **shadow** (loss-aware), not `inter-a`'s own reconstruction |
| | `ref/RESULTS-inter-*.md` | **re-measure after the syntax swap** -- `inter-a`'s numbers were taken with its own near-skip form |
| | `tests/ref/vectors.cpp` | renumber `v`/`r` vectors onto one sequence (4.3.2); rewrite the raw tool-bit pokes against constants (4.3.1) |
| | Appendix A | inter-a contributes **no** entry; ask for one (rules criterion 4) |
| | **inter-b's refresh, now merged** | `JUDGE-inter.md` 1.1: its tools-off encoder is **not** byte-identical to v1.4 (+11.5 %). The rolling refresh seeds its phase from `refresh_max_age` (720) instead of `intra_period` (180), so ~3/4 of tiles start past the threshold and are forced INTRA on frame 1. Fix the seed, then **re-measure**: that 11.5 % is the denominator of every delta in `ref/RESULTS-inter-b.md` section 3 |
| **rdo** (combination) | -- | **section 4.10.** `rdo-b` is the base; five items port from `rdo-a` |
| | `include/nxvc/nxvc.h` | `nxvc_preset` in `nxvc_config` -- preset is a **library** concept, not CLI-only (APPEND the field) |
| | `ref/src/rdo*`, `codec_impl.inc` | rdo-a's `requant_params` + bounded descent QP search; `RateCost::zero_cheapest` as the trellis-truncation identity check; chroma distortion weight explicit, default 1.0, per-sample-density scaling |
| | `ref/README.md`, `docs/SYNTAX.md` decision 53 | **correct to the code**: lambda is **0.22** not 0.30, the `kRefPersist` divisor is **removed**, `slow` does **not** set `--wm auto` |
| | `CHANGELOG.md` | add the entry rdo-b omits |
| | open item | rdo-a's luma SSIM **-25 %** -- chase, do not merge blindly |
| | all of `tests/vectors/` | regenerate, once, after this step |
| **percept** | `tools/quality/nxq/fvvdp.py` | **add/add** -- keep MAIN's (metric's) copy, drop the branch's verbatim duplicate; `scripts/resolve-percept.py` |
| | `tools/quality/README.md` | union both sides, ours first; same script |
| | `ref/tools/nxv-enc.cpp` | **`--rc` OFF by default** -- the ladder ships available, not enabled |
| | `include/nxvc/nxvc.h` | re-verify `nxvc_encoder_set_wm_map` and `nxvc_tile_info::warp_mad_q8` are APPENDED, after detail-a's own appended fields |
| | open issue | file **rc spatial ladder calibration** (4.8): needs 2160-px material and the encoder's skip decision fed to the allocator before allocation |
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
`JUDGE-detail.md`, `JUDGE-ctx.md` and `JUDGE-inter.md` have all landed and
their decisions are carried in sections 4.4 to 4.9. **`JUDGE-xform.md` and the
rdo verdict are still outstanding**, and the merge waits for them. What has been
done is a dry run of the machinery on `ctx-b` + `inter-a`, on the throwaway
branch `integ-scratch`, to prove the pipeline builds, renumbers, regenerates
and tests green -- it is not a claim about who should win. `merge-main` is
untouched at `e311de9` and nothing has been pushed.

The two decisions in 4.4 and 4.5 are fixed inputs to that merge whenever it
happens, and `scripts/tourney-merge.sh` prints both at the point of conflict so
they cannot be re-litigated at the keyboard.

---

## 8. Step 9, after the tournament: `exp/entropy-lite`

Added after the seven tournament steps, and not a tournament package: it did
not compete, it has no judge, and it is the only tool in the format that
spends rate to buy **decode time**.

`exp/entropy-lite` developed `ENTROPY_LITE` on bit 24, like every tournament
branch did, so it renumbers the same way -- to **bit 30**, the first bit the
tournament left free (`docs/TOOLBITS.md` 2 and 8). Only the tool commit is
taken; the branch also carries a GPU encoder and a Pass A performance series
that are separate work.

What lands:

* the **`FIXED` variant**, which is the one that makes a coefficient's bit
  position computable and therefore the one that buys the parallelism;
* **`RICE` stays in the syntax and stays off** -- it is worth 1-4 % of rate
  above ~140 Mbit/s and gives up exactly the property the tool exists for.
  Defined, reachable, documented as measured-and-not-shipped;
* `docs/SYNTAX.md` **9.10**, not the branch's 9.8: 9.8 is the 4x4 split and
  9.9 the 27-context model by the time this lands, so the section number
  moves with the bit;
* Pass A's specialisation constant 3, `ENTROPY_MODE`, decoding `FIXED` at one
  64-thread workgroup per tile;
* Appendix A **78**, renumbered from the branch's 53.

**It ships OFF and is negotiated.** The trade is +40-50 % bits for a 7.5x cut
in Pass A on a Pico 4 (138.5 -> 18.4 ms), where it is the only bitstream-side
lever that reaches the frame budget at all -- and on a desktop GPU, where Pass
A already fits, the same trade is bits spent for nothing. Whether to make it
depends on the decoder's own measured Pass A, which the encoder cannot know,
so the decoder asks at the handshake. That is a different discipline from the
tournament's "on when it pays in BD-rate", and section 8 of `docs/TOOLBITS.md`
states it.

The mandatory checks apply unchanged, with the gain check restated for a tool
whose gain is not in BD-rate: **bits +40 to +50 % at QP 24**, and **zero
mismatches through the Vulkan decoder's Pass A path on lavapipe**.
