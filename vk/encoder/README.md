# vk/encoder — the NX Warp GPU encoder

The encoder side of the codec: a chain of Vulkan compute passes, all indirect
where the tile count varies, with no CPU work between them. It runs on the
compositor's own `VkDevice` (Monado's on Linux, the helper's on Windows), so
the source frame is already a `VkImage` in the right device and the handoff is
an image memory barrier plus a timeline semaphore, not a copy.

Specification: `docs/PAPER.md` §3.6 (pipeline shape, timeline values, GPU-side
rate control, host-cached output buffer), §4.6 and §4.6.1 (rate-control inputs
and the degradation ladder), §5.2 (perceptual terms), §1.3 (colour), §3.7
(bit-exactness and vendor rules), §3.2.6 (subgroup portability).

## The pipeline

| Pass | Shape | Work | State |
|---|---|---|---|
| **E0** `convert` | one group per tile, 16×16 lanes | import-format source → tile-major coded planes. RGBA8 / RGB10A2 → YCoCg-R, or 2-plane 4:2:0 YCbCr passed through. 4:4:4 and 4:2:0. | **done** |
| E0b `warp` | fullscreen, 8×8 | warped reference from the previous reconstruction and the pose delta; writes the warped image and the per-tile corner displacements | not started |
| **E1** `stats` | one group per tile, 256 lanes | per-tile mean luma, moments, sum of squared deviations, structure tensor, warped SAD at a per-tile offset | **done** |
| **E2** `prefix` | workgroup scan + single-block second level | exclusive prefix sum over per-tile byte sizes, up to 8192 tiles | **done** |
| E2b `transform` | one group per listed tile | residual, forward 8×8 integer DCT, dead-zone quantisation, RDO-lite, int16 coefficients and an exact symbol count | not started |
| E3 `reconstruct` | one group per tile | the decoder's Pass B, *byte-identical SPIR-V*, writes the new reference | not started |
| E4 `entropy` | 8 lanes per tile, 8 tiles per group | rANS encoding backwards over the symbol list into a bounded per-tile slot | not started |
| E5 `packetize` | one group of 1024 per view | compaction into tile-row segments, headers, segment descriptor table, rate feedback | not started |

The pass letters follow the paper's table. E0b and E2b are placeholders for
work the paper folds into E0 and E2; whoever lands them should rename rather
than keep the `b`.

E3 being *byte-identical shader code to the decoder* is the single most
important rule in the project: the encoder must never hold a reference the
decoder cannot reproduce.

## What is in the tree today

```
stats/
  tile_stats.h        the per-tile statistics record: the ABI between the GPU
                      kernels and rc/, the mode decision, and the packetizer
  rc_adapter.h        converts a record into what nxrc::TileStats wants
  nxe_common.glsl     GLSL mirror of the above plus the shared integer helpers
  E0_convert.comp     colour conversion / passthrough
  E1_stats.comp       per-tile analysis
  E2_prefix.comp      two-level exclusive prefix sum (3 passes, one source)
  stats_cpu.h/.c      bit-exact CPU models of E0, E1 and E2
tools/
  vk_min.h/.cpp       throwaway Vulkan boilerplate; delete when vk/common lands
  nxvc-stats-test.cpp GPU-vs-CPU diff harness and timing
cmake/gen_spv.cmake   glslc → SPIR-V → C array
```

Tests live in `tests/vk-encoder/` and are named `vk.encoder.*`. Anything
needing a GPU exits 77 and is reported as a ctest skip when no ICD, no device,
or no device meeting the kernels' requirements is present.

## Data layout

**Tile-major packed planes.** A 64×64 tile is 4096 contiguous samples, so a
workgroup that owns a tile reads one contiguous run and never strides by the
frame width — the layout the Pass-B-style kernels want. Samples are 16-bit
two's complement, two per 32-bit word, low half first. That is the `int16`
storage width §3.7 mandates, packed by hand rather than through an `int16_t`
SSBO so nothing depends on `VK_KHR_16bit_storage` and the shader ALU stays in
`int32` throughout. A thread always owns whole words, so packing is never a
read-modify-write race. Planes are laid out Y, then Co, then Cg.

**Frames that are not a multiple of 64** are padded to the tile grid by edge
replication in E0. Those tiles carry `NXE_TS_F_PADDED`; their statistics
include the replicated samples, because those are the samples actually being
coded.

## Two source paths

On Linux the compositor hands over a `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`
image (`G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` at 10 bits), already
foveated: the colour conversion and the chroma decimation happened upstream.
On Windows the SteamVR helper hands over RGBA through the D3D11 shared texture
(§3.8). E0 has both paths and the stream header records which colour space the
planes are in — `YCoCg-R` for the RGB path, `YCbCr` for the passthrough.
Converting the compositor's YCbCr to YCoCg-R would inject a rounding error into
every frame before a single bit was coded, because the matrix is not
integer-reversible; passthrough costs nothing and loses nothing. The
passthrough subtracts the chroma midpoint so both paths hand the same shape of
data downstream: unsigned luma, signed zero-centred chroma.

E0 reads the *stored codes* through a UINT view and never applies a transfer
function. The source must therefore already be display-referred; the reasoning
is at the head of `E0_convert.comp`.

## Bit-exactness

The CPU models in `stats_cpu.c` are the specification and the shaders are
validated against them, in the same relationship §3.7 sets up for the decoder.
The argument that the GPU cannot disagree is short: every accumulator is an
integer sum, integer addition is associative (and the `u32` accumulators are
associative modulo 2³²), so the reduction tree the hardware happens to build
cannot change the answer. The kernels read `gl_NumSubgroups` and
`gl_SubgroupID` rather than assuming a width, use only `subgroupAdd` and
`subgroupExclusiveAdd` (core Vulkan 1.1, no clustered ops per §3.2.6), and
combine across subgroups through shared memory with a barrier.

That is why the harness has to run on both lavapipe and RADV: they differ by
4× to 8× in subgroup width, which is the only axis along which these kernels
could go wrong.

## The statistics record and `rc/`

`nxe_tile_stats` is an array of 52-byte integer structs, one per tile.
`nxrc::TileStats` is a struct of arrays of floats. The shapes differ on
purpose — a GPU workgroup wants to write an exact integer record, a CPU
library doing `log2` over a tile array wants floats — and `rc_adapter.h` is the
single place that reconciles them, doing the division and the logarithm on the
host where they are free and harmless.

The one thing that is *not* free to differ is the gradient operator. E1 uses
the central difference with the neighbour clamped inside the tile, which is
exactly what `nxrc::compute_one_tile_stats` uses, because rc's classifier
thresholds are calibrated in absolute units against it. E1 accumulates the
undivided difference (halving an odd difference would need a fraction), so the
record's tensor is exactly 4× rc's, and the cross term is carried as two
unsigned sums so it stays exact and in range at 10 bits as well.

## Building

The directory is self-contained. From the repo root with `-DNXWARP_BUILD_VK=ON`
it is added by `vk/CMakeLists.txt`; on its own:

```
cmake -S vk/encoder -B build-enc -DCMAKE_BUILD_TYPE=Release
cmake --build build-enc -j4
ctest --test-dir build-enc
```

`glslc` is found on `PATH` or in the NDK's `shader-tools`. If the Vulkan
headers are not in `/usr/include`, pass
`-DNXWARP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>`.

To run the diff against a specific device, or against lavapipe:

```
./build-enc/nxvc-stats-test --list
./build-enc/nxvc-stats-test --device 0
VK_ICD_FILENAMES=/path/to/lvp_icd.x86_64.json ./build-enc/nxvc-stats-test --device 0
```

## Measured

`nxvc-stats-test --device 0`, 2048×4096 (both eyes, 2048 tiles), RGBA8 4:2:0,
median of 50 iterations, timestamp queries around each dispatch:

| Device | E0_convert | E1_stats | sum |
|---|---|---|---|
| RX 7900 XTX (RADV) | 0.056 ms | 0.140 ms | 0.196 ms |

§3.6 budgets the whole encoder at 2.5–4 ms on an RX 580 and under 1 ms on a
7900 XTX, so the analysis front end is using about a fifth of the 7900 XTX
budget and leaves the transform, reconstruction and entropy passes the rest.
The RX 580 is the platform that decides, and it is not this box; the number
above is a sanity check that the analysis passes are not the problem, not a
verdict.
