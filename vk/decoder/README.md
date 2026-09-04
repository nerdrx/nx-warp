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
actually streams — decoded 30 times, best of run. Informational.

**RX 7900 XTX (RADV NAVI31)**, streams from `nxvc_encoder`'s **default
configuration**, which since v1.3 means `INTRA_DIR` + `CTX_V2` + `SIGN_HIDE`
are all on. "before" is the dense coefficient layout as it shipped in v1.3:

| QP | frame bytes | coef SSBO | Pass A | Pass B | GPU total | wall |
|---|---|---|---|---|---|---|
| 12 | 2.64 MB | 25.6 → **13.6 MB** | 1.63 → 1.44 ms | 1.90 → **1.28 ms** | 3.53 → **2.72 ms** | 3.60 ms |
| 24 | 1.32 MB | 25.6 → **11.6 MB** | 1.12 → 1.26 ms | 1.77 → **1.19 ms** | 2.89 → **2.46 ms** | 3.65 ms |
| 36 | 0.17 MB | 25.6 → **0.93 MB** | 0.34 → 0.54 ms | 1.63 → **1.20 ms** | 1.99 → **1.75 ms** | 2.88 ms |

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
| Pass A, RADV, sparse | 0.35 ms | 0.48 ms | **170 ns** |
| Pass B, RADV, dense | 1.82 ms | ~0 | 890 ns |
| Pass B, RADV, sparse | 0.51 ms | 0.33 ms | **248 ns** |
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

## The Adreno 650 estimate

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

* **`vk/common` is not used yet.** See above; the swap is mechanical and does
  not touch the C ABI.
* **One frame in flight.** The decoder owns a single command buffer and
  submits one frame at a time. A ring of command buffers and staging buffers
  is the obvious next step for the streaming client, and the timeline
  semaphore is already the synchronisation point it needs.
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
* **Pass B is one shader for both predictors.** `INTRA_DIR` is a push-constant
  branch, so a v1 stream pays the 136-VGPR footprint of the wavefront's
  reference arrays even though it never enters them. It costs nothing
  measurable today (0.241 ms against the 0.26 ms of the pre-v1.3 kernel), but
  it is the first thing to check if Pass B ever gets tighter.
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
