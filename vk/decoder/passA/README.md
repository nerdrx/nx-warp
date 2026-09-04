# Pass A — interleaved rANS entropy decode

Pass A turns each tile's rANS payload into int16 coefficients in the layout
Pass B consumes. It is the first of the two decoder dispatches (PAPER 3.2.1).

| file | role |
|---|---|
| `syntax_constants.h` | **every** bitstream-dependent constant, compiled as both C++ and GLSL |
| `rans_decode.comp` | the compute kernel |
| `passA_model.{h,cpp}` | CPU model of the kernel, line-for-line; GPU and CPU must agree bit for bit |
| `passA_test_encoder.{h,cpp}` | test-only rANS encoder (byte-identical to `nxvc::encode_units`) |
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

The kernel zeroes each tile's coefficient region and cbf words before decoding,
so a `CBF == 0` unit and the padding both read back as zero.

## Shared memory

| | bytes |
|---|---|
| `s_cum[8][16][16]` | 8192 |
| `s_scan[4][64]` | 1024 |
| per-tile geometry, flags | ~1000 |

About 10 KiB, up from 7.5 when the context count was 12 (`docs/SYNTAX.md` 9.3,
tool bit 21 `CTX_V2`), against a 32 KiB budget for the workgroup's 8 tiles. The
table is always uploaded at the 16-context stride whichever model the stream
selects, so the host has one layout to build and contexts past the coded count
simply are never selected. Implementing `CTX_V3` (tool bit 25, 22 contexts)
takes `s_cum` to 11264 bytes and the total to about 13.5 KiB. The paper assumed a
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

**`CTX_V3` is specified but not implemented here.** `nxvc_vkdec_parse.cpp`
does not list tool bits 24 or 25 in `kToolsSupported`, so a stream that sets
either is refused with `VERSION` rather than mis-parsed. What implementing it
would take is worth writing down, because it is unusually little.

`CTX_V3` (tool bit 25, `docs/SYNTAX.md` 9.8) conditions CBF and LAST on
`prev_cbf`, whether the previous coefficient unit **this lane** decoded in the
same unit class was coded. Each lane keeps three bits of state --
`prev_cbf[ucls]`, all zero at the start of the tile -- and writes one of them
when it finishes a unit. Because a lane owns units `l, l+N, l+2N, ...`, that
unit is always one the lane has already finished, so there is **no cross-lane
read, no extra barrier and no change to the round loop**; the context index
stays one add and one lookup. It costs LDS: `s_cum` grows from 8192 to 11264
bytes for the workgroup's 8 tiles. For the ordinary tile a lane owns one column
of 8x8 blocks, so `prev_cbf` is the coded flag of the block directly above.

`TAB_V2` (tool bit 24) is invisible to the kernel entirely: it only changes how
the host parses the frame's transmitted table sets into the same `cum` upload,
so it is a change to `nxvc_vkdec_parse.cpp` and nothing else.

`SIGN_HIDE` drops the sign at scan position `LAST` when `LAST >= 4` and makes
it the parity of the sum of the unit's magnitudes. The kernel stores that
coefficient provisionally positive, keeps its magnitude in a register, and
negates it at the end of the unit — so binding 3 stays write-only.

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
`--subgroup N`, `--iters N`, `--validate`, `--spv PATH`, `--quick`, `--list`.
Exit code 77 means "no usable ICD" and ctest reports it as a skip.

## Status

Zero mismatches against the CPU model on RADV (wave32 and wave64) and on
lavapipe (subgroup size 8), in both read-pointer modes. The test encoder is
byte-identical to `nxvc::encode_units` over random tiles spanning every
`res_level`, `chroma444`, `tskip` and `table_set`.

2048 tiles at 0.50 symbols/pixel decode in ~1.06 ms on a 7900 XTX (RADV),
informational — the kernel currently pays three `barrier()`s per scheduling
round to keep control flow uniform for the fallback path, which is the obvious
thing to attack if Pass A ever needs to be faster.
