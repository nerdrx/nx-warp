# 3. Vulkan implementation: encoder, decoder, and the compute budget

This section turns the tile architecture into dispatches, buffers and milliseconds. The governing constraint is the headset: an Adreno 650 that already struggled with one per-pixel sharpening pass. Every decision below is made with that number in front of it, and the Phase 0 gate exists because the estimate could be wrong by 2x in either direction.

## 3.1 Working numbers

| Quantity | Value |
|---|---|
| Frame | 2 views x 2048 x 2048 = 8.39 Mpixel |
| Frame period at 90 Hz | 11.1 ms (13.9 ms at 72 Hz) |
| GPU time already spent per vsync by WiVRn | reprojection + compositor, about 2 to 3 ms |
| Decoder budget we can defend | 5 ms p50, 7 ms p99 (pure compute) |
| Adreno 650 peak | 1.2 TFLOPS FP32, roughly 600 G int32 simple ops/s; assume 300 G/s sustained |
| Adreno 650 memory | LPDDR5 on a 64-bit bus, about 44 GB/s peak, 25 GB/s usable by the GPU |
| Bitstream per frame | 150 Mbit: 208 KB; 400 Mbit: 555 KB; 1 Gbit: 1.39 MB |
| Bitstream per 64x64 tile (2048 tiles) | 102 B at 150 Mbit, 270 B at 400 Mbit, 680 B at 1 Gbit |

The last row matters for the whole design. At 150 Mbit a 32x32 tile would carry 25 bytes. Per-tile headers, rANS flush bytes and per-datagram overhead are not free, so the normative tile is 64x64 (see 3.3 for the consequences for "tile = packet").

A frame-level ALU estimate: 8.39 Mpixel x 150 ops = 1.26 G ops = 4.2 ms at 300 G/s. Memory: about 105 MB per frame (table in 3.2.5) = 4.2 ms at 25 GB/s. ALU and memory overlap only partially. That is the whole story of the risk: the decoder lands at 4 to 6 ms on this GPU if everything goes right.

## 3.2 Decoder pipeline on the headset

### 3.2.1 Two dispatches, not one

The decoder is split into two dispatches per frame, plus an optional third for hybrid mode:

| Pass | Unit of work | Workgroup | Reads | Writes |
|---|---|---|---|---|
| A: entropy decode | 8 tiles per wave (8 rANS lanes per tile) | 64 threads | bitstream, tile table | coefficient buffer (int16), per-tile mode/MV/QP |
| B: reconstruct | one 64x64 tile | 256 threads | coefficient buffer, reference image | output storage image |
| C: hybrid enhancement | one 64x64 tile | 256 threads | HEVC base (AHardwareBuffer), previous residual image | output image, residual image |

A single fused kernel per tile was considered and rejected. Entropy decoding is inherently serial per rANS state, so it wants few lanes per tile and many tiles in flight; transform and prediction want many lanes per tile. Fusing them means either 8 of 256 lanes busy during the entropy stage or 64 rANS states per tile, and 64 states cost about 128 bytes of flush per tile against a 102 byte payload. The split costs a 16 MB round trip through a coefficient buffer (about 0.7 ms of overlapped bandwidth) and buys three things: the right dispatch shape for each stage, independent LDS budgets (Pass A needs its symbol tables in LDS, Pass B needs the transpose buffer), and incremental execution. Pass A runs on each tile-row group as its packets arrive (indirect dispatch, count patched by the network thread), so at the presentation deadline only Pass B remains. That hides most of the entropy cost under network arrival time.

Aggregate entropy work is small if occupancy is good: at 0.3 coded symbols per pixel, a tile has about 1200 symbols, 150 per lane, about 25 cycles each, which is 2048 x 8 x 3750 lane-cycles = 61 M lane-cycles, under 0.2 ms on 512 lanes. The measured number in Phase 0 will be larger because of memory latency on the table lookups, but the point stands: Pass A is cheap if there are enough tiles in flight, and 2048 tiles x 8 lanes is enough.

### 3.2.2 Pass A: interleaved rANS with a shared read pointer

Per tile: K = 8 interleaved rANS states (Duda's rANS, Giesen's interleaving; both public domain, no known patent claims on the basic scheme), 32-bit state, L = 2^16, 16-bit renormalization, 10-bit probability scale. All arithmetic fits in uint32: decode is `x = freq * (x >> 10) + (x & 1023) - cum`, with freq < 2^10 and x >> 10 < 2^22. No int64, no division in the decoder.

The 8 states read from one byte stream in a deterministic order. Each lane decides whether it renormalizes this step; a subgroup ballot masked to the 8-lane cluster and a bit count give each lane its offset from the shared pointer, and the cluster advances the pointer by the popcount. This requires ballot and a subgroup size of at least 8 with clusters not straddling subgroups; 32, 64 and 128 are all multiples of 8, so a 64-thread workgroup holds 8 tiles on Adreno (one wave) or 2 x 4 tiles on 32-wide hardware, and the code is identical. Flush cost is 8 states x 4 bytes, of which roughly 2 bytes per state are real overhead (the final state carries 16 useful bits), so about 16 bytes per tile.

Symbol decoding uses a 1024-entry cumulative-to-symbol table per context, 1 byte per entry, 8 contexts (significance, magnitude class, sign is bypass, DC plane, MV, mode) = 8 KB in LDS, loaded once per workgroup. Frequency tables are static per QP class in v1 and transmitted per frame in v2 (256 x 2 B per context).

Output: coefficients as int16 in block-raster order (8 KB per tile, 16 MB per frame) and a 16-byte tile record: mode (skip/inter/intra), 4 corner displacements in Q4 fixed point (int16 each) for the warp, QP, flags. Skip tiles write no coefficients; Pass A sets a bit in a skip mask and Pass B takes the cheap path.

Register budget: 8 rANS lanes x (state, pointer, cum, freq, symbol) plus the coefficient write pointer, under 32 VGPRs. Occupancy is limited by LDS (8 KB per group) rather than registers.

### 3.2.3 Pass B: one workgroup per 64x64 tile

256 threads. A 64x64 tile holds 64 blocks of 8x8; 4 threads per block, each thread owns 2 rows (16 coefficients).

1. Load 16 int16 coefficients (coalesced, 32 bytes per thread), dequantize: `c = (q * scale[QP][pos] + 8) >> 4`, all int32.
2. Row transform: 8-point integer DCT (the HEVC core transform is patent-encumbered by implementation detail in places; use the AV1/VP9-style or a Loeffler-derived integer lifting transform with published coefficients, or the JPEG XS style 5-3 wavelet for the lossless profile). Two 1D transforms of 8 points, about 44 adds and shifts each.
3. Write to LDS transposed: 64 x 64 x 2 B = 8 KB. Barrier.
4. Column transform: each thread reads 2 columns of its block, transforms, clamps to the residual range.
5. Prediction. Inter: the warp coordinate for each pixel is bilinear interpolation of the 4 transmitted corner displacements (Q4, 6 integer ops), then a bit-exact 4-texel bilinear from the reference image (4 imageLoad, integer weights, `(w00*p00 + w01*p01 + w10*p10 + w11*p11 + 128) >> 8`). Intra: DC-plane prediction (3.2.4). Skip: prediction only, no coefficients.
6. `out = clamp(pred + res)`, YCoCg-R to RGB (5 adds/shifts), one imageStore of RGBA8 or RGB10A2.

The hardware sampler is not used for the normative predictor. Sampler weight precision is vendor-specific (8-bit fractions on AMD and NVIDIA, undocumented on Adreno), and the encoder always runs on a different vendor than the decoder, so a sampler-based predictor would drift by +-1 LSB per frame until the next refresh. Four explicit loads hit the same cache line for almost every pixel; Phase 0 measures the real cost of gather-4 against one sampler tap. If gather-4 is the bottleneck, the fallback is loading the tile footprint plus a 16-pixel margin into LDS with coalesced loads (96x96x4 = 36 KB, over Adreno's 32 KB limit, so it would have to be 2 bytes per texel in a luma/chroma split), which is deferred as an optimization.

Register budget for Pass B is the tight one: 16 coefficients, 4 texel loads in flight, weights, coordinates. Target under 64 VGPRs so four groups fit per shader core. Adreno's compiler is opaque about spilling; the Phase 0 bench reports spill via the `VK_KHR_pipeline_executable_properties` statistics where available and by timing otherwise.

### 3.2.4 Intra without a wavefront

Spatial intra prediction in the H.264/HEVC sense creates a dependency chain across the 64 blocks of a tile: 15 wavefront steps with barriers and 4 active lanes per block. That is a poor fit and intra tiles are only 5 to 10 percent of tiles under rolling refresh, but a slow path still costs p99. Decision: the 64 DC coefficients of a tile form an 8x8 low-resolution image that is transformed and coded first (a second-level 8x8 DCT, 64 symbols). Pass B decodes the DC plane, and the predictor for each pixel is bilinear interpolation between the four nearest block DCs (planar-like). Fully parallel, no barrier beyond the transpose, about 12 ops per pixel. It gives up directional intra modes; at VR bitrates intra tiles are a refresh mechanism, not the workhorse, so the loss is acceptable and is measured in Phase 1. This is a bitstream decision and needs to be reflected in the syntax section.

### 3.2.5 Memory traffic and time estimate

| Traffic per frame (pure compute, 2 x 2048^2) | MB |
|---|---|
| Bitstream read | 0.2 to 1.4 |
| Pass A coefficient write (int16 dense) | 16.8 |
| Pass B coefficient read | 16.8 |
| Reference read (gather-4 through texture cache, nominal) | 33.5 (effective 40 to 50) |
| Output write (RGBA8 / RGB10A2, doubles as next reference) | 33.5 |
| Total | about 105 |

At 25 GB/s that is 4.2 ms if nothing overlaps. A sparse coefficient layout (run-length, roughly 4x smaller at typical QP) is the first optimization if Pass B is bandwidth-bound; it is not in v1 because the dense layout keeps Pass A trivial.

| Per-pixel op budget, inter tile | ops | fetches | stores |
|---|---|---|---|
| Entropy decode, amortized (0.3 symbols/pixel x 25) | 8 | | |
| Coefficient load, dequant | 6 | 0.06 (coalesced) | |
| Row + column 8x8 integer DCT (2 x 44 / 8) | 22 | | |
| LDS transpose traffic | 6 | | |
| Warp coordinate | 6 | | |
| Bit-exact bilinear (4 loads, weights, blend) | 14 | 4 (same line) | |
| Add, clamp, YCoCg-R to RGB | 10 | | |
| Store | 1 | | 1 |
| Total | about 75 | 4 | 1 |

That is inside the 150 to 300 budget on paper. Expected time on Adreno 650 at 90 Hz:

| Pass | Estimate |
|---|---|
| A (all tiles, if not hidden under arrival) | 0.5 to 1.0 ms |
| B | 3.5 to 5.0 ms |
| Total | 4 to 6 ms p50, 7 ms p99 |

Honest comparison with the CAS field data. The CAS pass was 9 texel taps plus one store per pixel per vsync on 2 x 2160^2; at the texture rates of this GPU that is 4 to 5 ms, and it ran on top of everything else every vsync, which is why it hurt. Pass B is 4 (cache-friendly) fetches, one store and about 5x the ALU, once per frame. It should land in the same 4 to 5 ms band. Two things must be said plainly. First, at a 90 fps stream "once per frame" and "once per vsync" are the same rate; the argument only helps at 72 Hz or when the stream runs below the panel rate. Second, this is GPU time the hardware HEVC path does not spend at all; the pure-compute decoder buys latency and prediction quality with 4 to 6 ms of GPU that the Pico 4 barely has. If Phase 0 measures 8 ms, the Pico 4 is a hybrid-mode device and pure compute waits for Adreno 740 class hardware (about 2.5x), and that is an acceptable outcome, not a failure of the design.

### 3.2.6 Subgroup portability rules for the shaders

- Workgroup sizes are 64 (Pass A) and 256 (Pass B); never assume a workgroup is one subgroup. Every cross-lane exchange beyond an 8-lane cluster goes through LDS with a barrier.
- Use `VK_EXT_subgroup_size_control` with `REQUIRE_FULL_SUBGROUPS` where offered; query `subgroupSize` at pipeline creation and refuse subgroups smaller than 8 (Mali Bifrost at 4 is unsupported for the pure-compute path; it gets hybrid).
- Cluster operations use `subgroupBallot` plus masks derived from `gl_SubgroupInvocationID & ~7`, never `subgroupClustered*` (weaker support on Adreno's proprietary compiler).
- Same SPIR-V binary everywhere; specialization constants for subgroup size and 10-bit mode, no vendor #ifdefs in normative code.

## 3.3 Consequence for transport: tile is not a packet

At 150 Mbit a tile is about 100 bytes and 2048 tiles at 90 fps would be 184 k datagrams per second, where UDP/IP overhead is 46 bytes each and the receiving CPU melts. The bitstream unit stays the tile; the transport unit must be a tile-row segment: consecutive tiles of one row packed to about 1200 bytes (12 tiles at 150 Mbit, 2 at 1 Gbit). Loss granularity becomes a segment, still concealed per tile. This contradicts the "tile = one datagram" rule in the design consensus and the transport section should adopt segments.

## 3.4 Phase 0 gate: the exact benchmark

A standalone Android app (NDK, C++, Vulkan 1.1, no OpenXR) run on the Pico 4 at 90 Hz with the display active and a fullscreen dummy reprojection pass (one sampler tap, one store, 2 x 2160^2) submitted every vsync so the decoder competes with realistic co-tenant work. Nothing is synthetic in shape; only the data is random.

Kernels, each real code that will be reused, timed with `VK_QUERY_TYPE_TIMESTAMP` pairs around each dispatch, `timestampPeriod` applied, 600 frames after 120 warm-up, reporting p50, p95, p99 and the on-device clock check for throttling (run 10 minutes, report the last-minute p50 against the first):

| Kernel | What it does | Pass thresholds |
|---|---|---|
| K1 copy | 8.39 Mpixel RGBA8 image to image via compute | reports achievable GB/s; expect over 20 |
| K2 gather-4 | per pixel: warp coordinate + bit-exact 4-load bilinear + store, from a full-frame reference | under 3.0 ms p50 |
| K2b sampler | same with one sampler tap | informational, quantifies the cost of bit-exactness |
| K3 idct | Pass B without prediction: coefficient load, dequant, 8x8 int DCT through LDS, store | under 2.5 ms p50 |
| K4 rans | Pass A on random symbol streams of 0.5 symbols/pixel, all 2048 tiles | under 1.5 ms p50 |
| K5 full | Pass A + Pass B as designed | under 5.0 ms p50, 7.0 ms p99 with the dummy reprojection pass running |
| K6 hybrid | MediaCodec HEVC 2x2048^2 at 90 fps into AHardwareBuffer, imported, plus Pass C | decoder latency p50 under 15 ms, Pass C under 2.0 ms |

Decision rule: K5 passes at 90 Hz: pure compute is the default on Pico 4. K5 between 5 and 8 ms: pure compute at 72 Hz or with 1.5x foveated tile reduction, hybrid the default. K5 over 8 ms: Pico 4 is hybrid-only; the pure-compute path continues on PC and next-generation Adreno. K6 failing on latency means MediaCodec low-latency mode is not working on this firmware and the hybrid path needs the vendor key `vendor.qti-ext-dec-low-latency.enable` verified.

## 3.5 Hybrid mode implementation

Base layer: HEVC through MediaCodec, exactly as WiVRn does today. Enhancement: the codec's tiles carry a residual relative to the base, temporally predicted from the warped previous residual (LCEVC-like layering, but LCEVC's specific tools are MPEG-5 Part 2 and licensed, so the enhancement syntax is the codec's own tile syntax with a different predictor, not LCEVC).

Data path without a copy:
1. `AImageReader_newWithUsage(..., AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, maxImages = 4)`; its `ANativeWindow` is the MediaCodec output surface. `KEY_LOW_LATENCY = 1` plus the Qualcomm vendor key; one output buffer released per input, never queued.
2. On the image-available callback, `AImageReader_acquireLatestImageAsync` gives the `AHardwareBuffer` and a sync fd. The buffer pool is small and recycled, so each distinct buffer is imported once: `vkGetAndroidHardwareBufferPropertiesANDROID`, a `VkImage` with `VkExternalFormatANDROID` (the decoder emits a vendor-tiled YCbCr format, typically UBWC NV12), memory bound via `VkImportAndroidHardwareBufferInfoANDROID`, and a `VkSamplerYcbcrConversion` on the external format. Imports are cached by buffer identity.
3. The sync fd is imported into a binary `VkSemaphore` (`VK_KHR_external_semaphore_fd`, `SYNC_FD`) and waited on by the Pass C submit. Pass C reads the base through the YCbCr sampler (one tap; here the sampler is allowed because the base is not in the normative bit-exact path, only the residual is), reads the warped previous residual with the bit-exact gather, adds the decoded delta, writes both the output image and the new residual image (signed 8-bit RGBA, 33.5 MB, or half resolution at 8.4 MB for the Lite profile).
4. Release: the submit signals a semaphore exported as a sync fd, passed to `AImage_deleteAsync`. No vkCmdCopyImage anywhere; the only extra traffic over pure compute is the residual image write and read.

Latency: the hardware decoder on XR2 Gen 1 delivers a 2x2048^2 HEVC frame 8 to 12 ms after the last slice arrives in low-latency mode and can add one full frame of pipelining if that mode silently fails; WiVRn's current 100 ms end to end includes this. Hybrid also loses tile-row pipelining on the base (MediaCodec delivers whole frames) although the enhancement tiles still decode incrementally. The hybrid path is a floor for weak devices, not the low-latency path.

## 3.6 Encoder pipeline on the PC

On Linux the WiVRn server's encoders already run on Monado's `VkDevice`, so the compositor's render target is a `VkImage` in the same device: no external memory, only an image memory barrier to `GENERAL` (storage read) and, if the encoder uses a dedicated compute queue, a queue family ownership transfer. Timeline semaphores carry the handoff: the compositor signals value F when frame F is rendered; the encode submit waits on it and signals its own timeline.

Passes per frame, all indirect where the tile count varies, no CPU between them:

| Pass | Shape | Work |
|---|---|---|
| E0 warp | fullscreen, 8x8 threads | warped reference from previous reconstruction and pose delta; writes the warped image and the per-tile corner displacements |
| E1 analyze | one group per tile | SAD of source against warped reference at 0 and 8 candidate integer offsets (+-4 px), variance, skip test; picks mode and MV, assigns QP = base + foveation offset + activity term + rate feedback; appends tile index to the inter/intra/skip lists via atomics |
| E2 transform | one group per listed tile | residual, forward 8x8 integer DCT, quantization with deadzone, RDO-lite (coefficient zeroing when rate estimate exceeds distortion gain, from a table), writes int16 coefficients and an exact symbol count |
| E3 reconstruct | one group per tile | the decoder's Pass B, same SPIR-V, writes the new reference |
| E4 entropy | 8 lanes per tile, 8 tiles per group | rANS encoding runs backwards over the symbol list, into a per-tile slot of bounded size (2 bytes per coefficient plus header); writes actual byte count |
| E5 packetize | one group of 1024 threads per view | prefix sum of tile sizes, compaction into tile-row segments, headers written by the shader (tile ids, sizes, pose id, timestamp, sequence), segment descriptor table for the network thread; rate feedback: actual bytes versus budget into the controller state buffer |

E3 being byte-identical shader code to the decoder is the single most important rule in the project: the encoder never has a "reference" that the decoder cannot reproduce. rANS encoding needs `x / freq`; integer division on GPU is 20 to 40 instructions but this is the PC side and 8 lanes per tile; a reciprocal table (`OpUMulExtended` is core SPIR-V) is an optimization.

Rate control on the GPU: the CPU's AIMD/BBR controller from WiVRn NX writes one number per frame, the byte budget, into a uniform ring. E1 converts it to a base QP through a per-stream model (bytes as a function of QP and activity, updated by E5 from the previous frame's actual bytes: a proportional correction with a clamp of +-2 QP per frame). No intra-frame two-pass; overshoot on a frame is absorbed by the pacer and corrected next frame. Per-tile-row re-encoding of outliers is a v2 option.

Output buffer to the network thread: E5 writes segments directly into a `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` buffer (system memory). GPU writes to it are DMA and coalesced by the compaction; host reads are cached, so `sendmmsg` sends straight from the mapped pointer. Where no cached host-visible heap exists (it exists on RADV, NVIDIA, ANV and Windows AMD/NVIDIA drivers), fall back to device-local plus `vkCmdCopyBuffer` at the end of each row group. Writing into the device-local host-visible BAR heap was rejected: host reads of write-combined memory are uncached and slow. `VK_EXT_host_image_copy` does not apply; it moves images, host to device, not packet buffers.

Row pipelining on the encoder: the frame is split into 4 row groups per view; each is its own command buffer signaling timeline value 8F + g. The network thread does `vkWaitSemaphores` on the next expected value and one `sendmmsg` per group. Per-frame CPU cost target: under 300 us at 90 fps, consisting of one `vkQueueSubmit` of pre-recorded command buffers (per-frame data through push constants and a uniform ring, no descriptor updates: all frame images live in one descriptor array indexed by frame slot), 8 semaphore waits and 8 `sendmmsg` calls.

Expected encoder time: RX 580 (6 TFLOPS, 256 GB/s) about 2.5 to 4 ms per frame for both views; 7900 XTX under 1 ms. The E0 and E3 fullscreen passes dominate on the RX 580 by bandwidth (about 250 MB per frame), which is fine against a 35 to 50 fps AMF ceiling.

## 3.7 Vendor differences

| Target | Subgroup size | Ballot | int64 | int16 storage | Notes |
|---|---|---|---|---|---|
| AMD GCN4 (RX 580, RADV and Windows) | 64 | yes | yes | yes | reference PC encoder platform |
| AMD RDNA (7900 XTX) | 32 or 64, driver chooses | yes | yes | yes | never assume which |
| NVIDIA | 32 | yes | yes | yes | |
| Intel ANV | 8, 16 or 32 per shader | yes | yes | yes | force 32 with subgroup size control; clusters of 8 work at any size |
| Adreno 6xx (Pico 4) | 64 (128 on 7xx) | yes | unreliable | yes | proprietary compiler; avoid clustered ops |
| Mali Valhall | 16 | yes | no | yes | hybrid only unless Phase 0 style bench passes |
| Mali Bifrost | 4 to 8 | partial | no | yes | unsupported for pure compute |
| Apple via MoltenVK | 32 | yes | no | yes | Metal has no 64-bit integer; sampler behavior differs |

Bit-exactness rules for the normative path (decoder and E3):
- int32 arithmetic only, with int16 storage. No float, no fp16, no int64, no integer division or modulo. Rounding shifts are written as `(x + (1 << (s - 1))) >> s` with arithmetic shift; SPIR-V defines this exactly.
- Shift amounts are compile-time constants or masked to the operand width: SPIR-V leaves out-of-range shifts undefined.
- No `OpSDiv`, `OpSRem`, `OpSMod` anywhere in normative shaders; no `precise`/`fast-math` questions arise because nothing is float.
- Every buffer and image load is bounds-clamped in the shader; `robustBufferAccess` behavior differs across vendors (zero versus garbage) and the codec cannot depend on it.
- Coefficient clamping ranges are normative so that overflow cannot differ by vendor.
- The CPU reference decoder is the specification; SPIR-V is validated against it, not the other way round.

## 3.8 Windows port

The Windows helper receives per-eye `ID3D11Texture2D` from SteamVR. Path: create a shared texture on the helper's D3D11 device (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`), copy the SteamVR texture into it on the D3D11 side (one GPU copy, unavoidable because SteamVR's texture is not created with sharing flags we control), then import the shared texture into Vulkan through `VK_KHR_external_memory_win32` with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` and a `VkImage` created with a matching format and `VkExternalMemoryImageCreateInfo`, checked with `vkGetPhysicalDeviceImageFormatProperties2`. Synchronization: prefer a D3D11.4 shared fence (`ID3D11Device5::CreateFence` with `D3D11_FENCE_FLAG_SHARED`) imported as a timeline semaphore via `VK_KHR_external_semaphore_win32` (`D3D12_FENCE` handle type), which makes the D3D11 copy and the Vulkan encode one timeline. Fallback where the driver lacks it: `VK_KHR_win32_keyed_mutex` with `VkWin32KeyedMutexAcquireReleaseInfoKHR` in the submit. Both work on AMD and NVIDIA Windows drivers; Intel needs the fence path. The RX 580 on Windows is GCN4 wave64 with full subgroup support, so the encoder shaders are the same binaries as on Linux.

## 3.9 Testing

- Reference decoder: single-threaded C++20, integer only, no SIMD, about 3000 lines, no dependencies. It is the normative specification; the syntax document is derived from it.
- Reference encoder: slow, exhaustive-ish, CPU, used only to produce conformance streams that exercise every syntax element (max magnitude coefficients, max displacement, all-skip frames, DC-plane extremes, 10-bit, lossless tiles, truncated tiles).
- GPU versus CPU diff harness: decodes each conformance stream on every available device, hashes the RGBA output per tile, reports the first mismatching tile and pixel. Runs on lavapipe and SwiftShader in CI without a GPU (both implement the required subgroup features; lavapipe subgroup size is 8, which is exactly why the cluster size is 8), on RADV and NVIDIA on the developer machines, and on the Pico 4 via a self-hosted adb runner nightly.
- Cross-vendor determinism: encode on AMD, decode on NVIDIA, lavapipe and Adreno; all hashes equal to the reference decoder. This test is the definition of done for Phase 1 and Phase 2.
- Fuzzing: libFuzzer on the reference decoder with a structure-aware mutator (tile boundaries, rANS state fields); property: never reads out of bounds, always emits a frame. The GPU decoder is fuzzed with the same corpus under `VK_LAYER_KHRONOS_validation` with GPU-assisted validation on lavapipe; timeouts are bugs.
- Quality harness: a server-side dump tool records raw render targets plus poses from WiVRn NX sessions; the harness encodes with the codec, x264 and x265 (`--tune zerolatency`, intra refresh, no B-frames, single reference, matched bitrates) and with NVENC/AMF captures where available, and reports PSNR, SSIM and VMAF (libvmaf via ffmpeg) plus BD-rate. Loss simulation drops tile segments at 1, 5 and 10 percent and reports drift versus the reference.
- Performance CI: the Phase 0 bench app remains in the tree as the regression benchmark; the nightly runner fails if Pass B p99 on the Pico 4 regresses by more than 5 percent.

## 3.10 Project structure

```
codec/
  CMakeLists.txt          C++20, CMake 3.25+, presets for linux, windows, android
  core/                   header-only: syntax constants, tables, tile record structs
                          (single .h shared by C++ and GLSL through a common-subset macro layer)
  ref/                    reference decoder and conformance encoder (no deps)
  shaders/                GLSL 4.60, Vulkan semantics, glslang to SPIR-V 1.4 at build time,
                          embedded as arrays; recon.comp is included by both decoder and encoder
  vk/                     device capability probe, pipeline cache, timeline and external-memory helpers
  vk-decoder/             Pass A/B/C, AHardwareBuffer import, decode-time telemetry
  vk-encoder/             E0 to E5, rate-control state, segment table, win32 interop
  tools/                  bench (Phase 0 app), conform, diff, fuzz, quality, dump
  tests/
```

Language: C++20, not Rust. WiVRn is C++/CMake on both ends, the Android client is NDK C++, and the codec's substance is in shaders and integer tables; Rust would add cargo-ndk and an FFI seam for no gain in the hot path. Shader language: GLSL through glslang, matching WiVRn (`reprojection.glsl`), Monado and every driver in the table; `GL_EXT_shader_explicit_arithmetic_types_int16` and `GL_KHR_shader_subgroup_ballot` are the only extensions needed. Slang was considered for its generics and module system and rejected for now because it adds a toolchain that neither WiVRn nor the Android build has, and the codec has perhaps fifteen shaders. HLSL via DXC was rejected on integer-semantics history and because there is no D3D target. `spirv-val` and `spirv-opt` run in CI.

Linking: `nx_codec` is a static library with a small C ABI (`nxcodec.h`) plus C++ convenience headers. WiVRn NX server gets a `video_encoder` implementation that owns the encoder passes and exposes segments to the existing pacer and FEC; the client gets a `decoder` implementation next to the MediaCodec one, selected by a new value in the protocol's codec enum (the protocol section should reserve it). The decoder's output image is handed to `reprojection.glsl` as a plain RGBA storage image, replacing the YCbCr sampler path.

## 3.11 Milestones and exit criteria

| Phase | Deliverable | Exit criteria (all measurable) |
|---|---|---|
| 0 (3 weeks) | bench app, capability probe | Phase 0 table filled on Pico 4; pure/hybrid decision recorded; K1 to K6 numbers in the tree |
| 1 (8 weeks) | intra-only codec: ref decoder, GPU decoder, GPU encoder, conformance, diff, quality harness | bit-exact on lavapipe, RADV, Adreno; within 1.0 dB PSNR of x264 intra (`--keyint 1`, zerolatency) at 100 to 400 Mbit on VR captures; Pass B under 5 ms p50 on Pico 4; encoder under 4 ms on RX 580; fuzz corpus 24 h clean |
| 2 (10 weeks) | pose-warped inter, DC-plane intra refresh, skip, per-tile reference tracking | cross-vendor determinism test green; BD-rate within 15 percent of x265 zerolatency single-reference on head-rotation sequences at 100 to 200 Mbit; 5 percent segment loss shows no drift beyond one refresh period; Pico 4 p99 under 7 ms at 90 Hz |
| 3 (6 weeks) | WiVRn NX integration, hybrid mode, telemetry | glass-to-glass under 40 ms at 150 Mbit on WiFi 6 measured by the existing HUD path, against about 100 ms today; encode plus decode under 12 ms combined; 1 hour session without a crash on Pico 4; hybrid mode selectable and functional |
| 4 (open) | stereo inter-view, foveated tiles, 4:4:4 fovea, depth stream | at least 25 percent bitrate saving at equal VMAF on the fovea region; decoder time not above the Phase 2 numbers |

## 3.12 Open risks

- The 4 to 6 ms estimate for Pass B rests on assumed Adreno int32 throughput and cache behavior for gather-4; a 2x miss puts the Pico 4 in hybrid-only mode.
- Adreno's proprietary compiler may spill or serialize the ballot-based shared-pointer scheme; the fallback is per-stream byte ranges in the tile header (about 8 extra bytes per tile).
- MediaCodec low-latency behavior on Pico firmware is unverified; the hybrid latency floor depends on it.
- The 16 MB coefficient round trip may be the bandwidth item that tips Pass B over; the sparse layout is the planned fix and should be prototyped in Phase 1 if K5 is within 20 percent of its threshold.
- Thermal throttling over a session can turn a 5 ms decoder into a 7 ms one; the decode-time governor from the design consensus must be wired from day one of Phase 3.
