# NX Warp Phase 0 gate

The benchmark defined in `docs/PAPER.md` section 3.4, as real code. Six kernels
(K1 to K6) run on-device with the display active and a dummy reprojection pass
submitted every frame as co-tenant load, timed with `VK_QUERY_TYPE_TIMESTAMP`
pairs, 120 warm-up plus 600 measured frames, reported as p50/p95/p99 with a
10-minute thermal mode.

The same kernel code builds two ways:

- **Android**: NativeActivity, Vulkan 1.1, no OpenXR, no Java. This is the gate.
- **Headless host CLI**: no swapchain, no Android, any Vulkan ICD. This is for
  iterating on the shaders without a phone in the room. Its numbers are a
  regression signal, never the verdict.

The GLSL is not a mock. It is the code the codec will reuse, written to the
bit-exactness and portability rules of PAPER 3.2.6 and 3.7 from the first line.

## Running it

```sh
./run.sh                      # K1..K5 on the attached device
./run.sh --kernels k5         # just the gate kernel
./run.sh --kernels all        # include K6 hybrid
./run.sh --thermal 600        # the 10-minute thermal run
./run.sh --selftest           # bit-exactness against the CPU reference
./run.sh --k1-sweep 9         # the K1 copy variants, timed one dispatch at a time

./run-host.sh                 # same kernels, headless, on this machine
./run-host.sh --selftest
NXB_LAVAPIPE=1 ./run-host.sh  # force the CPU ICD (CI)
```

`run.sh` builds the APK, installs it on `$ANDROID_SERIAL` (or the first adb
device), launches it, waits, pulls the JSON out of the app's files directory
with `run-as`, and prints the section 3.4 table with PASS/FAIL per threshold and
the decision-rule verdict. Results land in `bench/results/`.

Options reach the app as an intent string extra, so no Java layer is needed:

```sh
adb shell am start -n org.nxwarp.bench/android.app.NativeActivity --es args "--kernels k5"
```

## Layout

```
shaders/      the kernels. nxb_common.glsl is the normative integer core.
src/core/     Vulkan scaffolding, resources, the run loop, rANS CPU side,
              the CPU reference used by --selftest, JSON and table output.
src/host/     headless CLI frontend.
src/android/  NativeActivity frontend, swapchain, K6 MediaCodec/AHardwareBuffer.
report.py     renders the section 3.4 table from a result JSON.
```

---

# Decisions the paper does not make

The paper is the spec. Where it is silent, or where two sections disagree, this
is what the bench does and why. Everything here is a candidate for promotion
into the paper, or for correction.

## Frame layout: one 2048 x 4096 image

PAPER 3.1 fixes the frame at 2 views of 2048^2 and PAPER 3.2.5 counts 2048
tiles. The bench stacks the two views into a single 2048 x 4096 image, which
makes the tile grid exactly 32 x 64 = 2048 tiles and keeps every address
computation a shift. A two-layer image or two images would measure the same
thing; this way the tile index is `(y >> 6) * tilesX + (x >> 6)` with no
per-view branch.

## Transform normalisation: shifts of 8 and 12, not 7 and 12

PAPER 1.4 specifies "our own 9-bit integer constants and a defined two-stage
shift (7 bits after the first dimension, 12 after the second, 16-bit
intermediates)". 7 and 12 cannot be right for a 9-bit-constant pair, and the
bench uses **8 and 12**. The derivation:

The flow graph computes `sum k(u) F(u) cos(...)` scaled by 2^9, without the 1/2
that the IDCT definition carries, so one dimension has a gain of 2^10. Two
dimensions give 2^20, and 8 + 12 = 20 normalises exactly. With 7 the result
comes out 2x hot.

The intermediates work out at 8 as well, which is the real check:

| stage | value | range |
|---|---|---|
| dequantised coefficient | `c` | clamped to +-8191 |
| after row transform, `>> 8` | `4 * f_row` | +-32764, fits int16 |
| after column transform, `>> 12` | `f_2d` | clamped to +-2048 |

So the 16-bit LDS intermediate the paper asks for falls out of the shift choice
rather than being asserted on top of it.

**Constants** (`round(512 * cos|sin(k*pi/16))`, all under 512, so every product
with a 16-bit intermediate stays far inside int32):

```
C4 362   C2 473   S2 196   C1 502   S1 100   C3 426   S3 284
```

**Multiply count.** PAPER 1.4 quotes Loeffler's 11 multiplies and 29 adds. The
bench uses the same flow graph with plain 4-multiply rotations instead of the
3-multiply trick: 16 multiplies and 26 adds. On a GPU an integer multiply is a
full-rate operation and the 3-multiply rotation trades it for two dependent
adds, which is the wrong trade when the dependency chain is what limits the
kernel. This is a deliberate deviation and it is measurable: if K3 is close to
its threshold on Adreno, the 11-multiply form is the first thing to try.

**Clamp ranges are normative.** PAPER 3.7 requires that overflow cannot differ
by vendor but does not give numbers. The bench fixes: dequantised coefficient
+-8191, stage-1 output +-32767, residual +-2048. These are in
`shaders/nxb_common.glsl` and mirrored in the CPU reference.

## Q4 is 1/16 pel

PAPER 3.2.2 says the four corner displacements are "in Q4 fixed point" and
PAPER 3.2.3 gives the bilinear blend as `(w00*p00 + ... + 128) >> 8`. Those two
statements are only consistent if Q4 means 4 fractional bits: weights
`(16-fx)*(16-fy)` and friends sum to exactly 256. So Q4 is 1/16 pel, not
quarter-pel. PAPER 6.5 separately says the *tile motion vector* is quarter-pel;
that is a different field and the bench does not model it.

## One transform plane, no chroma

The bench transforms **64 blocks per tile, one plane**, because that is what the
paper's own numbers describe: 8 KB of coefficients per tile and 16.8 MB per
frame is exactly 4096 int16 per tile at one coefficient per pixel, and the
per-pixel op budget in 3.2.5 counts one row-plus-column transform per pixel.

A real 4:2:0 decoder adds 32 chroma blocks per tile: about +50% coefficient
traffic and +50% transform work. **K3 and the Pass B half of K5 therefore
understate a production decoder**, and the paper's 3.2.5 estimate understates it
by the same amount. This should be fixed in the paper, not just here. In Pass B
the residual lands on Y and chroma comes from the predictor, which keeps the
YCoCg-R conversion on the real path.

## LDS transpose: 8 KB with no 16-bit storage feature

PAPER 3.2.3 wants a 64 x 64 x 2 B = 8 KB transpose buffer. Declaring
`shared int16_t` needs `workgroupMemory16BitAccess`, which is another feature to
probe and another way for a vendor to differ. The bench packs two int16 into one
`uint` by hand and gets the same 8 KB with nothing but core Vulkan 1.1.

This is race-free by construction, which is the part worth stating: a thread
owns rows `2*sub` and `2*sub+1` of its block, those two rows are adjacent in the
transposed layout, and the pair is always 2-aligned, so **each `uint` is written
by exactly one thread**. The column pass then reads 4 consecutive `uint`s per
column, which is as coalesced as the layout allows.

## rANS: 8 contexts, and why the escape is fixed-width

PAPER 3.2.2 says 8 contexts and an 8 KB LDS table; PAPER 1.6 says 12 contexts
and 12 KB. PAPER 6.3 reconciles the lane count but not the context count. The
bench follows section 3, since 3.4 is the benchmark being implemented:
**8 contexts x 1024 slot-to-symbol bytes = 8 KB**, plus 512 B of packed
freq/cum. Context is `band(position) + 4 * (previous level != 0)`, causal within
the lane.

Two shape decisions the paper leaves open, both forced by the ballot:

- **Sign is one bypass bit**, and **escape is 8 raw bits**, not Exp-Golomb
  order-3. A variable-length escape would make the number of renormalisation
  points differ between lanes of a cluster, and every renormalisation point is a
  `subgroupBallot` that all 8 lanes must reach together. Fixed width keeps every
  ballot uniform without a bounding loop. Cost: three ballots per symbol (main,
  sign, escape) rather than one. If K4 misses its threshold on Adreno, folding
  the sign into a 32-symbol alphabet removes one third of them, and that is the
  first optimisation to try.
- **At most one renormalisation per point.** With L = 2^16, 16-bit renorm and
  10-bit probabilities this is provable rather than assumed, so there is no
  `while` loop in the kernel: after one 16-bit refill the state is always back
  above L.

The 8 initial states sit at the head of each tile slot as 8 x 4 bytes, matching
the paper's 32-byte flush accounting.

**Data rate.** 0.5 symbols/pixel over a 64x64 tile is 2048 symbols per tile, 256
per lane, and the encoder produces about **918 bytes per tile, 1.9 MB per
frame** at 3.6 bits/symbol. At 90 fps that is roughly 1.35 Gbit/s, far above the
150 to 400 Mbit operating point in PAPER 3.1. That is not a mistake: 3.4
specifies 0.5 symbols/pixel as the K4 stress point, and 3.2.1 separately
estimates real occupancy at 0.3. K4 is deliberately harder than the workload.

## Tile record is 32 bytes

PAPER 3.2.2 describes a 16-byte tile record. The four Q4 corner displacements
alone are 16 bytes, so the bench uses 32 and carries QP, mode and flags in the
same struct rather than in a side buffer.

**Pass A does not write the tile record.** The records are CPU-generated once
and shared by K2, K3, Pass B and Pass C. Adding the write would be 32 KB per
frame against 16.8 MB of coefficients, well under the noise floor, and keeping
them stable lets K2 and K5 warp identically.

## One kernel per pass

The kernels are not all recorded into one frame. Each kernel gets its own pass
of warm-up plus measured frames, with the reprojection co-tenant running in
every one of them. Recording all six into a single frame would put roughly 15 ms
of work in an 11.1 ms period, the device would present at a third of its refresh
rate, and the co-tenancy the gate is supposed to model would be gone.

## The co-tenant is adjacent, not concurrent

The reprojection dispatch is recorded immediately before the timed kernel in the
same command buffer, with a full barrier between them. It is deliberately not
concurrent: overlapping the two would put the co-tenant's time inside the
timestamp pair and make the number meaningless. What this arrangement does model
is the real effect — cache displaced by a full-frame pass, and the power and
clock state of a GPU doing this work every vsync.

## Timing method

- One timestamp pair per kernel per frame, `TOP_OF_PIPE` to `BOTTOM_OF_PIPE`,
  with `timestampPeriod` applied and `timestampValidBits` masked and unwrapped.
- A full barrier between kernels so pairs cannot overlap.
- One frame in flight: submit, wait the fence, read the query. Simple, and the
  present is what paces the loop.
- Percentiles are linear-interpolated on the sorted samples.
- K1's GB/s counts 8 bytes per pixel (one RGBA8 read, one write).

## The display copy is a crop

The Android frontend copies the output image into the swapchain with
`vkCmdCopyImage` and a 1:1 crop. `vkCmdBlitImage` is not legal here: the output
is `R8G8B8A8_UINT` and swapchains are UNORM, and blit forbids crossing between
integer and normalised formats, while copy only requires the texel size to
match. The display is proof of life, not a deliverable.

## K6 hybrid: what is real and what is not

**Real and measured**: Pass C itself. It reads the base through a sampler (which
is allowed here, because the base is not in the bit-exact path — only the
residual is), gathers the warped previous residual bit-exactly, adds the decoded
delta, and writes both the output and the new residual image. It is timed
against the 2.0 ms threshold like any other kernel.

**Present but unverified on hardware**: the `AHardwareBuffer` to Vulkan import in
`src/android/k6_hybrid.cpp` — `vkGetAndroidHardwareBufferPropertiesANDROID`, an
image on `VkExternalFormatANDROID`, dedicated import allocation, a
`VkSamplerYcbcrConversion` on the external format, and an immutable sampler in
the Pass C descriptor set layout. When it succeeds Pass C samples the real
decoder output; when anything fails it falls back to a synthetic base and says
so, and Pass C still times correctly.

**Not done** (marked TODO in the source):

- The decode-latency number has never run on hardware. `gen-asset.sh` builds
  the HEVC elementary stream with x265 at the real 2048x4096 geometry and
  `run.sh` calls it automatically, so the APK ships `assets/base.hevc`
  (about 4 MB for one second at 90 fps) and `HybridBase` loads it through the
  asset manager. What has not happened is a device actually decoding it.
- The Qualcomm vendor key `vendor.qti-ext-dec-low-latency.enable` is set but not
  verified as taken. PAPER 3.4 says K6 failing on latency means exactly this
  needs checking, so the check has to exist before the number means anything.
- Release uses `AImage_delete`, not `AImage_deleteAsync` with a sync fd exported
  from the Pass C submit.
- The acquire sync fd is not imported into a binary `VkSemaphore` and waited on.
- Imports are cached by buffer pointer identity, not across the whole pool.

So: **K1 to K5 are complete; K6 is a working skeleton** whose enhancement pass
is genuinely measured and whose decode path compiles and is structured
correctly, but whose end-to-end latency number does not yet exist.

## Bit-exactness

`--selftest` checks the two kernels where a silent mistake is plausible, against
a CPU reference in `src/core/`:

- **Pass A**: the whole coefficient buffer, 2048 tiles x 8 lanes x 256 symbols,
  against the symbols the CPU encoder put in. This exercises the ballot-derived
  shared read pointer, which is the single most delicate thing in the kernel.
- **Pass B**: a full tile row against a CPU implementation of the same flow
  graph, which exercises the packed-LDS transpose and the dequant-shift-clamp
  chain.

Both pass on RADV (wave64). Running the same check on lavapipe (subgroup size 8)
and on Adreno is what makes the cluster-of-8 rule real, and is why the rule
exists.

## Adreno and spirv-opt

`--selftest` reported `Pass B MISMATCH: tile 0 pixel (0,0): got 103 want 248` on
the Pico 4 (Adreno 650) while being bit-exact on RADV, on lavapipe and on both
wave widths. The bisect that `--selftest` now runs on a failure localised it
exactly: the raw coefficient word, the dequantised coefficient, the row-pass
result, the LDS store address and the LDS load address all matched the CPU model
to the bit, and only the word read back out of the transpose buffer was wrong --
carrying, at index 0, the value that belongs at index 2047.

It is not a shader bug. It is `spirv-opt`'s **`redundancy-elimination`**, which
common-subexpression-eliminates the duplicate `OpAccessChain` that a load and a
store to the same shared word each produce:

```
without:  %551 = OpAccessChain %sLds %535   with:  %551 = OpAccessChain %sLds %535
          %552 = OpLoad %551                       %552 = OpLoad %551
          %554 = OpAccessChain %sLds %535
          OpStore %554 ...                         OpStore %551 ...
```

Both modules pass `spirv-val`, and RADV and lavapipe are bit-exact on both. The
Adreno 650 driver is not: given the right-hand form it mixes up which shared
word an access refers to. Bisected pass by pass on device -- `-O` minus
`redundancy-elimination` is bit-exact, `-O` is not, and `loop-unroll` alone (the
pass that creates the duplicate access chains in the first place) is fine.

So `cmake/gen_spv.cmake` no longer hands the driver `glslc -O`. It runs
`glslc -O0` and then `spirv-opt` with the `-O` pass list minus both
`redundancy-elimination` steps, written out in full so the omission is visible.
Nothing is given up: the driver's own compiler does its own redundancy
elimination, and K3 measured *faster* afterwards (7.93 ms p50 against 8.41 ms).
`NXB_SPV_PASSES` overrides the list -- empty for none, `-O` to reproduce the
failure.

The same load-then-store-through-one-index shape is in the real decoder
(`vk/decoder/passB/reconstruct.comp`, and `passA/rans_decode.comp` line 209), so
both of those take the same pass list.

The Pass B kernel also no longer builds its packed word with a read-modify-write
(`sLds[w] = lo; ... sLds[w] |= hi << 16;`). Each `uint` is written once, whole.
That is not what fixed the miscompilation -- the single-store form failed under
stock `-O` too -- but it is one store instead of a load plus a store, and it
removes the load/store pointer pair that the pass was CSE-ing.

## K1 is slow because the image is an integer format

The gate reports K1 at 5 to 7.6 GB/s, against a 20 GB/s threshold and an
Adreno 650 that ought to manage 15 to 25. `--k1-sweep` takes that number apart:
each variant is one dispatch per submit with `vkQueueWaitIdle` on both sides
and no co-tenant, timed by the device's timestamp pair *and* by the host clock,
eight dispatches inside the timed region so submit overhead cannot dominate,
and a per-dispatch tag XORed into the stored value so no driver can elide the
write.

Pico 4, Adreno 650, GPU at 441.6 MHz, 2048x4096, 67.1 MB moved per dispatch:

| variant | dev ms | host ms | GB/s |
|---|---|---|---|
| `rgba8ui` 8x8 -- K1 as shipped | 6.37 | 7.47 | **10.5** |
| `rgba8ui` 64x1 | 14.54 | 15.86 | 4.6 |
| `rgba8ui` 16x16 | 16.42 | 17.76 | 4.1 |
| `rgba8ui` 64x1 tile-major | 19.11 | 20.68 | 3.5 |
| `r32ui` 8x8 | 6.74 | 7.27 | 10.0 |
| `r32ui` 64x1 | 14.94 | 15.68 | 4.5 |
| `rgba8` UNORM 64x1 | 4.50 | 4.94 | **14.9** |
| `rgba8` UNORM 8x8 | 4.61 | 5.06 | 14.6 |
| SSBO 64x1 | 5.05 | 5.56 | 13.3 |
| SSBO 8x8 | 5.38 | 5.94 | 12.5 |
| SSBO 16x16 | 5.48 | 6.05 | 12.2 |

Four things fall out of it:

- **The timestamps are not the problem.** Device and host times agree within
  about 10% on every row. The kernel really does take that long.
- **Integer storage images cost about 3x.** The same copy through an
  `rgba8` UNORM image runs at 14.9 GB/s and through an SSBO at 13.3; through
  `rgba8ui` it is 4.6 at the same workgroup shape. `r32ui` is no better, so it
  is the *integer* image path and not the channel count.
- **Only the integer path cares about workgroup shape**, and it cares a lot:
  8x8 is 2.3x faster than 64x1. UNORM and SSBO are flat across every shape
  tried, which is what a bandwidth-bound copy should look like. Tile-major
  indexing is the worst of the lot and not worth pursuing.
- **The hardware is fine.** 14.9 GB/s at 441.6 MHz scales to roughly 19.8 at
  the part's 587 MHz, which is the threshold. The gate's figure is the
  `rgba8ui` penalty plus the co-tenant, not a slow GPU.

The co-tenant accounts for the rest of K1's gap and almost nothing elsewhere:
with `--no-cotenant`, K1 goes from 12.2 ms to 7.2, while K3 (8.28 vs 7.93),
K4 (9.94 vs 10.13) and K5 (33.3 vs 28.0) do not move in any consistent
direction. **So K5's ~28 ms is kernel time, not measurement.**

What this does *not* do is license switching the codec's images to UNORM. The
output is `R8G8B8A8_UINT` because the residual path is bit-exact and a
normalised format puts a float conversion in it. 8-bit UNORM does round-trip
exactly, so it is probably safe, but "probably" is not the standard PAPER 3.7
sets and it needs its own proof and its own `--selftest`. It is the single
biggest lever measured so far: K3 writes 33.5 MB through exactly this path and
takes 7.9 ms, which is most of what the integer store alone would cost.

One caveat on every number here: the GPU sat at **441.6 MHz** for all of it,
not the part's 587 MHz, and the run-to-run spread is wide (K5 measured 28.0 and
33.3 ms in the same session). `/sys/class/kgsl/kgsl-3d0/gpuclk` is readable and
`run.sh` now records it either side of a run, but the devfreq nodes that would
pin it are not reachable without root, so the gate has no clock control.

## Known gaps

- No 4:2:0 chroma, as described above.
- No `VK_KHR_pipeline_executable_properties` register/spill reporting. PAPER
  3.2.3 asks for it where available; the extension is probed but not used.
- Intra (DC-plane) prediction is not a kernel. PAPER 3.4 does not ask for one,
  and 3.2.4 makes it cheap and branch-free, but it is untimed.
- The host CLI free-runs; it has no vsync to pace against, so its co-tenant load
  is continuous rather than periodic. This inflates thermal pressure relative to
  the device and is another reason its numbers are not the gate.
