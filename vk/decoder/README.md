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
| `passB/` | reconstruction: dequantize, inverse transform, DC-plane intra prediction, resample, colour → display image. `passB/README.md` |
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
with the same named status (`tests/vectors/rejects.md5` pins both).

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
transmitted. v1 has no reference frames, so the CPU reference refuses a
non-zero bitmap outright (`NXVC_ERR_UNSUPPORTED`, "no references in v1") and
so does this decoder by default — `nxv-dec` and `nxvc-vkdec` agree byte for
byte on `tests/vectors/r08_skip_bitmap.nxv`.

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

**RX 7900 XTX (RADV NAVI31)**

| QP | frame bytes | Pass A | Pass B | GPU total | wall |
|---|---|---|---|---|---|
| 12 | 2.79 MB | 1.93 ms | 0.26 ms | **2.19 ms** | 2.88 ms |
| 24 | 1.31 MB | 1.40 ms | 0.26 ms | **1.67 ms** | 2.10 ms |
| 36 | 0.17 MB | 0.46 ms | 0.33 ms | **0.80 ms** | 1.07 ms |

Host parse is 0.01–0.15 ms and command-buffer recording plus submit is
0.02–0.24 ms; the gap between "GPU total" and "wall" is the upload copy and
the fence wait. Pass A dominates and scales with the symbol rate, which is why
the table is a bitrate ladder rather than one number — Pass B is essentially
flat because it does the same work per tile whatever the coefficients are.

**llvmpipe (lavapipe, 32-thread CPU)**, best of 5, for scale: 150.6 ms at QP
12, 113.3 ms at QP 24, 55.5 ms at QP 36. It is a conformance oracle, not a
performance target.

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

The other per-frame traffic is small: the bitstream itself (0.2–2.8 MB), 16
uints of CBF bits and one status word per tile, 6 KB of probability tables and
2 KB of weighting matrices.

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
   4:4:4 case also run through the RGB10A2 store.

### Results

| ICD | streams | mismatching samples |
|---|---|---|
| RX 7900 XTX (RADV NAVI31) | 121 (35 vectors + 13 rejections + 73 synthetic) | **0** |
| llvmpipe (lavapipe) | 121 | **0** |

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
| `vk.decoder.bench` | the timing table above |
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
```

Every option `nxv-dec` takes means the same thing here, and the output bytes
are identical, so `tools/quality` can drive either. The build also drops
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

The Pass A and Pass B CPU models track their kernels line for line, as before,
and `vk.passA.*` and `vk.passB.*` stay green on RADV and lavapipe.

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
