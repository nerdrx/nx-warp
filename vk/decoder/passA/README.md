# Pass A — interleaved rANS entropy decode

Pass A turns each tile's rANS payload into int16 coefficients in the layout
Pass B consumes. It is the first of the two decoder dispatches (PAPER 3.2.1).

| file | role |
|---|---|
| `syntax_constants.h` | **every** bitstream-dependent constant, compiled as both C++ and GLSL |
| `rans_decode.comp` | the compute kernel — both the rANS and the ENTROPY_LITE path, selected by a specialisation constant |
| `passA_model.{h,cpp}` | CPU model of the kernel, line-for-line; GPU and CPU must agree bit for bit |
| `passA_test_encoder.{h,cpp}` | test-only encoders: rANS (byte-identical to `nxvc::encode_units`) and ENTROPY_LITE (byte-identical to `nxvc::lite_encode_units`) |
| `passA_test_gen.h`, `passA_test_corpus.h` | test-only corpus generation |
| `tools/nxvc-passA-test` | headless Vulkan harness |

## Dispatch shape

```
workgroup     64 threads  = 8 tiles x 8 rANS lanes
groups        ceil(num_tiles / 8)
tile slot     gl_LocalInvocationID.x >> 3
lane          gl_LocalInvocationID.x &  7
```

`local_size_x` is fixed at 64. `TILES_PER_GROUP` (spec constant 1) exists so the
value is visible to the host, but the kernel assumes 8 tiles of 8 lanes; the
lane count is a v1 syntax constant (`nsub_log2 == 3`), not a tuning knob.

The 8-lane cluster must not straddle a subgroup. A 64-thread workgroup satisfies
this for every subgroup size that is a multiple of 8 — 8 (lavapipe), 32 and 64
(RADV), 128 — which is why the workgroup is 64 and not 256. Create the pipeline
with `VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT` where
`subgroupSizeControl` is offered; the harness does.

## The shared read pointer

Per tile, the 8 lanes are 8 interleaved rANS states (32-bit, `L = 2^16`, 10-bit
probabilities) reading **one** byte stream. The reference walks lanes
`0..active-1` in order each scheduling round, so within a round the lanes that
renormalise consume their 16-bit words **in ascending lane order**. That is
exactly a prefix count over a ballot:

```glsl
uvec4 b = subgroupBallot(needs) & cluster_mask;   // cluster_mask = 8 bits at (id & ~7)
prefix  = subgroupBallotExclusiveBitCount(b);     // words consumed below me
total   = subgroupBallotBitCount(b);              // words the cluster consumes
if (needs) x = (x << 16) | load_u16be(g_pos + prefix * 2);
g_pos += total * 2;                               // every lane advances alike
```

Per PAPER 3.2.6 the cluster mask is built from `gl_SubgroupInvocationID & ~7`
rather than `subgroupClustered*`, which has weaker support on Adreno.

### Selecting the fallback path

Specialisation constant **0** (`READ_PTR_MODE`, `kSpecIdReadPtrMode`):

| value | path |
|---|---|
| `0` = `kReadPtrBallot` | subgroup ballot + prefix (default) |
| `1` = `kReadPtrLdsFallback` | per-stream byte range resolved through LDS, no subgroup ops |

The fallback publishes each lane's renormalise flag into a per-tile bitmask in
shared memory and derives `prefix`/`total` from that instead. It produces
**identical** offsets and therefore identical output, and it is the path to use
when `subgroupSize < 8` or when a driver's ballot is not trusted. Both paths are
tested on every ICD; the harness selects the fallback automatically when
`subgroupSize < 8`.

Control flow inside the round loop is workgroup-uniform in both modes and every
`barrier()` is unconditional, so the same SPIR-V is valid either way.

## Memory layout

Seven storage buffers, set 0:

| binding | buffer | contents |
|---|---|---|
| 0 | bitstream | the whole frame's tile bytes, uint-addressed; pad 16 bytes past the end |
| 1 | tile descriptors | 4 uints per tile: byte offset, byte length, coef index, cbf index |
| 2 | tables | `cum[8][27][16]` as uints (`cum[16] == 1024` is implicit) |
| 3 | coefficients | `int16_t`, `coef_stride` entries per tile |
| 4 | cbf bits | `cbf_words` uints per tile, one bit per coding unit |
| 5 | status | one uint per tile, `kStatus*` |
| 6 | intra modes and split flags | `kModeRegionUints` uints per tile: 32 words of 4-bit intra modes, 8 to a word, then 8 words of 1-bit `XFORM_4X4_SPLIT` flags, 32 to a word |

Push constants (20 bytes): `num_tiles`, `frame_nplanes`, `coef_stride`,
`cbf_words`, `tools`. The last is the frame-uniform tool mask —
`kToolFlagCtxV2 | kToolFlagIntraDir | kToolFlagSignHide | kToolFlagCtxV3 |
kToolFlagSplit4 | kToolFlagCfl | kToolFlagXformLarge`, derived by the host
from the stream's tool bits (`docs/SYNTAX.md` 2.3). It is a push constant
rather than a specialisation constant so that turning a tool on costs no
pipeline rebuild.

### The sparse layout

`sparse` (default 1) selects what a unit's slots hold. Section 8 of
`syntax_constants.h` is the normative description; in one paragraph:

* **dense** (`sparse == 0`) writes coefficient `k` at `scan_index(scan_id, k)`,
  the raster index inside the unit, and zeroes the whole `coef_stride` region
  first, so every tile moves 12.5 KB whatever the payload said;
* **sparse** (`sparse == 1`) writes coefficient `k` at slot `k` — scan order —
  and publishes `LAST + 1` in binding 7. Slots past `LAST` are neither written
  nor read, so nothing is zeroed. Every zero *inside* `[0, LAST]` is stored
  explicitly, which the level phase already does, so a reader that stops at
  the published length sees exactly the coefficients the unit coded.

The unit's base and its reserved width are the same in both, so `coef_stride`
and every plane base are untouched. `LAST` is already in the syntax (9.2) and
the scan is already normative (5): the layout is a re-indexing of the same
numbers, not a change to them.

Lanes interleave over units (unit `u` belongs to lane `u % LANES`), so the
units sharing a length word belong to different lanes and the write is an
`atomicOr`. It goes into a per-tile-slot LDS array — 8448 B at the shipped 32
tiles per group — and is flushed to binding 7 once per tile in
`kUnitLenWordsPerTile` coalesced stores, so the frame's ~200 000 atomics stay
local.

[minor 6] The **field width follows the tile's transform size**: eight bits and
four to a uint at 8x8, sixteen and two at 16x16 and 32x32, where `LAST + 1`
reaches 1024. The region does not grow either way, because the same thing that
makes those units big makes them few — 18 units per 64-edge plane at 16x16
against 66 at 8x8, so at most 72 against 264, and `kMaxUnitsPerTileLarge` is
the header check that keeps that a rule rather than an assumption. Widening
the field unconditionally would have taken this array from 8.4 to 16.9 KiB on
top of `CTX_V3`'s 13.8, which does not fit an Adreno 650's 32 KiB.

**Coefficient order** is the reference's `TileCoder::coef` order, so Pass B can
hand a tile straight to `reconstruct_plane()`:

```
for each coded plane p in (Y, Co, Cg [, A if alpha_mode == 2]):
    nb*nb        DC-plane coefficients
    [ nb*nb intra modes, iff INTRA_DIR ]   -> binding 6, not this buffer
    nb*nb blocks x 64 coefficients
```

`nb` comes from the tile header (`res_level`, `chroma444`) per SYNTAX.md 4.2.
This is a per-tile *variable* length; `coef_stride` is the padded stride the host
reserves (6240 for 4:2:0 at `res_level` 0; 16640 covers the widest 4:4:4 + alpha
tile, `kCoefStrideMax`). Positions past a tile's actual count are zeroed.

Note this is finer-grained than PAPER 3.2.3's "64 blocks x 64 coefficients"
sketch, which omits the DC plane and the chroma/alpha planes. `ref` is normative
and PAPER is not; see `syntax_constants.h` section 8.

**CBF bits**: bit `i` of a tile's cbf words is 1 when coding unit `i` was coded
(`CBF == 1`). Unit indices follow the order above. 16 uints per tile covers the
260-unit maximum.

Under the dense layout the kernel zeroes each tile's coefficient region and cbf
words before decoding, so a `CBF == 0` unit and the padding both read back as
zero. Under the sparse layout it zeroes the cbf and length words only: an
uncoded unit is the one whose published length is 0, and the padding is never
read.

## Shared memory

| | bytes |
|---|---|
| `s_cum[8][27][16]` | 13824 |
| `s_scan[5][64]` | 1280 |
| `s_ulen[32][66]` | 8448 |
| per-tile geometry, flags | ~1000 |

About 24 KiB against a 32 KiB budget, and the `s_cum` row is the whole of the
growth: it was 8192 bytes at the 16-context stride and is 13824 at the
27-context one since `CTX_V3` landed. **Every stream pays that, not only a v3
one** -- the table is uploaded at one stride whichever model the stream
selects, so the host has one layout to build and contexts past the coded count
are simply never selected, and the price of that simplicity is a 1.7x wider
per-workgroup table load on a v1 stream as well. It is visible in the RADV
Pass A column (`../README.md`, "Timing"), and if it ever has to come back the
lever is a specialisation constant on the stride rather than a second layout.

The scan array holds only the *tabulated* scans. `XFORM_LARGE`'s 16x16 and
32x32 zigzags are computed from `nxs_zigzag_*()` instead: a 1024-entry table
would have been 4 KiB against the 1.25 KiB all five small scans take, and the
zigzag is a rule rather than data.

The paper assumed a
1024-entry `slot2sym` table per context, but a workgroup holds 8 tiles that may
each name a different `table_set`, and 8 sets x 12 contexts x 1024 bytes does not
fit. The kernel stores cumulative frequencies instead and finds the symbol with a
4-step branchless binary search, which is exactly equivalent to indexing
`slot2sym` (`slot2sym[k] == s` iff `cum[s] <= k < cum[s+1]`).

## The v3 units

With `INTRA_DIR` each coded plane carries one **mode unit** between its
DC-plane unit and its block units (`docs/SYNTAX.md` 9.1), holding `nb*nb` intra
modes in raster order, each coded against the most probable mode of its left
and above neighbours. Both neighbours live in the *same* unit, and a unit
belongs to exactly one lane, so the MPM derivation only ever reads values that
lane has already produced — whatever the interleaved schedule does with the
other units. That is why the modes are a unit of their own rather than a symbol
attached to each block, and it is what lets the kernel read them straight back
out of binding 6 with a plain read-modify-write and no atomic: each plane's
64-slot region starts on a word boundary, so no two lanes ever share a word.

Under `CTX_V2` the mode is one symbol in context 15 over the alphabet 0..8;
without it, a 1-bit "is MPM" flag and a 3-bit non-MPM index, both bypass.

`CTX_V2` also gives the DC-plane unit CBF, LAST and LEVEL contexts of its own
(12, 13, 14). The LEVEL context therefore became a *field* on the unit rather
than a derivation: `kCtxNone` means the banded contexts of 9.3, anything else
is used as-is.

## The minor-6 units

`CTX_V3` (tool bit 25, `docs/SYNTAX.md` 9.9) conditions CBF and LAST on the
**neighbour class** the lane carries: 0 nothing to condition on, 1 the
previous coefficient unit **this lane** finished in this plane was not coded,
2 coded and sparse (`LAST < 4`), 3 coded and dense. Each lane keeps two
registers -- `g_nbr` and `g_ngrp`, the class and the plane it belongs to, both
reset at a plane boundary -- and writes them once when it finishes a unit.
Because a lane owns units `l, l+N, l+2N, ...`, that unit is always one the lane
has already finished, so there is **no cross-lane read, no extra barrier and no
change to the round loop**; the context index stays one compare, one add and
one lookup. For the ordinary tile a lane owns one column of 8x8 blocks, so the
class describes the block directly above.

The conditioning is per **coding unit** -- the coefficient group -- and never
per transform block, so the kernel never reads a transform size to derive a
context. What it costs is the `s_cum` row of the table above.

`LEVEL` additionally splits the coefficient at scan position `LAST` into two
rows by band, and the DC term of a DC plane into one of its own -- both are
derivations the kernel already had the inputs for. v3 is a *refinement* of v2,
not a replacement: it keeps v2's sixteen rows, which is why the stream header
refuses bit 25 without bit 21.

`TAB_V2` (tool bit 26) is invisible to the kernel entirely: it only changes how
the host parses the frame's transmitted table sets into the same `cum` upload,
so it is a change to `nxvc_vkdec_parse.cpp` and nothing else. Each context
inside a set gains a one-bit `row_coded` flag, an uncoded row is the built-in
default, and a set is therefore variable length -- so all the transmitted sets
are read as one bit sequence, padded to a byte boundary once at the end.

`XFORM_4X4_SPLIT` (tool bit 19) adds one phase, `kPhSplit`: a coded block unit
of a tile whose word1 bit 28 is set sends a 1-bit flag between its CBF and its
LAST, and when it is 1 the unit's scan becomes `kScan4Split` -- four
concatenated 4x4 sub-blocks -- and the LEVEL band of a scan position becomes
its position *within* its sub-block. Both are one line each, because the scan
id and `nxs_band_pos()` were already the only two things the storage and the
banding went through. The flag is published to Pass B as one bit per block in
binding 6's region, and unlike a mode word that word IS shared between lanes
(block `b` belongs to lane `b % LANES`), so the write is an `atomicOr` into the
pre-zeroed region exactly as the unit lengths are.

`INTRA_CFL` (tool bit 24) widens the mode-unit alphabet to ten for **chroma
planes only**, so `nmodes` is a field of the unit rather than a constant.

`XFORM_LARGE` (tool bit 27) is **half implemented**: everything on this side is
here and Pass B's reconstruction is not, so `kToolsSupported` does not list the
bit and no stream reaches any of it. What is here is the per-plane block edge
(`8 << xform_size`, capped by the plane's own coded extent), the two computed
zigzags, and the scan-*group* scaling of LAST and the LEVEL bands: a unit of
more than 64 coefficients reuses the same 64-position class table and the same
four bands over its 64 equal-sized groups, the class naming the group and
`last_shift` raw bypass bits the position inside it. No new context and no new
symbol exists at any transform size, which is what lets one trained set of
frequencies serve all three. The per-unit length field widens from eight bits
to sixteen on such a tile (LAST + 1 reaches 1024) without the region growing,
because the same thing that makes those units big makes them few.

`SIGN_HIDE` drops the sign at scan position `LAST` when `LAST >= 4` and makes
it the parity of the sum of the unit's magnitudes. The kernel stores that
coefficient provisionally positive, keeps its magnitude in a register, and
negates it at the end of the unit — so binding 3 stays write-only.

## ENTROPY_LITE (stream tool bit 24)

Specialisation constant **3** (`ENTROPY_MODE`, `kSpecIdEntropyMode`) selects
which entropy tool the dispatch decodes:

| value | path |
|---|---|
| `0` = `kEntropyRans` | everything above, unchanged in every respect |
| `1` = `kEntropyLiteFixed` | `ENTROPY_LITE`, `kLiteFixed` variant |

`main()` branches on it at the very top, so the test is dynamically uniform
and neither path's barriers leave uniform control flow — the same discipline
`READ_PTR_MODE` follows. The normative source is `ref/src/entropy_lite.{h,cpp}`;
only FIXED is implemented, and a tile whose header names RICE (the header's
`table_set` is the variant selector under this tool, since there are no
probability tables for it to name) is rejected as `kStatusBadHeader`.

### The layout

A tile's Lite payload is five byte-aligned sections, MSB-first inside a byte:

| | contents |
|---|---|
| **H0** | one bit per group of `kLiteCbfGroup` (16) units, "any unit here is coded" |
| **H1** | for each group whose H0 bit is 1, one bit per unit of that group |
| **P** | per coded *coefficient* unit: `lite_last_bits(ncoef)` bits of LAST, then 3 bits of magnitude class. Mode units contribute nothing |
| **S** | per coded unit: a mode unit's `nbx*nbx` is-MPM flags, or a coefficient unit's LAST significance bits for scan positions `0 .. LAST-1` (position LAST is nonzero by construction) |
| **B** | per coded unit: a mode unit's 3-bit non-MPM index per cleared flag, or, per nonzero scan position ascending, `kLiteMagBits[class]` bits of `|q| - 1` and one sign bit, 1 == negative |

Every offset is computable: H0's length is a constant of the unit list, H1's a
popcount over H0, P's the coded bits and the unit list, S's follows from P
(which carries LAST), and each unit's slot in B from a prefix sum over per-unit
body lengths that S and P determine. There is no coder state anywhere, so the
decode is **three workgroup prefix sums and then wholly independent per-unit
work**.

### Dispatch shape

```
workgroup     64 threads = 64 units in flight
groups        num_tiles            (one workgroup per tile)
unit u        thread u % 64
```

`lite_scan()` turns a per-unit width array into exclusive bit offsets in two
levels: each thread serially scans its own contiguous block of
`kLiteUnitsPerThread` (5) units, the 64 block totals go through a
Hillis-Steele scan, and the block offset is folded back in. `kLiteUnitsPad`
(320) is the padded unit count; `kMaxUnitsPerTile` is 264.

Section S is read twice — once to count the set (or cleared) flags that give a
unit its B length, once to decode — which costs a handful of byte loads and
saves keeping 64 flags per unit alive across a scan. The per-unit working set
is about 6.7 KiB of LDS on top of the rANS path's ~12 KiB.

The output is byte-for-byte the contract Pass B already consumes: binding 3 in
whichever layout `sparse` names, binding 4 CBF bits (a mode unit's bit stays
0, as in rANS), binding 5 status, binding 6 packed intra modes, binding 7 the
sparse length words. The tables binding (2) is never read. **Pass B cannot
tell which entropy tool produced the tile.**

### Where the time goes

Same corpus, same tiles, same coefficients — only the tool that codes them
changes. `--mode ballot`, sparse layout, best of 20 dispatches:

| | rANS | Lite | |
|---|---|---|---|
| RADV, 2048 tiles | 0.651 ms / 318 ns per tile | **0.158 ms / 77 ns per tile** | 4.1x |
| lavapipe, 512 tiles | 33.2 ms / 64.8 us per tile | **23.8 ms / 46.4 us per tile** | 1.4x |

RADV's Pass A was latency-bound on the longest tile's serial round chain, and
Lite has no chain to be bound by: what is left is the per-unit work plus three
prefix sums. lavapipe gains far less because it is CPU-bound on total
instructions rather than on a dependency chain, and the Lite payload is larger
(916 vs 612 bytes per tile on this corpus — the tool trades bitrate for
parallelism).

## Errors

A tile that cannot be decoded sets a non-zero status and stops; other tiles in
the workgroup are unaffected. Rejected: a reserved header field (SYNTAX.md 4.1),
`nsub_log2 != 3`, an initial state below `L`, a payload shorter than
`4 * lanes`, a renormalisation past the payload end, and any symbol illegal for
its phase — which now includes a MODE symbol of 9 or more in context 15 and a
non-MPM index above 7. The payload starts after the optional MV and alpha bytes, so
`nxs_tile_payload_offset()` is the only place that knows the header is variable
length.

## Building and running

```sh
cmake -S . -B build -DNXWARP_BUILD_VK=ON
cmake --build build -j4
ctest --test-dir build -R '^vk\.passA\.'
```

Harness directly:

```sh
build/vk/decoder/passA/nxvc-passA-test --tiles 2048 --iters 20
build/vk/decoder/passA/nxvc-passA-test --list
VK_DRIVER_FILES=/path/lvp_icd.x86_64.json build/vk/decoder/passA/nxvc-passA-test --tiles 512
```

Options: `--device SUBSTR`, `--tiles N`, `--seed S`, `--mode ballot|lds|both`,
`--subgroup N`, `--iters N`, `--entropy rans|lite`, `--intra`, `--validate`,
`--spv PATH`, `--quick`, `--list`.
Exit code 77 means "no usable ICD" and ctest reports it as a skip.

`--entropy lite` builds the corpus with `encode_tile_lite()` and specialises
the pipeline for `kEntropyLiteFixed`; the tiles, coefficients and RNG stream
are otherwise identical, so the two corpora are directly comparable.
`--intra` adds INTRA_DIR's mode units, which only the Lite test encoder codes
(the rANS one emits v1 syntax), so it is rejected with `--entropy rans`.
`--mode` still selects `READ_PTR_MODE`, which the Lite path does not use.

## Status

Zero mismatches against the CPU model on RADV (wave32 and wave64) and on
lavapipe (subgroup size 8), in both read-pointer modes, in both coefficient
layouts, and under both entropy tools. The test encoder is
byte-identical to `nxvc::encode_units` over random tiles spanning every
`res_level`, `chroma444`, `tskip` and `table_set`; `encode_tile_lite()` is
byte-identical to `nxvc::lite_encode_units` over the same span with INTRA_DIR
both on and off.

2048 tiles at 0.50 symbols/pixel decode in ~1.4 ms on a 7900 XTX (RADV),
informational. The kernel pays three `barrier()`s per scheduling round to keep
control flow uniform for the fallback path, and that is now **the** thing to
attack: with Pass B's fixed cost down from 890 to 248 ns per tile
(`../README.md`), Pass A is the larger of the two passes at every QP above 24,
and on the scaled Adreno estimate it is the pass that misses the frame budget.

The sparse layout cost Pass A a little: 124 → 170 ns per tile of fixed cost.
The length words and their atomicOr are part of it, but most of it is the loss
of the zeroing loop, which used to write the coefficient region in whole cache
lines and so warmed it for the scattered stores that followed. That is a real
trade and it is worth it — Pass B gained 642 ns per tile against it — but it is
worth knowing which direction it went.
