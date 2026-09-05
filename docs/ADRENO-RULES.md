# Adreno rules

Four rules that every shader in this codec obeys, the measurements behind them, and the checker
that keeps them true.

They all come from one device: the **Adreno 650** in the Pico 4, running Qualcomm's proprietary
Vulkan 1.1 driver. It is the target the whole pure-compute path is designed around
([COMPATIBILITY.md](COMPATIBILITY.md)), and it is the only device in the matrix where any of this
was observed. Two of the four are correctness rules, not performance rules: the driver did not
merely make slow code, it made **wrong** code, and it did so silently, with `spirv-val` clean and
every desktop driver bit-exact on the same binary.

That is what makes them worth a document and a linter rather than a review habit. A rule whose
violation shows up as a crash polices itself. These do not.

---

## The rules

| # | Rule | Lint rule id | Severity |
|---|---|---|---|
| 1 | No function-scope array indexed by a non-constant expression | `dynamic-local-array-index` | error |
| 1a | ... including one indexed only by a loop counter | `loop-local-array-index` | advisory |
| 2 | No large struct passed to a function by value, and no whole-record copy out of a buffer | `large-struct-by-value` | error |
| 3 | No `subgroupClustered*` in normative shaders | `subgroup-clustered` | error |
| 4 | No `glslc -O`, and no redundancy-elimination pass in any SPIR-V build rule | `spv-unsafe-opt` | error |

Run the checker over the tree:

```
python3 scripts/shader-lint.py            # or: ctest -R lint.shaders
python3 scripts/shader-lint.py --strict   # advisories fail too
```

It reads GLSL and CMake as text. No GPU, no `glslc`, no build.

---

## Rule 1 — indexed local storage

**A function-scope array indexed by anything the compiler cannot fold to a constant is lowered to
private (scratch) memory, and in at least one measured case was read back wrong.**

The evidence is commit `d47c095` ("perf(passB): Adreno puts local arrays in private memory, and
reads them back wrong") and the Pico section of [`vk/decoder/README.md`](../vk/decoder/README.md).
Pass B's reconstruction held its intermediate rows in local `int[8]` arrays. On the Adreno 650:

- Each such array cost hundreds of bytes of scratch per invocation. At 256 invocations per
  workgroup that is the occupancy budget spent on something a desktop driver keeps in registers.
- Worse, on 4:4:4 streams **the chroma residual came back dropped while luma stayed exact**. Same
  binary, same workgroup, two arrays of the same shape, one right and one wrong.

The second point is the whole reason this is rule 1. A performance cliff is a number in a table. A
plane of the picture quietly losing its residual is a decoder that produces the wrong image and
says nothing, and it was found only because Pass B has a GPU-vs-CPU harness that checks every
pixel.

### What counts

- `int tmp[8];` in a function, indexed by anything but a literal or a `#define`d constant.
- `const int table[4] = ...;` indexed dynamically. Read-only does not help: it is still a `Private`
  variable with an `OpAccessChain` into it. Bit-pack the table into a `uint` and shift instead.
- `out`/`inout` array **parameters**. These are the worst form: on top of the scratch allocation,
  glslang copies the whole array in at the call and back out at the return.

### The rewrites

In order of preference:

1. **A vector.** Eight values become `ivec4 lo, hi`; every access is a constant component select,
   which is a register on every driver. This is what the encoder's 8-point butterflies do
   (`nxe_fdct8_1d` / `nxe_idct8_1d` in `vk/encoder/forward/nxe_enc_common.glsl`) and what the bench
   Pass B does (`nxb_idct8`).
2. **A select ladder** when the index really is dynamic. `nxb_at8` / `nxb_set8` in
   `bench/shaders/nxb_common.glsl` are the pattern: an `if`/`?:` chain over the components, which
   compiles to predicated moves and touches no memory.
3. **Shared memory.** LDS *is* addressable and correct on this driver — the rule is about `Private`
   storage, not about indexing as such. `shared uint s_red[...][...]` indexed by
   `gl_SubgroupID` is fine and is what `E1_stats.comp`'s cross-subgroup reduction uses.
4. **An SSBO slice.** Same reasoning: a computed index into a buffer is a normal load.

Restructuring so the value never has to be held at all is better than any of them. `E0_convert.comp`
used three `int[16]` staging arrays for a lane's 4×4 pixel quad; it now walks the quad two rows at
a time, because a row pair is exactly what the 4:2:0 box filter consumes and nothing needs to live
longer than that. The arrays did not get rewritten, they got deleted.

### Why 1a is only an advisory

A local array indexed *only* by the counter of a constant-bound `for` loop is correct as long as
the unroller reaches it, at which point every index is a constant and the array is scalarised away.
That holds today: `--loop-unroll` is in the shared pass list, and the trip counts are 4 and 8.

It is reported separately rather than ignored because it is a standing bet on the optimiser, and
the bet gets worse as trip counts grow. `--strict` fails on it.

---

## Rule 2 — struct copies

**A struct passed by value, or copied whole out of a buffer, becomes a private-memory temporary.**

The same lowering as rule 1, reached a different way. `TileParam p = params[tileIdx];` in
`warp/glsl/warp_tile.comp` read like a cheap alias and was not: `TileParam` is 20 ints with an
`int h[9]` inside it, so the statement materialised 80 bytes of scratch per invocation, and
`compute_corner(int i, TileParam p)` then copied it again.

The fix is to read the fields you actually need, where you need them: `params[t].h[6]` is a plain
load with a constant member index and a workgroup-uniform buffer index. Where several callers want
the same handful of fields, pass those fields, or pack them into a vector — `nxb_warp_delta` takes
the four packed corner words as a `uvec4` rather than the whole `NxbTileRec`.

The checker flags a struct above `--struct-threshold` components (8 by default) **or**, at any
size, a struct with an array member — an array member is the part that cannot be scalarised into
registers, so size alone is the wrong test. A five-scalar record assembled field by field and
stored once is fine and is not flagged.

---

## Rule 3 — no `subgroupClustered*`

This one is not an Adreno measurement but a spec rule, paper 3.2.6:

> Cluster operations use `subgroupBallot` plus masks derived from `gl_SubgroupInvocationID & ~7`,
> never `subgroupClustered*` (weaker support on Adreno's proprietary compiler).

Emulate with a ballot masked by a cluster mask, then `subgroupShuffle` / `bitCount` over it. The
emulation is not taken on trust: `vk/common/shaders/nxvc_subgroup_semantics.comp` computes both and
compares them, pinned to subgroup sizes 8, 16, 32 and 64, against a CPU reference. That kernel holds
the only two `subgroupClustered*` calls in the tree, they are its oracle, they sit behind
`NXVC_HAVE_CLUSTERED`, and they carry `nxvc-lint: allow` waivers saying so.

The rule is enforced on `vk/decoder`, `vk/encoder` and `vk/common`. In `bench/`, `android/` and
`warp/` a clustered op is unwise rather than a spec violation, so it is not flagged there.

---

## Rule 4 — the SPIR-V pass list

**`spirv-opt`'s redundancy-elimination passes miscompile on the Adreno 650, so no shader that
reaches a device may be built with `glslc -O`.**

Redundancy elimination common-subexpression-eliminates the duplicate `OpAccessChain` that a load
and a store to the same shared-memory word each produce, so both end up using one pointer id. The
result is valid SPIR-V: `spirv-val` accepts it, and RADV (wave32 and wave64) and lavapipe are
bit-exact on it. The Adreno 650 driver miscompiles Pass B's LDS transpose when fed that form — the
word that comes back out of shared memory belongs to a different slot, while every input to it (the
coefficient word, the dequantised coefficient, the row-pass result, and both the store and the load
address) is verified bit-exact by the bench's `--selftest` bisect.

Dropping the two passes is what makes the Phase 0 gate bit-exact on the target device. Nothing is
given up: the driver's own compiler does its own redundancy elimination. Bisected pass by pass on
device; see `bench/README.md`, "Adreno and spirv-opt".

The excluded passes are `--redundancy-elimination` and `--local-redundancy-elimination`. **`glslc
-O` runs the full built-in list, both of them included**, which is the trap: it looks like an
ordinary optimisation flag and it is the one thing that must not be used.

### One rule for the whole tree

The pass list lives in **`vk/common/cmake/NxvcShaderPasses.cmake`** (`NXVC_SPIRV_SAFE_PASSES`) and
the build-time worker in **`vk/common/cmake/nxvc_gen_spv.cmake`**. Every component compiles with
`-O0` and then runs `spirv-opt` with that list:

| Component | How |
|---|---|
| `bench/` | `bench/CMakeLists.txt` → `nxvc_gen_spv.cmake`, `STYLE=plain` |
| `android/` | `android/CMakeLists.txt` → same, `STYLE=plain` |
| `vk/encoder/` | `vk/encoder/CMakeLists.txt` → same, `STYLE=guarded` |
| `warp/` | `warp/CMakeLists.txt` → same, `STYLE=raw` |
| `vk/common/` | `NxvcEmbedShaders.cmake`, which includes the pass list directly |
| `vk/decoder/` | `passA/cmake/gen_spv.cmake`, `passB/cmake/gen_spv.cmake` — still their own copies |

This used to be one copy of the rule per component, and they had drifted: `android/`, `vk/encoder/`,
`warp/` and `vk/common/` were all still passing `glslc -O`, so four of the six components — including
the Android client that ships to the Pico 4 — were being built with the pass that was known to break
that exact device. Only `bench/` and the two decoder passes had the safe list.

`NXVC_SPV_PASSES` in the environment overrides the list (empty means no optimisation at all). It
exists to bisect vendor miscompilations, which is how the excluded pass was found. `NXB_SPV_PASSES`
is honoured as an alias because `bench/README.md` documents it under that name.

---

## Waivers

A rule can be waived in the source, with a reason:

```glsl
// nxvc-lint: allow subgroup-clustered -- oracle for the emulation this kernel validates
const uint oracle_sum = subgroupClusteredAdd(v, 8);
```

On the offending line or the line above it. The reason is mandatory — a bare `allow` does not
suppress anything. There are two waivers in the tree, both in
`vk/common/shaders/nxvc_subgroup_semantics.comp`.

---

## Verifying a rewrite

Every fix under these rules changes the SPIR-V, so every fix has to be shown not to change the
*output*. The components each have a GPU-vs-CPU harness; run it on RADV and on lavapipe, and
compare against the same run from before the change:

```
# encoder E0/E1/E2, then E3/E4/E5 with their pinned stream digests
nxvc-stats-test --device 0 --no-bench
nxvc-vkenc --selftest --device 0

# the predictor, 10000 tiles against the CPU reference
nxvc-warpdiff

# bench Pass A and Pass B
nxbench-host --selftest

# and the same four with the CPU ICD
VK_DRIVER_FILES=/path/to/lvp_icd.x86_64.json ...
```

Zero mismatches is necessary but not sufficient: `nxvc-vkenc --selftest` prints a digest per
configuration, and those digests are what actually establish that the bitstream is byte-identical.
`ctest -R 'vk.encoder|warp'` runs the same checks with the lavapipe-pinned variants wired in.
