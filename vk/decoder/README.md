# NX Warp Vulkan decoder

`nxvc_vk_decoder` turns an `.nxv` byte stream into decoded images on a Vulkan
compute queue. An intra frame costs **two dispatches** (PAPER 3.2.1); an inter
frame costs three, because the pose-warp predictor is its own kernel:

```
   bytes ──► host parse ──► Pass A ──►[Pass W]──► Pass B ──► output image(s)
             (this dir)     passA/     inter/     passB/  └► reference ring
```

| directory | role |
|---|---|
| `passA/` | interleaved rANS entropy decode: tile payloads → int16 coefficients + CBF bits. `passA/README.md` |
| `inter/` | Pass W, the Phase 2 pose-warp predictor, the reference ring and the per-tile prediction state. "The inter path" below |
| `passB/` | reconstruction: dequantize, inverse transform, DC-plane and directional intra prediction, the inter prediction hook, resample, colour → display image. `passB/README.md` |
| this directory | the container parse, the Vulkan runtime around the three kernels, the C ABI and the CLI |

The normative specification is `docs/SYNTAX.md` and the CPU reference in
`ref/`. This decoder reproduces `nxv-dec` output **bit for bit**; where it
cannot, it refuses the stream rather than guessing.

| file | role |
|---|---|
| `../../include/nxvc/nxvc_vk.h` | the C ABI |
| `nxvc_vkdec_parse.{h,cpp}` | host-side container parse, Vulkan-free |
| `inter/inter_layout.h` | the one description of every buffer the inter path uses, shared by the kernels, the CPU model and the host |
| `inter/inter_state.h` | the prediction history, the ring bookkeeping and the missing-tile map, Vulkan-free |
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
/* optional, before a frame whose tiles the client did not all receive:      */
nxvc_vk_decoder_mark_missing(dec, tile_ids, count);   /* SYNTAX.md 13.6      */
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
* **The tool mask.** `nxvc_vk_decoder_tools_supported()` returns the bits this
  decoder implements — the decoder's half of the `docs/SYNTAX.md` 2.3
  handshake, and exactly the mask a stream's `tools` field must be a subset of.
  It is a property of the build, so it takes no handle and may be called before
  `nxvc_vk_decoder_create()`. `nxvc_vkd_stream_info::tools` is the other end:
  what a given stream asks for. Several tools are specified as *negotiated*
  rather than defaulted — `ENTROPY_LITE` above all, which buys Pass A time with
  bits and whose worth only the decoder can judge — and until this call existed
  a caller had to try a stream and read the refusal. The conformance harness
  asks it rather than restating the mask, because a copy of a tool mask is a
  copy that goes stale.
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
| `CTX_V3` | tool bit 25 | 27 coded contexts, the `kDefaultFreqV3` family, 270-byte table sets, and the per-lane neighbour class | — |
| `TAB_V2` | tool bit 26 | — (the upload is the same either way) | — |
| `XFORM_4X4_SPLIT` | tool bit 19 | a split flag per coded block unit, and the split scan | the 4x4 inverse transform |
| `INTRA_CFL` | tool bit 24 | a ten-mode alphabet on chroma mode units | the chroma-from-luma predictor |
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

## The inter path

`docs/SYNTAX.md` 13. Everything in sections 6 to 9 is unchanged by inter
prediction: the transform, the quantiser, the scan, the contexts and the rANS
schedule are the same for every mode. What changes is what the residual is
measured against, and that is the whole of what `inter/` is.

```
                        ┌──────────────── reference ring, 4 slots ───────────┐
                        │  coded sample domain (Y/Co/Cg), full tile extent   │
                        └───▲───────────────────────────────────────┬───────-┘
                            │ written per tile by Pass B            │ read by Pass W
   Pass A ──coefficients──► │                                       ▼
                            └──────────── Pass B ◄──WPred──── Pass W
```

### Pass W, and why it is a third dispatch and not a stage of Pass B

`inter/warp_pred.comp` is one workgroup per tile, 256 invocations. For every
coded plane of an inter tile it derives four corner source coordinates, walks
the tile's samples through the corner interpolation and one bilinear tap of the
reference, box-averages the result down to the tile's coded extent, folds in a
near-skip tile's mean field, and writes the samples to the **WPred** buffer as
i16. Pass B's prediction hook reads one of them per sample.

It could have been a stage of `reconstruct.comp`, and it is not, for a reason
that is about the file rather than about the GPU: `reconstruct.comp` is the
intra path's kernel, it is 1450 lines of scheduling that three drivers agree
on, and the inter path does not get to restructure it. The whole of what the
inter path adds to it is **seven places, five of them one line**:

* one `#include` of `inter/inter_hook.glsl`, after the shared-memory
  declarations because it reads `sPlane`;
* the **prediction hook** at the `INTER HOOK` marker, which is one `if`:
  `pred` is the DC plane's planar interpolation for an intra tile and
  `clamp(W + planar(M) - dc_offset, 0, maxval)` for an inter one;
* three lines — `nxvwIsInterTile` at the `bool intra` marker, and the two
  places the DC plane clamps a block mean — because 13.3 says an inter tile's
  block means are **not** clamped to the sample domain: they are
  `dc_offset + a residual mean`, whose range is wider on both sides, and
  clamping them would cap the DC correction the warp needs on exactly the
  tiles where it matters most;
* one line in the unit numbering, because 13.3 also says the **mode unit** of
  9.6 is present only for `mode == INTRA` ("an inter tile's prediction is the
  warp, and nine ways of saying nothing is not a tool"), which makes
  `INTRA_DIR`'s effect on the unit list a property of the tile rather than of
  the frame — in Pass A as well;
* one call to `nxvwRefRingStore()` at the end of `main()`.

Everything else — the predictor, the ring, the near-skip mean field, the
parse, the host state — is in `inter/`.

### The predictor is `nxvc_warp_ref` and nothing else

`warp_pred.comp`'s corner derivation, its two-step corner interpolation and its
bilinear tap are a **line-for-line copy** of `warp/glsl/warp_tile.comp`, which
is itself the line-for-line GLSL twin of `warp/ref/warp_ref.cpp`, the normative
implementation. The emulated 64-bit accumulator, the fixed 32-iteration
restoring divide, the saturating add, the modular shift, the `(c + 2) >> 2`
Q.6 → Q.4 step: all of it is copied rather than re-derived, because a second
derivation of the same arithmetic tests the derivation and not the kernel.

The CPU model, `inter/inter_model.cpp`, goes the other way and calls
`nxvc::warp::warp_tile_quad()` **directly**, exactly as `ref/src/inter.cpp`
does. What it models is what the kernel adds around the library: the plane
loop, the conjugated matrix, the halved vectors, the ring addressing, the
`res_level` box average and the near-skip field. A model that copied the
arithmetic a third time would agree with the kernel for the wrong reason.

That asymmetry is what makes `vk.passW.gpu_vs_cpu` worth running: it compares
the GPU's emulated 64-bit restoring divide against the normative library's
over whatever matrices the sweep produces, which is a property the sixteen
end-to-end vectors can only test at their sixteen points.

| ICD | scenes | differing samples |
|---|---|---|
| RX 7900 XTX (RADV NAVI31) | 57 | **0** |
| llvmpipe (lavapipe) | 57 | **0** |
| Adreno 650 (Pico 4) | 57 | **0** |

19 configurations x 3 seeds: identity and warped matrices, one kind sitting
just inside the denominator envelope, 4:2:0 and 4:4:4, YCoCg-R, alpha, a
`res_level` cycle, quadrant vectors, near-skip records, stereo, pictures whose
width and height are *not* multiples of 64 so the clamp-to-edge fetch is
exercised, and tiles naming a reference the decoder does not hold — the
"leave mid-grey" arm, which no conformance vector reaches.

Four things the kernel does that the library does not:

* **The conjugated matrix** (13.3 step 1) is computed on the host, once per
  frame, for the two subsampling factors of each eye: four matrices of nine
  integers. The halving of `h02`/`h12` rounds to nearest, ties away from zero;
  the doubling of `h20`/`h21` is exact.
* **The plane's vector** is `mv >> 1` for `sub == 2`, an arithmetic shift.
  A quadrant vector (13.10) is a *delta*, so the shift is applied to the sum.
* **The corner basis is fitted over 64 samples whatever the plane's extent
  is.** That is 13.7's chroma caveat as code: `warp_tile()` emits a fixed
  64×64 block and a 4:2:0 chroma tile takes its top-left 32×32, so a chroma
  sample is interpolated in a basis spanning 64 chroma samples and not 32.
  Both sides of the codec do this, so it is exact; a decoder that re-fitted
  the basis at 32×32 would produce different samples and pass no vector.
* **The quadrant split is the plane's own half extent**, 32 luma samples and
  16 chroma. The vector enters as a per-sample constant *after* the corner
  interpolation, so four equal quadrant vectors are bit-identical to a
  single-vector tile — which is why the kernel has one sample loop and not
  two, and why quadrant vectors cost one extra select per sample and nothing
  else.

### The reference ring

Four slots addressed by `frame_number mod 4` (13.2), one buffer, u16 samples
packed two per uint. A slot holds every eye's whole reconstructed picture in
the **coded sample domain** — Y/Co/Cg before the inverse colour transform,
never the RGB the output image carries — because that is the domain the
predictor predicts in. It is written per tile by Pass B as a second store of
the same samples, which is the shape `kOutSecond` already established, rather
than by a second reconstruction.

Two details are load-bearing:

* **The row stride is padded to an even number of samples.** A store writes a
  whole uint — two horizontally adjacent samples — so every tile's x origin
  and every row start has to be even, or two workgroups would read-modify-write
  one uint. A tile's origin is `eye * plane_width + col * extent` and `extent`
  is 64 or 32, so it is even whenever `eye * plane_width` is; padding the
  stride makes the rows agree.
* **A `STEREO` tile reads eye 0 of the slot being written**, this frame's own.
  On a serial decoder 3.3's row order (row-major, eye-minor) is what makes
  that available. A dispatch has no order inside it, so a frame that carries
  a `STEREO` tile runs **Pass W and Pass B once per eye**, with a barrier
  between, and the workgroup → tile map is rebuilt eye-major so each eye is a
  contiguous `vkCmdDispatchBase` range. Every other frame runs each pass once,
  so the extra dispatch is paid only by the frames that need it.

### Concealment, and the API that drives it

`nxvc_vk_decoder_mark_missing(dec, tile_ids, count)` marks the tiles the client
did not receive. The marks apply to the next frame decoded and are consumed by
it, whether or not that frame is accepted.

A marked tile's bytes are still parsed — the frame stays self-delimiting — and
then discarded; the tile is reconstructed by running the `WARP_SKIP` predictor
with its stored `last_mv` and no residual, which is *bit-identically* a
legitimately skipped tile. That is the whole design of 13.6: there is no
separate concealment path to test, and the encoder can replay it. Two rules
are easy to get wrong and are worth naming:

* **A concealed tile's prediction state does not advance.** Getting this wrong
  is invisible for one frame and shows up two or three frames later as a
  vector applied from the wrong place, which is why the loss test runs over a
  hundred frames rather than one.
* **A near-skip correction naming a missing tile is not applied.** The
  correction travelled in a row header the transport does not replicate, so
  applying it would make the decoder's picture depend on bytes it may never
  have seen — exactly the divergence the shadow contract exists to prevent.

`vk.decoder.loss` is the assertion: 100 frames of an inter stream using every
inter tool at once, random tiles dropped every frame, decoded beside `ref/`
fed the same drops through `nxvc_decoder_set_lost_tiles()`, compared byte for
byte. Twice: mono, and stereo.

| device | stream | frames | with drops | tiles dropped | differing bytes |
|---|---|---|---|---|---|
| RX 7900 XTX (RADV NAVI31) | mono | 100 | 66 | 175 | **0** |
| RX 7900 XTX (RADV NAVI31) | stereo | 100 | 71 | 336 | **0** |
| llvmpipe (lavapipe) | mono | 100 | 66 | 175 | **0** |
| llvmpipe (lavapipe) | stereo | 100 | 71 | 336 | **0** |
| Adreno 650 (Pico 4) | mono | 100 | 66 | 175 | **0** |
| Adreno 650 (Pico 4) | stereo | 100 | 71 | 336 | **0** |

**The stereo arm covers what 13.7 says the ENCODER cannot do.**
`nxvc_encoder_set_received_tiles()` returns `UNSUPPORTED` for a concealed
left-eye tile, because re-deriving what a `STEREO` tile of the same row was
predicted from needs a full-frame replay the reference encoder does not do.
A *decoder* has no such gap: it conceals the left-eye tile into the ring slot
it is filling, and the `STEREO` tile of the same row then predicts from what
the decoder actually holds. Both decoders do exactly that, so they agree byte
for byte, and the test says so rather than leaving the case to the reader.

### `eyes = 2`, and the one thing that is refused

A stereo frame is `eyes` **pictures**, not one double-width picture (3.3), and
this decoder merges them into a single raster of 64-pixel columns:
`tilesX = eyes * cols_per_eye`, `imageW = eyes * width`, and the readback
layout is the reference's `width * eyes`. That merge is exact only when each
eye's last tile column is full — otherwise eye 1 starts at pixel `width`
rather than at `cols_per_eye * 64` and every tile-to-pixel mapping in Pass B
would need a per-eye x origin. So `eyes == 2` with a width that is not a
multiple of 64 is `UNSUPPORTED`, in one line, at the stream header. Refusing
it is honest; mis-mapping it silently is not. Every stereo configuration the
headset streams has a width that is a multiple of 64.

### Timing: a 36-frame inter sequence

The intra bench decodes one frame `iters` times, which is the right shape for
a pass whose cost depends only on the frame in front of it. The inter path's
does not: its cost depends on the mode mix, and the mode mix is a property of
*where in the sequence* the frame is. Frame 0 is all `INTRA`, frame 1 is
mostly `WARP_MV`, and by frame 10 a static region is `WARP_SKIP` and costs one
Pass W dispatch and no entropy decode at all. `--bench-inter N` decodes the
whole sequence in order.

**RX 7900 XTX (RADV NAVI31)**, 36 frames, 4:2:0, QP 24, from
`nxvc_encoder`'s default inter configuration (`INTER` + `WARP` + `NEAR_SKIP`
+ `QUAD_MV` on top of the intra default), best sequence of 4:

| 36 frames, 4:2:0 | 2048 tiles, QP 24 | 2048 tiles, QP 36 | 256 tiles, QP 24 |
|---|---|---|---|
| whole sequence, GPU | **53.2 ms** | **50.3 ms** | 9.7 ms |
| whole sequence, wall | 71.6 ms | 63.4 ms | 13.5 ms |
| frame 0 (all `INTRA`), Pass A | 2.625 ms | 1.702 ms | 1.823 ms |
| frame 0, Pass B | 1.861 ms | 1.231 ms | 0.419 ms |
| frame 0, GPU | 4.493 ms | 2.941 ms | 2.250 ms |
| frames 1-35, Pass A | **1.021 ms** | 0.833 ms | 0.152 ms |
| frames 1-35, Pass W | **0.179 ms** | 0.139 ms | 0.029 ms |
| frames 1-35, Pass B (predictor included) | 0.426 ms | 0.554 ms | 0.086 ms |
| frames 1-35, GPU | **1.452 ms** | **1.393 ms** | 0.244 ms |
| frames 1-35, wall | 1.962 ms | 1.757 ms | 0.347 ms |
| bytes per inter frame | 39.5 kB | 61.4 kB | 4.2 kB |
| tiles skipped | 0.2 % | 32.8 % | 0.3 % |

An inter frame at the headline shape costs **1.45 ms of GPU against the intra
frame's 4.49 ms** — a third — and the whole 36-frame sequence is 53 ms, which
is 1.5 ms a frame against a 4 ms budget at 90 Hz.

### What an inter frame cost the Adreno 650, and why

The Pico 4 is where the inter path's shape mattered, and the first measurement
of it was **six times too slow**. 36 frames, 1024x1024 4:2:0, QP 24, 256
tiles, from the encoder's default configuration:

| Adreno 650, per inter frame | one module | split by group | `INTRA_DIR` off |
|---|---|---|---|
| Pass A | 1.32 ms | 1.25 ms | 2.78 ms |
| Pass W | 2.95 ms | 2.93 ms | 2.94 ms |
| Pass B (predictor included) | **42.87 ms** | **21.76 ms** | 6.44 ms |
| GPU total | 44.19 ms | **23.01 ms** | 9.22 ms |
| 36-frame sequence, GPU | 1838.9 ms | **1119.8 ms** | 358 ms |

Nothing in Pass B's *work* changed between the first two columns. What changed
is which **module** the inter tiles are dispatched with.

`INTRA_DIR` is a build variant, two SPIR-V modules from one source, for a
reason this document already gave: the wavefront needs `predictOne()`'s two
17-entry reference arrays, and the driver reports the combined kernel at a
328-word register footprint against 16 for the v1 form. That footprint is paid
by **every workgroup of a dispatch that uses the module**, whether or not the
workgroup ever reaches the wavefront — and an inter tile never reaches it,
because `dir` in the kernel is `intra && kIntraDir`.

So on an inter frame the whole picture was being decoded at the occupancy of a
branch none of it took. Proving it took one run: the same sequence encoded
with `INTRA_DIR` off decodes its inter frames in **6.4 ms** of Pass B against
42.9, and Pass A — the control for thermal drift — does not move.

Choosing the module **per frame** would have been the obvious fix and would
have fired almost never: the rolling intra refresh puts at least one `INTRA`
tile in nearly every frame. So the choice is **per group**. `build_tile_order`
stable-partitions each eye's segment of the workgroup → tile map into the
tiles whose mode is not `INTRA` and the tiles whose mode is, and Pass B is
dispatched twice over the two contiguous ranges, once with each module. The
map already existed for exactly this kind of regrouping, the partition is
stable so raster order survives inside each group, and every write address is
derived from the tile index rather than from the workgroup index, so the
output is bit-identical — which the whole 200-stream sweep re-checks on all
three ICDs.

It costs one more cached pipeline and one more `vkCmdDispatchBase` per frame,
and it is free on RADV, where the two modules measure the same.

**The third column says where the rest of it went.** With the tool off
entirely the same sequence is 6.44 ms of Pass B, so the split recovers about
half of the 36 ms and the remaining 15 ms is the intra tiles that are really
there: the rolling intra refresh puts about fourteen of the frame's 256 tiles
on the wavefront every frame, and fourteen tiles of the heavy module cost
roughly a twentieth of the all-intra frame's 270 ms, which is what 15 ms is.
That is not the inter path's problem to fix -- it is the directional
wavefront's Adreno cost, which this document already measures -- but the inter
path is what made it visible, because it is the first configuration in which
most of a frame does not want the wavefront at all.

`tile_sort` still applies, inside each group: the partition is what a dispatch
boundary needs and the sort is what a warp scheduler wants, and neither cares
about the other.

**The QP 36 column is the one that says what the floor is.** It codes a third
of its tiles as `WARP_SKIP` and 50 % *more* bytes per frame than the QP 24 one
(the rate controller spends the saved tiles elsewhere), and it still lands
within 4 % of the same GPU time. Pass A falls with the payload, Pass B rises
slightly with it, and Pass W does not move: it is one divide per corner and
one bilinear tap per sample whatever the tile codes. That is the shape of an
inter frame's cost, and Pass W is the part of it that no rate decision can
reduce.

The shape of the number, not its magnitude, is the point: **Pass A collapses**
once the sequence is running, because most of a well-predicted frame codes
nothing, and what is left is Pass W plus a Pass B that is doing almost no
transform work. Pass W is the only cost that does not fall with the payload —
it is one divide per corner and one bilinear tap per sample whatever the tile
codes — which is what makes it the floor of an inter frame and the thing to
attack next.

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
actually streams — decoded 30 times, best of run. Informational.

**RX 7900 XTX (RADV NAVI31)**, streams from `nxvc_encoder`'s **default
configuration** — which at bitstream minor 6 means `INTRA_DIR` + `CTX_V2` +
`SIGN_HIDE` + `XFORM_4X4_SPLIT` + `INTRA_CFL`, so these frames exercise the
two detail tools as well:

| QP | frame bytes | coef SSBO | Pass A | Pass B | GPU total | wall |
|---|---|---|---|---|---|---|
| 12 | 2.70 MB | 13.9 MB | 1.91 ms | 1.34 ms | 3.26 ms | 3.75 ms |
| 24 | 1.36 MB | 11.8 MB | 1.35 ms | 1.02 ms | 2.38 ms | 2.84 ms |
| 36 | 0.19 MB | 0.94 MB | 0.33 ms | 0.74 ms | 1.08 ms | 1.53 ms |

The rows are **not** comparable with the v1.3 ones this table used to carry:
the default encoder emits a different stream now (a 2.70 MB frame at QP 12
against 2.64), so both the payload and the coefficients differ. The
like-for-like control is on the Pico, below, where the same `.nxv` file was
decoded before and after.

**llvmpipe (lavapipe)**, pinned to 4 cores, for scale — it is a conformance
oracle, not a performance target, and the 4-core pin makes it about twice the
figures this document used to quote. Two runs, best of 3 and best of 6; the
after column is the range across them, because llvmpipe's spread at these
iteration counts is about ±20 % and only the QP 36 row is outside it:

| QP | Pass A | Pass B |
|---|---|---|
| 12 | 209 ms | 237 → 276–409 ms |
| 24 | 159 ms | 242 → 224–260 ms |
| 36 | 35 ms | 245 → **170–176 ms** |

The QP 12 row is not noise all the way: on a CPU rasterizer the coefficient
fetch is an L2 hit whichever layout it is, so the scan permute and the
per-coefficient length check are added work with no bandwidth saving to pay
for them, and at QP 12 almost every unit is coded. lavapipe is the closest
thing here to a part where compute is scarce and bandwidth is not, and it says
what that costs.

**Pass B now scales with the content.** The coefficient buffer between the
passes is sparse (`passA/syntax_constants.h` section 8): each unit carries its
coefficients in scan order and Pass A publishes how many there are, so a unit
that coded nothing costs no bytes, no dequantize and no IDCT. Together with
the mode-0 fast path below, that took Pass B's fixed cost from 890 ns per tile
to 248 ns. The next sections take the remaining number apart.

**Pass A costs slightly more at low rates** — 124 → 170 ns per tile fixed — and
that is the price of the layout: the length words and their atomicOr replace a
coalesced zeroing loop that also warmed the coefficient region's cache lines.
It is a good trade against Pass B's 642 ns, and it makes Pass A the larger of
the two passes at every QP above 24.

### Fixed cost versus per-byte cost

The tile-size question — is 64x64 the right unit for the *decoder*, separately
from the header-bytes argument — is a question about the intercept. A
least-squares fit of each pass against payload size over the QP 63/51/36/24/12
ladder, same 2048 tiles, separates the two:

| | fixed, per frame | slope, per MB of payload | fixed, per tile |
|---|---|---|---|
| Pass A, RADV, dense | 0.25 ms | 0.54 ms | 121 ns |
| Pass A, RADV, sparse (minor 6) | 0.24 ms | 0.65 ms | **115 ns** |
| Pass B, RADV, dense | 1.82 ms | ~0 | 890 ns |
| Pass B, RADV, sparse (minor 6) | 0.31 ms | 0.38 ms | **153 ns** |
| Pass A, lavapipe, dense | 39.7 ms | 84.6 ms | 19.4 µs |
| Pass A, lavapipe, sparse | 25.8 ms | 56.6 ms | **12.6 µs** |
| Pass B, lavapipe, dense | 303 ms | -8 ms | 148 µs |
| Pass B, lavapipe, sparse | 100 ms | 71 ms | **49 µs** |

**Pass B used to be entirely fixed cost.** Its slope was zero to within the
noise on both ICDs, at every QP from 63 (0.10 MB of payload) to 12 (2.87 MB) —
a 28x range in payload that moved Pass B by less than 3 %. That was not an
artefact of the measurement: the coefficient buffer between the passes was
**dense**, so Pass B read the same 25.6 MB whatever the stream said, and the
wavefront ran its 22 steps per plane whether the blocks it was stepping over
carried coefficients or not.

Both halves of that are now false, and the fixed cost is 248 ns per tile
rather than 890. The ladder itself is the clearest way to see it (RADV, best
of 30, sparse):

| QP | payload | coef SSBO | Pass A | Pass B |
|---|---|---|---|---|
| 63 | 0.102 MB | 0.87 MB | 0.29 ms | **0.20 ms** |
| 51 | 0.115 MB | 0.88 MB | 0.31 ms | **0.20 ms** |
| 36 | 0.155 MB | 0.93 MB | 0.50 ms | 1.09 ms |
| 24 | 1.424 MB | 11.6 MB | 1.31 ms | 1.18 ms |
| 12 | 2.869 MB | 13.6 MB | 1.94 ms | 1.62 ms |

**Pass B no longer fits a line**, so the least-squares numbers above should be
read as a summary rather than a model. The step between QP 51 and QP 36 is not
bandwidth: it is the mode-0 fast path. At QP 51 and above the encoder's
directional modes are almost all mode 0 — the DC-plane predictor, which reads
no neighbour — so the plane takes the parallel path and the wavefront does not
run at all. Below that it does. The mode-0 fast path is a step, the sparse
coefficients are a slope, and the two together are the 890 → 248 ns.

Two consequences worth stating plainly:

* **The tile is no longer the unit of cost.** 248 ns per tile is what an empty
  64x64 tile costs; a dense one at QP 12 costs about three times that. Larger
  tiles would still amortise the fixed part over more pixels, but the fixed
  part is now a quarter of what the tile-size argument was weighed against.
* **Pass A is now the expensive pass.** Its intercept, 170 ns, is 69 % of Pass
  B's rather than 13 %, and above QP 24 it is the larger of the two outright.
  `passA/README.md` names the thing to attack: three `barrier()`s per
  scheduling round, paid to keep control flow uniform for the LDS fallback.

### The `INTRA_DIR` wavefront, priced

2048 tiles, 4:2:0, QP 24, best of 20, Pass B only. `docs/SYNTAX.md` 7.6 prices
these three schedules in **rate**; this prices them in **time**. Barriers are
counted off the kernel, not off the idealised schedule, so they include the
DC-plane and transform barriers each plane pays anyway.

| schedule | steps | barriers/tile | occupancy | rate | Pass B, RADV | Pass B, lavapipe |
|---|---|---|---|---|---|---|
| `INTRA_DIR` off (v1) | — | 23 | 100 % | — | 0.255 → **0.241 ms** | 90 → 112 ms |
| 0 — as written | 22 | 62 → 65 | 4.5 → **18.2 %** | — | 1.780 → **1.183 ms** (4.9x) | 252 → 261–291 ms |
| 1 — no above-right | 15 | 49 → 52 | 6.7 → **26.7 %** | +0.24 % | 1.310 → **0.885 ms** (3.7x) | 229 → 244–266 ms |
| 3 — no above-right + 32x32 | 7 | 41 → 44 | 14.3 → **57.1 %** | +1.8 % | 1.041 → **0.732 ms** (3.0x) | 246 → 200–289 ms |

RADV figures are the median of three runs at best-of-30; lavapipe's are a
range across two runs, and it is wide enough that **lavapipe says nothing
about the wavefront**. That is the expected answer, and it is the reason to
believe the RADV column: llvmpipe has no occupancy to gain and no barrier cost
to lose, so a change that is purely about occupancy should move RADV and not
lavapipe, and that is exactly what the two columns show.

The headline is still the first row against the second: **directional intra
costs 4.9x on Pass B**, +0.94 ms per frame on a 7900 XTX, for the 22.5 BD-rate
points `ref/RESULTS-intra.md` measures. It used to be 7.3x and +1.49 ms.

Occupancy is what moved. The wavefront no longer inherits the transform's
four-threads-per-block mapping: the column pass stages the residual in the
shared sample store — free, because a block's own 8x8 region holds its
residual until its step and its reconstruction afterwards, and `dirAt()` never
reads a region whose block is not done — and `dirBlockOfStep()` then enumerates
the blocks of a step so each gets `kDirLanesPerBlock` = 16 threads. The extra
barrier per plane that the staging costs is the 62 → 65.

RADV shader statistics (`RADV_DEBUG=shaderstats`, RDNA3, wave64):

| | Pass A | Pass B, 4:4:4 store | Pass B, two-plane 4:2:0 |
|---|---|---|---|
| SGPRs / VGPRs | 108 / 60 | 108 / **136** | 108 / 84 |
| spilled | 0 / 0 | **0 / 0** | 0 / 0 |
| LDS | 12.0 KB | 25.8 KB | 13.3 KB |
| code size | 17 KB | 190 KB | 80 KB |

Nothing spills, which is the thing that could have made this much worse: the
`A[17]`/`L[17]` reference arrays of 7.4 are what take Pass B to 136 VGPRs, and
144 is the last step before RDNA3 drops from 4 waves per SIMD to 3. Replacing
`predictCols()` (two columns, `P0[8]` and `P1[8]`) with `predictOne()` (one
sample) is where the 144 → 136 came from. The register footprint is
**identical for all three schedules** — they change the loop trip count, not
the working set — and it is also identical for a stream with `INTRA_DIR` off,
because the tool is a push-constant branch inside one shader rather than a
second pipeline. A v1 stream therefore pays the occupancy cost of the wider
register file even though it never enters the wavefront; that is visible as
nothing at all in the table above, so it has not been worth splitting the
shader in two.

Pass A's LDS grew from 10.2 to 12.0 KB for the per-tile unit-length words it
now accumulates before flushing them once (`passA/syntax_constants.h` section
8); its register footprint did not move.

### Two stores from one Pass B

A frame that needs two display formats used to reconstruct every tile twice.
That is the shape a 4:2:0 stream with a coded alpha plane already has — the
two-plane store has nowhere to put alpha, so the A channel comes from a second
pass in RGBA8 — and it is the shape **the reference ring slot will have** when
the inter path lands, because the slot and the display image are two stores of
the same samples. Specialization constant 3, `kOutSecond`, does both from one
dispatch; it defaults to `kOutNone`, so a one-store pipeline compiles to
exactly the kernel it did before.

2048 tiles, 4:2:0 with a coded alpha plane, QP 24, Pass B only:

| | RADV | lavapipe |
|---|---|---|
| two dispatches (ycbcr420, then rgba8) | 2.271 ms | 473–667 ms |
| one dispatch, both stores | **1.685 ms** | **246–257 ms** |
| | **-26 %** | **-46 to -63 %** |

So it is cheaper, on both ICDs, by about what the second reconstruction costs
minus the second store's bandwidth — and much more than that on lavapipe,
where the reconstruction is the whole cost and the store is nearly free.
`NXVC_VKD_FLAG_SPLIT_STORES` keeps the two-dispatch path for measurement.

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
| raster order | 1.737 → 1.097 ms | 308 → 201 ms |
| sorted by shape | 1.553 → 0.942 ms | 315 → 221 ms |
| | -10.6 % → **-14 %** | +2.2 % → **+9.7 %** |

The sign is unchanged — it helps on RADV, hurts on llvmpipe — but the RADV
figure is no longer stable: across three runs the delta ranges -14 % to +5 %,
because Pass B is now three times cheaper and the mixed frame's own variance
dominates. **It stays off by default**, for the same reason as before and with
less confidence than before; it needs the Adreno number, where workgroup
convergence should matter most, before it can have a sensible default.

A uniform frame gains nothing from it, and a VR stream at a steady operating
point is close to uniform, so whatever the number is, it is the top of the
range rather than the middle of it.

### Memory traffic

The coefficient SSBO is the decoder's traffic budget. At `res_level` 0 a
4:2:0 tile slot is 6240 int16 (luma 64 DC + 64 blocks x 64 = 4160, plus 1040
for each of the two chroma planes), so a dense 2048-tile frame is **25.6 MB**
written by Pass A and read back by Pass B — about 51 MB of device traffic per
frame, which at 90 Hz is 4.6 GB/s, at every QP. 4:4:4 doubles the slot to
12480 int16 and the frame to 51.1 MB. PAPER 3.2.5's 16.8 MB estimate counts
the luma plane only.

**The sparse layout named there as the first optimisation has landed**
(`passA/syntax_constants.h` section 8, ADR 0026). Inside a coding unit a
coefficient is stored at its *scan* position rather than its raster one, the
unit's base and reserved width unchanged; Pass A publishes one byte per unit
holding `LAST + 1` and writes slots `[0, LAST]` only, so nothing is zeroed and
nothing past `LAST` is written or read. `LAST` is already in the syntax (9.2)
and the scan is already normative (5), so this is a re-indexing of the same
numbers: same bitstream, same coefficients, same pixels. Nothing in the C ABI
moved either, because the layout is entirely between the two passes.

Measured, 2048 tiles 4:2:0, the same frames as the timing table:

| QP | payload | dense | sparse | |
|---|---|---|---|---|
| 63 | 0.10 MB | 25.6 MB | **0.87 MB** | 29x |
| 36 | 0.16 MB | 25.6 MB | **0.93 MB** | 27x |
| 24 | 1.42 MB | 25.6 MB | **11.6 MB** | 2.2x |
| 12 | 2.87 MB | 25.6 MB | **13.6 MB** | 1.9x |

The sparse figure includes the length words themselves — 66 uints per tile,
0.54 MB per frame, which is the floor the QP 63 row is sitting on. Turn the
whole thing off with `NXVC_VKD_FLAG_DENSE_COEF`, and read the exact number
back in `nxvc_vkd_stats::coef_bytes` with `NXVC_VKD_FLAG_COEF_STATS`.

**What it did not buy is time on a discrete GPU.** Sparse against dense with
everything else equal, Pass B is within noise at QP 36 and about 5 % *slower*
at QP 12 and QP 24: 960 GB/s is not the scarce resource on a 7900 XTX, and
checking every coefficient against its unit's length is real work. The time
Pass B did gain came from the fast paths the lengths make possible — an
uncoded unit runs no IDCT at all — not from the bytes. On a part where 25 GB/s
is shared with the display controller the arithmetic is the other way round;
see "The Adreno 650 estimate" below.

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
| RX 7900 XTX (RADV NAVI31) | 228 checked, 6 skipped | **0** |
| llvmpipe (lavapipe) | 228 checked, 6 skipped | **0** |
| Adreno 650 (Pico 4, Qualcomm 1.1.128) | see "Conformance on Adreno" | 24 streams, all `XFORM_LARGE` |

The last two streams are the loss test, mono and stereo: each is a sweep of
its own and they are counted here because a decoder that concealed differently
from the reference would be as non-conformant as one that decoded differently.

**The Adreno column is not clean and that is `XFORM_LARGE`'s doing alone**:
the 24 streams that reach the 16x16 / 32x32 module come back wrong on that
driver, and every other stream on the device is as clean as it was. "The
transform size, priced" has the footprint and what has been tried.

The Adreno column is the one that matters: it is the target part, it is a
third driver rather than a second one, and getting to it found three defects
nothing else had (see "Android") and, at bitstream minor 6, four more (see
"Timing on the Pico 4").

**The skip count is the number to watch.** It is exactly "how many conformance
streams this decoder cannot yet speak", decided from each stream's own `tools`
field and never from its file name, so a regression that starts refusing a
supported vector still fails. Driving it to zero is what finishing the tool
set means. It was 60 when `merge-main`'s encoder default first set tool bits
this decoder did not have; the minor-6 realignment took it to 46, the inter
path to 15 and `XFORM_LARGE` to **6**, which is one tool and nothing else:

| skipped | why |
|---|---|
| `v76`–`v81` | `ENTROPY_LITE` (bit 30) — Pass A has the kernel, the decoder does not offer the bit |

`XFORM_LARGE` accounts for the nine that went between those last two numbers:
`v68`–`v73`, and the three rejection vectors `r36` (`xform_size` 3, reserved),
`r38` (`xform_size` with transform skip) and `r39` (`split4x4` with
`xform_size`), which are now refused for the reason the syntax gives rather
than at the tool mask. The synthetic sweep gained sixteen more streams: each
size on its own in both chroma formats, both with and without the directional
wavefront, the plane cap of 6.7 over cycling `res_level` — where one frame
carries 32x32, 16x16 and 8x8 luma blocks and a chroma plane capped one step
further again — and the encoder's own per-tile RD choice, which mixes all
three sizes inside one frame.


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
| `vk.decoder.unorm_roundtrip[_radv,_lavapipe]` | is an 8-bit UNORM storage image exact on this driver ("The UNORM store") |
| `vk.decoder.loss` | [inter] 100 frames of random tile loss beside `ref/` fed the same drops |
| `vk.decoder.bench_inter` | [inter] a 36-frame inter sequence, decoded in order |
| `vk.passW.gpu_vs_cpu[_radv,_lavapipe]` | [inter] the predictor kernel against `inter_model.cpp` |

Everything exits 77 — a ctest skip — when there is no usable ICD.

The harness binary takes `--quick`, `--verbose`, `--only-vectors`,
`--only-synthetic`, `--vectors DIR` and `--bench [iters]`, and honours
`NXVC_VKD_DEVICE` as a device-name substring. `--bench-qp QP` runs the one-QP
slice of the bench and `--bench-v1` pins `INTRA_DIR` off in it; on a device
those two are the usable form, because `--bench` encodes eleven 2048-tile
frames on the CPU before it dispatches anything.

---

## The CLI

```sh
nxvc-vkdec --in file.nxv --out out.yuv [--icd PATH] [--device SUBSTR]
           [--pix yuv420p|yuv444p] [--frames N] [--nv12] [--stats]
           [--format auto|rgba8|rgb10a2|ycbcr420] [--lds] [--quiet]
           [--dir-sched 0..3] [--tile-sort]
           [--repeat N] [--no-out] [--dense]
```

`--repeat N --no-out` decodes the first frame N times and prints the best
per-pass device time, with no readback and no output file. That is the whole
timing loop on a device: push one `.nxv` and measure the two kernels over it,
with no encoder and no vectors on the phone. It is what "Timing on the Pico 4"
below was measured with.

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

The build itself is nothing special: plain C++20 over core Vulkan, and the NDK
supplies both `glslc` (in `shader-tools/`) and `libvulkan`, so arm64 is a
normal cross build of the same target. `android/` links it.

`vk/decoder/tools/run-android.sh` does the whole loop — configure, build, push
the test binary and `tests/vectors/` to `/data/local/tmp/nxwarp/`, run it over
`adb shell`, print the verdict and the GPU clock either side of the run. No
APK, no Java, no `NativeActivity`: the conformance harness is an ordinary
executable.

```sh
./vk/decoder/tools/run-android.sh              # the full 168-stream sweep
./vk/decoder/tools/run-android.sh --quick      # a subset, for a smoke test
./vk/decoder/tools/run-android.sh --bench 10   # the timing table
./vk/decoder/tools/run-android.sh --unorm 1    # opt into the UNORM store
```

Two environment variables exist for taking a kernel apart on the device, both
off by default and neither on any timed path:

* `NXVC_VKD_SHADER_STATS=1` enables `VK_KHR_pipeline_executable_properties` on
  a device this library creates and prints the driver's own statistics —
  registers, spill, scratch, shared memory, instruction counts — for every
  pipeline it compiles. Opt-in because `CAPTURE_STATISTICS` is a pipeline
  creation flag and a driver may compile differently with it set.
* `NXVC_SPV_PASSES` overrides Pass B's `spirv-opt` pass list at build time
  (empty for none, `-O` for the stock list), exactly as `NXB_SPV_PASSES` does
  in `bench/`, so a suspected miscompile can be bisected against the optimiser
  without editing `passB/cmake/gen_spv.cmake`.

or by hand:

```sh
NDK=$ANDROID_SDK/ndk/<version>
cmake -S . -B build-vkdec-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DNXWARP_BUILD_VK=ON -DNXWARP_BUILD_TESTS=ON -DNXWARP_BUILD_TOOLS=ON \
  -DNXWARP_BUILD_EXAMPLES=OFF -DNXWARP_BUILD_TRANSPORT=OFF
cmake --build build-vkdec-android -j4 \
      --target test_vk_decoder_conformance nxvc-vkdec
```

The client should adopt its own `VkDevice` rather than let the library create
one, and `NXVC_VKD_OUT_AUTO` then resolves to the two-plane 4:2:0 store the
reprojection shader already samples.

### What the first real device changed

"Nothing special" was true of the *compile* and false of everything else.
Four things had to change before a single stream decoded on a Pico 4
(Adreno 650, driver 1.1.128, build 10/31/22), and three of them were defects
this decoder had been carrying:

* **The API floor was 1.2 and the part is 1.1.128.** Device selection skipped
  anything below 1.2 outright, so the decoder refused the target part with
  "no Vulkan 1.2 device". Of the two things 1.2 was being asked for, 16-bit
  storage is core in **1.1** and timeline semaphores are
  `VK_KHR_timeline_semaphore` there, which this driver advertises. The floor
  is now 1.1 plus that extension. Two details bite on the way:
  `VkPhysicalDeviceVulkan11Features` is itself a 1.2 structure, so the 1.1
  path queries `VkPhysicalDevice16BitStorageFeatures` and
  `VkPhysicalDeviceTimelineSemaphoreFeatures` instead; and the instance is
  created at the loader's own version rather than at 1.3, because Android's
  1.1 loader fails `vkCreateInstance` outright on a 1.3 request.
* **`vkWaitSemaphores` is not in Android's `libvulkan.so`.** The API 29 stub
  exports no 1.2 entry point, so the link failed. It is resolved through
  `vkGetDeviceProcAddr` now (core name, then the KHR alias), which an adopted
  device needs anyway.
* **The driver advertises timeline semaphores and cannot create one.**
  `VK_KHR_timeline_semaphore` is in the extension list, `timelineSemaphore` is
  `VK_TRUE`, `vkGetDeviceProcAddr("vkWaitSemaphoresKHR")` returns a pointer —
  and `vkCreateSemaphore` on a `VK_SEMAPHORE_TYPE_TIMELINE` returns 5 while a
  binary semaphore succeeds. There is nothing to do about that from here, so
  the decoder falls back to a **`VkFence`**: the decode path only ever asks
  "has this frame finished", which a fence answers exactly.
  `nxvc_vk_decoder_timeline()` returns `VK_NULL_HANDLE` on such a device and
  a compositor must wait through `nxvc_vk_decoder_wait()` instead. This is the
  one place where the headset gets a worse interface than the desktop, and it
  is the driver's doing.
* **The descriptor pool was one short and two ICDs had been hiding it.** Pass
  A takes 8 storage buffers and Pass B takes 6 (bindings 0–2 and 7–9); the
  pool asked for 12. Bindings 8 and 9 arrived with the tile map and the sparse
  unit lengths and the count did not follow. RADV and lavapipe hand out
  descriptors past the declared pool size, so `vkAllocateDescriptorSets`
  succeeded on both for as long as the bug existed. The Adreno driver returns
  `VK_ERROR_OUT_OF_POOL_MEMORY`, which is the conformant answer. **This is a
  real bug that only a third driver could find**, and it is the argument for
  running the sweep on hardware rather than on two ICDs that agree.

None of the four is a *decoding* difference. Once the device came up, the
pixels were right the first time, which is what the spirv-opt pass list
(`bench/README.md`, "Adreno and spirv-opt") was already there to ensure.

### Conformance on Adreno

The same streams as the desktop table, on the Pico 4, from `run-android.sh`:

| ICD | streams | mismatching samples |
|---|---|---|
| Adreno 650 (Qualcomm 1.1.128), UINT store | 201 checked, 15 skipped | **0** |
| ... `--bench-inter`, 36-frame sequence | see "The inter path" | **0** |

The skip set is decided from each stream's own `tools` field, exactly as on
the other two ICDs, so it is the same 15 streams and not a device-specific
exemption.

**[inter] The Phase 2 path needed nothing device-specific.** Pass W passed on
the Adreno 650 first time, and the 100-frame loss test is byte-identical to
the reference there as it is on the two desktop ICDs. That is worth saying
because the last three tools that landed each cost the Adreno column a defect
nobody else had; this one did not, and the reason is that `warp_pred.comp` is
a copy of a kernel (`warp/glsl/warp_tile.comp`) that had already been through
this driver, and that `docs/ADRENO-RULES.md` rule 1 was applied while writing
it rather than after: the four quadrant vectors are eight scalars and a select
ladder, not `int mvx_q6[4]`, and the tile's matrix is read field by field out
of the SSBO rather than copied into a local record.

### The UNORM store

`bench/README.md` measures an **integer storage image at about 3x the cost of
a UNORM one** on the Adreno 650 — 4.6 GB/s through `rgba8ui` against 14.9
through `rgba8`, at the same workgroup shape, with `r32ui` no better, so it is
the integer image path and not the channel count. Pass B writes
`R8G8B8A8_UINT`, `R8_UINT` and `R8G8_UINT`, which is exactly that path. The
bench declined to draw the conclusion: "8-bit UNORM does round-trip exactly,
so it is probably safe, but 'probably' is not the standard PAPER 3.7 sets and
it needs its own proof."

**It now has one.** `reconstruct.comp` specialization constant 5,
`kUnormStore`, switches the three 8-bit stores to bindings 10–12, which are
`rgba8`, `r8` and `rg8` UNORM images holding the same pixels. The sample goes
out as `v / 255.0`. `kOutRgb10A2` is untouched and stays integer: it is a
desktop path and its samples are not 8-bit.

The proof is `tests/vk-decoder/unorm`, a standalone binary that links no
decoder code — so a driver can be disqualified from the UNORM path without
running the sweep, and so the answer is about the driver rather than about
this codec. It checks two things over all 256 values in every channel of all
three formats:

* **store side** — the kernel stores `v / 255.0`; the byte pulled out with
  `vkCmdCopyImageToBuffer` must be exactly `v`. This is what the decoder's
  readback and any sampler downstream of the image see.
* **load side** — `imageLoad` of that texel, re-encoded, must be exactly `v`.
  This is what the Phase 2 reference-ring reads will see.

Each value is written into one channel with a distinct filler (`255 - v`) in
the others, so a channel swizzle cannot hide behind a value that happens to
match.

| ICD | store side | load side |
|---|---|---|
| RX 7900 XTX (RADV NAVI31) | exact, 0/768 per value | exact |
| llvmpipe (lavapipe) | exact, 0/768 per value | exact |
| Adreno 650 (Pico 4) | exact, 0/768 per value | exact |

and the whole conformance sweep re-run through the UNORM path:

| ICD | streams | mismatching samples |
|---|---|---|
| RX 7900 XTX (RADV NAVI31), UNORM | 152 checked, 23 skipped | **0** |
| llvmpipe (lavapipe), UNORM | 152 checked, 23 skipped | **0** |
| Adreno 650 (Pico 4), UNORM | 152 checked, 23 skipped | **0** |

(measured before the minor-6 realignment; the UNORM path is orthogonal to the
tool set and has not been re-run on the 168.)

`ctest -R '^vk\.decoder\.unorm'` runs the exactness proof on the default
device, on RADV and on lavapipe; `run-android.sh` pushes it alongside the
sweep.

**It is exact on all three drivers, and it is not worth turning on.** Pass B
on the Pico 4, 2048 tiles, best of 10, alternating runs so the GPU's clock and
thermal state cannot favour one arm:

> **Re-measured after the 2026-09-05 Pass B work, and the 7% is gone.** On the
> present kernel, under the cold-start protocol of "Timing on the Pico 4":
> QP 24 v1, 22.25 / 22.33 ms with the integer store against 22.51 / 22.48 with
> UNORM, and QP 36 v1, 15.31 / 15.24 against 15.50 — **about 1% worse at both
> QPs**, not 7% better at one. The 7% was measured when the store was a larger
> share of a slower pass; it was never a large enough result to change a format
> across the ABI, and it is now the wrong sign. The exactness result below is
> the part that keeps its value.
>
> The store *is* still worth about 2.6 ms of Pass B at QP 36 (measured by
> shrinking the store loop), so there is a real target there — it is just not
> the pixel format. It is the store count: 4096 one-byte `r8ui` stores per
> tile. See "What remains".

| stream | UINT store | UNORM store | |
|---|---|---|---|
| QP 24, `INTRA_DIR` off | 368.9 / 368.2 / 367.3 ms | 338.2 / 344.3 / 340.6 ms | **-7.4 %** |
| QP 36, `INTRA_DIR` off | 295.9 / 297.4 ms | 301.3 / 302.2 ms | **+1.7 %** |
| QP 24, `INTRA_DIR` on | 1348.5 ms | 1347.8 ms | **0.0 %** |

The bench's 3x was real and it was measured on a **pure copy kernel**, where
the store is the entire workload. Pass B is not a copy kernel: at QP 24 with
the wavefront on, the 12.6 MB store is under a percent of a 1.35-second pass
and no store format is visible through it. With the wavefront off the store is
finally a measurable fraction — and it is worth 7 % at one QP and *costs* 2 %
at another, which is not a result that generalises.

So the switch is **off by default on every platform**, including Android, and
is not in the C ABI either: `NXVC_VKD_UNORM=1` or `nxvc-vkdec --unorm 1`. The
deciding argument is not the 7 %, it is what the 7 % would buy it — the switch
changes the `VkFormat` that `nxvc_vk_decoder_images()` hands out, which every
consumer of the image sees, and the WiVRn NX client samples that image
directly. A format change visible across the ABI needs more than 7 % of a pass
that is 30x over its frame budget either way. A caller that wants it must read
`nxvc_vkd_images::format` rather than assume the integer formats, which is
what the field is for.

The exactness result stands on its own regardless, and it is the part worth
keeping: **8-bit UNORM storage images are a bit-exact substitute for integer
ones on all three drivers**, so this is a lever that can be pulled later, on a
part where the store *is* the constraint, without re-opening ADR 0023.

### Timing on the Pico 4

Two 2048x2048 eyes at 4:2:0, 2048 tiles — the same frame as the desktop table
— best of 12. `--bench-qp QP [--bench-v1]` is the one-QP slice of `--bench`;
the full sweep encodes eleven such frames with the CPU reference encoder, which
is over an hour of a phone's CPU before a single dispatch runs. **`--bench-save
FILE` writes exactly that stream and exits**, so the encode happens once on a
desktop and the device side is `nxvc-vkdec --in FILE --repeat N --no-out`,
which is what every number below was taken with.

**The measurement protocol matters more than it looks.** The Pico 4 throttles
inside a single `--repeat 30`: the same build measured 103.2 ms of Pass A on a
cold device and 117.7 ms on a hot one, with the reported `gpuclk` *higher*
(587 MHz) on the slow run than on the fast one (441 MHz), because the clock is
sampled after the run and the GPU has already ramped and started to give the
heat back. Every arm below is 12 repeats with a 20-second idle before it and
15 seconds after, and any pair whose *Pass A* numbers move is thrown away —
Pass A is a control for thermal drift in every Pass B experiment, because no
Pass B change can touch it.

"pre-Adreno" is where this document stood when the only thing that had ever
been tuned against was a 7900 XTX. The commits between the columns are
"Adreno puts local arrays in private memory", "16 tiles per workgroup",
"32 tiles per workgroup" and "the planar predictor pays for LDS loads".

| stream | Pass A | Pass B | GPU total |
|---|---|---|---|
| QP 24, `INTRA_DIR` off (v1) | 153.7 → **79.4 ms** | 366.1 → **22.3 ms** | 521.7 → **102.2 ms** |
| QP 36, `INTRA_DIR` off (v1) | 26.7 → **10.1 ms** | 417.9 → **15.3 ms** | 445.4 → **25.4 ms** |

**[inter] The inter sequence is a different measurement and is not in that
table.** It is a smaller picture (1024x1024, 256 tiles) because the encode
runs on the device, and its point is the ratio rather than the magnitude: an
inter frame is **23.0 ms of GPU against the intra frame's 298 ms**, at the
encoder's default tool set. "The inter path" above has it broken out, and the
reason the intra frame is 298 ms rather than a quarter of the 102.2 ms in this
table is the directional wavefront, which that row has switched off.

Zero mismatching samples on the 152-stream sweep at every point in that table,
on the Adreno 650, on RADV and on lavapipe.

#### The live VR shape: 1088x1088, 289 tiles, 22-49 KB a frame

Every number above is a 2048x2048 frame of 2048 tiles carrying 600-900 bytes
each. **The shape a headset actually streams is nothing like it**, and the
conclusions do not carry across. A WiVRn eye is 1088x1088 -- 17x17 = 289
tiles -- and a frame of live VR content at QP 28-36 is 22-49 KB, i.e. 75 to
170 bytes per tile, a fifth of the density this document was tuned on.

Measured with `nxvc-vkdec --repeat`, best of 12, on the Pico 4 while another
process held the GPU, so these are contended and pessimistic. Streams from
`nxv-enc --nsub 3 --intra-dir off`, which is the shape the Vulkan encoder
emits (`nxvc_vk_encoder`'s config fixes `nsub_log2 = 3` and `intra_dir =
false`):

| payload | Pass A | Pass B | GPU | wall | host parse+submit |
|---|---|---|---|---|---|
| 22.3 KB (QP 51) | 3.33 | 2.56 | 5.90 | 6.54 | 0.12 |
| 32.3 KB (QP 44) | 5.33 | 3.55 | 8.99 | 9.44 | 0.17 |
| 48.6 KB (QP 40) | 7.96 | 3.92 | 12.02 | 12.92 | 0.14 |

**Pass A is 0.20 ms per KB of payload plus about 0.4 ms; Pass B is 2.5-4 ms
and barely moves.** The host's own share -- parsing every tile header,
building 289 descriptors and records, recording the command buffer -- is
0.15 ms and is not worth looking at. A frame at the live bitrate is a 6-13 ms
GPU frame, which is two eyes inside 26 ms and not inside 11.1.

Three things this says that the 2048-tile table does not:

* **A live decode wall much above 13 ms is not the codec.** A wrapper that
  reports 47 ms for a 47 KB frame is spending about 34 ms somewhere else --
  the queue it shares with the renderer, its own fence round trips, or the
  other decoder instances on that queue.
* **`INTRA_DIR` is catastrophic at this shape and nearly free to turn off.**
  The same content, same QP, same tile count, `--intra-dir on` against
  `--intra-dir off`:

  | | Pass B, dir off | Pass B, dir on | bits |
  |---|---|---|---|
  | QP 51 | **2.56 ms** | 68.9 ms | 22.29 -> 22.31 KB |
  | QP 44 | **3.66 ms** | 315 ms | 32.34 -> 30.06 KB |

  87x of Pass B for 0 % of the rate at QP 51 and 7 % at QP 44. The Vulkan
  encoder hard-codes it off; the reference encoder does not, and WiVRn's
  `intra-dir` option defaults to **true**, so a session on the `ref` backend
  is one config key away from a 300 ms frame.
* **`ENTROPY_LITE` is a regression here.** See below.

##### `ENTROPY_LITE` at 289 tiles, and why the 7.5x does not survive

Tool bit 30 is now offered (`kToolsSupported`), decodes byte-identically to
`ref/` on the Adreno 650, RADV and lavapipe, and **should stay off for VR**:

| 1088x1088, 289 tiles | rANS payload | rANS Pass A | Lite payload | Lite Pass A |
|---|---|---|---|---|
| QP 51 | 22.3 KB | **3.33 ms** | 18.9 KB | 6.75 ms |
| QP 44 | 32.3 KB | **6.06 ms** | 36.5 KB | 7.84 ms |

`docs/TOOLBITS.md` 8 prices the tool at 7.5x of Pass A, and that measurement
is not wrong -- it is 2048 tiles at 911 bytes each, where the rANS chain is
long and Lite's fixed cost is amortised. **Lite's cost follows the tile AREA,
not the bit count**: one workgroup per tile computes bit positions across the
tile's whole coefficient region whether or not the coefficients are there. At
75-170 bytes a tile the rANS chain is already short and there is nothing left
for the tool to remove, so all that is left is its own floor -- about
6.7 ms for 289 tiles either way -- plus the bits it costs.

The lever it was meant to be is real; it is a lever for dense frames.

#### An inter frame at the live shape costs 2.6x an intra frame at half the bytes

The reason to want the inter path was that its frames are small: 12.7 KB an eye
against 22.2 KB, at 83 % `WARP_SKIP`. **It is 2.6x more expensive to decode.**

Idle Pico 4, 1088x1088, 289 tiles, `--intra-dir off`, streams built with the GPU
encoder's own flag set (`--inter on --int-decision on --int-coded-vectors off
--preset fast --me-effort 1 --quad-mv off --near-skip off`, `--intra-period 6`),
46-50 coded INTRA tiles and 239-243 `WARP_SKIP` per frame:

| | payload | Pass A | Pass W | Pass B | GPU |
|---|---|---|---|---|---|
| intra, QP 51 | 22.2 KB | 3.33 | — | 2.56 | **5.93 ms** |
| inter, QP 38 | 12.7 KB | 6.5 | 4.25 | 9.1 | **15.6 ms** |
| inter, QP 30 | 25.4 KB | 10.4 | 4.27 | 9.2 | **19.6 ms** |

Three things went wrong, and only one of them is about bits:

* **Pass A is 0.51 ms/KB against intra's 0.150 — 3.4x worse per byte.** This is
  the occupancy result below in its sharpest form. A skipped tile gets no Pass A
  descriptor, so 83 % skip dispatches **48 tiles, not 289**: two workgroups, 384
  lanes, on a part that was already not full at ten. The bits do not leave with
  the tiles either, they concentrate — 265 B per coded tile against the intra
  frame's 77.
* **Pass W is 4.25 ms and does not move with the payload at all** (4.27 ms at
  twice the bytes). Per-tile over all 289: **14.7 us a tile**, a flat entry fee
  for the inter path.
* **Pass B is 9.1 ms and also does not move** — **31.5 us a tile against the
  intra kernel's 8.96**, on a frame where five tiles in six are `WARP_SKIP` and
  ought to be close to free. That shape has a history here: it is what
  `INTRA_DIR` did, and what the minor-6 realignment did, and the lesson both
  times was that **on this part, code you never execute is not free**. It is the
  largest single term and the one with a precedent for being mostly recoverable.

Two eyes of that is 31 ms of GPU, about 32 fps, against 11.1 ms for the pair.
The intra path at 22 KB an eye is nearly three times closer to the budget than
the inter path at 12.7.

**Caveat on the fixture.** It is a static scene with a matching pose track, so
its coded tiles are the encoder's periodic refresh rather than a head turn's
leading edge, and the content is photographic and so denser than a rendered
scene. The tile mix (83 % skip) and the byte count (12.7 KB) are the live shape;
*which* tiles are coded is not.

##### The inter Pass B cost is real work, not a folding defect

The first suspicion was the one this file has been right about twice already:
`INTRA_DIR` and `XFORM_LARGE` were both specialisation constants that the
Adreno driver would not fold, and both became build variants because of it, the
second one after costing an 8x8-only stream 34 % of Pass B.  `kInterPred` and
`kRefRingStore` are specialisation constants 7 and 8 and look exactly like the
next instance.

**They are not, and the desktop said so before any device time was spent.**
Inter against intra Pass B, per tile:

| | intra | inter | ratio |
|---|---|---|---|
| RADV | 0.073 us | 0.45 us | **6.2x** |
| Adreno 650 | 8.96 us | 31.5 us | **3.5x** |

A driver that folds specialisation constants properly shows a *larger* inter
penalty than the one that supposedly does not.  Whatever the inter path costs,
both drivers are paying it for the same reason, and it is work.

Which work, from `NXVC_VKD_ABL_NORING` / `NXVC_VKD_ABL_NOWPRED` on the
1088x1088 head-turn fixture, 289 tiles, 13.4 KB a frame, 82 % `WARP_SKIP`:

| | Pass B (RADV) |
|---|---|
| baseline | 0.110 ms |
| no reference-ring store | 0.069 ms — **the ring store is 37 %** |
| no predictor hook | 0.078 ms — **the hook is 29 %** |
| neither | 0.068 ms |

**The reference-ring store is the single largest piece**, and it is a whole
second store of the tile: 6144 samples a tile written as u16 into an SSBO, on
top of the u8 display store, with a `planeAtFull()` resample per sample rather
than a copy.  Three times the store traffic of the intra path, to hold a
picture that on this configuration is 8-bit in every plane.

So the lever is the one "Open issues" already names — **a u8 ring for a stream
with no colour transform** — and not a module split.  The remaining 62 % is the
predictor add and the mean-clamp widening, which are per sample and unavoidable
on a tile that really is inter; on a `WARP_SKIP` tile, which is five tiles in
six, they are reconstructing a residual that is not there.

##### The u8 ring is not the lever, and a WARP_SKIP tile costs MORE than an intra one

The ring store is the largest single piece of inter Pass B, and "Open issues"
has long named a **u8 ring for a stream with no colour transform** as the fix.
It was built as an ablation first -- `NXVW_PASSB_EXTRA_DEFS=-DNXVW_ABL_RING8`,
four samples a uint instead of two, which writes a ring no reader can use but
pays exactly the store traffic a u8 ring would -- and on a 7900 XTX it is a
**regression**:

| Pass B, head-turn fixture, RADV | |
|---|---|
| baseline, u16 ring | **0.077-0.080 ms** |
| u8 store traffic | 0.086-0.098 ms |
| no ring store at all | 0.070 ms |

The whole ring store is 0.007-0.010 ms of it. **The bytes were never the cost:
the cost is the per-sample `planeAtFull()` resample and the address
arithmetic**, and halving the traffic while keeping both buys at most 6 % of
Pass B and in this form loses more than that to the packing loop.

That is a desktop result and the Adreno is a bandwidth-poor part where "Pass B
is traffic" has been the finding before, so the u8 ring is not dead -- it is
*not worth the format change on RADV's evidence alone*, and the ablation build
is what the device round should measure before anything is rewritten.

**The bigger number is next to it.** Same stream, same 289 tiles, same
pipeline, RADV, by tile mix:

| frame | INTRA | WARP_SKIP | skip % | Pass B |
|---|---|---|---|---|
| 0 | 289 | 0 | 0.0 % | **0.037 ms** |
| 1-15 | 25-45 | 244-264 | 84-91 % | **0.072-0.102 ms** |

A frame that is five-sixths `WARP_SKIP` costs **2.2x** the Pass B of the same
tiles all-intra, on a quarter of the bytes, and a skipped tile has no residual
to transform at all. It should be the cheap case and it is the expensive one.
Inside the 84-91 % band there is no trend -- 0.094 at 91.3 %, 0.088 at 84.4 % --
so the cost is not proportional to the number of skips; it is what an inter
frame costs.

Two things follow. The **`WARP_SKIP` dispatch partition** is the lever, not the
ring format: it is 45 % of inter Pass B and it is being spent making the free
case expensive. And a client that reports its drops, so the encoder answers
with more INTRA tiles, moves the frame *toward* the 0.037 ms floor rather than
away from it -- the mix that costs more is the one with more skips.

#### Pass A is occupancy-starved at 289 tiles, and Pass B is not

Where the two passes' time goes at the live shape, measured on an idle Pico 4.
The two streams are the same content and the same bytes per tile; the second is
simply twice as tall, so it is **exactly twice the work** and any departure from
2.0x is the machine and not the frame:

| | tiles | payload | Pass A | Pass B |
|---|---|---|---|---|
| 1088x1088 | 289 | 22.2 KB | 3.34 ms | 2.56 ms |
| 1088x2176 | 578 | 44.3 KB | **5.10 ms (1.53x)** | **5.15 ms (2.01x)** |

**Pass B is 2.01x: perfectly linear, no fixed cost at all.** Fitting the pair
gives 8.96 us per tile and an intercept of −0.03 ms. The "flat 2.5 ms" it looks
like at 289 tiles is 289 x 9 us, not a floor, and the only way to move it is to
make a tile cheaper — which is what the wider store and the fused predict+store
below are for, both of them per-tile.

**Pass A is 1.53x, and that gap is the lever.** Per byte it reads

| tiles | Pass A |
|---|---|
| 289 | 0.150 ms/KB |
| 578 | 0.115 ms/KB |
| 2048 | 0.064 ms/KB |

The frame does not get cheaper per byte because the bytes change; it gets
cheaper because there are more tiles to run at once. Parallelism in Pass A is
`tiles x 8 lanes`, so a 289-tile eye offers 2312 lanes and 10 workgroups, and
the part is not full. The same kernel on the same bytes per tile is **2.3x more
efficient at 2048 tiles**, which is the whole of the factor the live shape needs.

That says where to look, and where not to:

* **Not the workgroup shape.** Re-swept at the live shape, and 32 tiles per
  group is still the answer: at 22.2 KB, Pass A is 3.33 ms at TPG 32, 3.92 at
  16 and 6.09 at 8. TPG 64 was not run — it hangs this device, and the device is
  someone's headset.
* **Not tile sorting.** A workgroup runs until its LONGEST tile is done, so
  sorting by payload length should pay — but at the live shape the tiles are
  uniform (58-76 B at QP 51) and grouping the sorted order costs 0.943 of the
  shuffled one: **6 %**. It is worth 21 % at 48 KB, where the spread is
  116-238 B, and nothing where it matters.
* **Not a register spill.** `NXVC_VKD_SHADER_STATS=1` on the live frame:
  Pass A is 1816 instructions, register footprint 6, scratch 310 B against the
  driver's own 278 B floor. There are 32 bytes to reclaim and no more.
* **The dependent chain is real and is what the lanes are waiting on.**
  `decode_symbol()` is a branchless binary search: four dependent `s_cum[]`
  reads to find the symbol, then two more for `c` and `hi`. **Six dependent
  shared-memory reads per symbol**, and with only 10 workgroups there is not
  enough other work resident to hide any of them. Both halves of that sentence
  matter, and the occupancy half is the cheaper one to fix.

**The cheapest real win is to stop dispatching one eye at a time.** Two decoder
instances each decoding 289 tiles cost 12.5 ms of GPU between them; one dispatch
over the same 578 tiles costs 10.2 ms. **18 %, for no bitstream change and no
kernel change** — the eyes are independent, nothing orders them, and the only
reason they are two dispatches is that they are two decoder instances. What it
needs is a decode entry point that takes several frames, or the `eyes = 2` path
that 13.2 already describes and this decoder still refuses.

#### The minor-6 realignment, and what it cost before it was paid back

The tools of bitstream minor 6 landed correct on all three ICDs and **slow on
the one that matters**. Measured on the same `qp36_v1.nxv` this table was
taken with — a stream that sets *none* of the new tools — the first correct
version of the realignment read Pass A 26.3 ms and Pass B 96.6 ms against
10.1 and 15.3. The whole of that was dead code and register pressure, and
three of the four causes are the same mistake in three places: **on this part,
code you never execute is not free.**

| | after the realignment | after this round | this table's figure |
|---|---|---|---|
| Pass A | 26.3 ms | **12.9 ms** | 10.1 |
| Pass B | 96.6 ms | **15.4 ms** | 15.3 |
| Pass A instruction count | 3530 | **1817** | 1320 |
| Pass B register footprint / scratch | 168 / 680 B | **0 / 352 B** | 6 / 342 B |

* **The 4x4-split transform's live set.** It first staged a whole sub-block
  through the plane slot and read the columns back after the barrier, then
  held its sixteen intermediates as named scalars — the letter of
  `docs/ADRENO-RULES.md`, and *worse* at 111.1 ms and a footprint of 168,
  because the rule is about what the compiler can keep in registers and
  sixteen more live values is past what it has: it answered by putting
  `res0`/`res1` in private memory as well. The fix was to halve the live set.
  `idct_block()` writes transposed in both passes, so pass 2's row `r` reads
  `out[r]` of every pass-1 row and produces COLUMN `r` — so a thread that
  wants the two columns it already owns runs four pass-1 rows keeping two of
  each four outputs, then two pass-2 rows. Twelve 4-point transforms, **eight**
  live intermediates, no staging and no extra barrier.
* **`kSplitTool`, Pass B specialization constant 6.** Even halved, the branch
  cost 10.8 ms on a frame where every split flag is zero. It is frame-uniform,
  so a stream without tool bit 19 now compiles a kernel with no split path in
  it at all.
* **`CTX_STRIDE`, Pass A specialization constant 4.** `CTX_V3` took the widest
  context model from 16 rows to 27, and the shared cumulative-frequency table
  was sized at the widest so the host would have one layout to build. That made
  every v1 and v2 stream carry 13824 bytes of LDS instead of 8192 — on a 32 KiB
  SP, the difference between two resident workgroups and one. The constant now
  SIZES the array rather than merely indexing it, the host uploads at the
  stride the frame's model needs, and the v3 derivations are gated on it too.
* **`XFORM_LARGE_TOOL`, Pass A specialization constant 5.** The two computed
  zigzags were 844 instructions of Pass A and about 10 ms of it, on every
  stream, for a path only the dense measurement layout can reach — and which no
  stream reaches at all, because bit 27 is still refused. A closed form was
  written for them first (the diagonal counts are triangles from either end,
  so one float square root and four integer corrections invert them exactly)
  and measured 1587 instructions against the walk's 844. The walk stays; what
  mattered was not compiling either.
* **One more runtime-indexed const array.** `kInvScan4Split` is computed from
  the 4x4 inverse zigzag that was already there — the split scan is four of it
  laid over the quadrants — so no third table exists.

Pass B is back where it was. **Pass A's remaining 28 % is the honest price of
the tools themselves** — the `CTX_V3` derivation, the split phase and the
per-plane transform size — on a stream that uses none of them, and the lever
left for it is the same one: gate the split phase on its tool bit as Pass B
now does.

A minor-6 stream at the encoder's own default (QP 36, `INTRA_DIR` on) reads
Pass A **16.7 ms**, Pass B **1275 ms**. The second number is not a minor-6
result: it is the directional-intra wavefront, which this document already
prices at 1348 ms with the tool on, and `INTRA_DIR` remains a desktop-decoder
tool negotiated by capability. With it off, the same content is 12.9 / 15.4.

**What each change bought**, all on the same device under the protocol above,
2048 tiles, `INTRA_DIR` off:

| | Pass A QP 24 | Pass B QP 24 | Pass A QP 36 | Pass B QP 36 |
|---|---|---|---|---|
| 8 tiles/group, before this round | 153.7 | 366.1 | 26.7 | 417.9 |
| … local arrays out of private memory, 16 tiles/group | 102.5 | 23.55 | 12.8 | 16.70 |
| **+ 32 tiles/group** (descriptor-array fix) | **79.8** | 23.55 | **10.1** | 16.70 |
| **+ bilinear: one set of mean loads for both columns** | 79.4 | **22.82** | 10.1 | **15.75** |
| **+ bilinear: reload the means only when the row moves** | 79.4 | **22.25** | 10.1 | **15.31** |
| the UNORM store on top of all of it | 79.4 | 22.49 (**worse**) | 10.1 | (see below) |

Three things were measured and are *not* in that table because they made it
worse or changed nothing; they are in "What was not the problem" below.

The earlier revision of this table quoted Pass A at 402–415 ms at QP 24 and
62 ms at QP 36. Those were measured with the `passa-fast` kernel in the tree;
the revert put the pre-merge kernel back and Pass A started at 153.7 / 26.7,
which is the "before" column above. The 3.5x that section describes is real
and is still unbisected — it is just already paid back.

Against an 11.1 ms budget at 90 Hz:

| | RADV | pre-Adreno | now | still over budget by |
|---|---|---|---|---|
| Pass B, `INTRA_DIR` off, QP 24 | 0.241 ms | 366 ms | **22.3 ms** | 2.0x |
| Pass A, QP 24 | 1.26 ms | 154 ms | **79.4 ms** | 7.2x |
| Pass A, QP 36 | 0.54 ms | 26.7 ms | **10.1 ms** | 0.9x — **inside budget** |
| Pass B, QP 36 | — | 418 ms | **15.3 ms** | 1.4x |

**A v1 frame at QP 36 now decodes in 25.4 ms**, against 445 ms, and QP 24 in
102 ms against 522. The Phase 0 bench kernels predicted about 30 ms for the
same work (K3 11 ms + K4 12 ms plus the co-tenant), so the QP 36 frame is now
*under* the bench's own number and the QP 24 frame is a factor of three away
from it, three quarters of it in Pass A.

#### What the driver's own statistics said

`VK_KHR_pipeline_executable_properties` is present on the part and was unused;
`NXVC_VKD_SHADER_STATS=1` now enables it and prints what the Qualcomm compiler
did with each pipeline. For the QP 24 4:2:0 v1 frame:

| | Pass A, before | Pass A, now | Pass B, before | Pass B, now |
|---|---|---|---|---|
| instructions | 1320 | 1320 | 2197 | **1477** |
| ALU 32-bit / 16-bit | 95 / 946 | 95 / 946 | 66 / 1500 | 47 / 1044 |
| full-precision registers | 33 | 33 | 48 | 41 |
| **overall register footprint** | 6 | 6 | **328** | **6** |
| **scratch (private) memory** | 278 B | 278 B | **840 B** | **342 B** |
| shared memory | 12.0 KB | 15.1 KB | 12.8 KB | 13.1 KB |

The 328 and the 840 B are the whole story of the 40x. Scratch on this part is
off-chip, and a 328-word footprint is one wave in flight. Both came from local
arrays the Qualcomm compiler could not prove constant-indexed and therefore put
in private memory rather than registers — `predictOne()`'s two 17-entry
by-value reference arrays, `idct8_1d`'s `out int y[8]`, and
`planeStoreBase[4]` / `sizeP[4]` / `fullP[4]`.

The last three were also a **correctness** defect on this driver, latent behind
the register pressure the first two created: with the wavefront compiled out,
the driver read planes 1 and 2 of those arrays back wrong, and every 4:4:4
stream decoded with luma exact and both chroma planes carrying prediction and
no residual. Ten of the 152 streams, byte-for-byte reproducibly, under `-O0` as
well as under the shipped pass list, and whether the wavefront was removed by
the driver (a specialization constant), by spirv-opt, or by the preprocessor.
As scalars the arrays cannot be demoted to memory and the sweep is clean.

**The rule this leaves for every kernel in this codec**: on Adreno, a local
array is a memory allocation unless every index is a compile-time constant
after unrolling, and it is not only slow, it is not reliably correct. Prefer
scalars. `NXVC_VKD_SHADER_STATS=1` is how to check.

##### The rule has a floor, and `res0[8]` is under it

Pass B's remaining 342 B of scratch is **278 B of driver baseline plus 64 B**,
and the baseline is not addressable: Pass A reports exactly 278 B with no local
array anywhere in it. The 64 B is `res0[8]` / `res1[8]`, the sixteen residual
values a thread carries from the column pass across a barrier into the
prediction, and all three ways of writing them were built and measured on
device:

| form | instructions | scratch | Pass B QP 24 | Pass B QP 36 |
|---|---|---|---|---|
| `int res0[8], res1[8]` (shipped) | 1477 | 342 B | **22.25 ms** | **15.31 ms** |
| sixteen named scalars | 3062 | 448 B | 24.93 ms | 18.22 ms |
| four `ivec4` lo/hi, the encoder's shape | 2831 | 464 B | 22.46 ms | 15.84 ms |

Both rewrites are *worse*, and the reason is that these sixteen values are
genuinely live across a barrier: the compiler spills them whatever they are
called, and taking the array away only costs it the addressing it was doing
well. Constant-indexing the array in a throwaway probe (every `res0[c]` forced
to `res0[0]`) does drop the scratch to the 278 B floor — which is what says the
64 B is really this array — but that probe computes one value, not sixteen.

So the array stays, and `scripts/shader-lint.py`'s two `loop-local-array-index`
advisories on it are advisories that were followed up and declined on evidence.
The rule is about arrays whose *indices* the compiler cannot fold; it is not a
licence to hand-scalarise sixteen live values.

#### What was not the problem

Measured and ruled out, so the next person does not re-run them:

* **Dispatch count.** A frame is two `vkCmdDispatch` calls, one per pass, and
  `nxvcd_stats::dispatches` says so. Pass A issues one per distinct lane count
  and a normal frame has exactly one. There is no per-dispatch overhead to find.
* **Buffer placement.** Every buffer the kernels touch is `DEVICE_LOCAL`; the
  bitstream, descriptors, tables, records, weights and the tile map arrive
  through one host-visible staging buffer and a `vkCmdCopyBuffer` per region.
  Nothing is read by a shader out of host-visible memory.
* **Pipeline switching.** One Pass A pipeline and one Pass B pipeline are bound
  per frame, from a cache keyed on the frame's shape. Nothing is compiled or
  rebound mid-frame.
* **`spirv-opt`.** The 4:4:4 miscompilation above survived an empty pass list.
  `NXVC_SPV_PASSES` now overrides Pass B's list, as `NXB_SPV_PASSES` already
  does in `bench/`, so that check costs one build.
* **The UNORM store.** It was worth 7% at one QP when Pass B was slower; on the
  present kernel it is 1% *worse* at QP 24 and 1% worse at QP 36. See "The
  UNORM store". It is not where the time was, and it is not where it is.
* **Shared-memory oversubscription.** Padding Pass B's shared allocation to
  31 KB, so no second workgroup could possibly be resident, changed the
  miscompiled output by not one byte.
* **LDS occupancy, and this one closes a line of enquiry.** Pass B's 12.8 KB of
  shared memory is two workgroups per 32 KB SP, and the obvious next move was
  to get it under 10.6 KB for three — by packing the transpose buffer, padding
  the stride against bank conflicts, or splitting luma and chroma into separate
  dispatches. **Pass B is not occupancy-limited and none of that is worth
  doing.** Padding the allocation to 17.6 KB, which forces one workgroup per
  SP, cost 0.09 ms at QP 24 and 0.02 at QP 36; padding it to 30.8 KB, which
  cannot be anything but one workgroup, cost 0.08 and −0.03. If halving
  residency is free then doubling it is worth nothing, and the LDS budget is
  free to be spent rather than saved.
* **The multiplies in the planar predictor.** Halving them (the separable form)
  changed Pass B from 23.549 ms to 23.548. Halving its LDS loads instead was
  worth 0.73 ms. On this part the predictor is a load-count problem, not an
  arithmetic one — which is the same shape as the occupancy result above:
  Pass B is limited by LDS and image traffic, not by ALU and not by residency.

#### Pass A's workgroup shape

Pass A's shared memory is dominated by two tables that do not grow with the
number of tiles in the workgroup — the 8 KB cumulative-frequency sets and the
1 KB scan tables. At 8 tiles x 8 lanes = 64 threads that is 9 KB of fixed cost
against 3 KB of per-tile state, so a 32 KB SP holds two workgroups: two waves,
on a kernel whose inner loop is a dependent chain of shared reads.
`NXVW_PASSA_TPG` is that shape as one number, reaching the GLSL and the C++
that has to agree with it from the same cache variable. **32 is the default:**

| Pass A, 2048 tiles | 8/group | 16/group | 32/group |
|---|---|---|---|
| QP 24 v1 | 153.7 ms | 102.5 ms | **79.8 ms** |
| QP 36 v1 | 26.7 ms | 12.8 ms | **10.1 ms** |

32 used to be blocked: it failed `v42_dir_res_tskip420`, and the earlier note
here guessed that "something in the kernel does not survive four subgroups per
workgroup" and priced it at about 9% of Pass A. Both halves of that were wrong.
It is worth 22%, and it was never in the kernel at all.

**The bug was in the descriptor array's size.** Pass A does not dispatch over
the tile array; it dispatches over a *descriptor* array in which the frame's
tiles are sorted into one group per distinct `nsub_log2`, each group aligned up
to its own tiles-per-group so `vkCmdDispatchBase` can address it with no extra
push constant. A frame that uses all six lane counts therefore pays up to
`tpg - 1` padding slots six times over, and the descriptor and status buffers
are indexed by that padded index — not by the tile index.

They were allocated at `ntiles + 64`. The padding a frame can need is 76 slots
at 16 tiles per group and 152 at 32, so **the allowance was already short at
the shipped shape** — it just needs a frame using all six `nsub` values to
show, which none of the 56 vectors does. `v42_dir_res_tskip420` is six tiles
using four lane counts, which at 16 tiles per group needs 50 descriptor slots
against an allowance of 70 and fits, and at 32 needs 98 against 70 and does
not: the copy overruns the buffer and Pass A reads tile headers out of range.
It reproduces bit-identically on lavapipe, which is what says it was never a
driver quirk, and `--lds` makes no difference, which is what says it was never
the ballot.

`nxs_desc_slots()` derives the bound from `NXVW_PASSA_TPG` instead, so it
follows the shape rather than having to be remembered. 64 still hangs the
device and is still not a supported value.

#### What remains

Pass A at QP 24 is 79.4 ms of a 102 ms frame, and it is a serial rANS chain:
2048 tiles x 8 lanes, each lane stepping its own symbols with a ballot at every
renormalisation point. Widening the workgroup bought the occupancy that was
available; the chain length did not move. Two measured routes out, in order of
what they are worth:

* **`ENTROPY_LITE`** (`exp/entropy-lite`, tool bit 24) removes the chain
  instead of shortening it. Timed on this device with `nxvc-passA-test`, 2048
  tiles, the same coefficients coded both ways:

  | | payload | Pass A (ballot/sparse) |
  |---|---|---|
  | rANS | 1.25 MB | 138.5 ms |
  | `ENTROPY_LITE` / FIXED | 1.87 MB | **18.4 ms** |

  **7.5x on the target part** for +49.6% bits, zero mismatches against the CPU
  model in both read-pointer modes and both coefficient layouts. The desktop
  measurement predicted 4.1x; the part where entropy is the whole cost gives
  nearly twice that. (That 138.5 ms was measured at 8 tiles per group; the
  ratio has not been re-measured at 32, and the `ENTROPY_LITE` arm has no
  reason to have moved, so the multiplier is probably now nearer 4x than 7.5x
  — but with Pass B at 22.3 ms it is still a **~41 ms** v1 frame.)
* ~~**The 32-tiles-per-group bug**~~ — found and fixed; see "Pass A's
  workgroup shape". It was worth 22%, not the 9% guessed here.

Pass B's remaining 22.3 ms is the place to look after that, and the two
experiments above narrow it a great deal. It is **not** occupancy (halving
residency is free), **not** ALU (halving the predictor's multiplies is free)
and **not** private memory (342 B of which 278 is the driver's own floor). It
is traffic. Ablations on the QP 36 stream, where the coefficient path is
almost entirely skipped and 15.3 ms is nearly pure fixed cost:

| stage | cost | how it was measured |
|---|---|---|
| the two-plane 4:2:0 image store | **~2.6 ms** | 16 → 1 luma iterations, 4 → 1 chroma |
| the planar predictor | ~3.5 ms | `bilinearMeans` → `sMeans[0]`, less what has since been taken back |
| everything else | ~9 ms | DC-plane IDCT, its barriers, the sample-store write and read-back, the per-plane loop |

The store is 4096 one-byte `r8ui` stores plus 1024 `rg8ui` per tile, and it is
the *count* that costs, not the 12.6 MB: a wider store format — luma as
`rgba8ui` at a quarter of the width, four pixels per `imageStore` — is the
obvious next lever, and like the UNORM switch it is an ABI change, so it needs
`nxvc_vkd_images::format` and a consumer that reads it.

The 9 ms is the largest single unexplained block left and has not been
subdivided. The most promising structural move in it is fusing the prediction
with the store on the `kOutYcbcr420` path: at `res_level` 0 with no colour
transform the stored luma sample *is* what the prediction wrote, so the sample
store's write and read-back — 4096 LDS words each way per tile — are pure
round trip. It is a fast path conditional on format and `res_level`, which is
why it was scoped out of this round rather than attempted.

### Pass A's barrier removal is a 3.5x regression on Adreno

`perf(passA): the round loop's three barriers were only ever real off wave64`
replaced one workgroup-uniform round loop with two, so the ballot path's exit
test comes from a cluster-local `subgroupBallot` rather than from a
workgroup-wide barrier. It measured 2x better on lavapipe, unchanged on RADV,
and it predicted the Adreno 650 would behave like RADV wave64, "because the
workgroup is 64 threads, so where the subgroup is 64 wide the workgroup is one
subgroup and the driver elides `barrier()` outright".

The Adreno 650's subgroup **is** 64 wide, and it does not behave like RADV.
Same frame, same binary except for `rans_decode.comp`, six runs in both
orders so clock ramp cannot explain it:

| Pass A, QP 36 v1, 2048 tiles | before | after |
|---|---|---|
| Adreno 650 | **20.3 / 21.0 / 21.0 / 20.3 ms** | **72.2 / 72.8 / 74.2 / 66.3 ms** |
| RADV wave64 (from the commit) | 0.671 ms | 0.645 ms |
| lavapipe, 512 tiles (from the commit) | 64.5 ms | 16.5 ms |

**3.2 to 3.6x slower on the target part**, and correct throughout — the
sweep passes on Adreno with the current kernel. So this is a
performance regression, not a conformance one, and it is the single largest
one measured here: 52 ms of a frame, against a Pass A that was 20 ms.

The lesson is the same one the descriptor pool taught. "The driver elides
`barrier()` on a one-subgroup workgroup" is a statement about RADV that was
generalised to a second wave64 device without a second wave64 device in the
room. `READ_PTR_MODE` already selects between the two loops, so the fix has
somewhere obvious to go; which of the restructuring's parts costs the Adreno
compiler this much has not been bisected.

### `eyes = 2` is refused, and it is not just the tool mask

`nxvc_vkdec_parse.cpp` rejects `eyes != 1` as `UNSUPPORTED`, which
`docs/SYNTAX.md` 12 lists as a Phase 1 decoder's duty. The reference decoder
has moved past that — `codec_impl.inc` accepts `eyes` 1 or 2 — so the two
disagree today, and the disagreement is demonstrable rather than theoretical:

```sh
nxv-enc --in sbs.yuv --out sbs.nxv --w 256 --h 128 --eyes 2 --qp 24
nxv-dec    --in sbs.nxv --out ref.yuv    # 1 frame(s), 256x128 yuv420p
nxvc-vkdec --in sbs.nxv --out gpu.yuv    # stream header: unsupported
```

**Deleting the check would not make it work.** A stereo frame is `eyes`
pictures, not one double-width picture (`docs/SYNTAX.md` 3.3), and nothing in
this decoder carries the eye dimension:

* `si.tiles_x/tiles_y/tile_count` are computed per eye, and the frame's tile
  rows are iterated `tiles_y` times, not `eyes * rows`, so half the tile-row
  headers of a stereo frame would never be parsed;
* the output images are created at `si.width x si.height`, while the reference
  writes planes of `width * eyes` (`codec_impl.inc`: `*w = d->g.width *
  d->g.eyes`), so the readback layout would not match either;
* Pass B's `imageW` and `tilesX` push constants follow the same per-eye
  numbers.

The cheap shape is real but partial: with `tilesX = eyes * cols_per_eye` and
`imageW = eyes * width` the existing raster machinery covers a stereo frame
unchanged — **but only when `width` is a multiple of 64**. Otherwise each eye's
last tile column is partial and eye 1 starts at pixel `width`, not at
`cols_per_eye * 64`, so the merged grid is not a raster of 64-pixel columns and
the tile-to-pixel mapping needs a per-eye x origin. That is the actual work,
and it is Phase 2's to do alongside `STEREO`.

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

### Realigning to bitstream minor version 6

The tournament merge (`docs/MERGE-REPORT.md`) added seven tool bits and made
two of them -- `XFORM_4X4_SPLIT` and `INTRA_CFL` -- part of the reference
encoder's **default** configuration. That is why the realignment was not
optional: on `merge-main` this decoder refused every synthetic stream in its
own conformance sweep at the handshake, 60 skipped and 85 failing.

| where | change | why |
|---|---|---|
| `passA/*` | `kPhSplit`: a coded block unit of a tile whose word1 bit 28 is set codes a 1-bit flag between its CBF and its LAST, and a set flag redirects the unit's scan to `kScan4Split` and bands the LEVEL context by position *within* the 4x4 sub-block | `docs/SYNTAX.md` 6.8 / 9.3, tool bit 19. Both are one line, because the scan id and `band_pos()` were already the only two things the storage and the banding went through |
| `passA/*` | binding 6 grows from 32 to 40 uints per tile: the split flags are one BIT per block after the mode words, written with an `atomicOr` | a split flag exists whether or not `INTRA_DIR` does, so it cannot be a fifth mode field; and unlike a mode word the split word IS shared between lanes, because block `b` belongs to lane `b % LANES` |
| `passA/*` | the mode unit's alphabet is a field, 9 or 10 | `INTRA_CFL` is a CHROMA mode, so the alphabet is per plane |
| `passA/*` | 27 contexts, and two registers of per-lane neighbour class carried inside one plane's run of block units | `docs/SYNTAX.md` 9.9, tool bit 25. Every input is a value the lane has just decoded, so there is no cross-lane read and not one extra `barrier()` |
| host | a transmitted table set is variable length under `TAB_V2`, so all the sets are read through one `BitR` padded to a byte boundary once at the end | `docs/SYNTAX.md` 9.4, tool bit 26. Reading both forms through the same reader is what keeps them one piece of code |
| `passB/*` | the 4-point inverse transform, and the split block's four sub-blocks | `ref/src/transform.cpp` `idct_block(n = 4)`. Its shift chain is the 8x8's unchanged: the 4-point graph has one butterfly level fewer AND one fewer sqrt(2) of gain, and the two cancel |
| `passB/*` | chroma from luma: the model is fitted once per block from the same reconstructed neighbours the other predictors read, then evaluated per sample | `docs/SYNTAX.md` 7.7. The tile's reconstructed luma is still in its plane slot when the chroma planes decode, because the slots coexist |
| host | `kToolsSupported` gains bits 19, 24, 25 and 26 | which is the whole point: the skip count is what it buys |

`XFORM_LARGE` (bit 27) was **half done and not offered** at that point: Pass A
carried the per-plane transform edge, the scan-*group* scaling of the LAST
classes and the LEVEL bands, the two computed zigzags and the wider
unit-length field, and Pass B still reconstructed only the 8x8 transform. It
is now done; "The transform size, priced" below is the other half and what it
costs.

Pass B's CPU model carries both new tools, and the split transform is pinned
against the normative one: `vk.passB.ref_conformance` runs
`ref/src/transform.cpp`'s `idct_block(n = 4)` against the model at all four
sub-block positions with saturating inputs, and checks the model's open-coded
subsampled weight index against ref's `weight4()`. Every sub-block position is
checked because the thing that goes wrong at 4x4 is never the butterfly -- it
is which quadrant a sub-block occupies, which entry of the 8x8 weighting
matrix its coefficient takes, and the two transposing passes.

The model spells the split transform very differently from the kernel -- four
whole sub-blocks with a 16-entry scratch, against the kernel's two columns per
thread and eight live values -- and deliberately so: the model is the readable
statement of the arithmetic, and the kernel is that arithmetic scheduled for a
part where the live set is the constraint.

The Pass A and Pass B CPU models track their kernels line for line, as before,
and `vk.passA.*` and `vk.passB.*` stay green on RADV and lavapipe.

### The transform size, priced

`XFORM_LARGE`, tool bit 27, `docs/SYNTAX.md` 6.7. The tile header names one
transform edge -- 8, 16 or 32 -- capped by each plane's own coded extent, and
every block grid in the plane follows it: the DC plane is `nb x nb` with its
second-level transform firing only at `nb == 8`, the planar interpolation
reads block centres at `bsize` spacing, the weighting matrix is the same
transmitted 8x8 one replicated, the scan is the zigzag of the block, and the
directional predictors are 7.4's formulas with the block edge left as `n`.

**What Pass B does with it.** `ref/RESULTS-xform-a.md` 5 called the shape and
it survived: **one thread per 1D transform**, not four threads per block. The
odd half of a 32-point transform is a dense 16 x 16 product over all sixteen
odd coefficients, so splitting one transform across threads means either
duplicating the 16-point even half or an unbalanced 3.4-to-1 split. The counts
fit the workgroup exactly once -- at `bsize >= 16` a plane has at most
`nb*nb*bsize <= 256` rows and half that many column pairs -- so neither pass
loops and no thread owns two blocks.

Three things about it are not in that plan and are the whole of the work:

* **The intermediate is stored un-transposed and read back transposed**, which
  is the opposite of what the 8x8 path does. A pass-1 thread owns source row
  `r` and writes the block's row `r`: `bsize/2` whole uints, contiguous. Store
  the transpose instead, as the 8x8 path does, and pass 1 writes a *column* --
  half of every uint across the block's width, and a race with the
  neighbouring row's thread. Pass 2 then reads a column of that array, which
  is a read and races with nothing.
* **The column pass lands its output in the plane slot, not in registers.**
  Pass 2's index `r` produces column `r` of the block, so the two indices a
  thread runs are the two columns it already owns for the prediction *and* the
  two halves of the same `bsize` uints -- private to that thread. It runs
  column `2s` reading only the low halves and writes them back leaving the
  high halves alone, then runs column `2s+1` on those. So `res0[8]` stays
  eight at every transform size and an n x n block costs **no** residual
  registers; the prediction and the wavefront read their residual out of the
  slot where the wavefront wanted to stage it anyway. `RESULTS-xform-a.md`'s
  register estimate is low precisely because it forgets this term.
* **The reference arrays are sized per build variant, and the obvious
  alternative to them is a trap.** `predictOne()` takes
  `int A[NXVW_INTRA_REFS], L[NXVW_INTRA_REFS]` by value; 7.4's arrays are `2n`
  long, so that is the 17 ints each the 8x8 module always had and 65 only in
  the module where a 32x32 block can occur. Reading each reference through
  `dirAt()` where it is used instead -- which removes the arrays entirely, and
  which RADV compiled *smaller* (22346 instructions and 84 VGPRs down to 21410
  and 72) -- is a **correctness** failure on an Adreno 650. A predictor has
  about twenty-five reference sites and `dirAt()` inlines the whole DC-plane
  bilinear behind its fallback, so the wavefront module went to 9630
  instructions, an 892-word footprint and 1660 B of scratch, and all 123
  streams that reach the wavefront came back wrong -- including every stream
  that has nothing to do with this tool. That is the second time in this
  section that a change which is free or better on RADV is a defect on the
  target part, and it is why the Adreno sweep is part of finishing a tool and
  not part of tuning one.

**LDS is unchanged.** The transpose buffer is a whole plane and always was:
8192 B for luma, 2048 B per 4:2:0 chroma plane, 12288 B for the tile. The
normative `clamp16` after pass 1 (6.3) is what keeps it int16 at every size.

#### It is a build variant, and that was measured twice

`NXVW_XFORM_LARGE` joins `NXVW_INTRA_DIR` as a preprocessor variant, so Pass B
is now four modules of one source indexed `[intra_dir][xform_large]`. It was
written as specialization constant 7 first -- which is what `kSplitTool` is,
and what worked there -- and on an Adreno 650 that cost an **8x8-only stream
34 % of Pass B**, 16.4 ms to 21.9. Two separate causes, and only the second is
about the constant at all:

| | instructions | 16-bit ALU | flow control | footprint | scratch | Pass B, QP 36 v1 |
|---|---|---|---|---|---|---|
| `merge-main` | 1531 | 722 | 0 | 0 | 352 B | **16.2 ms** |
| specialization constant 7 | 1727 | 1513 | 14 | 6 | 390 B | 21.9 ms |
| build variant, first cut | 1724 | 1511 | 14 | 6 | 406 B | — |
| **build variant, scans gated** | **1527** | **719** | **0** | **0** | **352 B** | **16.4 ms** |

* **`nxvw_scan_pos()` is called once per COEFFICIENT**, and the two new arms
  for the 16x16 and 32x32 zigzags sat in it unconditionally. Two more compares
  in the innermost loop of the pass were most of the damage, and they are why
  the build variant on its own changed nothing: the cost was never behind the
  constant. `NXVW_SCAN_LARGE` gates them, and the CPU model always has them.
* **Everything in a plane hangs off `lb`**, and `lb` was
  `kXformLarge != 0 ? nxvw_block_log2(...) : 3`. Left a specialization
  constant, the block edge, the block grid, the coefficient count, the thread
  mapping and every loop bound in the plane stay runtime values and nothing
  looks dead to the driver's dead-code pass. As a preprocessor variant `lb` is
  the literal 3 and glslang folds the lot.

The second is the same lesson as "the minor-6 realignment" above, one level
further in: **on this part, code you never execute is not free -- and a
constant the compiler cannot see through is not a constant.** The 8x8-only
module is now instruction-for-instruction what `merge-main` compiled, four
instructions fewer, and the Pico measures it at 16.35 ms against 16.23.

#### What it costs

RADV, 2048 tiles (two 2048x2048 eyes at 4:2:0), QP 36, `INTRA_DIR` off, best
of 30, the same stream encoded four ways:

| `--xform` | payload | Pass A | Pass B | GPU total |
|---|---|---|---|---|
| 8 (no tool bit) | 182 020 B | 0.539 ms | **0.176 ms** | 0.723 ms |
| 16 | 157 580 B | 0.375 ms | **0.450 ms** | 0.833 ms |
| 32 | 185 224 B | 0.469 ms | **0.530 ms** | 1.006 ms |
| auto (per-tile RD) | 175 462 B | 0.395 ms | **0.340 ms** | **0.741 ms** |

**The encoder's own per-tile choice is the fastest of the four**, and faster
than 8x8 end to end: it picks a large transform on the tiles where it also
saves rate, so Pass A shrinks with the payload while Pass B pays the extra
arithmetic on only some of the tiles. At a *fixed* size Pass B is 2.6x and
3.0x, against 3.4x and 7.5x the multiply count -- the tool removes rounds and
barriers with one hand while adding multiplies with the other, and 6.7.1 says
so. The `--bench` sweep's own arm (`--bench-xform`, QP 24, both `INTRA_DIR`
settings) shows the same shape and the wavefront half of it, which is the
larger of the two effects:

| | 8x8 | 16x16 | 32x32 | auto |
|---|---|---|---|---|
| `INTRA_DIR` off | 0.129 ms | 0.299 (2.33x) | 0.551 (4.29x) | 0.533 (4.14x) |
| `INTRA_DIR` on | 1.018 ms | 1.446 (1.42x) | **1.061 (1.04x)** | 1.093 (1.07x) |

With the wavefront on, 32x32 is 1.04x 8x8 rather than 4.29x: the wavefront
falls from 22 steps to 4 and gives back almost exactly what the 7.5x multiply
count costs.

RADV compiles the large module at 120 VGPRs against 72, with **no scratch**.

#### On the Adreno 650 it is a desktop tool, and the reason is one number

Same protocol as "Timing on the Pico 4" -- 12 repeats, 20 s idle before, 15 s
after, Pass A as the thermal control:

| `--xform` | Pass A | Pass B | GPU total |
|---|---|---|---|
| 8 (no tool bit) | 13.81 ms | **16.73 ms** | 30.54 ms |
| 16 | 18.57 ms | **168.5 ms** | 194.8 ms |
| 32 | 52.29 ms | **162.9 ms** | 245.9 ms |
| auto | 38.70 ms | **151.4 ms** | 225.0 ms |

The 8x8 row is the control and it is `merge-main`'s: 16.73 ms against 16.23
and 16.39 measured either side of it, which is inside the protocol's noise.
The other three are 10x, and **on this driver they are also WRONG** -- the 24
conformance streams that reach the module come back with most of the picture
zero. It is the documented failure mode, not a new one: a large private-memory
footprint on this part "is not only slow, it is not reliably correct", and the
module reports **440 words of overall register footprint and 1208 B of
scratch** against 0 and 352 for the 8x8 one.

It is not the arithmetic. Scratch on this part is off-chip, and this
document already prices a 328-word footprint at one wave in flight. The
32-entry transform vector and the even/odd recursion's `od[16]` cannot be
scalars -- a dense 16-wide rotation against `kOdd32` is an indexed dot product
and has no scalar spelling -- so they go to private memory and stay there.

Three things are worth recording about the route here, because all three are
`docs/ADRENO-RULES.md` in action and two of them were worth a factor of two
each:

* the first spelling, `void idct32_1d(int x[32], out int y[32])` with
  `int xe[16], e[16]` inside, is four 128-byte function-scope arrays plus
  glslang's copy-in/copy-out of both parameters, and the Adreno driver
  **refused to compile it at all**: `vkCreateComputePipelines` returned
  `VK_ERROR_INVALID_SHADER_NV`, with and without `spirv-opt`. Rewriting both
  transforms to work in place on one vector is what made it compile;
* taking the 16-point transform's even half out of an array and into eight
  scalars with its output loop unrolled -- so that only `od`, the operand of
  the rotation itself, is still an array -- took the footprint from 996 to 440
  and the scratch from 1764 B to 1208, and Pass B from 390 ms to 168 at 16x16
  and 492 to 163 at 32x32. It did **not** make the output correct, which is
  what says the remaining footprint is still over whatever this driver's
  threshold is rather than that some other thing is wrong: RADV and lavapipe
  compile the same source to the same bits as `ref/` on all 228 streams;
* the fallback `docs/SYNTAX.md` 6.7.1 offers -- "stage the coefficient vector
  through a second LDS buffer" -- is the obvious next lever and is **not**
  taken here. At 32x32 only 128 threads are live in the row pass, so 128 x 32
  int32 is 16 KB on top of the tile's 12.5 KB: it fits a 32 KB SP for 4:2:0
  and does not for 4:4:4, which makes it a real design question rather than a
  patch, and it wants measuring against this table rather than guessing.

**So bit 27 is offered, and on the Adreno 650 it is not yet safe to send.**
That is a stronger statement than `INTRA_DIR`'s -- that tool is correct on the
device and merely slow -- and it is the open issue this round leaves. The
decoder's tool mask is a property of the build, not of the device, so a
headset build that must refuse the bit has to say so at the handshake; until
the footprint comes down, the encoder should not select `xform_size != 0` for
an Adreno client. Nothing about this is in the bitstream: the same stream
decodes bit-exactly on RADV and on lavapipe.

---

## The Adreno 650 estimate

> **Superseded, 2026-09-05.** An Adreno 650 has now been measured; see "Timing
> on the Pico 4" above. Assumption A1 below is wrong by about 40x and every
> scaled-compute figure in this section is far too optimistic. The section is
> kept for its *method* — the estimate is the larger of a scaled-compute term
> and a bandwidth term — and because the bandwidth half held up while the
> compute half did not. Read the numbers as an illustration of the method, not
> as figures for the part.

`docs/SYNTAX.md` 7.6 asks for "a *measured* Pass B barrier cost on the target
part, and that number does not exist yet". It still does not: **no Adreno 650
has been measured**, and everything in this section is an estimate from the
two ICDs that have been. The assumptions are stated first because they are
doing all the work.

**A1 — compute and serialization scale by 20–30x from the 7900 XTX.** That is
the range the brief gives for the part. It is a throughput ratio applied to a
kernel whose problem is serialization, and a serialization-bound kernel
usually scales worse than throughput, so 20–30x is a floor rather than a
bracket. Treat every scaled figure here as optimistic.

**A2 — memory is 25 GB/s, shared.** PAPER 3.1's figure for the class of part.
A discrete 7900 XTX has 960 GB/s, so bandwidth that is invisible on RADV is
38x more expensive there.

**A3 — the frame is 2 x 2048x2048 at 4:2:0, 2048 tiles, at 90 Hz**, so the
budget for the whole decode is 11.1 ms.

**A4 — lavapipe bounds nothing, but it separates two things RADV cannot.**
llvmpipe's "workgroups" are CPU tasks with no barrier cost worth the name and
no occupancy to lose, and it runs on a CPU whose bandwidth per lane is far
closer to a phone's than a discrete GPU's. Where a change helps on RADV and
not on lavapipe it was an occupancy change; where it helps on both it moved
work or bytes. That is the only thing lavapipe is used for below.

### The two terms

The estimate is the larger of a scaled-compute term and a bandwidth term,
because they overlap only partly and neither is a bound on the other.

Bandwidth, per frame, at 25 GB/s, QP 24 with the two-plane 4:2:0 store:

| | dense | sparse |
|---|---|---|
| bitstream read | 1.3 MB | 1.3 MB |
| coefficient write + read | 51.2 MB | **23.2 MB** |
| unit lengths, modes, records | 0.4 MB | 1.4 MB |
| output write | 12.6 MB | 12.6 MB |
| **total** | 65.5 MB = **2.6 ms** | **38.5 MB = 1.5 ms** |

At QP 36 the sparse total is 15.3 MB = 0.6 ms against the same dense 2.6 ms.

Scaled compute, Pass B only, 2048 tiles at QP 24, before and after this work:

| schedule | rate | RADV | at 20x | at 30x |
|---|---|---|---|---|
| `INTRA_DIR` off | -22.5 pts worse | 0.255 → **0.241 ms** | 5.1 → **4.8 ms** | 7.7 → **7.2 ms** |
| 0 — as written | — | 1.780 → **1.183 ms** | 35.6 → **23.7 ms** | 53.4 → **35.5 ms** |
| 1 — no above-right | +0.24 % | 1.310 → **0.885 ms** | 26.2 → **17.7 ms** | 39.3 → **26.6 ms** |
| 3 — no above-right + 32x32 | +1.8 % | 1.041 → **0.732 ms** | 20.8 → **14.6 ms** | 31.2 → **22.0 ms** |

and Pass A, which the same scaling makes the larger pass:

| QP | RADV | at 20x | at 30x |
|---|---|---|---|
| 36 | 0.34 → 0.54 ms | 6.8 → 10.8 ms | 10.2 → 16.2 ms |
| 24 | 1.12 → 1.26 ms | 22.4 → 25.2 ms | 33.6 → 37.8 ms |

**The compute term dominates the bandwidth term by an order of magnitude in
every row.** That is the finding, and it contradicts what this document and
ADR 0025 assumed: sparse coefficients were called "the larger lever" and they
are not. Even dense, the whole memory system is 2.6 ms of an 11.1 ms budget;
the wavefront alone is 24–36 ms. The sparse layout's real contribution was the
per-unit length, which let an uncoded unit skip its transform — a compute
saving that happens to arrive with a bandwidth saving attached.

### What that says

* **A v1 stream (`INTRA_DIR` off) fits, and comfortably.** Pass B at 4.8–7.2 ms
  plus Pass A at 6.8–16.2 ms at QP 36 is 11.6–23.4 ms against 11.1 — over
  budget at the pessimistic end, and **Pass A is now the reason**, not Pass B.
  That is a different problem from the one this document described a version
  ago, and a more tractable one: Pass A's three barriers per scheduling round
  are an implementation choice, not a syntax property.
* **`INTRA_DIR` still does not fit on that part.** The cheapest schedule is
  14.6 ms of Pass B alone at the optimistic end. It was 4–8x over budget; it is
  now 2–4x over. Directional intra remains a desktop-decoder tool, negotiated
  by capability exactly as ADR 0025 decided, and the negotiation costs nothing
  to specify because `docs/SYNTAX.md` 2.3 already intersects capabilities.
* **The sparse layout is worth having on Adreno even though it is not the
  lever it was billed as.** 23.2 MB against 51.2 MB is 1.1 ms of a shared
  memory system that the display controller and the reprojection shader are
  also using, and unlike the compute terms it is a *measured* ratio rather
  than a scaled one.
* **What to measure first on a real Adreno 650**, in order: Pass A with
  `INTRA_DIR` off at QP 36 (the number the Pico 4 stream actually depends on),
  the tile-sort delta (the one knob whose sign is still unknown), and the
  sparse-versus-dense delta, which A2 says should flip sign from the -5 % it
  costs on RADV.

## Which wavefront should v1 adopt

The recommendation below predates this work and is unchanged by it: every
schedule got 30 % cheaper, in the same proportion, so nothing about the
ordering moved.

### The recommendation: adopt restriction A, and only A

**Narrow `INTRA_DIR` to drop the above-right reference (schedule 1) and make
that the v1 derivation.** Two reasons, one of them the interesting one.

*The trade is ten times better than the next one.* Restriction A buys 0.30 ms
of Pass B for 0.24 % of rate — **1.24 ms of decode per percent of rate**.
Adding the 32x32 sub-tile restriction buys a further 0.15 ms for a further
1.56 % — **0.10 ms per percent**, a twelfth as good. The first restriction is
where the whole of the return is, which is exactly what 7.6 predicted from the
rate side ("essentially free — a quarter of a percent for a third of the
barriers") and is now true from the time side as well. Both figures fell by
about 30 % with the occupancy work and the ratio between them did not move, so
there is still no operating point at which you would want B without already
having taken A.

*And the extra 0.15 ms does not rescue anything.* At 2048 tiles and 90 Hz,
**no variant fits an Adreno 650** — the cheapest is 14.6 ms against an 11.1 ms
budget for Pass B alone, before Pass A, and Pass A at QP 24 is another 25–38 ms
scaled. Directional intra was 4–8x too expensive on that part and is now 2–4x
too expensive; a 1.8 %-rate schedule change moves it from 2.7x over to 2.2x
over. Paying rate to close a gap you are not going to close is still the wrong
purchase. The one corner where it changes an answer is a *single* eye at 20x
(7.3 ms for schedule 3 against 8.9 ms for schedule 1, both now inside the
budget) — and a corner that narrow should be decided on a real Adreno
measurement, not on a 20–30x guess.

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
The free one has now been taken: **more threads per block**. The occupancy
numbers in 7.6 were 4 threads per 8x8 block against a 256-thread workgroup;
they are 16 now, the schedules run at 18–57 % rather than 4.5–14 %, and the
wavefront cost about 30 % less at every schedule for no rate at all. What
remains is:

* **Restriction C, the 16x16 super-block**, which 7.6 models at 4 steps
  combined with 32x32 sub-tiles and leaves unpriced. It is the only entry in
  the menu that changes the *prediction distance* rather than the dependency
  graph, so its rate cost genuinely cannot be extrapolated from the others —
  but 4 steps against 22 is the only remaining order-of-magnitude in the
  wavefront.
* **Splitting the reference construction across a block's threads.** All 16
  threads currently build the same `A[17]`/`L[17]`, and `dirBase()` inside it
  recomputes the DC-plane prediction sample by sample. It costs a second
  barrier per step to fix, which is why it has not been tried; against 65
  barriers per tile that may still be the right trade.

The first costs rate; the second does not, and the second should be measured
first.

---

## Open issues

* **Do not reach for `VK_EXT_global_priority`, in any combination.** It
  measured as the largest lever available -- HIGH on the decode queue took
  submit-to-fence from 11.87 ms to 6.63 ms with the live session running, twice,
  agreeing to 0.03 ms -- and on a headset it produced a **10x regression**:
  Pass B, which had never exceeded 4.4 ms, read 34-165 ms, and the session fell
  to 3-19 frames per two seconds.
  The bench was measuring the wrong thing. Global priority on this driver is
  effectively per PROCESS, not per queue: raising the co-tenant's queue helps
  the decode exactly as much as raising the decode's own. So all the win ever
  was is *beating another application's context* -- in the bench, the live WiVRn
  app; in the client, the **XR compositor**, which is a separate process
  (`PxrMetric` in logcat is its metric line). Beating the compositor means it
  misses vsync, frames back up, and the decode's own timestamps balloon because
  its dispatches sit preempted mid-flight, which is exactly the shape that came
  back. With no competing application at all the extension changes nothing:
  queue wait 3.10 ms without it against 3.20 ms with it.
  The measurement that settles it is the compositor's, not the decoder's, and
  `nxvc-vkdec-wrap --render-hz` plus `PxrMetric` is how to take it.
* **The workload does not fit, and that is the real finding.** On an idle
  Adreno 650 one 22 KB 1088x1088 eye is 6.2 ms of GPU, so two eyes at 90 Hz ask
  for 12.4 ms of GPU per 11.1 ms of wall -- 112 % of the part, before the
  compositor's own 4.3-5.9 ms of ATW per frame (`PxrMetric` `ATWGPU`, 51-62 %
  GPU at 90/90). The decoder bench alone, at default priority, drives compositor
  FPS from 90 to 41 and the GPU to 99 %. No scheduling policy fixes a workload
  that does not fit; it only chooses which side loses. The levers that remain are
  the ones that make the decode SMALLER -- fewer bytes per frame, at 0.20 ms per
  KB of Pass A -- and not the ones that reorder it.
* **The live decode wall is about 3.5x the codec's own GPU time**, and the
  gap is not in `nxvc`. A 48.6 KB 1088x1088 frame is 12.0 ms of GPU and
  12.9 ms of wall standalone on the Pico 4; the WiVRn client reports ~47 ms
  for the same shape. The suspects are all on its side of the ABI: one
  graphics queue shared with the renderer and with the other eye's decoder,
  and two host fence round trips per frame because this driver refuses a
  timeline semaphore. `NXVC_VKD_SUBMIT_SIGNAL_BINARY` removes one of the two
  -- the client can now wait on a binary semaphore between the decode and its
  own copy instead of on the host. A command-buffer ring in `nxvc`, which
  "One frame in flight" already asks for, is what would let the two eyes
  overlap at all.
* **`nxv-enc`'s default `--nsub auto` picks ONE rANS lane per tile at high
  QP**, which is a 4x Pass A regression on the Adreno for a 30 % rate saving:
  a 20 KB QP 51 frame reads Pass A 13.4 ms at `ns0` against 3.6 ms at
  `--nsub 3`. One lane per tile means 289 lanes for the whole frame and a
  serial walk of the tile in each. The GPU encoder fixes `nsub_log2 = 3` and
  is not affected; anything driving the reference encoder for a headset must
  pass `--nsub 3` explicitly. `NSUB_VAR` should probably be renegotiated as a
  decoder-declined tool rather than an encoder-chosen one.
* **`XFORM_LARGE` is wrong on the Adreno 650**, and it is the only tool this
  decoder offers that is not correct on all three ICDs. The 24 conformance
  streams that reach the 16x16 / 32x32 module come back with most of the
  picture zero; RADV and lavapipe decode the same streams bit-exactly against
  `ref/`. The module's private-memory footprint is the suspect and has been
  halved once already without fixing it -- "The transform size, priced" has
  the numbers and the two levers left. **Until it is fixed, an encoder must
  not select `xform_size != 0` for an Adreno client**, and a headset build
  that cannot rely on that has to drop bit 27 from `kToolsSupported`, which
  today is a build-wide constant rather than a per-device one.
* **`vk/common` is not used yet.** See above; the swap is mechanical and does
  not touch the C ABI.
* **One frame in flight.** The decoder owns a single command buffer and
  submits one frame at a time. A ring of command buffers and staging buffers
  is the obvious next step for the streaming client, and the timeline
  semaphore is already the synchronisation point it needs.
  **[inter] This is now a correctness constraint and not only a throughput
  one.** Frame N's Pass B writes the reference-ring slot that frame N+1's
  Pass W reads, so `NXVC_VKD_SUBMIT_ASYNC` followed by another
  `nxvc_vk_decode_frame` without a `nxvc_vk_decoder_wait()` in between is a
  read-after-write across submissions on an inter stream, on top of the
  staging-buffer reuse it already was. The synchronous path -- the default,
  and what every test and the CLI use -- waits, so nothing in the tree can
  reach it; the command-buffer ring is what makes async safe, and it has to
  land before a client uses it.
* **Pass A is now the expensive pass** at every QP above 24, and its fixed
  cost went *up* with the sparse layout (124 → 170 ns per tile). Three
  `barrier()`s per scheduling round, paid to keep control flow uniform for the
  LDS read-pointer fallback, is what `passA/README.md` names as the thing to
  attack.
* **The bitstream buffer grows but never shrinks,** and growing it recreates
  the buffer mid-stream. Harmless for a stream of similar frames; worth a
  high-water-mark policy if a client ever sees one huge IDR.
* **Alpha on the two-plane path costs a second store, not a second dispatch**
  any more (specialization constant 3, `kOutSecond`), which took it from
  2.27 ms to 1.69 ms. An `r8ui` alpha binding on the two-plane path would still
  be cheaper than a whole RGBA8 store if it ever stops being rare.
* **`profile` and `level` in the stream header are carried and reported but
  not enforced.** The reference does not enforce them either.
* **[inter] `eyes == 2` needs a width that is a multiple of 64.** The merged
  eye-pair raster is exact only there; see "The inter path". Lifting it is a
  per-eye x origin in Pass B's store and in the ring store, which is real work
  in `reconstruct.comp` and buys a configuration nothing streams.
* **[inter] The directional wavefront is still 15 ms of an Adreno inter
  frame,** and it is the rolling intra refresh's own tiles paying it. The
  module split took Pass B from 42.9 ms to 21.8 ms; the rest is a real
  wavefront over about fourteen tiles, and the lever left is the wavefront
  itself, not the dispatch shape. "The Adreno 650 estimate" is where that
  belongs.
* **[inter] `passB_reconstruct_ref_tile()` has no direct test.** It is the CPU
  model of the reference-ring store, and the store is checked end to end --
  the inter vectors would not decode if the ring were wrong -- but
  `nxvc-passB-test` builds its own corpus and has no ring to compare against.
  Giving it one is a corpus change, not a model change.
* **[inter] Pass W runs over every tile and exits early on the intra ones.**
  A compacted dispatch list — the inter tiles only, and per eye — is one host
  array and would take the empty workgroups out of a frame that is mostly
  intra, which is every frame right after a refresh.
* **[inter] The ring is four full pictures per eye pair in u16.** At the v1
  configuration that is 100 MB, which is fine on a discrete part and is the
  largest single allocation the decoder makes on the headset. `ref_sel == 3`
  is reserved, so three slots are reachable and the fourth is the one being
  written; nothing smaller is possible without changing 13.2, but a u8 ring
  for a stream with no colour transform (where every plane is 8-bit) would
  halve it.
* **[inter] `pass_w_ms` covers the first Pass W dispatch only.** For every
  frame that does not carry a `STEREO` tile that is the whole of it; a stereo
  frame runs the pair once per eye and the reported number is eye 0's.
* ~~**Pass B is one shader for both predictors.**~~ Resolved: `INTRA_DIR` is a
  build variant, two SPIR-V modules from one source, and on the Adreno 650 that
  was most of Pass B. See reconstruct.comp's note on why it is not a
  specialization constant.
* **Pass B's next lever is the store count, and it is an ABI change.** The
  two-plane 4:2:0 path issues 4096 one-byte `r8ui` stores plus 1024 `rg8ui` per
  tile, ~2.6 ms of Pass B, and the count is the cost rather than the 12.6 MB.
  Writing luma through an `rgba8ui` image a quarter as wide — four pixels per
  `imageStore` — is the obvious move and, like the UNORM switch, changes the
  `VkFormat` that `nxvc_vk_decoder_images()` hands out. It wants the same
  treatment: a consumer that reads `nxvc_vkd_images::format`.
* **Fusing Pass B's prediction with its store** on the `kOutYcbcr420` path, at
  `res_level` 0 with no colour transform, where the stored sample *is* what the
  prediction wrote and the sample store's write plus read-back — 4096 LDS words
  each way per tile — is a pure round trip. It is a fast path conditional on
  format and `res_level`, and it is the largest untried item in Pass B's
  remaining ~9 ms of unattributed fixed cost.
* **The wavefront's per-block reference construction is still redundant.** All
  16 threads of a block build the same `A[17]`/`L[17]`, and `dirBase()` inside
  it recomputes the DC-plane prediction sample by sample. Splitting the
  reference build across the block's threads costs a second barrier per step;
  materialising the base plane costs 8 KB of LDS that 4:4:4 does not have.
  Neither is obviously worth it, and neither has been measured.
* **Sparse coefficients cost about 5 % of Pass B on a discrete GPU** at QP 12
  and QP 24 — the per-coefficient length check — for 2x less traffic. That is
  the wrong side of the trade on a 7900 XTX and the right side on the target
  part, and it is the reason `NXVC_VKD_FLAG_DENSE_COEF` exists.
* **Tile sorting is off by default**, and now for a weaker reason than before:
  it used to be -11 % on RADV and +8 % on lavapipe, and against the cheaper
  Pass B neither delta is stable. It needs the Adreno number.
* **Pass A's shape has only been swept at 8, 16 and 32.** 32 is the default and
  64 hangs the device, but nothing between 32 and 64 has been tried, and the
  gain from 16 to 32 (22 %) was larger than the gain the 8 → 16 step predicted,
  so the curve has not obviously flattened. The descriptor-array bound now
  follows the shape, so a sweep costs one cache variable per point.

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

**Section 13 has since landed here** — see "The inter path" above. Tool bit 23
did turn out to be the one to watch, and in the direction the note predicted:
it is refused at the stream header, which is what makes *every* conforming
version 1 stream bilinear and lets `warp_pred.comp` carry one sampling filter
instead of two. The Catmull-Rom tap table is deliberately absent from the
kernel: carrying it would put a filter selection in the inner loop that no v1
stream can reach.
