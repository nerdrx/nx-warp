# Texture-native atlas patches: a feasibility measurement

- **Status**: measurement report, no decision taken
- **Date**: 2026-09-06
- **Branch**: `texture-native`
- **Affects**: nothing yet. This measures a candidate patch source for ADR-0029's atlas.
- **Probe**: `probe/` (this branch), run on the attached Pico 4 over adb
- **Scripts**: `tools/texnative/` (this branch); raw data in `nx-scratch/texnative-host/`

## The idea under test

Under ADR-0029 the reference is a per-tile atlas and display is one warp step from
it. The atlas needs a *patch source*: something that puts refreshed pixels into it.
Two are already costed — nxvc coded tiles, and a hardware-HEVC base layer at
1.9 us/tile. This is a third:

> The server encodes a refreshed 64x64 tile **directly into hardware
> texture-compression blocks** (ASTC LDR, ETC2, EAC). The client copies those
> blocks into a compressed-format atlas image. The display warp samples that image
> natively. The headset GPU does **no decode work at all** — the texture unit
> expands blocks in fixed-function hardware, for free, as part of the tap it was
> going to do anyway.

The target is ADR-0029's: **4.2 ms of headset GPU per displayed frame pair, all in**.

Everything below is measured. Where a number is derived, interpolated or
extrapolated it says so. Where something could not be measured it says that too.

---

## Verdict

**The gate passes and the display side is cheap — but the wire cost is the problem,
and 6x6 is structurally wrong for a 64x64 tile grid.**

1. **Device support: unqualified pass.** The Adreno 650 samples every candidate
   compressed format — ASTC 4x4/6x6/8x8 UNORM and SRGB, ETC2, EAC — with
   `SAMPLED_IMAGE_FILTER_LINEAR` under optimal tiling, at the full atlas size. The
   idea is not blocked at the device.
2. **Display is cheap and the compressed formats are the *fastest* ones.** A full
   2176x1088 display pass (1088x1088 per eye, one frame pair) costs **1.053 ms**
   from an ASTC 8x8 atlas, against **1.327 ms** from R16_UNORM. Bilinear filtering
   of ASTC is **free** — identical to point sampling — where R16 pays 0.18 ms for
   it. That is 25 % of the 4.2 ms budget, and it leaves room.
3. **The atlas update is affordable but dominated by per-region overhead**, not
   bytes: **3.43 us per scattered 64x64 tile** for every compressed format, versus
   1.70 us uncompressed. Coalescing the same bytes into full-width strips is up to
   **28x cheaper** (full-atlas refresh: 0.071 ms instead of 1.981 ms).
4. **ASTC 6x6 cannot express a 64x64 tile.** 64/6 = 10.67. Tile origins are not
   block-aligned, so per-tile `vkCmdCopyBufferToImage` is not expressible, and —
   worse — every tile-boundary block would hold samples from two tile positions
   with **different source poses and different source frames**. Only 4x4 and 8x8
   divide 64. Padding a tile to 66x66 works but costs 6.2 % and breaks ADR-0029's
   picture-shaped atlas layout.
5. **The wire cost is the blocker, and on Wi-Fi it is decisive.** At matched luma
   PSNR, ASTC costs **1.9x to 2.4x** the bytes of intra nxvc; at matched *rate*
   nxvc is about **9 dB better**. Against the **43 Mbit/s these streams actually
   reached tonight**, ASTC 4x4 affords **22 refreshed tiles per frame at 47 fps and
   12 at 90 fps**; ASTC 6x6 affords **45 and 24**. A full-eye 289-tile refresh at
   90 fps would need 1065 Mbit/s at 4x4 or 523 at 6x6 — **25x and 12x over the
   measured link**. **Fast head turns must therefore go to the hardware-HEVC base
   layer, not to texture refreshes.**
6. **Server encode does not fit a real-time budget on CPU.** astcenc `-medium` on
   8 cores costs **27 ms** for a 289-tile luma frame — 2.4 frame periods at 90 fps.
   40 tiles fits (3.7 ms at 4x4). **No open-source GPU ASTC encoder exists** to
   measure (see 3c).

**Reading.** Texture-native patches are real, and they are the cheapest display
path measured so far — but they are a *bandwidth-for-GPU-time* trade, and with a
wired link off the table the trade is unfavourable at every rate this headset has
demonstrated. The honest niche is a **steady-state trickle of 12–45 tiles per frame
on top of an HEVC base layer** — 4 % to 16 % of one eye — where the display saving
is banked and the wire cost is affordable. As a general replacement for nxvc coded
tiles, and as the mechanism that repaints the picture on a fast head turn, it does
not pay.

---

## 1. Device support

Probe: `probe/src/nxtexnative.c --formats`. Raw output: `probe/results/formats.txt`.

```
device        Adreno (TM) 650
api           1.1.128        driver 0x802aa000
tsPeriod      52.083 ns      queueFamily 0, timestampValidBits 48
feat.textureCompressionASTC_LDR  1
feat.textureCompressionETC2      1
feat.textureCompressionBC        1
```

`vkGetPhysicalDeviceFormatProperties`, `optimalTilingFeatures`:

| format | SAMPLED | **FILTER_LINEAR** | TRANSFER_DST | STORAGE | image at 2176x1088 |
|---|---|---|---|---|---|
| `ASTC_4x4_UNORM` / `_SRGB` | yes | **yes** | yes | no | ok (max 16384²) |
| `ASTC_6x6_UNORM` / `_SRGB` | yes | **yes** | yes | no | ok |
| `ASTC_8x8_UNORM` / `_SRGB` | yes | **yes** | yes | no | ok |
| `ETC2_R8G8B8_UNORM` / `_SRGB` | yes | **yes** | yes | no | ok |
| `ETC2_R8G8B8A1_UNORM` | yes | **yes** | yes | no | ok |
| `ETC2_R8G8B8A8_UNORM` | yes | **yes** | yes | no | ok |
| `EAC_R11_UNORM` | yes | **yes** | yes | no | ok |
| `EAC_R11G11_UNORM` | yes | **yes** | yes | no | ok |
| `R8_UNORM`, `R16_UNORM`, `R8G8_UNORM`, `R8G8B8A8_UNORM` | yes | yes | yes | **yes** | ok |

Every compressed format reports the identical feature word `0x0001f401` =
`SAMPLED_IMAGE | BLIT_SRC | SAMPLED_IMAGE_FILTER_LINEAR | TRANSFER_SRC |
TRANSFER_DST | MIDPOINT_CHROMA_SAMPLES`.

**The gate passes.** But note what is *absent* from that word:

> **No compressed format carries `STORAGE_IMAGE`.** A compute shader cannot write
> the atlas. The only write path is `vkCmdCopyBufferToImage` from blocks that
> arrived over the wire — which is exactly the model this idea proposes, so it
> fits; but it also means **the atlas can never be written by a decode kernel**.
> A texture-native atlas and an nxvc-reconstructed atlas cannot be the same image.
> Mixing the two patch sources requires two atlases and two taps, or a re-encode
> of nxvc output into blocks on the headset, which is the decode work this idea
> exists to avoid.

That is the first real architectural consequence, and it is a hard one.

---

## 2. Display cost

Probe: `--bench`. The pass is ADR-0029 §5 exactly — one warp step from the atlas,
per-tile homography from a table (`vec4[3]` per tile, 578 tiles), bilinear by the
sampler, non-normative float. **The shader is byte-identical between runs; only the
atlas `VkFormat` changes**, so the delta is the format's sampling cost and nothing
else. Output is 2176x1088 = 1088x1088 per eye = **one displayed frame pair**.
GPU timestamps, 60 iterations after 20 warm-up, p50 reported.

Raw: `probe/results/bench-linear-synth.txt`, `bench-linear-real.txt`,
`bench-nearest-synth.txt`, `bench-sizesweep.txt`, `bench-planes.txt`.

### 2.1 One texture, one tap

| atlas format | atlas VRAM | **p50 ms/pair** | min | p90 | NEAREST p50 | linear surcharge |
|---|---|---|---|---|---|---|
| **`ASTC_8x8_UNORM`** | 0.60 MiB | **1.053** | 1.048 | 1.073 | — | — |
| `ASTC_6x6_UNORM` | 1.08 MiB | **1.071** | 1.065 | 1.090 | 1.070 | **0.000** |
| `ETC2_R8G8B8_UNORM` | 1.13 MiB | 1.071 | 1.063 | 1.097 | — | — |
| `EAC_R11_UNORM` | 1.13 MiB | 1.073 | 1.067 | 1.088 | — | — |
| `ASTC_4x4_UNORM` | 2.26 MiB | 1.086 | 1.080 | 1.110 | 1.086 | **0.000** |
| `R8_UNORM` | 2.26 MiB | 1.086 | 1.082 | 1.111 | 1.082 | 0.004 |
| `R8G8_UNORM` | 4.54 MiB | 1.105 | 1.099 | 1.142 | — | — |
| `EAC_R11G11_UNORM` | 2.26 MiB | 1.125 | 1.122 | 1.141 | — | — |
| `R8G8B8A8_UNORM` | 9.08 MiB | 1.136 | 1.130 | 1.193 | 1.136 | 0.000 |
| **`R16_UNORM`** | 4.52 MiB | **1.327** | 1.326 | 1.352 | 1.147 | **+0.180** |

SRGB variants are within noise of their UNORM twins in every case.

Three things worth stating:

- **The compressed formats are the fastest**, not merely acceptable. ASTC 8x8 beats
  R8_UNORM by 3 % while carrying three channels instead of one, in a quarter of the
  memory.
- **ASTC bilinear filtering is free.** LINEAR and NEAREST are identical to three
  decimal places. The texture unit decodes and filters in the same fixed-function
  step. `R16_UNORM` is the outlier that proves it: it pays **0.180 ms**, 16 %, for
  the same filtering.
- **Content does not matter.** Re-run with real ASTC encoded from
  `inter_pan8.yuv` (2176x1088 atlas, astcenc `-medium`): 4x4 **1.121**, 6x6
  **1.083**, 8x8 **1.053** — within 3 % of the synthetic payload, as expected of a
  fixed-rate hardware decoder.

### 2.2 The pass is per-pixel bound, not bandwidth bound

Size sweep, p50 ms, same shader (`bench-sizesweep.txt`):

| output size | pixels | ASTC 6x6 | R8 | R16 | RGBA8 |
|---|---|---|---|---|---|
| 1088x1088 | 1.18 M | 0.538 | 0.545 | 0.666 | 0.571 |
| 2176x1088 | 2.37 M | 1.079 | 1.088 | 1.328 | 1.141 |
| 3072x1536 | 4.72 M | 2.153 | 2.166 | 2.643 | 2.270 |
| 4352x2176 | 9.47 M | 4.328 | 4.347 | 5.299 | 4.528 |

Exactly linear in output pixels (4x pixels → 4.01x time), and the format ordering
is stable at every size even as the atlas grows from 0.56 MiB to 36 MiB. **The
display pass is bound by per-output-pixel work, not by atlas footprint**, so these
conclusions do not depend on the atlas fitting in cache.

### 2.3 The comparison that actually decides it: taps, not bytes

ADR-0029's atlas is a `RefPicture` — **planar** Y/Co/Cg. A planar atlas needs three
taps. A three-channel compressed atlas needs one. Measured with a three-tap variant
of the same shader (`probe/src/display3.comp`, `bench-planes.txt`):

| atlas layout | taps | p50 ms/pair | vs 1-tap ASTC 8x8 |
|---|---|---|---|
| ASTC 8x8, one RGB texture | 1 | **1.053** | — |
| ASTC 8x8, three planes | 3 | 1.282 | +0.229 |
| EAC_R11, three planes | 3 | 1.290 | +0.237 |
| R8_UNORM, three planes | 3 | 1.298 | +0.245 |
| R16_UNORM, three planes | 3 | **2.124** | **+1.071** |

So against the atlas layout ADR-0029 actually specifies at 16-bit precision, a
one-tap compressed atlas saves **1.07 ms per frame pair — a full quarter of the
4.2 ms budget**. Against an 8-bit planar atlas it saves 0.245 ms.

**Caveat, and it is a large one.** §4 below finds that packing Y/Co/Cg into one
ASTC RGB texture is *not* the right encoding for quality; the host study recommends
a separate luma texture and a half-resolution chroma texture, which is **two taps,
not one**. The two-tap cost was not measured directly; it is bracketed by the
measured 1-tap (1.053) and 3-tap (1.282) figures at roughly **1.17 ms**. Stated as
an interpolation, not a measurement.

### 2.4 Atlas update cost

`--update`. `vkCmdCopyBufferToImage` of N scattered 64x64 tiles. Raw:
`update-cost.txt`, `update-scaling.txt`, `update-strips.txt`.

| refreshed tiles | ASTC 4x4 | ASTC 8x8 | ETC2/EAC | R8 | R16 | RGBA8 |
|---|---|---|---|---|---|---|
| 1 | 0.010 | 0.008 | — | 0.004 | — | — |
| 40 | 0.141 | 0.141 | 0.141 | 0.071 | 0.090 | 0.072 |
| 100 | 0.348 | 0.346 | 0.347 | 0.172 | 0.226 | 0.182 |
| 289 | 0.994 | 0.993 | 0.993 | 0.492 | 0.653 | 0.540 |
| 578 | 1.981 | 1.980 | 1.981 | 0.984 | 1.312 | 1.080 |

**Every compressed format costs exactly the same**, whether a tile is 1024 B (8x8)
or 4096 B (4x4). The cost is **per region**, not per byte: a clean linear
**3.43 us/tile** compressed, **1.70 us/tile** uncompressed R8. Compressed regions
cost almost exactly 2x uncompressed ones, and the payload size is irrelevant.

For scale: the hardware-HEVC base layer is 1.9 us/tile. **A scattered ASTC tile
copy is 1.8x the cost of an HEVC base-layer tile** — the copy is not free.

The same bytes as full-width 64-row strips (`--strips`), i.e. the fewest legal
regions covering whole tile rows:

| | 578 tiles as 578 regions | as 17 strip regions | speed-up |
|---|---|---|---|
| ASTC 8x8 | 1.980 | **0.071** | **28x** |
| ASTC 4x4 | 1.981 | **0.217** | 9.1x |
| R8_UNORM | 0.984 | **0.381** | 2.6x |

**Per-region overhead is the whole cost.** A dense refresh should be coalesced; a
sparse one (40 tiles, 0.141 ms) is cheap enough not to care.

### 2.5 The alignment problem — ASTC 6x6 is structurally wrong here

`64 / 6 = 10.67`. Tile origins sit at multiples of 64, which are not multiples of 6.
Vulkan requires a compressed copy's `imageOffset` and `imageExtent` to be multiples
of the block extent, so **per-tile `vkCmdCopyBufferToImage` is not expressible for
6x6 at a 64x64 tile grid.** The probe refuses to measure it and says why.

This is not an API inconvenience that a workaround fixes. Under ADR-0029 each tile
position carries its own composed homography `C`, its own `src_frame` and its own
`gen`. **A 6x6 block straddling a tile boundary holds samples belonging to two tile
positions with different source poses and different source frames.** There is no
copy that refreshes one without corrupting the other, and no display warp that can
sample it correctly, because the block is a single filtering unit under one
homography.

| block | 64 / block | tile-aligned? |
|---|---|---|
| 4x4 | 16.00 | **yes** |
| 6x6 | 10.67 | **no** |
| 8x8 | 8.00 | **yes** |
| ETC2 / EAC (4x4) | 16.00 | **yes** |

The escape is to pad each tile to 66x66 (11x11 blocks), which the host study
measured: it costs **6.2 %** of the luma payload and 18.8 % of the chroma leg. But
a padded tile no longer tiles a picture, so the atlas stops being a `RefPicture`
and becomes a slot-indexed sheet — which contradicts ADR-0029's hard requirement
that "the atlas pixel layout never depends on any per-tile choice" and that a pass
"samples it directly with no repacking".

**Recommendation: if this idea proceeds, it proceeds at 4x4 or 8x8.**

---

## 3. Server encode cost (RX 7900 XTX host)

ARM astc-encoder **v5.7.0** built from source (`astcenc-avx2`, Release, AVX2,
`-j4` under `chrt -i 0 taskset -c 0-7 nice -n 19`). Not previously installed.
Preset `-medium`, LDR linear. Data: `nx-scratch/texnative-host/task3_timing.json`.

Tiles are laid on a block-aligned sheet (pitch 64 for 4x4/8x8, **66** for 6x6) so
each tile owns a disjoint set of blocks — this reproduces "each patch encoded
independently" in one invocation per frame.

### 3.1 Batch rate (one 1088x1088 luma frame ÷ 289) — the number that matters

| block | `-j 1` ms/tile | `-j 8` ms/tile | frame `-j 8` |
|---|---|---|---|
| 4x4 | 0.626 | **0.0934** | 27.0 ms |
| 6x6 | 1.165 | **0.175** | 50.5 ms |
| 8x8 | 1.865 | **0.284** | 82.0 ms |

Counter-intuitively **larger blocks are slower**: astcenc's search space per block
grows faster than the block count falls. `-j 8` scales 6.6–6.7x on 8 cores.

Extrapolated ms per frame at the batch rate (luma only; add ~25 % for chroma):

| tiles | 4x4 j1 | **4x4 j8** | 6x6 j1 | **6x6 j8** | 8x8 j1 | **8x8 j8** |
|---|---|---|---|---|---|---|
| 40 | 25.0 | **3.74** | 46.6 | **6.99** | 74.6 | **11.3** |
| 289 | 180.8 | **27.0** | 336.7 | **50.5** | 539.0 | **82.0** |
| 578 | 361.7 | **54.0** | 673.4 | **101.0** | 1078 | **164** |

At 90 fps the frame period is 11.1 ms. **On 8 cores, only a 40-tile refresh at 4x4
fits (3.74 ms). 289 tiles does not fit at any block size. 578 tiles is out of reach
everywhere.** This is a genuine constraint on the idea, not a tuning detail.

### 3.2 Per-invocation overhead

One 64x64 image in its own process: 2.06–3.42 ms wall at `-j 8`, of which **~2 ms
is fixed process and I/O overhead** — 11–22x the batch cost at 4x4. **Forking
astcenc per patch is a non-starter**; an in-process library call is mandatory.

### 3.3 GPU compute ASTC encoder — **NOT MEASURED**

Stated plainly, with the reason:

- **betsy**: cloned and inspected. It has **no ASTC codec at all** — `bin/Data/`
  holds only BC1/BC4/BC6H/EAC/ETC1/ETC2 kernels; `grep -i astc src/` is empty.
  Disqualified on capability, so it was not built.
- **AMD Compressonator**: cloned. `CMakeLists.txt:176` hard-disables ASTC
  (`cmp_option(OPTION_BUILD_ASTC ... OFF FALSE)`) and the tree contains no ASTC GPU
  kernels.
- **astc-encoder upstream**: CPU only. No OpenCL/Vulkan/CUDA path exists.

Also relevant: **RDNA3 has no hardware ASTC texture support**, so even a GPU
encoder on this host would need a software decode path to verify against.

**Conclusion: host-side GPU ASTC encoding is unproven and has no off-the-shelf
option. Cost the server path from the CPU numbers, which do not fit at 289 tiles.**

---

## 4. Rate and quality

Content: `nx-scratch/inter_pan8.yuv`, 12 frames, 1088x1088, 4:2:0, 289 tiles/frame
= 3468 tile samples. Data: `task4_psnr_tiles.csv`, `task4_final.json`.

### 4.1 How 4:2:0 is handled — plane-separated, and why

**Chosen and measured: Option B.** Luma → one 64x64 ASTC texture (greyscale);
chroma → one 32x32 ASTC texture with R=U, G=V, B=128, encoded `-cw 1 1 0 0` so the
unused channel costs nothing in the error metric.

**Option A (convert to RGB, encode as ASTC RGB) is rejected**, on two grounds:

1. **Structural.** ADR-0029's atlas holds the **coded sample domain** (Y/Co/Cg,
   before the inverse colour transform), plane-separated. Converting to RGB to
   encode and back to compose inserts two matrix round-trips with clipping into the
   path, and forfeits ASTC's luminance endpoint modes.
2. **Measured**: 28.88 dB at 4x4 and 26.71 dB at 8x8, versus 58.86 / 36.59 dB for
   Option B.

**Caveat on the Option A number, stated honestly**: most of that loss is not ASTC.
The YUV→RGB→Y round trip alone caps at 26.47 dB on this clip because **56.3 % of
pixels clip out of the RGB gamut** — this is a synthetic full-range test pattern,
not natural video, and the clip rate is pathological. The structural argument
stands on its own and Option B wins by 30 dB, so the conclusion is not sensitive to
the clip; but "Option A is 30 dB worse" should not be quoted as a general fact.

> **Untested third option, and the obvious next experiment.** Store Y, Co and Cg
> *directly* in an ASTC RGB texture's three channels — no colour conversion, no
> gamut, no clipping, and **one tap** instead of two, which §2.3 values at ~0.12 ms.
> Neither Option A nor Option B tests this. ASTC's endpoint modes are tuned for
> correlated RGB and YCoCg channels are deliberately decorrelated, so quality would
> likely land between the two, but nothing here measures it.

### 4.2 Per-tile luma PSNR (Option B, 3468 samples)

| block | mean | median | 5th pct | 25th pct | min | max | std |
|---|---|---|---|---|---|---|---|
| 4x4 | **58.86** | 58.85 | 57.00 | 58.15 | 55.34 | 62.30 | 1.10 |
| 6x6 | **42.48** | 42.44 | 40.76 | 41.73 | 39.72 | 46.19 | 1.06 |
| 8x8 | **36.59** | 36.50 | 34.40 | 35.62 | 32.80 | 41.95 | 1.46 |

Chroma (U; V tracks within 0.05 dB):

| block | U mean | U median | U p5 |
|---|---|---|---|
| 4x4 | 47.15 | 46.47 | 38.67 |
| 6x6 | 39.54 | 38.68 | 31.44 |
| 8x8 | 35.76 | 34.78 | 27.60 |

**Chroma is the weak leg**: its p5 sits ~8 dB below its mean at every block size,
where luma's sits ~2 dB below. On this clip the quality risk in Option B is chroma,
not luma.

### 4.3 Rate

Fixed-rate, confirmed: 4x4 = 8.000, 6x6 = 3.5556, 8x8 = 2.000 bpp. But the
*delivered* cost is higher at 6x6 because of the padding in §2.5:

| block | luma B/tile | luma bpp/px | chroma B/tile | **total B/tile** | **total bpp per luma px** |
|---|---|---|---|---|---|
| 4x4 | 4096 | 8.000 | 1024 | **5120** | **10.000** |
| 6x6 | 1936 | **3.781** | 576 | **2512** | **4.906** |
| 8x8 | 1024 | 2.000 | 256 | **1280** | **2.500** |

Chroma adds exactly 25 % at 4x4 and 8x8; 29.8 % at 6x6, because the 32x32 chroma
plane pads to 36x36.

### 4.4 Lossless entropy gain on the block payloads (frame 0)

Whole-frame = all 289 tiles concatenated (cross-tile LZ available). **Per-tile =
each tile compressed alone — the realistic wire model for an atlas patch.**

| block | leg | raw B | zstd −19 whole | **zstd −19 per-tile** | order-0 whole | **order-0 per-tile** |
|---|---|---|---|---|---|---|
| 4x4 | luma | 1183744 | −56.4 % | **−36.2 %** | −20.0 % | −22.1 % |
| 4x4 | chroma | 295936 | −45.0 % | −25.3 % | −12.7 % | −20.5 % |
| 6x6 | luma | 559504 | −42.5 % | **−19.9 %** | −12.9 % | −15.3 % |
| 6x6 | chroma | 166464 | −38.1 % | −14.5 % | −8.0 % | −16.1 % |
| 8x8 | luma | 295936 | −44.1 % | **−16.6 %** | −14.5 % | −18.4 % |
| 8x8 | chroma | 73984 | −43.2 % | −7.6 % | −11.1 % | −24.3 % |

**ASTC payloads are not near-random, but most of the whole-frame gain is not real.**
The order-0 model, which cannot exploit repetition, finds only 8–20 % — that is the
content-robust figure. The gap between whole-frame (−42 to −56 %) and per-tile
(−7.6 to −36 %) is LZ matching *between* tiles, which this synthetic repeating
pattern makes trivially available and real content will not.

**Budget roughly 10–20 % wire gain from entropy-coding ASTC payloads, not 45 %.**
The bandwidth table below takes none of it.

### 4.5 Against nxvc coded tiles

`nxv-enc --pix yuv420p --qp N --threads 8`, **intra-only** — under a texture-native
atlas every refreshed tile is an independently decodable patch, so the fair nxvc
comparison is its intra cost. `nxv-info --tiles` confirms all 3468 tiles are
`INTRA`. Bytes/tile from `nxv-info`, PSNR from `nxv-dec`.

| QP | B/tile mean | median | p95 | luma PSNR mean | p5 |
|---|---|---|---|---|---|
| 22 | **719.8** | 712 | 908 | **37.44** | 36.46 |
| 26 | **541.9** | 536 | 692 | **34.15** | 33.12 |
| 30 | **399.2** | 396 | 516 | **30.90** | 29.80 |

Sweep to bracket ASTC: QP 0 → 2319.8 B @ 55.11 dB; QP 10 → 1479.8 @ 48.20;
QP 14 → 1204.3 @ 44.72; QP 18 → 947.9 @ 41.05; QP 38 → 162.9 @ 23.78.

**Matched luma PSNR** (ASTC = luma + chroma; nxvc = full 4:2:0 tile; QP and bytes
**interpolated** on log(bytes) vs dB):

| ASTC block | ASTC dB | ASTC B/tile | matched nxvc QP | nxvc B/tile | **ASTC : nxvc** |
|---|---|---|---|---|---|
| 4x4 | 58.86 | 5120 | **out of range** | ~2893 (extrapolated) | ~1.77 : 1 |
| 6x6 | 42.48 | 2512 | 16.4 (interp.) | 1040 | **2.41 : 1** |
| 8x8 | 36.59 | 1280 | 23.0 (interp.) | 669 | **1.91 : 1** |

**The 4x4 row is not a measurement.** nxvc cannot reach 58.86 dB at any legal QP;
its ceiling is 55.11 dB at QP 0, 2320 B/tile. So at nxvc's ceiling ASTC 4x4 buys
+3.75 dB for **2.21x the bytes**. Do not quote 1.77:1 as measured.

**Iso-rate, the same data read the other way**: at 1280 B/tile nxvc sits at QP ~12.9
= **45.67 dB**, i.e. **+9.08 dB over ASTC 8x8 at identical bytes.**

> At matched quality ASTC costs **1.9–2.4x** the bytes of intra nxvc. At matched
> rate nxvc is **~9 dB** better. That is the central finding of this section, and
> it is what makes the wire, not the GPU, the binding constraint.

---

## 5. Bandwidth — Wi-Fi only

**A wired link is off the table.** This section sizes the refresh budget against
the Pico 4's Wi-Fi 6 in practice, and nothing else.

Assumptions, stated:

- **Link budget. 43 Mbit/s is the measured figure**: that is what the live nxwarp
  streams reached tonight on this Pico 4 over Wi-Fi 6. **How much headroom sits
  above it is unknown** — it was not a saturation test, so 43 is a *demonstrated*
  rate, not a measured ceiling. 60 and 100 Mbit/s are carried alongside as
  optimistic cases; 100 Mbit/s is the figure `docs/01-bitstream.md` already uses
  for its "Standard wireless" operating point. If the real ceiling is nearer 43
  than 100, every ASTC row below gets worse, not better.
- Bytes are the **measured** Option B per-tile payloads of §4.3 (luma + chroma),
  including 6x6's padding. **Payload only** — no tile headers, no rANS flush, no
  UDP/IP (46 B per datagram), no FEC, no retransmit. Real wire cost is strictly
  higher; `docs/01-bitstream.md` budgets fixed per-tile cost at under 8 B.
- **No entropy coding applied.** §4.4 suggests 10–20 % is robustly available on
  the ASTC rows; taking it would move them down by that much and does not change
  any conclusion below.
- Tile counts are **totals across both eyes** (the v1 stereo configuration is 578;
  one full eye is 289).

### 5.1 Refreshed tiles per frame that fit

| scheme | B/tile | **43 Mbit** 47 fps | 60 | 100 | **43 Mbit** 90 fps | 60 | 100 |
|---|---|---|---|---|---|---|---|
| **ASTC 4x4** | 5120 | **22** | 31 | 52 | **12** | 16 | 27 |
| **ASTC 6x6** | 2512 | **45** | 63 | 106 | **24** | 33 | 55 |
| ASTC 8x8 | 1280 | 89 | 125 | 208 | 47 | 65 | 108 |
| nxvc QP 22 | 720 | 159 | 222 | 369 | 83 | 116 | 193 |
| nxvc QP 26 | 542 | 211 | 294 | 491 | 110 | 154 | 256 |
| nxvc QP 30 | 399 | 286 | 400 | 666 | 150 | 209 | 348 |

**The explicit answer, at the measured 43 Mbit/s:**

- **ASTC 4x4 affords 22 refreshed tiles per frame at 47 fps, and 12 at 90 fps.**
- **ASTC 6x6 affords 45 refreshed tiles per frame at 47 fps, and 24 at 90 fps.**

Neither reaches the 40-tile working figure at 90 fps. 4x4 does not reach it at
either frame rate. Only ASTC 8x8 clears 40 tiles at 90 fps (47), and 8x8 is the
2.00 bpp / 36.59 dB point — the lowest-quality option measured.

### 5.2 What a fast head turn needs, and why it cannot come from here

A fast head turn is the case where the warp stops predicting: large disocclusion,
most of the picture invalid at once. That is a **289-tile refresh (one full eye)**,
or 578 for both. What that costs on the wire:

| scheme | 289 tiles @ 47 fps | 289 tiles @ 90 fps | vs the 43 Mbit/s measured link |
|---|---|---|---|
| ASTC 4x4 | 556 Mbit/s | **1065 Mbit/s** | **25x** over |
| ASTC 6x6 | 273 Mbit/s | **523 Mbit/s** | **12x** over |
| ASTC 8x8 | 139 Mbit/s | 266 Mbit/s | 6.2x over |
| nxvc QP 26 | 59 Mbit/s | 113 Mbit/s | 2.6x over |
| nxvc QP 30 | 43 Mbit/s | 83 Mbit/s | 1.9x over |

**Conclusion, stated plainly: fast head turns must go to the hardware-HEVC base
layer, not to texture refreshes.** At 90 fps a full-eye ASTC 6x6 refresh needs
523 Mbit/s and a 4x4 refresh needs 1065 Mbit/s, against a demonstrated 43. There is
no plausible Wi-Fi 6 headroom multiple that closes a 12x–25x gap. Even intra nxvc
misses it by 1.9x. The base layer is the only patch source measured that carries a
whole-picture refresh at a whole-picture bitrate, and at 1.9 us/tile it is also the
cheapest to apply.

That fixes the role of texture-native patches, if they have one:

> **A steady-state, small-refresh source on top of an HEVC base layer** — of order
> 12–45 tiles per frame at 43 Mbit/s, i.e. **4 % to 16 % of one eye** — never the
> mechanism that repaints the picture when the head moves fast.

And it sharpens the objection from §4.5: in exactly the regime where the budget is
tightest, ASTC spends 1.9–2.4x the bytes of intra nxvc for the same quality. On a
Wi-Fi-only link that is the wrong currency to be spending.

## 6. The budget, assembled

ADR-0029 targets **4.2 ms of headset GPU per displayed frame pair**. Adding the
separately-measured stages (stated as a sum of independent measurements, not an
end-to-end run):

| | ASTC 8x8, 40 tiles | ASTC 8x8, 289 tiles | ASTC 8x8, 289 coalesced | R16 planar today |
|---|---|---|---|---|
| display warp | 1.053 | 1.053 | 1.053 | 2.124 |
| atlas update | 0.141 | 0.993 | ~0.05 | 0.653 |
| **subtotal** | **1.19** | **2.05** | **1.10** | **2.78** |
| headroom to 4.2 ms | 3.01 | 2.15 | 3.10 | 1.42 |

Against ADR-0029's starting point — an 8.8 ms skip warp *per eye* — the display
side of a texture-native atlas is not the problem. Entropy decode (~1 ms/eye) and
coded-tile reconstruction (~1 ms/eye) still have to fit in the headroom, and at
289 scattered tiles they do, narrowly.

**Add the two-tap correction from §2.3** (Option B is two textures, not one):
roughly +0.12 ms, which does not change the conclusion.

**But read this table against §5.** The 289-tile columns are what the *GPU* can
afford, not what the *link* can deliver: at 43 Mbit/s and 90 fps the wire affords
12 tiles at 4x4 and 24 at 6x6. **The headset GPU is no longer the binding
constraint on this patch source — the Wi-Fi link is.** That is a reversal of the
problem ADR-0029 set out to solve, and it is the reason this report does not
recommend adopting texture-native patches as a general refresh mechanism.

---

## 7. What this measurement says, and what it does not

**Says:**

- The device supports it. Linear-filtered ASTC/ETC2/EAC sampling on the Adreno 650
  is real, is the fastest atlas format measured, and its bilinear filtering is free.
- The display pass fits the budget with room, and beats a 16-bit planar atlas by
  1.07 ms per frame pair.
- The atlas update is per-region-bound at 3.43 us/tile, and coalescing recovers up
  to 28x.
- ASTC 6x6 is incompatible with a 64x64 tile grid, for a reason that is about pose
  semantics and not just API alignment.
- The wire cost is 1.9–2.4x nxvc at matched quality; on the 43 Mbit/s these
  streams actually reached, that buys 12–45 refreshed tiles per frame, and a
  full-eye refresh is 12x–25x out of reach — so fast head turns belong to the HEVC
  base layer. The server also cannot encode 289 tiles per frame on 8 CPU cores.

**Does not say:**

- Nothing here is an end-to-end run. Every figure is a stage measured in isolation
  on an idle device with no compositor, no session and no thermal load. The Pico 4
  throttles; none of this was measured hot.
- The content is a **synthetic panning test pattern** with full-range,
  near-uncorrelated chroma. It inflates the whole-frame zstd gains, tightens the
  PSNR spread unrealistically, and makes the Option A gamut clipping pathological.
  **The ratios are the durable part; the absolute numbers should be re-measured on
  a real render before they drive a decision.**
- `-medium` only. `-thorough` would raise PSNR and roughly triple encode time.
- nxvc was run **intra-only**, which is the fair comparison for an independently
  decodable patch. Inter-coded nxvc tiles are far cheaper; that comparison was not
  made and would need a `--poses` sidecar.
- The 4x4 matched-PSNR row is **extrapolated past nxvc's QP range** and must not be
  quoted as measured.
- The two-tap display cost is interpolated, not measured.
- Storing Y/Co/Cg directly in an ASTC RGB texture — the encoding that would keep
  the one-tap saving *and* avoid the gamut problem — was not tested at all. It is
  the first thing to measure if this goes further.
- No GPU ASTC encoder was measured because none exists to measure.

## Reproducing

```
probe/build.sh                                            # build + push, no install
adb shell /data/local/tmp/nxtex/nxtexnative --formats
adb shell /data/local/tmp/nxtex/nxtexnative --bench --iters 60 --warmup 20
adb shell /data/local/tmp/nxtex/nxtexnative --bench --format ASTC_8x8_UNORM --planes 3 \
                                            --spv /data/local/tmp/nxtex/display3.spv
adb shell /data/local/tmp/nxtex/nxtexnative --update --ntiles 289 [--strips]
```

Host side: `tools/texnative/{prep_images,task3_timing,task4_quality,task4e_nxvc,task4_final}.py`.

The device runs were gated on `adb logcat -d -t 200 | grep 'nxwarp\['` being empty
and no `hello_xr` on the host. Nothing was installed, nothing was rebooted, and
`wivrn-server` and the installed headset app were not touched.
