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

### The round loop, and its barriers

The two modes have **two loops**, selected by the specialisation constant — so
the test is dynamically uniform and each loop's barriers (or absence of them)
are in uniform control flow.

| | barriers per round |
|---|---|
| ballot | **0** |
| LDS fallback | **1** (was 3) |

*Ballot.* Nothing crosses the cluster: the mask, the prefix and the total all
come from `subgroupBallot` restricted to the tile's own lanes, and so does the
loop's exit test (`subgroupBallot(has_op) & cluster_mask`). A cluster therefore
runs its own schedule and needs no workgroup synchronisation at all. Every lane
of a cluster takes the same branch every round, so the cluster is never split
across a ballot even once a neighbouring cluster in the same subgroup has
finished — the same "a cluster never straddles a subgroup" invariant the mask
already rests on.

*LDS fallback.* A mask has to be published, consumed and then cleared, and each
hand-off used to take a barrier. Three masks per tile slot, used round-robin on
`round % 3`, remove two of them: at round *r* the cluster clears the buffer it
will next write at round *r + 2*, and the barrier of round *r + 1* sits between
the clear and that write — while the same barrier sits after round *r − 1*'s
readers, so the clear cannot race them either. Two buffers are not enough; the
clear would land in the same barrier interval as the write it prepares for.
The "is anyone still running" flag became a round **stamp** (`round + 1`, the
same value from every writer, exactly as racy as the flag it replaces) rather
than a flag, so it is never cleared and costs no barrier of its own.

One consequence, in an unreachable state, is worth stating: the `kMaxRounds`
limit is now a property of the **tile** rather than of the workgroup. The
ballot loop is per cluster, so a cluster that has already stopped is not marked
bad on a neighbour's account. `kMaxRounds` is 2^20 and a lane's symbol count is
bounded by the unit structure, so no stream reaches it; the model was changed
with the kernel so the two still agree exactly.

### Phase grouping

A lane's phase selects one of eleven handlers, and the eleven were a flat
if-chain every lane walked from the top. The three **mode** handlers now sit
behind the frame-uniform `INTRA_DIR` tool bit and the two **escape** handlers
behind a group of their own, which shortens the chain for the hot phases. An
exact subgroup vote on "is anyone escaping" was also measured and is *not* kept:
it cost RADV 4.5 % and bought lavapipe nothing, because both drivers already
skip a branch no lane takes.

## Memory layout

Seven storage buffers, set 0:

| binding | buffer | contents |
|---|---|---|
| 0 | bitstream | the whole frame's tile bytes, uint-addressed; pad 16 bytes past the end |
| 1 | tile descriptors | 4 uints per tile: byte offset, byte length, coef index, cbf index |
| 2 | tables | `cum[8][12][16]` as uints (`cum[16] == 1024` is implicit) |
| 3 | coefficients | `int16_t`, `coef_stride` entries per tile |
| 4 | cbf bits | `cbf_words` uints per tile, one bit per coding unit |
| 5 | status | one uint per tile, `kStatus*` |
| 6 | intra modes | `kModeWordsPerTile` uints per tile, one 4-bit mode per 8x8 block, 8 to a word |

Push constants (20 bytes): `num_tiles`, `frame_nplanes`, `coef_stride`,
`cbf_words`, `tools`. The last is the frame-uniform tool mask —
`kToolFlagCtxV2 | kToolFlagIntraDir | kToolFlagSignHide`, derived by the host
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

Lanes interleave over units (unit `u` belongs to lane `u % LANES`), so the four
units sharing a length word belong to four different lanes and the write is an
`atomicOr`. It goes into a per-tile-slot LDS array — 2112 B, taking the kernel
from 10.0 to 12.0 KB — and is flushed to binding 7 once per tile in
`kUnitLenWordsPerTile` coalesced stores, so the frame's ~200 000 atomics stay
local.

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
| `s_cum[8][16][16]` | 8192 |
| `s_scan[4][64]` | 1024 |
| per-tile geometry, flags | ~1000 |
| `s_renorm[3][8]`, LDS fallback only | 96 |

About 10 KiB, up from 7.5 when the context count was 12 (`docs/SYNTAX.md` 9.3,
tool bit 21 `CTX_V2`). The table is always uploaded at the 16-context stride
whichever model the stream selects, so the host has one layout to build and
contexts past the coded count simply are never selected. The paper assumed a
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

2048 tiles at 0.50 symbols/pixel decode in ~0.65 ms on a 7900 XTX (RADV),
informational.

### Where the round goes

The three `barrier()`s per round are gone (see above) and that is worth **2x on
lavapipe and nothing at all on RADV**, for a reason worth writing down: the
workgroup is 64 threads, so on any device whose subgroup is 64 wide the
workgroup *is* one subgroup and the driver elides `barrier()` outright. RADV
wave64 and the Adreno 650 are both in that case. Where the workgroup spans
several subgroups the barriers are real, and removing them shows up
immediately — lavapipe (8-wide subgroups, eight per workgroup) and RADV forced
to wave32 (two per workgroup):

| 2048 tiles, ballot, sparse | before | after |
|---|---|---|
| RADV wave64 | 0.671 ms | 0.645 ms |
| RADV wave32 | 0.617 ms | **0.526 ms** |
| lavapipe, 512 tiles | 64.5 ms | **16.5 ms** |

What is left on RADV is not a synchronisation cost and not a bandwidth cost.
Pass A there is **latency-bound on the single longest tile**: 128 tiles cost
0.60 ms and 2048 tiles cost 0.68 ms, so the frame time is very nearly one
workgroup's serial round chain, and the corpus' worst workgroup runs 630
rounds against a mean of 432. Four separate things were measured against that
chain and none of them is the answer:

| removed / added | RADV effect |
|---|---|
| every coefficient store | −1 % |
| a second complete 5-load LDS binary search per symbol | +5 % |
| the three barriers | 0 % (wave64) |
| the mode and escape handlers, grouped out of the chain | −2 % |

The cost is spread evenly across the round body — the union of the *hot* phase
handlers, which a 64-lane wave pays every round because its lanes are in
several phases at once. Halving it needs fewer symbols on the chain, not a
cheaper symbol; the levers that remain are the bitstream's own (`nsub_log2`
above 3 gives a tile more lanes and a shorter chain) and the host's
(`requiredSubgroupSize` 32 is worth 1.2x on RADV and is a pipeline flag, not a
kernel change).

Three of the candidates that looked promising were measured and rejected:
tables in registers instead of the LDS binary search is worth at most the 5 %
above; decoding two symbols per renormalisation step saves only the round's
frame, which the same measurement bounds at a few percent; and grouping tiles
by byte length cannot help, because after the ballot loop went per cluster the
frame's critical path is the longest *tile*, not the longest workgroup, and
sorting does not make that tile shorter.

The sparse layout cost Pass A a little: 124 → 170 ns per tile of fixed cost.
The length words and their atomicOr are part of it, but most of it is the loss
of the zeroing loop, which used to write the coefficient region in whole cache
lines and so warmed it for the scattered stores that followed. That is a real
trade and it is worth it — Pass B gained 642 ns per tile against it — but it is
worth knowing which direction it went.
