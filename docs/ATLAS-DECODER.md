# The GPU decoder under `ATLAS` (tool bit 31)

How `vk/decoder/` implements ADR-0029 and SYNTAX 13.12. The normative rules are
in those two documents -- `docs/adr/0029-atlas-reference.md` and SYNTAX 13.12,
both merged into this branch from `atlas` (`af108db`, which also brings the
reference codec, `row_present` and the ATLAS conformance vectors v82-v88) and
not yet in main; this one is the decoder's side of them — which buffers
exist, which kernels run, which existing modules survive, which die, what the
client is handed, and what conformance compares.

**Status:** partly implemented, on branch `atlas-decoder`. What exists and is
verified on RADV, lavapipe **and the Adreno 650**:

| | where | verified by |
|---|---|---|
| the composition of 13.12.2, CPU | `atlas/atlas_model.cpp` | `vk.atlas.compose` (128-bit oracle), `vk.atlas.vs_ref` (the reference codec itself) |
| the compose+renorm kernel, one dispatch per frame over both eyes | `atlas/atlas_compose.comp` | `vk.atlas.gpu_vs_cpu`, three ICDs |
| the lazy per-entry advance, `advanced_to`, the 64-deep `H` ring | same kernel, `sel = LIST` | eager vs lazy, both flushed, byte-identical |
| per-tile matrices from the composed `C`, and the 13.12.3 step 3 write-back | `atlas/atlas_tiles.comp` | against `nxvcvk::plane_homography()` |

| the host bookkeeping -- monotonicity, lazy selection, the ring window | `atlas/atlas_state.h` | `vk.atlas.state` |
| `row_present` (3.1.2), which tool bit 31 streams carry and which is REQUIRED in v1 | `nxvc_vkdec_parse.cpp` | the conformance sweep's `row_present` arm, three ICDs |

What does NOT exist yet: the host integration in `nxvc_vkdec.cpp` (ATLAS mode,
the single-slot atlas, coded-only Pass A/B, the display store turned off),
`nxvc_vk_decode_tiles`, `nxvc_vk_atlas_write_tiles`, the sampled atlas view,
and the conformance leg against the reference's
`nxvc_decoder_atlas_table()` / `nxvc_decoder_atlas_plane()`.

**The stream-header gate is open now and was not before.** Tool bit 31 is
still absent from `kToolsSupported`, so an `ATLAS` stream is refused at the
stream header -- deliberately, until the host integration exists to honour it.
Tool bit 32 `ROW_PRESENT` IS accepted, which it had to be before two of the
v82-v88 vectors could be reached at all -- `v84_atlas_refresh_eff` and
`v86_atlas_row_present` carry it, and the other five do not:

| vector | bit 31 `ATLAS` | bit 32 `ROW_PRESENT` |
|---|---|---|
| `v82_atlas_warp` | 1 | 0 |
| `v83_atlas_420` | 1 | 0 |
| `v84_atlas_refresh_eff` | 1 | **1** |
| `v85_atlas_nbr` | 1 | 0 |
| `v86_atlas_row_present` | 1 | **1** |
| `v87_atlas_base_sourced` | 1 | 0 |
| `v88_atlas_superseded` | 1 | 0 |

Of the two, only `v86` regenerated differently once the encoder could emit the
bitmap. `v84` advertises the tool and elides nothing -- its sequence has no
row that is idle in the whole of it -- so its bytes are unchanged, and it is
worth knowing that it tests the tool no more than the old `v86` did.

## Why: the budget

Measured on the Pico 4 (Adreno 650, 490 MHz), 1088x1088 per eye, 289 tiles,
`ENTROPY_LITE`, one eye. `Pass B` as the decoder reports it includes Pass W, so
the rows below separate them. Everything in the "today" column is measured; the
`ATLAS` column is measured where the work already exists in that shape and
marked otherwise.

| | today | under `ATLAS` | |
|---|---|---|---|
| Pass A | 1.534 ms | **~0.64 ms** | measured, 40 fully-coded tiles |
| compose + renorm | — | **0.0048 ms/eye** | MEASURED ON THE ADRENO 650, see below |
| Pass W | 0.661 ms | **~0.66 ms** | measured; same kernel, atlas-sourced |
| Pass B | 10.760 ms | **~1.1 ms** + atlas store | measured coded module, 39 tiles |
| **total per eye** | **12.293 ms** | **~2.4-3.0 ms** | |

**4-5x, and essentially all of it is one deletion:** 8.889 of the 10.760 ms is
`reconstruct_skip_store` running the normative integer warp over ~250 skipped
tiles, and under `ATLAS` those tiles are not reconstructed at all. The rest of
the decoder barely moves.

**The compose dispatch is no longer the unmeasured line.** `nxvc-atlas-gpu-test
--bench` times it with GPU timestamps over the full 578-entry stereo table --
one dispatch, both eyes -- targeting a fresh frame number each iteration so
every thread takes a real one-step advance and nothing is a null dispatch:

| ICD | median, 578 entries | per eye | runs |
|---|---|---|---|
| RADV NAVI31 | 0.0051 ms | **0.0026 ms** | 300 |
| llvmpipe (LLVM 21) | 0.0107 ms | 0.0054 ms | 200 |
| **Adreno 650 (Pico 4)** | **0.0096 ms** | **0.0048 ms** | 300 |

**The device leg is taken and the target part is the slowest of the three by
under 2x** — 0.0096 ms against RADV's 0.0051, best 0.0091, at gpuclk 490 MHz
with gpuss-max-step 43.2 C before the run and 51.8 C after. The worst of the
300 is 1.2949 ms and it is the first iteration: pipeline warm-up, which is why
the median is what the table reports.

Median and best rather than the mean: a headset's clocks move under a long run
and the mean becomes a number about thermals rather than about the kernel. On
**all three parts, the Adreno included**, it is **three orders of magnitude**
below the smallest line in the table — 0.0048 ms/eye against Pass W's 0.66 —
which settles the "9 divisions a tile" worry, and settles it on the part that
has to run it. The divisions are not even the cheap kind, because the
renormalisation needs a full 64-bit quotient that `warp_pred.glsl`'s 32-bit
`warp_div` cannot produce.

That the compose dispatch does not scale with the desktop/device ratio the
other rows show is what one thread per tile and no shared memory buys: 578
threads is too small a dispatch for the Adreno's memory system to be the thing
being measured, and what is left is nine integer divisions.

The same run is the correctness leg, and it is reported here because a timing
number from a run that computed the wrong answer is worth nothing: 19940 tiles
coded (1943 of them `STATIC_MV`), 21207 entries invalidated — **8670 by
13.12.2's `2^33` guard and 12537 by the 3.1.1 envelope** — 12 entries
force-advanced before the ring wrapped, and 10990 coded tiles refused by
13.12.4. `PASSED` on the Adreno 650.

The other caveat stands: the absolutes in the table above are from a device at
61-66 C at the end of a long session, and the ratios are what carry.

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

## Tile streaming and the lazy advance

The client does not wait for a frame (WiVRn `docs/NXWARP-TILESTREAM.md`, ADR
Cheat 1): coded tiles are applied to the atlas as they arrive, and display never
waits. The decoder therefore needs an entry point whose unit is a run of tiles,
and it takes **Option A** of that document:

```c
nxvc_vkd_status nxvc_vk_decode_tiles(nxvc_vk_decoder *dec,
                                     uint64_t frame_number,
                                     const uint8_t *frame_header, size_t header_len,
                                     const uint8_t *bytes, size_t len,
                                     uint32_t first_tile, uint32_t tile_count,
                                     uint32_t submit_flags);
```

Option B keeps the reassembly wait, which is most of what Cheat 1 exists to
remove. A is the larger change here and it is the one worth making.

**The advance becomes lazy, and the decoder owns it.** SYNTAX 13.12.3 step 1
advances every valid entry once per frame, and 13.12.4 has a coded tile of frame
`N` read its entry after frame `N`'s advance and no later. Arrivals are not in
frame order, and the advance cannot be walked back -- `renorm` is a rounding
integer division, so `C . H . H^-1 != C`. So an entry is advanced only when a
coded tile is about to read it:

```
apply(t, N):
    if table[t].src_frame >= N: drop, report `superseded`
    while advanced_to[t] < N:
        C[t] := renorm(C[t] . H[advanced_to[t] + 1][eye]);  gen[t] += 1
        advanced_to[t] += 1
    decode t against C[t]
    write back: C := I, src_frame := N, gen := 0, advanced_to := N, valid := 1
```

This is bit-exact against the eager form because 13.12.2 composes one
right-multiplication at a time and this performs the same steps in the same
order. The decoder performs it rather than exposing the table for the client to
advance, so 13.12.2's integer arithmetic stays in one implementation.

Two pieces of state follow.

**`advanced_to` is decoder-private and NOT in the table.** 13.12.1's 64 bytes
are fully specified and its 20 reserved bytes are zero and compared, so there is
nowhere to put it and it does not belong there anyway: it is an implementation
detail of when the composition ran, not part of the atlas. It lives in a
parallel array, one u32 per entry, initialised to `src_frame`.

**The `H` ring.** As built: **ten uints per (slot, eye)** -- nine matrix words
and a flags word whose bit 0 is `warp_present`. 64 slots x 2 eyes = **5120 B**,
not the 4.6 kB this document first claimed, which counted the matrix words
only.

The flags word is not padding and it cannot be dropped. A frame with
`warp_present == 0` contributes **no step at all** -- not a composition and not
a `gen` increment, because 13.12.3 step 1 is conditioned on it in its entirety
and `atlas_advance_frame()` in the reference returns early. The bit cannot be
inferred from the matrix, whose `h22` is `2^29` for every legal value, so it has
to travel with the slot.

The ring bounds how far behind an entry may fall: an entry whose `advanced_to`
is older than the ring must be invalidated, because the steps to advance it no
longer exist. In practice the decoder advances such an entry *before* the slot
is recycled rather than invalidating it -- the host tracks `advanced_to` anyway
-- and `nxvc-atlas-gpu-test` exercises that path deliberately by swinging the
coded rate from 2 % to 21 % over a 300-frame run. At a flat 15 % it never fired
and the policy was untested.

**A note on what the envelope check actually catches.** 13.12.2 has two ways to
fail a composition -- the `2^33` guard on `P`, and 3.1.1's envelope on the
renormalised result -- and for a head-motion homography the ENVELOPE always
trips first. Measured: over 300 frames of yaw-shaped matrices, **0 of 12537
invalidations** were the guard. That is the spec being right rather than a bug
("anything at `2^33` is already eight times outside the envelope"), but it means
a near-identity sweep cannot test the guard however long it runs, and the guard
is still normative because it is what a 128-bit implementation must reproduce.

The guard IS reachable from matrices that are legal as written: condition 2
bounds every entry at `kEntryMax = 2^30` and condition 3 constrains only ROW 2,
so a matrix whose linear part is a 512x scale is legal, and composing two of
those gives a `P` around `2^39`. `nxvc-atlas-gpu-test` therefore carries a
second phase of legal-but-extreme matrices purely to reach it (8670 guard trips)
and FAILS if the guard fires zero times, because a run that never reaches it
proves less than it looks like it proves.

**The depth is set by the envelope, and the envelope is measured.**
`vk.atlas.compose` composes a yaw homography with itself until 3.1.1's
condition 2 or 3 trips:

| yaw per frame | at 90 Hz | compositions before the envelope trips |
|---|---|---|
| 1.37 deg | 123 deg/s, the fastest the paper measures | **19** |
| 2.74 deg | 247 deg/s | 9 |
| 5.00 deg | 450 deg/s | 5 |
| the most aggressive matrix 3.1.1 permits at all | -- | 2 |

So at the fastest rotation a head produces, a tile's `C` survives **19**
compositions and then invalidates itself -- which is the ADR's claim that "the
envelope check is the staleness bound, for free", now with a number on it. The
ring only has to outlast that. **A 64-frame ring (4.6 kB for the pair) clears
the fastest measured rotation by more than 3x**, so the ring is never the
binding constraint and a tile is always invalidated for a reason the atlas can
state, never because the decoder forgot an `H`.

The bottom row is worth keeping in view: a matrix at the very edge of what
3.1.1 permits survives two compositions. Nothing forbids an encoder emitting
one, so the ring depth is not what makes the lazy advance safe -- the envelope
check on every step is, and the ring depth only stops it being the thing that
trips first.

**The normative atlas is the flushed state.** With a lazy advance, at a frame
boundary some entries are behind, so the table is not the eager form's table
until every entry is advanced to `N`. Materialising it -- the flush -- is what
conformance and any client read of the table observe, and the frame-complete
path flushes every frame, which is exactly the eager form. So:

> **The required equivalence test:** the same input decoded frame-complete and
> decoded as tile runs in arrival order, both flushed, must produce a
> byte-identical atlas -- pixels and all 64 bytes of every entry. Where the link
> is clean they agree trivially; where it loses, they agree because the set of
> tiles applied is the same and only the order differs.

`superseded` is a report, not a loss: the position already holds a newer
generation than the dropped tile, and the encoder must not answer it with a
refresh.

**One bug class belongs only to the lazy form, and it is worth naming here
because it passed every eager test.** `C` must be written back even when the
entry ends INVALID. The eager form persists `C` after every successful frame, so
an entry that survives k steps and fails at step k+1 is left holding the k-step
matrix; a lazy implementation that discards the successful steps along with the
failed one diverges the moment k > 0 -- which, for the eager path, is never,
because k is always 0 there. All 64 bytes are compared, so this shows up as a
table mismatch and not as a wrong pixel.

## The base layer: importing tiles the decoder did not decode

The base layer ships in atlas v1 as a patch source. The client HEVC-decodes it,
converts to the coded YCoCg-R domain in the side-by-side u16 layout with its own
kernel (WiVRn branch `nx-warp-hybrid`), and hands the decoder tiles to import:

```c
nxvc_vkd_status nxvc_vk_atlas_write_tiles(nxvc_vk_decoder *dec,
                                          uint32_t eye,
                                          uint32_t first_tile, uint32_t count,
                                          const nxvc_vkd_atlas_src *src,
                                          uint64_t src_frame,
                                          uint32_t submit_flags,
                                          uint32_t *applied,
                                          uint32_t *superseded);
```

**The two ends must agree, and the reference end is already built.** The
encoder and reference side of this is `nxvc_{decoder,encoder}_atlas_patch_base`
(`atlas-encoder` `948cf2a`, and the prototype is in `<nxvc/nxvc.h>` on this
branch since the `atlas` merge). Three properties of it are the contract this
entry point mirrors, and none of them is a free choice here:

* **supersede is `>=`, and it is a PER-TILE drop, not a refusal of the call.**
  "A write whose `src_frame` is not greater than the one the position already
  holds is dropped." The positions that are not superseded still take. This is
  the same rule and the same comparison a coded tile takes, which is the point
  -- a base tile can never move a position backwards over a coded one, and the
  two sources compose under one rule. `AtlasHostState::apply()` already
  implements exactly this and returns both lists;
* **`applied` and `superseded` are reported as counts**, both optional, rather
  than the call failing on a superseded position;
* **`base_sourced` is written.** 13.12.9 makes flags bit 2 normative in
  version 1, so the metadata is `C = identity`, `gen = 0`, `static = 0`,
  `valid = 1`, `base_sourced = 1`, `res_level = 0` -- and NOT a decoder-side
  side table, which would be the one thing that makes this decoder's atlas
  differ from the encoder's shadow on exactly the tiles the two exist to agree
  about.

It copies those tiles into the atlas pixels and sets their table entries:
`C := I`, `src_frame := F`, `gen := 0`, `valid := 1`, `static := 0`,
`base_sourced := 1`. **The same `src_frame` monotonicity rule as a coded tile**
-- an import at a position whose `src_frame` is already `>= F` is dropped and
reported -- so a base tile can never move a position backwards over a coded one,
and the two sources compose under one rule rather than two.

It is **ordered against in-flight decodes on the decoder's queue**: the import
is recorded on the decoder's command buffer, so it and `decode_tiles` serialise
by submission order and the client needs no fence of its own. `submit_flags`
carries the same `NXVC_VKD_SUBMIT_ASYNC` / `SIGNAL_BINARY` meanings as the
decode calls.

**The layout the import kernel writes to, normatively:** the atlas pixels are
`nxvw_ring_layout()`'s -- coded sample domain, u16 samples packed two per uint,
per-plane offsets `ringPlaneOff[p]`, row stride `ringStride[p]` (padded to an
even number of samples so every row starts on a uint boundary), and both eyes
side by side within each plane with eye `e` beginning at column `e * planeW[p]`.
`nxvc_vk_atlas_write_tiles` takes `eye` explicitly and the tile indices are
within that eye, so the caller does not have to reproduce the eye-minor table
index -- the decoder derives it.

Note the **two eye conventions**, which is the thing most likely to be got
wrong: the pixels are side by side, and the table is interleaved per row. The
import API is deliberately per eye to keep the caller on the pixel side of that.

## The publish contract

`ATLAS` needs an output mode that hands the client the atlas rather than a
picture. Concretely, alongside the existing `nxvc_vkd_images`:

* the atlas image, view and format: ONE image whose planes hold both eyes'
  sub-pictures side by side, matching `nxvw_ring_layout()`, with `planeW[]` and
  the strides so the client can find eye `e` at column `e * pw`.
  For a `CT_NONE` stream this is the 8-bit two-image NV12-shaped view above, one
  luma tap and one chroma tap, because three R16 planes cost 2.124 ms of display
  pass against 1.086 -- and the u16 layout remains what conformance compares;
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

0. ~~**Blocked on the ADR owner:** the `base_sourced` bit.~~ **Unblocked:**
   13.12.1 now names `flags` bit 2 `base_sourced` and 13.12.9 says what it
   means, so the import entry point is fully specified.
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

Then, in order: `nxvc_vk_decode_tiles` with the lazy advance and the
frame-complete equivalence test, and then `nxvc_vk_atlas_write_tiles`.

What must wait: everything with a byte-identity obligation — the write-back, the
atlas-sourced prediction, `NEAR_SKIP` in place, and the conformance leg.

Implementation goes on `atlas-decoder` off `atlas`.

## Open questions

* **The display format is decided, and it was decided by a measurement.**
  MEASURED on the Pico 4 by the texture-native feasibility agent (branch
  `texture-native`, `docs/TEXTURE-NATIVE-PATCHES.md`, commit `05791be`), a full
  2176x1088 display pass, one frame pair:

  | atlas the display pass samples | ms/frame pair |
  |---|---|
  | three-plane R16, which is what this document specified | **2.124** |
  | one-tap 8-bit | **1.086** |
  | one-tap R16 | 1.327 |
  | compressed (ASTC/ETC2/EAC), bilinear free | 1.05 - 1.09 |

  **The difference is the number of TAPS, not the format.** Three planes at
  R16 costs about twice a single-tap read of the same picture, and that is the
  whole gap; going from 16-bit to 8-bit on the same tap count is worth 0.24 ms
  against the 1.04 ms the tap count is worth.

  So for `CT_NONE` streams -- 8-bit YCbCr 4:2:0, which is what a live WiVRn
  stream actually is -- the decoder exposes the atlas for display as an 8-bit
  layout the pass samples in **one luma tap plus one chroma tap**: two images,
  `R8_UNORM` luma at full resolution and `R8G8_UNORM` chroma at half, which is
  NV12-shaped. A packed `R8G8B8A8` holding Y with the co-sited CbCr in a single
  tap is the alternative and costs 2x the luma memory; both are to be priced on
  the device. The **u16 storage layout stays the conformance representation**
  and nothing about 13.12.1 moves: this is a view, produced beside the atlas,
  and it is not what conformance compares.

  Two facts from the same report that constrain anything built here: **no
  compressed format carries `STORAGE_IMAGE`**, so a kernel can never write one
  -- a compressed atlas can only ever be fed by `vkCmdCopyBufferToImage` from a
  server that encoded the blocks. And **ASTC 6x6 cannot tile 64x64** (64/6 is
  not an integer), so tile origins are not block-aligned and a boundary block
  would hold samples from two tile positions with different source poses and
  different source frames. Only 4x4 and 8x8 divide 64.

* **Atlas writes are bound by the number of REGIONS, not by bytes.** Measured
  at **3.43 us per scattered 64x64 tile** on this driver for every compressed
  format, 1.70 us uncompressed -- and coalescing the same bytes into full-width
  row strips is up to **28x cheaper** (a full-atlas refresh: 0.071 ms instead of
  1.981 ms). So `nxvc_vk_atlas_write_tiles`, and the decoder's own atlas store,
  must **coalesce a frame's tiles into full-width row strips** rather than
  issuing one region per tile. At 40 coded tiles a frame the per-region form is
  0.14 ms of pure overhead, which is a fifth of Pass W; at a full refresh it is
  1.98 ms, which is most of the ATLAS budget. The API already takes
  `(first_tile, count)` as a RUN for exactly this reason, and the run must be
  turned into strips inside the decoder rather than passed through as regions.

* **The atlas image format, and what was actually built.** The design above
  wants the atlas to be an IMAGE so the client's display pass can sample it.
  The kernels as built keep it as the SSBO of u16 pairs that
  `nxvw_ring_layout()` already describes, and for a reason that is not
  laziness: `warp_pred.glsl` reads the reference through that layout, it is
  pinned byte-for-byte against the encoder, and ADR-0029 turns on it being
  unmodified. So the sampled view has to be produced BESIDE the atlas rather
  than instead of it -- a small kernel over the CODED tiles only, which is ~40
  of 289, not a full-picture copy. That is the plan; it is not built and it is
  not measured, and the R8_UNORM variant for `CT_NONE` streams is priced
  against it when it is.

* ~~**`base_sourced` needs a syntax change, and the decoder cannot make it.**~~
  **SETTLED, and in the decoder's favour.** The `atlas` merge brings SYNTAX
  13.12.1 with `flags` bit 2 named `base_sourced` and pointing at a new clause
  13.12.9, "the base layer as a patch source"; the reserved run is now bits
  3-7. It is **normative in version 1** -- written, and compared with the other
  63 bytes -- and `v87_atlas_base_sourced` is the vector for it. So
  `nxvc_vk_atlas_write_tiles` can be implemented as specified, bit and all,
  and item 0 of "What can start now" is no longer blocked on the ADR owner.

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
