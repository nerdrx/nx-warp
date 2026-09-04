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
| **E3** `forward` | one group per tile, 64 lanes | DC-plane intra prediction, residual, forward 8×8 DCT through LDS, dead-zone quantisation with the weighting matrix, sign hiding, coefficients in coding-unit order. Directional intra behind a specialization constant | **done** |
| **E4** `rans_encode` | 8 lanes per tile, 8 tiles per group, persistent | rANS backwards over the operation list into a bounded per-tile slot; writes the tile header and the byte count E2 scans | **done** |
| **E5** `packetize` | one group per tile | compaction into tile-row segments, tile-row and frame headers, straight into the host-cached output buffer | **done** |
| E3b `reconstruct` | one group per tile | the decoder's Pass B, *byte-identical SPIR-V*, writes the new reference | not started |

The pass letters follow the paper's table with one renaming: the paper calls
the transform pass E2 and the reconstruction pass E3, but E2 is taken here by
the prefix sum, so the transform is E3 and the reconstruction becomes E3b.
E0b is a placeholder for work the paper folds into E0.

The reconstruction pass being *byte-identical shader code to the decoder* is
the single most important rule in the project: the encoder must never hold a
reference the decoder cannot reproduce.  E3 already contains the reconstruction
the directional predictor needs (it is the same arithmetic), so E3b is a matter
of writing the reference picture out, not of writing the maths again.

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
forward/
  nxe_enc.h           the E3/E4/E5 contract: frame parameters, the per-tile
                      job record, the coefficient and slot geometry
  nxe_tables.h/.c     the normative constant tables, so the directory builds
                      without the reference codec
  nxe_enc_common.glsl GLSL mirror of all of the above
  forward.comp        E3
  rans_encode.comp    E4
  packetize.comp      E5 (two variants: zero the frame's bytes, then write)
  forward_cpu.h/.c    bit-exact CPU model of E3
  rans_cpu.h/.c       bit-exact CPU model of E4 and of E5's layout
tools/
  vk_min.h/.cpp       throwaway Vulkan boilerplate; delete when vk/common lands
  nxvc-stats-test.cpp GPU-vs-CPU diff harness and timing for E0/E1/E2
  nxvc-vkenc.cpp      the encoder harness: --in yuv --out nxv
  nxe_host.cpp        its frame driver (the E0 stand-in, the table-set choice)
  nxe_vk.cpp          the Vulkan backend for E3/E4/E5
  nxe_selftest.cpp    the built-in configuration table and its digests
cmake/gen_spv.cmake   glslc → SPIR-V → C array
```

## The two things in E4 that are not obvious

**A GPU cannot materialise the operation list.** `ref/src/entropy.cpp` encodes
a tile by building the whole global operation list and walking it backwards.
But `encode_units` drives the lane machines in *rounds* — one operation per
unfinished lane per round, in lane order — and a lane machine reaches `kDone`
once and never restarts. So lane *l* occupies rounds 0..nops[l]-1 and the
global order is (round ascending, lane ascending). The backward sweep is
therefore eight lanes in lockstep over descending rounds, with a running
emission counter for the byte order and no list at all.

**The bytes are produced back to front.** Their offsets are only known once the
emission count is, which would force a counting pass and then a placing pass
over the same operations. Instead the renormalisation words are anchored at the
*end* of the tile's slot, one whole word each, emission *e* at
`slot_end - 1 - e`: the position is known the moment the word is produced, and
reading that region forwards yields exactly the bitstream's order. It costs a
word per 16-bit emission and buys the whole second sweep.

The other thing worth knowing is that the sign-data-hiding decision, which the
reference makes by comparing `double` squared errors, is reproduced exactly in
integers. Multiplying the comparison by 256 and factoring the difference of
squares gives `-d * step * (32a - (2m + d)*step)`, formed as a 64-bit product
with `imulExtended`; the derivation is at `nxe_hide_sign_unit`.

Tests live in `tests/vk-encoder/` and are named `vk.encoder.*`. Anything
needing a GPU exits 77 and is reported as a ctest skip when no ICD, no device,
or no device meeting the kernels' requirements is present.

`vk.encoder.acid.*` is the one that decides whether the coding passes are
right: it drives `nxv-enc` and `nxvc-vkenc` from one description of the same
synthesized picture, requires the two streams to be byte-identical, and then
requires `nxv-dec` to decode both to identical pixels. It needs the reference
tools, so it exists only in a build from the repo root; the standalone encoder
build gets `vk.encoder.forward.cpu`, which pins the same streams by digest.

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

The coding passes have the same shape of harness:

```
./build-enc/nxvc-vkenc --selftest --cpu          # the models and their digests
./build-enc/nxvc-vkenc --selftest --device 0     # GPU against the models
./build-enc/nxvc-vkenc --in f.yuv --w 4096 --h 2048 --eyes 2 --pix yuv420p \
                       --qp 24 --out f.nxv --check --bench 50
```

`--check` runs the CPU models alongside every frame and fails on the first
coefficient or byte that differs; `--cpu` is a complete encoder with no Vulkan
at all. The flags mirror `nxv-enc`'s, which is what lets the two be pointed at
the same input and diffed.

What the coding passes do **not** implement, and refuse rather than ignore:
inter prediction, resolution levels, alpha, custom probability tables, and
more than eight rANS lanes on the GPU path (`--nsub 4` and `5` are CPU-only;
paper 6.3 fixes v1 at eight). The RD trellis is deliberately absent: it changes
which levels are coded, never how they are decoded, and it is a `double`
trellis. `--no-rdo` is the reference configuration this pipeline reproduces.

## Measured

`nxvc-stats-test --device 0`, 2048×4096 (both eyes, 2048 tiles), RGBA8 4:2:0,
median of 50 iterations, timestamp queries around each dispatch:

| Device | E0_convert | E1_stats | sum |
|---|---|---|---|
| RX 7900 XTX (RADV) | 0.056 ms | 0.140 ms | 0.196 ms |

`nxvc-vkenc --bench 50`, the same geometry (2 × 2048², 2048 tiles), 4:2:0,
8 rANS lanes, frame matrix 1, no directional intra, RX 7900 XTX on RADV:

| QP | E3 forward | E4 rans_encode | E2 prefix | E5 packetize | total | bpp |
|---|---|---|---|---|---|---|
| 0  | 1.295 | 4.185 | 0.010 | 0.616 | **6.107** | 7.27 |
| 16 | 1.239 | 3.578 | 0.009 | 0.261 | **5.087** | 2.77 |
| 24 | 0.915 | 2.219 | 0.009 | 0.103 | **3.247** | 1.11 |
| 32 | 0.454 | 0.394 | 0.009 | 0.025 | **0.882** | 0.26 |
| 45 | 0.438 | 0.343 | 0.009 | 0.023 | **0.813** | 0.12 |

Directional intra costs E3 8.2 ms at QP 24: the reference derivation of
SYNTAX.md 7.4 has the full 8×8 raster dependency, so the 96 blocks of a tile
are strictly serial and only the 64 lanes inside one block go wide.

### The RX 580 extrapolation

The RX 580 is the platform §3.6 budgets and it is not this box, so the number
has to be derived. Two independent ways of deriving it agree, which is the only
reason it is quoted at all.

The second RADV device here is the 9950X3D's integrated Raphael, two RDNA2 CUs
against the 7900 XTX's ninety-six. At 1024² (256 tiles, QP 24) it takes 15.8 ms
against the 7900 XTX's 1.59 ms — **ten times slower on forty-eight times fewer
CUs**, which is the measurement that says E4 is latency-bound rather than
throughput-bound, because a throughput-bound kernel would have been forty-eight
times slower. Scaled to 2048 tiles that is about 126 ms; an RX 580 has eighteen
times Raphael's CUs and a slightly lower clock, giving **11–12 ms**.

From the other direction: all 512 waves are resident on an RX 580 (GCN4 holds
forty wave64 per CU, so thirty-six CUs hold 1440), so what is left is clock —
1.34 against 2.4 GHz — and issue rate, wave64 on GCN4's SIMD16 taking four
cycles where RDNA3's SIMD32 takes two. That is 3.6× on E4, and E3 scales closer
to its 10× ALU ratio but is not saturating either. **10–17 ms** for the three
passes together.

§3.6 budgets the whole encoder at 2.5–4 ms on an RX 580 and under 1 ms on a
7900 XTX. So the coding passes are three times over on the 7900 XTX and three
to five times over on the platform that decides. The reason is structural
rather than a missing optimisation, and it is worth stating plainly.

E4 has exactly `tiles × 8` lanes of parallelism — 16384 for this frame, 512
waves, under three per SIMD on a 96-CU part — because eight rANS lanes per tile
is the bitstream, not a choice (§6.3). Every one of those lanes is a
dependent chain: rANS state feeds forward, and the operation list has to be
generated before it can be walked backwards. So E4 is latency-bound at low
occupancy and the machine is mostly idle. Three changes took it from 29.5 ms to
2.2 ms and none of them touched the inner loop: making every tile resident
rather than 1024 of them, materialising each lane's whole operation list once
instead of regenerating a unit at every boundary crossing (with an exact
fallback when it does not fit), and anchoring the emission words so one sweep
does the work of two.

What is left is the generation itself. The remaining lever is to parallelise it
over *coding units* rather than over lanes — 99 units per tile is 203k threads
instead of 16k — which needs a per-unit scratch, a prefix sum over each lane's
units, and a gather. That is the next thing to do here, and it is why the RX
580 verdict is deferred rather than claimed.
