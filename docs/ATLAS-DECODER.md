# The GPU decoder under `ATLAS` (tool bit 31)

How `vk/decoder/` implements ADR-0029 and SYNTAX 13.12. The normative rules are
in those two documents -- `docs/adr/0029-atlas-reference.md` and SYNTAX 13.12,
both on branch `atlas` (commit `0b162e5`) and not yet in main; this one is the
decoder's side of them — which buffers
exist, which kernels run, which existing modules survive, which die, what the
client is handed, and what conformance compares.

**Status:** design. Nothing here is implemented. The reference codec (Phase 2,
branch `atlas`) has to produce a byte-identity target before the decoder can be
written against it; the parts that do not depend on that target are listed under
"What can start now".

## Why: the budget

Measured on the Pico 4 (Adreno 650, 490 MHz), 1088x1088 per eye, 289 tiles,
`ENTROPY_LITE`, one eye. `Pass B` as the decoder reports it includes Pass W, so
the rows below separate them. Everything in the "today" column is measured; the
`ATLAS` column is measured where the work already exists in that shape and
marked otherwise.

| | today | under `ATLAS` | |
|---|---|---|---|
| Pass A | 1.534 ms | **~0.64 ms** | measured, 40 fully-coded tiles |
| compose + renorm | — | **unmeasured** | new; 9 divides x 289 tiles |
| Pass W | 0.661 ms | **~0.66 ms** | measured; same kernel, atlas-sourced |
| Pass B | 10.760 ms | **~1.1 ms** + atlas store | measured coded module, 39 tiles |
| **total per eye** | **12.293 ms** | **~2.4-3.0 ms** | |

**4-5x, and essentially all of it is one deletion:** 8.889 of the 10.760 ms is
`reconstruct_skip_store` running the normative integer warp over ~250 skipped
tiles, and under `ATLAS` those tiles are not reconstructed at all. The rest of
the decoder barely moves.

Two honest caveats on that table. The compose dispatch is the one cost that is
new and unmeasured — it is 9 divisions per tile per eye against a Pass W that
already does two per tile per plane and costs 0.66 ms for 38 tiles, so it should
be small, but "should be" is not a measurement and it is the first thing to
price. And the absolutes are from a device at 61-66 C at the end of a long
session; the ratios are what carry.

Not in this budget: the client's own display warp (13.12.5), one sampler pass
over every tile. That is new work on the same GPU, outside the decoder, and it
is the trade the ADR makes deliberately — one non-normative filtered warp at
panel rate instead of 250 normative integer ones at decode rate.

## Buffers and images

Three objects replace the reference ring.

**Atlas pixels.** One image per stream covering every eye, in the coded sample
domain, at full tile extent — byte-for-byte the layout `nxvw_ring_layout()`
already computes for ONE ring slot. So the geometry code is reused verbatim and
`ringSlotU16`, `ringPlaneOff[]`, `ringStride[]`, `planeW[]` keep their meaning;
what goes is the four-slot indexing and `curSlot`. Memory falls 4:1 — one
generation, not four — which is the ADR's argument for `ref_sel == 0`.

It must be **sampleable by the client's display pass**, which the ring is not:
the ring is an SSBO of u16 pairs because only the decoder reads it. The atlas is
read by a fragment or compute pass with a hardware sampler, so it wants to be an
image. That is a real format decision and it is not free — see "Open questions".

**The per-tile table.** 64 B per tile position, one entry for every tile of
every eye: 578 entries at the v1 stereo configuration is 37.0 kB, which fits a
uniform buffer. Written by the decoder, read by the decoder's own compose and
predict passes, and read by the client. Layout is SYNTAX 13.12.1 exactly: nine
i32 of `C`, `src_frame`, `gen`, `flags`, `res_level`, and 20 reserved bytes
that a v1 decoder zeroes and conformance still compares.

**The index is the linear tile index of SYNTAX 3.3, which is row-major and
eye-MINOR:**

```
cols = eyes * cols_per_eye
n    = row * cols + eye * cols_per_eye + tile_index
eye  = (n % cols) / cols_per_eye
row  =  n / cols
```

So the two eyes' entries **interleave within each row**. The table is not two
contiguous per-eye halves and neither is the tile order, and anything that
treats it as "eye 0's tiles, then eye 1's" is wrong -- including a client
display pass that walks the table linearly assuming one eye.
`build_tile_order()` already computes this index today
(`row * cols + eye * cols_per_eye + col`); what it reorders is the DISPATCH, so
a stereo frame's two eye passes are contiguous, and that does not change the
index. The compose kernel, the write-back and the table all address by `n`
directly, so one thread per entry needs no eye arithmetic at all; only the
display pass, which has to know whose pose to compose with, applies the `eye`
formula.

The atlas PIXELS carry the eyes differently, and correctly so: they follow
`nxvw_ring_layout()`, where each plane holds both eyes' sub-pictures side by
side and eye `e` begins at column `e * pw`. So the pixels are one image with
per-eye sub-pictures while the table is one interleaved array -- two eye
conventions in the same feature, which is the kind of thing that gets confused
once and then stays confused. Stated here so it is checked in review rather
than in a picture.

**The pose bytes.** SYNTAX 3.2's 26 bytes per frame, which the decoder already
parses and does not interpret. Under `ATLAS` the client must retain them
alongside the atlas to compose `C_tile . H(pose_t <- pose_N)` for late-latched
display. The decoder's only new obligation is to keep publishing them and to say
which frame number they belong to.

## The kernels

### 1. Compose and renorm (new, ONE dispatch per frame)

One thread per table entry, covering every tile of every eye in a single
dispatch. For each entry with `valid && !static`:
`C := renorm(C . H_N[eye])`, `gen += 1`; `static` entries advance `gen` only.

The homography is per eye and the table is eye-interleaved, so the thread reads
its own eye out of its index -- `eye = (n % cols) / cols_per_eye` -- and picks
`H_N[eye]`. Two eyes' worth of entries in one dispatch is the right shape here
for the same reason one submission beats two decoders elsewhere in this
codebase: 289 entries is already in the starved region of the workgroup-count
curve and splitting it per eye would halve the occupancy of each half.

The arithmetic is SYNTAX 13.12.2 verbatim: two independently rounded partial
sums per element so every intermediate fits `int64`, then
`C'[i][j] = sdiv_round(P[i][j] << 29, P[2][2])`. Nine divisions a tile.

Three things make this cheap on the Adreno rather than another `lite_scan()`:
it is one thread per tile with no shared memory and no barrier; `sdiv_round` is
the same fixed restoring division `warp_pred.glsl`'s `warp_div()` already
implements, so there is one definition of it; and 578 threads for a stereo
frame is one small dispatch. The risk is the opposite of Pass A's — too few threads, not too many —
and the workgroup-count step function of `passA/README.md` applies, so size the
workgroup for the tile count and measure it against that curve.

If the composed matrix leaves SYNTAX 3.1.1's envelope, or `gen` would exceed
`gen_max`, the kernel clears `valid`. That is the whole staleness rule; there is
no separate age cap and no second pass.

### 2. Pass A — unchanged, over coded tiles only

The entropy decode does not change by one bit. What changes is that a skipped
tile carries no payload and no descriptor, so the dispatch covers coded tiles
only: ~40 of 289 rather than 289 of 289. The measured 1.534 ms at 289 tiles is
mostly empty tiles; 40 fully-coded tiles cost 0.641 ms.

`build_tile_order()`'s skip partition (`order_nskip`, the leading WARP_SKIP
range) has nothing to partition and goes. The INTRA_DIR partition stays — that
one is about which Pass B module a tile takes and is orthogonal.

### 3. Pass W — unchanged kernel, atlas-sourced

`warp_pred.comp` and `inter/warp_pred.glsl` are **not modified**. A renormalised
`C` satisfies the same legality conditions as a transmitted `H`, which is the
property the ADR's renorm step exists to preserve, so the predictor consumes it
unchanged. This matters more than it looks: the predictor is the one kernel in
this decoder that must never quietly change, it is pinned byte-for-byte against
the encoder by `vk.encoder.passw.same`, and every attempt to make it faster has
been measured and refused (`passB/README.md`).

What changes is around it. The matrix comes from the co-located tile's `C` in
the table rather than from the frame's `H`; the source is the atlas rather than
a ring slot; `refBase` loses its slot arithmetic. `WARP_MV` reads `C`,
`STATIC_MV` reads identity, `INTRA` reads nothing. Pass W already dispatches
over the coded range only.

### 4. Pass B — coded tiles only

The residual path is untouched: transform, quantiser, scan, contexts, DC plane,
`QUAD_MV`, `tskip`, `res_level`, 4:4:4, alpha, `INTRA_DIR`, `XFORM_LARGE` all
stay exactly as they are. The `reconstruct` module and its dir x xform_large
variants survive unchanged.

**`reconstruct_skip_store` dies.** It exists to reconstruct a skipped tile, and
under `ATLAS` a skipped tile is not reconstructed. With it goes the skip
dispatch segment, `order_nskip`, and the `NXVW_SKIP_ONLY` / `NXVW_SKIP_STORE`
build variants. That module was built three tasks ago and made 17-25 % faster
since; deleting it is the right outcome and the measurements that justified it
are what justify this ADR.

**The ring store becomes the atlas store.** `nxvwRefRingStore()` already writes
the coded sample domain at full tile extent with the `res_level` upsample — the
atlas wants exactly that, at a fixed address instead of `curSlot`'s. What is new
is the metadata write beside it: `C := I`, `src_frame := N`, `gen := 0`,
`static := (mode == STATIC_MV)`, `valid := 1`, `res_level`.

**The display store leaves the decoder.** Under `ATLAS` the normative output is
the atlas, so `store.glsl`'s colour transform, chroma upsample and RGBA/two-plane
stores are no longer part of decoding — they become the client's display pass.
The existing `nxvc_vkd_output` formats stay for non-`ATLAS` streams.

### 5. `NEAR_SKIP` — the one place a skipped tile touches the atlas

A small kernel over the tiles the row header names: one add per sample against
the atlas tile in place, `C`, `src_frame`, `gen`, `static` and `res_level`
unchanged. Nine bytes of DC and ramps, no warp.

## The publish contract

`ATLAS` needs an output mode that hands the client the atlas rather than a
picture. Concretely, alongside the existing `nxvc_vkd_images`:

* the atlas image, view and format: ONE image whose planes hold both eyes'
  sub-pictures side by side, matching `nxvw_ring_layout()`, with `planeW[]` and
  the strides so the client can find eye `e` at column `e * pw`;
* the per-tile table as a buffer handle the client can bind as a uniform buffer,
  with its element stride (64), `cols` and `cols_per_eye`, so the client applies
  the eye-minor mapping above rather than guessing it;
* the frame number and the 26 pose bytes that the table's `src_frame` values are
  relative to;
* the per-tile mode and skip map, which 13.12 makes part of the normative
  output.

The client owns the display warp. It composes `C_tile . H(pose_t <- pose_N)` in
floating point at panel rate and samples the atlas however it likes. Nothing in
that path is normative and nothing in it is tested here.

Synchronisation is the existing shape: the decoder already exposes a binary
semaphore (`nxvc_vk_decoder_binary_semaphore()`) for a client pass that consumes
its output, and `nxvc-vkdec-wrap --mode binsem` already exercises it.

## Conformance

**Conformance becomes byte-identity of the atlas** — pixels and all 64 bytes of
every table entry, including the 20 reserved bytes a v1 decoder zeroes — plus
the per-tile mode and skip map, after each frame. The displayed picture is not
compared, because under `ATLAS` the decoder does not produce one.

That is a harness change, not just a new vector set: the conformance runner
compares a decoded picture's md5 today. Under `ATLAS` it compares the atlas, so
`check_stream()` needs an atlas readback path and the manifest needs an atlas
digest per stream. `nxvc_vk_decoder_read_planes()` reads display planes and is
the wrong shape; the atlas readback is closer to what `--only-vectors` does with
the reference ring in the loss test.

The three existing legs keep their roles: RADV and lavapipe for the two-ICD
cross-check, and **the device run is mandatory, not a formality** — a
subgroup-arithmetic scan in Pass A validated byte-exactly on both desktop ICDs
and silently miscomputed on the Adreno (`passA/README.md`). Two ICDs agreeing is
not evidence about the target part.

## What can start now

Before the reference codec gives a byte-identity target:

1. **The compose kernel and its CPU model.** The composition of 13.12.2 is
   fully specified arithmetic with no dependency on the rest of the atlas: a CPU
   model, a GPU kernel, and a test that they agree exactly over random legal
   matrices and over long composition chains. That is the same shape as
   `passA_model.cpp` against `rans_decode.comp`, and it is the piece most likely
   to contain a rounding bug.
2. **The buffers.** Atlas image and table allocation, the `nxvw_ring_layout()`
   reuse, and the format decision for a sampleable atlas.
3. **The envelope and staleness check**, which is 3.1.1's existing condition
   applied to `C` — reusable and testable on its own.

What must wait: everything with a byte-identity obligation — the write-back, the
atlas-sourced prediction, `NEAR_SKIP` in place, and the conformance leg.

Implementation goes on `atlas-decoder` off `atlas`.

## Open questions

* **The atlas image format.** The ring is u16 samples packed two per uint in an
  SSBO. The atlas must be sampled by the client, so it wants an image — but
  Pass B writes it and `bench/README.md` measured an integer storage image at
  about 3x the cost of a UNORM one on this part, which is why the decoder's
  UNORM store exists and is off by default. A 9-bit YCoCg-R chroma plane does
  not fit 8-bit UNORM. This needs pricing before it is chosen.
* **The compose dispatch's shape.** 289 threads is small enough to land in the
  starved region of the workgroup-count curve.
* **Where the display pass lives.** The client's, by the ADR — but the decoder
  should probably ship a reference implementation, or the first client to write
  one will get the pose composition wrong.
* **`gen_max` and drift.** Default 0 means no cap; the ADR's precision estimate
  is analytic and Phase 2 owes a measurement.
