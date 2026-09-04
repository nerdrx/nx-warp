# NX Warp Vulkan decoder

`nxvc_vk_decoder` turns an `.nxv` byte stream into decoded images on a Vulkan
compute queue. A frame costs **two dispatches** (PAPER 3.2.1):

```
   bytes ──► host parse ──► Pass A ──► Pass B ──► output image(s) ──► [readback]
             (this dir)     passA/     passB/
```

| directory | role |
|---|---|
| `passA/` | interleaved rANS entropy decode: tile payloads → int16 coefficients + CBF bits. `passA/README.md` |
| `passB/` | reconstruction: dequantize, inverse transform, DC-plane and directional intra prediction, resample, colour → display image. `passB/README.md` |
| this directory | the container parse, the Vulkan runtime around the two kernels, the C ABI and the CLI |

The normative specification is `docs/SYNTAX.md` and the CPU reference in
`ref/`. This decoder reproduces `nxv-dec` output **bit for bit**; where it
cannot, it refuses the stream rather than guessing.

| file | role |
|---|---|
| `../../include/nxvc/nxvc_vk.h` | the C ABI |
| `nxvc_vkdec_parse.{h,cpp}` | host-side container parse, Vulkan-free |
| `nxvc_vkdec.cpp` | device, buffers, pipelines, one command buffer per frame |
| `tools/nxvc-vkdec.cpp` | CLI, option-for-option `nxv-dec` |

Tests live in `tests/vk-decoder/conformance/` and are registered as
`vk.decoder.*`.

---

## The C ABI in one page

```c
nxvc_vkd_create_info ci;
nxvc_vk_decoder_create_info_default(&ci);
ci.flags = NXVC_VKD_FLAG_READBACK;          /* only if you want host planes */
ci.output_format = NXVC_VKD_OUT_AUTO;

nxvc_vk_decoder *dec;
nxvc_vk_decoder_create(&ci, &dec);
nxvc_vk_decoder_parse_stream_header(dec, buf, len, &consumed);
nxvc_vk_decode_frame(dec, buf + consumed, len - consumed, &consumed);

nxvc_vkd_images img;   nxvc_vk_decoder_images(dec, &img);
nxvc_vkd_stats  st;    nxvc_vk_decoder_stats(dec, &st);
```

* **Device.** Leave `create_info.device` NULL and the library creates its own
  instance, picks a physical device (optionally filtered by `device_name`) and
  creates a device with one compute queue. Fill all five handles instead and
  it **adopts** the caller's device and allocates nothing it does not own —
  which is how WiVRn NX's server runs on Monado's `VkDevice` and how the
  Android client runs on the client's.
* **One decode call.** `nxvc_vk_decode_frame()` parses, uploads, dispatches
  both passes, signals the decoder's timeline semaphore and waits.
  `nxvc_vk_decode_frame_ex(..., NXVC_VKD_SUBMIT_ASYNC, ...)` returns straight
  after submission; wait with `nxvc_vk_decoder_wait()` or, better, have the
  compositor wait on `nxvc_vk_decoder_timeline()` at
  `nxvc_vk_decoder_timeline_value()`.
* **Output.** `nxvc_vk_decoder_images()` names the images Pass B wrote; they
  stay in `VK_IMAGE_LAYOUT_GENERAL` and are overwritten in place by the next
  frame. `NXVC_VKD_OUT_AUTO` picks the two-plane 4:2:0 YCbCr store for a 4:2:0
  passthrough stream (the WiVRn NX client samples it directly and it halves
  reference-slot memory on the headset, `docs/INTEGRATION-DECISIONS.md` 3) and
  RGBA8 otherwise. `NXVC_VKD_OUT_RGB10A2` and explicit `RGBA8` / `YCBCR420`
  are also available.
* **Host planes.** With `NXVC_VKD_FLAG_READBACK`,
  `nxvc_vk_decoder_read_planes()` copies the frame back in the reference
  decoder's planar layout (Y/Co/Cg[/A], chroma at half size for 4:2:0) — byte
  for byte what `nxv-dec` writes, which is what makes the conformance test a
  pixel-for-pixel comparison.
* **Stats.** `nxvc_vkd_stats` carries host parse / submit / total milliseconds,
  device timestamps per pass, frame and payload bytes, coefficient traffic and
  the tile counts.
* **Tuning, for measurement only.** `nxvc_vk_decoder_set_dir_sched()` selects
  the directional-intra wavefront Pass B is compiled with (0 the normative
  derivation and the default, 1 without the above-right reference, 2 with
  32x32 sub-tiles, 3 both). It is a **bitstream** property, not a performance
  option: a value other than 0 decodes a conformant stream to different
  pixels, and `ref/` produces streams that match it only when built with
  `-DNXVC_DIR_SCHED_EXPERIMENT`. `nxvc_vk_decoder_set_tile_sort()` groups Pass
  B's workgroups by tile shape; that one is pure host-side reordering and the
  output is bit-identical either way. Both are also reachable from the CLI as
  `--dir-sched N` and `--tile-sort`.
* **Errors.** `nxvc_vkd_status` numbers 0 and −1..−6 exactly as
  `nxvc_vk_status` in `vk/common`'s `<nxvc/vk/nxvc_vk.h>` does, and adds
  −7..−9 for the bitstream errors of `<nxvc/nxvc.h>`. Both headers can be
  included in the same translation unit.
  `nxvc_vk_decoder_last_error()` explains the most recent failure in words.

---

## What the host parses

Everything above the entropy-coded tile payload, on the CPU, in
`nxvc_vkdec_parse.cpp`: the 64-byte stream header and its TLV area, the
40-byte frame header with its optional quantization matrices and 120-byte
probability-table deltas, the 12-byte tile-row headers with their skip
bitmaps, and the 8-byte tile headers with their optional MV and alpha bytes.
It follows `ref/src/codec_impl.inc` function by function and makes **every
check the reference makes, in the same order**, so a stream this decoder
accepts is exactly a stream `nxv-dec` accepts — and one it refuses is refused
with the same named status (`tests/vectors/rejects.md5` pins both). The order
matters as much as the checks: `flags` bit 2 without tool bit 17 is rejected
before `frame_bytes` is even read, exactly where `parse_frame_header()` rejects
it, so `r14` gets `BITSTREAM` rather than `TRUNCATED`.

Three frame-level facts come out of the tool bits and the frame header and are
handed to both kernels (`docs/SYNTAX.md` 2.3, 3.1):

| | from | Pass A | Pass B |
|---|---|---|---|
| `CTX_V2` | tool bit 21 | 16 coded contexts, the `kDefaultFreqV2` family, 160-byte transmitted table sets | — |
| `INTRA_DIR` | tool bit 17 | one extra mode unit per coded plane | the wavefront |
| layered form | frame `flags` bit 2 | — | the modes predict the DC-plane residual |
| `SIGN_HIDE` | tool bit 22 | the sign at scan position `LAST` is the parity of the unit | — |

A transmitted probability-table set is 120 bytes under the v1 context model and
**160** under `CTX_V2` (`docs/SYNTAX.md` 9.4), and the two models have separate
built-in families: contexts 0–11 keep their meaning in both but not their
statistics, because under `CTX_V2` they no longer see the DC plane.
`ref/src/default_tables.inc` is still included verbatim rather than copied, so
neither family can drift.

What comes out is the two descriptions the kernels want, plus the byte offset
of every tile's payload inside the frame buffer:

* **Pass A's tile descriptors** — `{byte offset, byte length, coefficient
  index, CBF index}` per tile — and
* **Pass B's `NxvwTileRec` array** in raster order, its `NxvwPassBPush` push
  constants, the frame's cumulative-frequency tables and its weighting
  matrices.

This closes "the tile-record gap" `passB/README.md` describes: both the
descriptor and the record are derived from the same parsed tile header, here,
once.

### Skipped tiles

Bit *i* of a tile row's `skip_bitmap` means tile *i* is `WARP_SKIP` and is not
transmitted. Bits above the row's tile count must be zero — the bitmap covers
one tile row of one eye — and a skip references a frame that a stream without
the `INTER` tool bit cannot have, so both are **`BITSTREAM`**, not
"unsupported": the stream is malformed rather than merely ahead of this
profile. `nxv-dec` and `nxvc-vkdec` agree on `tests/vectors/r08_skip_bitmap`.

With `NXVC_VKD_FLAG_ALLOW_SKIPPED_TILES` set, a skipped tile instead becomes a
`WARP_SKIP` record over a coefficient slot the host zeroes with a
`vkCmdFillBuffer`, and Pass B reconstructs it deterministically as "no
coefficients, no prediction". That is the shape the Phase 2 inter predictor
plugs into (`passB/README.md`, "Inter prediction hook"): the record and the
zeroed slot stay, only `pred` changes.

---

## Two dispatches, and why Pass A is sometimes more than one

Pass B is always one dispatch, `(tiles_x, tiles_y)` workgroups, one per tile.

Pass A is one dispatch **per distinct `nsub_log2` in the frame**. The tile
header's lane count is a free per-tile field (`docs/SYNTAX.md` 4.1; vectors
v24/v25/v26 carry 1, 2 and 32 lanes), and the kernel's cluster width is a
specialisation constant, so the host groups the frame's tiles by lane count
and issues one dispatch for each group over a contiguous slice of the
descriptor array. The slice is addressed with `vkCmdDispatchBase`, so no extra
push constant and no reordering of the coefficient buffer is needed: each
descriptor already names its own destination. A normal frame has exactly one
group; `nxvc_vkd_stats::lane_groups` says how many.

The read-pointer mode is chosen per dispatch: the subgroup-ballot path
whenever the device's subgroups are at least as wide as the tile's lane
cluster, the LDS fallback otherwise (so a 32-lane tile on lavapipe's 8-wide
subgroups takes the fallback automatically). Both produce identical output and
both are covered by the conformance sweep.

---

## Timing

Two 2048x2048 eyes at 4:2:0 — 2048 tiles in one frame, the shape the headset
actually streams — decoded 20 times, best of run. Informational.

**RX 7900 XTX (RADV NAVI31)**, streams from `nxvc_encoder`'s **default
configuration**, which since v1.3 means `INTRA_DIR` + `CTX_V2` + `SIGN_HIDE`
are all on:

| QP | frame bytes | Pass A | Pass B | GPU total | wall |
|---|---|---|---|---|---|
| 12 | 2.64 MB | 1.61 ms | 1.92 ms | **3.53 ms** | 4.72 ms |
| 24 | 1.32 MB | 1.18 ms | 1.88 ms | **3.06 ms** | 4.00 ms |
| 36 | 0.17 MB | 0.38 ms | 1.87 ms | **2.26 ms** | 2.73 ms |

**llvmpipe (lavapipe, 32-thread CPU)**, best of 5, for scale: 253 ms at QP 12,
238 ms at QP 24, 155 ms at QP 36. It is a conformance oracle, not a
performance target.

Pass A still scales with the symbol rate. **Pass B no longer does, and it is no
longer cheap**: directional intra turned it from a 0.26 ms flat cost into a
1.9 ms flat cost, and the whole of that increase is the wavefront of
`docs/SYNTAX.md` 7.4. The next two sections are that number taken apart.

### Fixed cost versus per-byte cost

The tile-size question — is 64x64 the right unit for the *decoder*, separately
from the header-bytes argument — is a question about the intercept. A
least-squares fit of each pass against payload size over the QP 63/51/36/24/12
ladder, same 2048 tiles, separates the two:

| | fixed, per frame | slope, per MB of payload | fixed, per tile |
|---|---|---|---|
| Pass A, RADV | 0.25 ms | 0.54 ms | **121 ns** |
| Pass B, RADV | 1.86 ms | 0.01 ms | **906 ns** |
| Pass A, lavapipe | 15.1 ms | 41.7 ms | 7.4 µs |
| Pass B, lavapipe | 128.5 ms | ~0 | 62.7 µs |

**Pass B is entirely fixed cost.** Its slope is zero to within the noise on
both ICDs, at every QP from 63 (0.10 MB of payload) to 12 (2.87 MB) — a 28x
range in payload that moves Pass B by less than 3 %. That is not an artefact
of the measurement: the coefficient buffer between the passes is **dense**, so
Pass B reads the same 25.6 MB whatever the stream said, and the wavefront runs
its 22 steps per plane whether the blocks it is stepping over carry
coefficients or not.

Two consequences worth stating plainly:

* **The tile is the unit of cost, not the byte.** 906 ns per tile on a 7900 XTX
  is what a 64x64 tile costs to reconstruct at QP 24 and at QP 63 alike. Larger
  tiles would amortise the fixed part over more pixels, and the sparse
  coefficient layout of PAPER 3.2.5 would attack the other end by making the
  25.6 MB scale with the content. They are independent levers and the second
  one is much the larger.
* **Pass A's intercept is small** — 121 ns per tile, 13 % of Pass B's — so the
  per-tile overhead the entropy decoder pays for tile independence is not what
  is expensive. Tile independence is cheap; the wavefront inside a tile is not.

### The `INTRA_DIR` wavefront, priced

2048 tiles, 4:2:0, QP 24, best of 20, Pass B only. `docs/SYNTAX.md` 7.6 prices
these three schedules in **rate**; this prices them in **time**. Barriers are
counted off the kernel, not off the idealised schedule, so they include the
DC-plane and transform barriers each plane pays anyway.

| schedule | steps | barriers/tile | occupancy | rate | Pass B, RADV | Pass B, lavapipe |
|---|---|---|---|---|---|---|
| `INTRA_DIR` off (v1) | — | 23 | 100 % | — | **0.236 ms** | 40.5 ms |
| 0 — as written | 22 | 62 | 4.5 % | — | **1.723 ms** (7.3x) | 139.8 ms |
| 1 — no above-right | 15 | 49 | 6.7 % | +0.24 % | **1.293 ms** (5.5x) | 118.3 ms |
| 3 — no above-right + 32x32 | 7 | 41 | 14.3 % | +1.8 % | **1.014 ms** (4.3x) | 114.6 ms |

The headline is the first row against the second: **directional intra costs
7.3x on Pass B**, +1.49 ms per frame on a 7900 XTX, for the 22.5 BD-rate
points `ref/RESULTS-intra.md` measures. Nothing else in the decoder moved.

RADV shader statistics (`RADV_DEBUG=shaderstats`, RDNA3, wave64) for the Pass B
pipeline, 4:4:4 store:

| | Pass A | Pass B |
|---|---|---|
| SGPRs / VGPRs | 108 / 48 | 108 / **144** |
| spilled | 0 / 0 | **0 / 0** |
| LDS | 10.2 KB | 25.6 KB |
| code size | 16 KB | 150 KB |

Nothing spills, which is the thing that could have made this much worse: the
`A[17]`/`L[17]` reference arrays of 7.4 are what take Pass B to 144 VGPRs, and
144 is the last step before RDNA3 drops from 4 waves per SIMD to 3. The
register footprint is **identical for all three schedules** — they change the
loop trip count, not the working set — and it is also identical for a stream
with `INTRA_DIR` off, because the tool is a push-constant branch inside one
shader rather than a second pipeline. A v1 stream therefore pays the occupancy
cost of the wider register file even though it never enters the wavefront;
that is visible as nothing at all in the table above (0.236 ms against the
0.26 ms this document reported before directional intra existed), so it has
not been worth splitting the shader in two.

### Grouping Pass B's workgroups by tile shape

Pass A has always issued one dispatch per distinct rANS lane count. The same
idea one level up — sort the workgroup-to-tile map so that adjacent workgroups
decode tiles of like shape (mode, `res_level`, `chroma444`, `alpha_mode`,
`tskip`) — is `nxvc_vk_decoder_set_tile_sort()`. It is host-side reordering
only; every write address in the kernel comes from the tile index the map
yields, so the decoded image is bit-identical either way, and that is checked
against all 44 vectors.

Measured on a deliberately mixed frame (2048 tiles, cycling `res_level` 0/1/2
and the encoder's own per-tile transform-skip decision), Pass B only:

| | RADV | lavapipe |
|---|---|---|
| raster order | 1.772 ms | 124.2 ms |
| sorted by shape | **1.556 ms** | 139.6 ms |
| | **-12.2 %** | **+12.4 %** |

It is worth 12 % on a real GPU and costs 12 % on llvmpipe, which is the result
one would predict: on RADV neighbouring workgroups share a shader engine and
converge, while llvmpipe's "workgroups" are CPU tasks that gain nothing from
converging and lose the output image's write locality. It is **off by default**
for that reason — a 12 % win on one ICD does not justify a 12 % loss on
another until the shipping target is measured — and the switch is there so the
Android client can turn it on once the Adreno number exists.

A uniform frame gains nothing from it, and a VR stream at a steady operating
point is close to uniform, so 12 % is the top of the range rather than the
middle of it.

### Memory traffic

The coefficient SSBO is the decoder's traffic budget. At `res_level` 0 a
4:2:0 tile slot is 6240 int16 (luma 64 DC + 64 blocks x 64 = 4160, plus 1040
for each of the two chroma planes), so a 2048-tile frame is **25.6 MB**
written by Pass A and read back by Pass B — about 51 MB of device traffic per
frame, which at 90 Hz is 4.6 GB/s. 4:4:4 doubles the slot to 12480 int16 and
the frame to 51.1 MB. PAPER 3.2.5's 16.8 MB estimate counts the luma plane
only. The sparse coefficient layout named there as the first optimisation
would cut all of these by roughly 4x at typical QP; nothing in the C ABI
changes if it lands, because the layout is entirely between the two passes.
The fit above says this is now the *only* lever on Pass B that scales with the
content, because Pass B's cost is otherwise entirely fixed per tile.

The v3 intra modes add one more inter-pass buffer, and it is small: **32 uints
per tile**, one 4-bit field per 8x8 block, 8 fields to a word, each plane's
region starting on a word boundary. That is 128 B against the coefficient
slot's 12.5 KB — 0.26 MB for a 2048-tile frame, 1 % of the coefficient
traffic.

The other per-frame traffic is small: the bitstream itself (0.2–2.8 MB), 16
uints of CBF bits and one status word per tile, 8 KB of probability tables
(16 contexts since `CTX_V2`, up from 6 KB) and 2 KB of weighting matrices.

---

## Conformance

`vk.decoder.conformance` is the exit criterion of PAPER 3.11 — **zero**
mismatching samples against the CPU reference. It checks, on whatever ICD the
environment selects:

1. every `tests/vectors/v*.nxv`, twice: against the `decoded_md5` pinned in
   `tests/vectors/vectors.md5` (the normative answer, independent of what
   `ref/` compiles to today) and pixel-for-pixel against an in-process
   `nxvc_ref` decode, so a failure names the first differing sample;
2. every `tests/vectors/r*.nxv` rejection vector, which must be refused with
   exactly the status `tests/vectors/rejects.md5` names;
3. a synthetic sweep encoded on the spot with `nxvc_encoder` over QP 0..63,
   `res_level` patterns, 4:2:0 and 4:4:4, YCoCg-R and passthrough, transform
   skip, lane counts 1/2/8/32 and the encoder's own choice, custom
   probability tables, all four quantization matrices, per-tile `wm_id`
   overrides, 4:2:0 tiles inside a 4:4:4 stream, alpha, lossless, per-tile QP
   maps, odd and extreme picture sizes and a multi-frame stream — with every
   4:4:4 case also run through the RGB10A2 store. Since v1.3 the encoder's
   default has `INTRA_DIR`, `CTX_V2` and `SIGN_HIDE` on, so every case above
   exercises all three; twelve further cases walk the combinations the other
   way — each tool alone, all three off, and the layered form — against
   `res_level` cycling and transform skip, so "additive, and off unless the
   bit is set" is checked on synthetic content as well as on the vectors.

### Results

| ICD | streams | mismatching samples |
|---|---|---|
| RX 7900 XTX (RADV NAVI31) | 152 checked, 23 skipped | **0** |
| llvmpipe (lavapipe) | 152 checked, 23 skipped | **0** |

The 152 are the 44 Phase 1 vectors, the 18 rejection vectors whose refusal this
decoder is responsible for, and 64 synthetic streams of which 26 are re-run
through the RGB10A2 store. The 23 skips are the Phase 2 inter vectors
(`v45`–`v56`) and the rejection vectors that are malformed *inside* the inter
syntax (`r18`–`r29`): a Phase 1 decoder refuses those at the tool mask, earlier
and with a different but equally correct status, so the harness requires the
refusal and does not require the manifest's exact status. The skip is decided
from each stream's own `tools` field, never from its file name, so a
regression that starts refusing a Phase 1 vector still fails.

The 44 vectors include `v36`–`v44`, which pin the v1.3 intra tools:
`INTRA_DIR` alone in 4:4:4 and 4:2:0, `INTRA_DIR` with `CTX_V2`, `CTX_V2`
alone, the layered form, all of it at once with transmitted 160-byte table
sets, the combination with `res_level` cycling and transform skip,
`SIGN_HIDE` alone, and the reference encoder's shipped default. The 17
rejection vectors include `r14` (`flags` bit 2 without tool bit 17), `r15`
(YCoCg-R declared with 4:2:0 chroma), `r16` (a `CTX_V2` table set that
overruns the tile rows) and `r17` (`LOSSLESS` together with `SIGN_HIDE`).

**The three restricted wavefronts are checked the same way.** `ref/` built
with `-DNXVC_DIR_SCHED_EXPERIMENT` emits streams under restrictions 1, 2 and 3
of `docs/SYNTAX.md` 7.6; decoded with `nxvc-vkdec --dir-sched N` at the
matching `N`, the GPU output is byte-identical to `nxv-dec`'s, at QP 8 and
QP 24, in both the replace and the layered form — twelve stream/schedule pairs,
zero differing bytes — and a schedule-0 stream decoded at schedule 1 differs,
which is the check that the specialization constant is doing anything at all.

`vk.decoder.cli` additionally checks that `nxvc-vkdec` is byte-identical to
`nxv-dec` over every vector, which is the contract `tools/quality` relies on.

```sh
cmake -S . -B build-vkdec -DNXWARP_BUILD_VK=ON \
      -DNXVC_LAVAPIPE_ICD=/path/to/lvp_icd.x86_64.json
cmake --build build-vkdec -j4
ctest --test-dir build-vkdec -R '^vk\.decoder\.'
```

| test | what |
|---|---|
| `vk.decoder.conformance` | the sweep on the default device |
| `vk.decoder.conformance_radv` | pinned to RADV |
| `vk.decoder.conformance_lavapipe` | pinned to lavapipe, `VK_DRIVER_FILES` from `-DNXVC_LAVAPIPE_ICD` |
| `vk.decoder.bench` | the timing table above, the wavefront variants, the fixed/per-byte fit and the tile-sort delta |
| `vk.decoder.cli` | `nxvc-vkdec` vs `nxv-dec`, byte for byte |

Everything exits 77 — a ctest skip — when there is no usable ICD.

The harness binary takes `--quick`, `--verbose`, `--only-vectors`,
`--only-synthetic`, `--vectors DIR` and `--bench [iters]`, and honours
`NXVC_VKD_DEVICE` as a device-name substring.

---

## The CLI

```sh
nxvc-vkdec --in file.nxv --out out.yuv [--icd PATH] [--device SUBSTR]
           [--pix yuv420p|yuv444p] [--frames N] [--nv12] [--stats]
           [--format auto|rgba8|rgb10a2|ycbcr420] [--lds] [--quiet]
           [--dir-sched 0..3] [--tile-sort]
```

Every option `nxv-dec` takes means the same thing here, and the output bytes
are identical, so `tools/quality` can drive either. `--dir-sched` and
`--tile-sort` are the two measurement knobs of the C ABI and have no `nxv-dec`
counterpart; `--dir-sched` other than 0 changes the pixels, so `tools/quality`
must not pass it. The build also drops
`nxvgpu-dec` and `nxvgpu-enc` symlinks next to the binaries so the harness'
`--codec-cmd <prefix>` form works directly:

```sh
python3 tools/quality/compare.py --seq ... \
    --codec-cmd build-vkdec/bin/nxvgpu --anchors x264-intra
```

Exit 0 decoded, 1 error, 2 usage, 77 no usable Vulkan ICD.

---

## Android

Nothing special: plain C++20 over the core Vulkan 1.2 API, and the NDK
supplies both `glslc` (in `shader-tools/`) and `libvulkan`, so arm64 is a
normal cross build of the same target. `android/` links it.

```sh
NDK=$ANDROID_SDK/ndk/<version>
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DNXWARP_BUILD_VK=ON -DNXWARP_BUILD_TESTS=OFF \
  -DNXWARP_BUILD_EXAMPLES=OFF -DNXWARP_BUILD_TRANSPORT=OFF \
  -DNXWARP_BUILD_TOOLS=OFF
cmake --build build-android --target nxvc_vk_decoder
```

Builds warning-clean for `arm64-v8a` against NDK 29 (API 29). The client
should adopt its own `VkDevice` rather than let the library create one, and
`NXVC_VKD_OUT_AUTO` then resolves to the two-plane 4:2:0 store the
reprojection shader already samples.

---

## Relationship to `vk/common`

The Vulkan boilerplate in `nxvc_vkdec.cpp` — instance and device creation, the
buffer and image allocator, the pipeline cache — is deliberately minimal and
self-contained, the same shape the two pass harnesses already carry. When
`vk/common`'s `nxvc_vk_context_create()` / `nxvc_vk_context_adopt()` and the
`nxvc::vk::Buffer` / `Pipeline` helpers settle, those three pieces are what
should be deleted in favour of them; the decode path itself does not change,
and neither does the C ABI. `vk/common` owns the `nxvc_vk_*` **type** prefix
(`<nxvc/vk/nxvc_vk.h>`); this library's types are `nxvc_vkd_*` and its entry
points `nxvc_vk_decoder_*`, with the one-shot call spelled
`nxvc_vk_decode_frame`, so the two headers coexist.

---

## Edits made inside `passA/` and `passB/`

The two kernels landed verified against the spec as it stood then. Integration
needed five changes, each marked in place with
`[nxvc_vk_decoder glue, marked edit]`:

| where | change | why |
|---|---|---|
| `passA/rans_decode.comp`, `syntax_constants.h`, `passA_model.{h,cpp}` | `LANES` becomes specialisation constant 2; the workgroup shape follows `nxs_tiles_per_group()`; `active = min(LANES, nunits)` | `nsub_log2` is a free per-tile field, and vectors v24/v25/v26 use 1, 2 and 32 lanes. `ref/src/entropy.cpp decode_units()` initialises only `active` rANS states |
| `passA/rans_decode.comp` | the coefficient and CBF bases come from the tile descriptor, not `tile * stride` | so one dispatch can cover an arbitrary subset of the frame's tiles, which is how the lane-count grouping works. The fields already existed and already carried these values |
| `passA/syntax_constants.h` | reserved bits of tile word1 are 28-31, not 26-31 | bits 26-27 are now `wm_id` |
| `passB/*` | the DC plane is quantized at `qp >> 1`, not `qp - 6` | `ref/src/codec.cpp dc_qp_of()` and `docs/SYNTAX.md` 7.1. The old rule decoded **every** vector wrong; `vk.passB.ref_conformance` carried its own copy of the stale rule, which is why it never caught it |
| `passB/*` | `(P ± Q) * C4` uses the exact two-word product `nxvw_mul_c4_rnd9()` | `docs/SYNTAX.md` 6.3: dequantized coefficients are int16-clamped, so `\|P ± Q\| ≤ 8.62e7` and `8.62e7 * 362` is outside int32. `ref/src/transform.cpp mul_c4_rnd9()` does the same. Without it every stream whose coefficients saturate decodes differently — `tests/vectors/v35_saturate420` is exactly that stream |
| `passB/*` | the weights SSBO holds four 128-entry sets, indexed by the tile's `wm_id` | `wm_id` is a per-tile override of the frame's weighting matrix (`docs/SYNTAX.md` 4.1, tool bit `WM_ID`, vectors v33/v34) |

### Realigning to bitstream minor version 3

| where | change | why |
|---|---|---|
| `passA/syntax_constants.h` | `kNumCtx` becomes 16 and the four v2 contexts are named; `kCtxNone` arrives as the "no context selected" sentinel | `docs/SYNTAX.md` 9.3 / `ref/src/common.h`. The cumulative-frequency table is always uploaded at the 16-context stride so the host has one layout whichever model the stream picks; contexts past the coded count carry defaults and are never selected, which is what `build_default_set()` does |
| `passA/rans_decode.comp`, `passA_model.cpp` | a unit's LEVEL context is a field, not a derivation: `kCtxNone` means the banded contexts of 9.3, anything else is used as-is | the DC plane's LEVEL context under `CTX_V2` is a single context with no banding, so the derivation had to become a fallback rather than the rule |
| `passA/*` | one **mode unit** per coded plane between the DC unit and the block units, MPM-coded | `docs/SYNTAX.md` 9.1 / 9.6. The modes are a unit rather than a symbol on each block precisely so the MPM only ever reads values the same lane has already produced, whatever the interleaved schedule does — which is why the kernel can read them straight back out of the output buffer with no atomic |
| `passA/*` | binding 6: 32 uints per tile of packed 4-bit intra modes | the second thing Pass A produces for Pass B. Each plane's 64-slot region starts on a word boundary, so the single lane that owns a plane's mode unit is the only thread that touches those words |
| `passA/*` | sign data hiding: the magnitude at scan position `LAST` is stored positive and negated at the end of the unit if the sum of magnitudes is odd | `docs/SYNTAX.md` 9.7. The kernel keeps the magnitude in a register rather than reading the coefficient back, so binding 3 stays write-only |
| `passA/syntax_constants.h` | `kTileDescUints` 4 → 8 | the descriptor gained the mode-region offset and was padded to a power of two so the shader addresses it with a shift |
| `passB/*` | the nine directional predictors of 7.4 and the reference construction of `at()` | `docs/SYNTAX.md` 7.4. References clamp **into the tile**, so a tile still never reads a neighbour: its top and left borders read its own DC-plane prediction. Tile independence, which the transport's per-tile loss recovery depends on, is unchanged |
| `passB/reconstruct.comp` | the wavefront, as specialization constant 2 | see "The `INTRA_DIR` wavefront, priced". The shared sample store holds the running reconstruction during the wavefront; in the layered form it holds the reconstructed DC-plane residual and is converted back with one pass of `sample = pred + recon`, which is exact because `recon` was formed as `clamp(pred + v) - pred` |
| `passB/reconstruct.comp` | binding 8, a workgroup-to-tile map, and a 1D dispatch | so the host can group like-shaped tiles without any output address depending on the workgroup index |
| host | `LOSSLESS` + `SIGN_HIDE`, and YCoCg-R with 4:2:0 chroma, are refused | `r17` and `r15`. The second was a check the reference made and this decoder did not; it is the one place the v3 realignment found an existing conformance gap rather than adding a feature |

The Pass A and Pass B CPU models track their kernels line for line, as before,
and `vk.passA.*` and `vk.passB.*` stay green on RADV and lavapipe.

---

## Which wavefront should v1 adopt

`docs/SYNTAX.md` 7.6 says the choice between the three schedules should be made
"against a *measured* Pass B barrier cost on the target part, and that number
does not exist yet". Half of it exists now. This section is the recommendation
that follows from it, with the assumption it rests on stated first, because the
assumption is doing a lot of work.

**Assumption.** No Adreno 650 was measured. Everything below scales the 7900
XTX numbers by **20–30x**, the range the brief gives for that part. That is a
throughput ratio applied to a kernel whose problem is *serialization*, and a
serialization-bound kernel usually scales worse than throughput, not better —
so 20–30x is a floor, not a bracket. Treat every Adreno figure here as
optimistic.

Measured, Pass B only, 2048 tiles at QP 24, and the same scaled to a 90 Hz
frame budget of 11.1 ms:

| schedule | rate | RADV | at 20x | at 30x |
|---|---|---|---|---|
| `INTRA_DIR` off | -22.5 pts worse | 0.24 ms | 4.7 ms | 7.1 ms |
| 0 — as written | — | 1.72 ms | 34.5 ms | 51.7 ms |
| 1 — no above-right | +0.24 % | 1.29 ms | 25.9 ms | 38.8 ms |
| 3 — no above-right + 32x32 | +1.8 % | 1.01 ms | 20.3 ms | 30.4 ms |

### The recommendation: adopt restriction A, and only A

**Narrow `INTRA_DIR` to drop the above-right reference (schedule 1) and make
that the v1 derivation.** Two reasons, one of them the interesting one.

*The trade is ten times better than the next one.* Restriction A buys 0.43 ms
of Pass B for 0.24 % of rate — **1.8 ms of decode per percent of rate**. Adding
the 32x32 sub-tile restriction buys a further 0.28 ms for a further 1.56 % —
**0.18 ms per percent**, a tenth as good. The first restriction is where the
whole of the return is, which is exactly what 7.6 predicted from the rate side
("essentially free — a quarter of a percent for a third of the barriers") and
is now true from the time side as well. There is no operating point at which
you would want B without already having taken A.

*And the extra 0.28 ms does not rescue anything.* This is the part the
measurement settles that the estimate could not. At 2048 tiles and 90 Hz,
**no variant fits an Adreno 650** — the cheapest is 20.3 ms against an 11.1 ms
budget for Pass B alone, before Pass A, and Pass A at QP 24 is another 24–35 ms
scaled. Directional intra is not 20 % too expensive on that part; it is 4–8x
too expensive, and a 1.8 %-rate schedule change moves it from 4.6x over to 3.7x
over. Paying rate to close a gap you are not going to close is the wrong
purchase. The one corner where it changes an answer is a *single* eye at 20x
(10.1 ms for schedule 3 against 12.9 ms for schedule 1) — and a corner that
narrow should be decided on a real Adreno measurement, not on a 20–30x guess.

So: take the free restriction because it is free, and do not take the priced
one until someone has run Pass B on the actual part.

### What to do about the headset, which is a different question

The schedule choice is not what makes `INTRA_DIR` affordable on Adreno.
Nothing in this menu does. The honest reading of the table is that **tool bit
17 is a desktop-decoder tool**, and the syntax already has the mechanism for
that: capability negotiation is an intersection (`docs/SYNTAX.md` 2.3), so a
headset that does not offer bit 17 gets v1.2 streams and the sender keeps the
22.5 points wherever the decoder is a discrete GPU. That costs nothing to
specify because it is already specified.

If `INTRA_DIR` on Adreno is a hard requirement, the lever is not the schedule.
It is one of:

* **Restriction C, the 16x16 super-block**, which 7.6 models at 4 steps
  combined with 32x32 sub-tiles and leaves unpriced. It is the only entry in
  the menu that changes the *prediction distance* rather than the dependency
  graph, so its rate cost genuinely cannot be extrapolated from the others —
  but 4 steps against 22 is the only remaining order-of-magnitude in the
  wavefront, and this document's measurement is the argument for finally
  pricing it.
* **Fewer threads per block.** The occupancy numbers in 7.6 and here are
  4 threads per 8x8 block against a 256-thread workgroup. Nothing forces that
  ratio during the prediction step; 16 threads per block would quadruple
  occupancy at every schedule without touching the bitstream at all. That is a
  Pass B change, not a `SYNTAX.md` change, and it should be tried before any
  further rate is spent.

Both of those are cheaper than 1.8 % of rate, and the second one is free.

---

## Open issues

* **`vk/common` is not used yet.** See above; the swap is mechanical and does
  not touch the C ABI.
* **One frame in flight.** The decoder owns a single command buffer and
  submits one frame at a time. A ring of command buffers and staging buffers
  is the obvious next step for the streaming client, and the timeline
  semaphore is already the synchronisation point it needs.
* **Sparse coefficients.** 25.6 MB of coefficient traffic per 2048-tile 4:2:0
  frame is the single largest number in the decoder, and most of it is zeros
  at any usable QP. PAPER 3.2.5's sparse layout is entirely a Pass A ↔ Pass B
  affair.
* **The bitstream buffer grows but never shrinks,** and growing it recreates
  the buffer mid-stream. Harmless for a stream of similar frames; worth a
  high-water-mark policy if a client ever sees one huge IDR.
* **Alpha on the two-plane path costs a second Pass B dispatch.** The
  two-plane 4:2:0 store has nowhere to put alpha, so a 4:2:0 stream carrying
  one is reconstructed twice — once for Y/Cb/Cr and once in the RGBA8 format
  whose A channel is the alpha plane. Conformant and rare (VR streams do not
  carry alpha), but an `r8ui` alpha binding on the two-plane path would be
  cheaper if it ever stops being rare.
* **`profile` and `level` in the stream header are carried and reported but
  not enforced.** The reference does not enforce them either.
* **Pass B is one shader for both predictors.** `INTRA_DIR` is a push-constant
  branch, so a v1 stream pays the 144-VGPR footprint of the wavefront's
  reference arrays even though it never enters them. It costs nothing
  measurable today (0.236 ms against the 0.26 ms of the pre-v1.3 kernel), but
  it is the first thing to check if Pass B ever gets tighter.
* **Four threads per 8x8 block during the wavefront.** That is what caps
  occupancy at 4.5 %; it is inherited from the transform's thread mapping,
  where it is the right ratio, and nothing requires the prediction step to keep
  it. See "Which wavefront should v1 adopt".
* **Tile sorting is off by default** because it is worth -12 % on RADV and
  +12 % on lavapipe. It needs the Adreno number before it can have a sensible
  default.

### Where this decoder sits against the Phase 2 syntax

`docs/SYNTAX.md` grew section 13 and `tests/vectors/v45`–`v56`, `r18`–`r29`
while this work was in flight. Two bit allocations were briefly in conflict —
`spec/annex-d-inter-decisions.md` D-1 wanted frame `flags` bit 2 for
`warp_present`, which v1.3 spends on the layered form of `INTRA_DIR` (`v40`,
`r14`), and D-5 wanted tool bit 20 for Catmull-Rom, which v1.3 spends on
`WM_ID` (`v33`, `v34`, `r11`). **Both were resolved the other way** before
landing (Appendix A decision 52): `warp_present` is frame `flags` **bit 3** and
`FILTER_CATMULL_ROM` is tool bit **23**. Nothing this decoder implements moved,
and the inter prose went into a new section 13 rather than displacing section
8, so every section reference in this directory is still valid.

The rest of Phase 2 does not disturb the intra decoder either: `warp_ext()` is
a `36 * eyes` frame-header extension gated on a flag bit that requires a tool
bit this decoder refuses, the reference ring and the per-tile prediction state
are Pass B state that does not exist yet, and the 16-bit STEREO disparity lands
in the tile's optional-byte area, which is the one place
`nxs_tile_payload_offset()` already knows is variable length. When the inter
path does land, tool bit 23 is the one to watch: it changes the *resampling
filter*, which is `passB/`'s `planeAtFull()` and `bilinearMeans()`, not the
predictor.

**One disagreement is open and this decoder follows `ref/`.** A `skip_bitmap`
bit *at or above* the row's tile count is `BITSTREAM` in both documents and in
both implementations (`r08`). A bit *within* range on a stream with no `INTER`
tool bit is `UNSUPPORTED` per `docs/SYNTAX.md` 12 and `BITSTREAM` per
`ref/src/codec_impl.inc`. No vector pins it, so either passes today;
`nxvc-vkdec` returns `BITSTREAM`, because the contract this decoder is held to
is that it returns exactly what `nxv-dec` returns.
