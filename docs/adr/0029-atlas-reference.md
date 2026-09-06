# ADR-0029: The reference is a per-tile atlas, and display is one warp step from it

- **Status**: Proposed
- **Date**: 2026-09-06
- **Source**: paper 2.1, 2.2, 2.6, 2.7, 2.8, 2.9; docs/SYNTAX.md 13; ADR-0010, ADR-0014, ADR-0027, ADR-0028
- **Affects**: `ref/src/`, `ref/tools/`, `docs/SYNTAX.md`, `vk/decoder/` (later), `include/nxvc/nxvc.h`

## Context

### The measurement that forces this

Measured on the Pico 4 (Adreno 650 class, 490 MHz), 1088x1088 per eye, 64x64 tiles, 17x17 = 289
tiles per eye:

| stage | measured |
|---|---|
| normative bit-exact integer warp of one `WARP_SKIP` tile | **34 us** |
| `WARP_SKIP` tiles on a typical frame | **~250 of 289** |
| skip warp, per eye per frame | **8.8 ms** |
| Pass A entropy decode (`ENTROPY_LITE` module), per eye per frame | **~1 ms** |
| coded-tile reconstruction (Pass B), per eye per frame | **~1 ms** |
| Lite Pass A, per coded tile | **16 us** |
| coded Pass B, per coded tile | **~25 us** |
| intra tile, per tile | **~12 us** |
| resulting frame rate | **~34 fps** |

The two eyes do not overlap on this GPU, so the per-eye numbers add. Every micro-optimisation of the
skip warp kernel has been measured and refused. The skip warp is 80 % of the decoder and it is spent
re-deriving, every frame, a picture that is mostly the same pixels resampled one more time.

The target is **240 fps-equivalent: 4.2 ms of headset GPU per displayed frame pair, all in** —
entropy, coded-tile reconstruction, atlas update, display warp. That is 5x the current budget, and
no schedule of micro-optimisations reaches it. The cost model has to change.

### What the current model actually pays for

Under SYNTAX 13.2 the reference is *the previous decoded picture*. To have one, every tile position
must be reconstructed every frame, including the 250 that carry no bits. `WARP_SKIP` is free on the
wire and costs 34 us on the GPU. Worse, the picture is built by chaining: a tile skipped for `k`
frames has been through `k` successive bilinear resamplings, which is the blur PAPER 2.2 flags as
risk 2 and which the encoder pays to correct.

Both problems have the same root: **the reference is a picture at one pose, so it must be rebuilt at
every new pose.**

## Decision

**The reference is a per-tile atlas.** For each tile position the atlas holds the pixels from the
most recent frame in which that tile was *coded* (`INTRA`, `WARP_MV`, `STATIC_MV` — anything that
produced residual-reconstructed samples), together with the composed warp from the current frame's
pose back to that source frame's pose, and the source frame number. **A skipped tile carries no
pixels, produces no reference pixels, and does not touch the atlas.**

Enabled by tool bit 31 `ATLAS`. `profile` stays informative (SYNTAX 2), so the tool bit is the
mechanism.

### 1. The atlas

Two objects, both per eye:

**Atlas pixels.** Byte-for-byte the layout of a `RefPicture` (SYNTAX 13.2): the whole picture of
every eye, every plane, in the **coded sample domain -- whatever domain the stream's colour
transform leaves**, at full tile extent. That is **YCbCr at 8 bits for a `CT_NONE` stream**, which
is what a live WiVRn NX stream is (its capture path is already
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` and its encoder leaves the transform at `CT_NONE`), and
Y/Co/Cg-R with 9-bit chroma when a colour transform is set. Earlier drafts of this ADR wrote
"Y/Co/Cg" as though it were the only case; it is the *rarer* case, and the difference decides
whether an 8-bit single-tap atlas image is legal -- which the display measurement above turns into
a millisecond of headset GPU per frame pair. A tile coded at `res_level > 0` is upsampled into the atlas by the
existing `store_ref_tile` kernel, so **the atlas pixel layout never depends on any per-tile choice**
and a fragment or compute pass samples it directly with no repacking. This is a hard requirement of
the 4.2 ms budget and it is why per-tile resolution is metadata and not layout.

**The per-tile table.** 64 bytes per tile position per eye. At the v1 configuration 578 tiles ->
**37.0 kB**, which fits a uniform buffer.

| off | size | field | v1 |
|---|---|---|---|
| 0 | 36 | `C[9]`, the composed homography, current frame -> source frame, rows 0-1 Q10.21, row 2 Q2.29 | normative |
| 36 | 4 | `src_frame`, u32, the frame number that last coded this tile | normative |
| 40 | 2 | `gen`, u16, composition steps since `src_frame` (the drift/cadence clock) | normative |
| 42 | 1 | `flags`: bit0 `valid`, bit1 `static`, bit2 `base_sourced` (reserved), bit3 `has_depth` (reserved) | bits 0-1 normative, 2-3 reserved zero |
| 43 | 1 | `res_level` the atlas tile was coded at (informative; the pixels are full extent) | written, advisory |
| 44 | 4 | reserved — Phase 2 depth-plane handle | zero |
| 48 | 12 | reserved — Phase 2 per-tile affine inverse depth `(a, b, c)`, 3 x i32 | zero |
| 60 | 4 | reserved | zero |

A v1 encoder and decoder write bytes 0..43 and zero 44..63. Conformance compares all 64.

### 2. The update rule (normative)

Per frame `N`, in this order:

1. **Advance.** If `warp_present`, then for every tile position with `valid == 1` and `static == 0`:
   `C := renorm(C . H_N[eye])`, `gen := gen + 1`. If the result leaves the legality envelope of
   SYNTAX 3.1.1 (conditions 2 and 3), or `gen` would exceed `gen_max`, set `valid := 0`. Tiles with
   `static == 1` are not advanced; `gen` still increments.
2. **Decode tiles.** A tile's prediction reads the atlas as specified in 4 below.
3. **Write back.** A *coded* tile (any mode that produces reconstructed samples) writes its
   reconstruction into the atlas pixels at its own tile position and sets
   `C := I`, `src_frame := N`, `gen := 0`, `static := (mode == STATIC_MV)`, `valid := 1`,
   `res_level := the tile's res_level`. A *skipped* tile writes nothing — no pixels, no metadata —
   with the single exception of `NEAR_SKIP` (see 6).
4. **Display.** At any time and at any rate, non-normatively (see 5).

`renorm` and the composition are defined in 3. `gen_max` is a stream constant, default 0 meaning
"no cap"; it exists for the drift-tolerant experiment of the Cheats section.

**The normative atlas is the FLUSHED state**, the one this process produces with step 1 applied to
every valid entry every frame. An implementation MAY advance an entry lazily -- only when something
is about to read it, one right-multiplication per intervening frame in frame order -- and must
flush before the atlas is published or compared. That is bit-exact, and it is bit-exact *because*
section 3 defines composition as one right-multiplication with its own renormalisation: the lazy
form does the same multiplications with the same rounding in the same order. Folding the
intervening matrices into one product first is a different answer and is not permitted.

The two ends do not carry the same obligation about age, and the difference is the whole loss
contract. **A decoder MAY invalidate** an entry it can no longer advance from the history it
retains -- it costs itself one `INTRA` tile and nothing else. **An encoder MUST NOT rely** on an
entry older than the envelope bound of step 1. *That bound is the guarantee*: it is derived
identically on both sides from data both sides parse, so an entry inside it is an entry every
conforming decoder still holds. A decoder's own retention limit is additional and unsignalled, and
what protects the encoder there is the receipt discipline of section 7, not the envelope.

**The invariant the budget depends on:** step 2 never reconstructs a skipped tile, and never reads
anything that a skipped tile would have had to produce. The atlas is persistent storage; a skipped
neighbour's pixels are already in it, from whenever it was last coded.

### 3. Composing the warp (normative)

`warp_ext()` gives `H_N`, mapping frame-`N` centred sample indices to frame-`N-1` centred indices.
For a tile last coded at `S`, the mapping frame-`N` -> frame-`S` is
`C = H_{S+1} . H_{S+2} . ... . H_N`, built incrementally by right-multiplication, which is exactly
step 1 above.

The wire scales differ per row (SYNTAX 3.1.1: rows 0-1 Q10.21, row 2 Q2.29). The product is
therefore defined with explicit per-term scale bookkeeping and **two independently rounded partial
sums**, which is what keeps every intermediate inside `int64` without 128-bit arithmetic:

```
for i in {0,1}, j in {0,1,2}:
    t_lin = C[i][0]*H[0][j] + C[i][1]*H[1][j]          // exact, Q42
    t_per = C[i][2]*H[2][j]                            // exact, Q50
    P[i][j] = ((t_lin + (1<<20)) >> 21) + ((t_per + (1<<28)) >> 29)     // Q21

for j in {0,1,2}:
    t_lin = C[2][0]*H[0][j] + C[2][1]*H[1][j]          // exact, Q50
    t_per = C[2][2]*H[2][j]                            // exact, Q58
    P[2][j] = ((t_lin + (1<<20)) >> 21) + ((t_per + (1<<28)) >> 29)     // Q29
```

Then `renorm` restores `C[2][2] == 2^29`, so that the composed matrix lives in exactly the envelope
the wire format already defines:

```
C'[i][j] = sdiv_round(P[i][j] << 29, P[2][2])
```

`sdiv_round` is sign-magnitude, half away from zero, over the same fixed restoring division the
corner derivation of SYNTAX 3.1.1 already mandates. Nine divisions per tile per eye per frame:
5202 per stereo frame at the v1 configuration, one small dispatch.

Two properties this buys, both load-bearing:

* **`warp/` is not modified.** A renormalised `C` satisfies the same legality conditions as a
  transmitted `H`, so `warp_plane_tile()` consumes it unchanged. The normative predictor kernel —
  the one thing in this codec that must never quietly change — is reused verbatim.
* **The envelope check is the staleness bound, for free.** A composition that would leave the
  envelope invalidates the tile, and an invalid tile cannot be skipped or inter-predicted, so the
  encoder must code it. No separate "too old" rule exists.

**Precision.** Each composition step rounds twice, at 1 ulp of Q21 (2^-21 relative) and 1 ulp of
Q29, and the error is bit-identical on both sides by construction, so it is a quality question and
never a conformance one. **The drift is measured, and the measurement is what this ADR carries.**
An earlier draft carried a random-walk *estimate* of ~5e-6 relative and a 0.0026-sample budget
derived from it; the estimate was optimistic by an order of magnitude and is not repeated here as
though it were a result.

**Measured** (two independent implementations, both against a double-precision recomposition):

| measurement | result | source |
|---|---|---|
| worst relative divergence over a 100-step chain | **4.8e-5** (~10x the 5e-6 estimate) | encoder-side CPU model |
| the same, as translation drift at a 512-sample term | **0.00035 samples** | derived from the above |
| worst corner divergence over a 30-step chain at 3.3 deg/frame | **0.0048 samples** | `ref/` composition check, this branch |

The conclusion survives the correction: **4.8e-5 relative, 0.00035 samples of translation drift at
a 512-sample term over 100 composition steps**, 7x inside the budget the estimate had set, and a
tenth of a hundredth of a sample is not visible in any predictor. Those two numbers are this ADR's
statement about precision. The estimate they replaced is recorded only so that nobody re-derives
it and believes it.

### 4. Prediction for a coded tile (normative)

A coded tile predicts from the **atlas**, through the composed matrix of its own **co-located**
atlas tile, read after step 1:

| mode | source | matrix | vector |
|---|---|---|---|
| `WARP_MV` | atlas pixels | `C` of the co-located tile | coded `mv` |
| `STATIC_MV` | atlas pixels | identity | coded `mv` |
| `INTRA` | none | — | — |
| `WARP_SKIP` | atlas pixels (display only; no reconstruction) | `C` of the co-located tile | stored `last_mv` |

Everything else — the residual syntax, the transform, the quantiser, the scan, the contexts, the DC
plane, `QUAD_MV`, `tskip`, `res_level`, 4:4:4, alpha — is untouched. **The coded-tile residual
syntax does not change by one bit.**

A coded tile whose co-located atlas tile has `valid == 0` is malformed unless `mode == INTRA`.

**The honest part.** The warp displacement plus the motion vector reach outside the tile's own
position, into neighbouring atlas tiles whose source poses differ. Those samples are read as if they
were at *this* tile's source pose, so they carry the pose difference as error. This is not drift and
not a mismatch: the encoder computes the prediction from the same atlas with the same matrix, so the
error lands in the residual and is corrected in the same frame. It is a pure rate cost, it is
confined to a border strip whose width is the warp displacement, and it is bounded by the encoder's
own skip threshold — which now measures the *real* one-step prediction error rather than the error
of a chained one.

The alternative — assembling a pose-aligned reference picture before decoding coded tiles — is
rejected below; it reintroduces the warp cost we are removing.

### 5. Display (non-normative)

The normative output of the decoding process under `ATLAS` is **the atlas (pixels + per-tile table)
after each frame, plus the per-tile mode/skip map**. Conformance is byte-identity of those. The
displayed picture is not normative and is not part of conformance.

To display, the client warps each atlas tile from its source pose to the pose it wants, **in one
step**, with whatever it likes: the hardware texture sampler, fp16, bilinear or anisotropic
filtering, sharpening, upscaling. For late-latched display at present time `t` the client composes
`C_tile . H(pose_t <- pose_N)` in floating point, where `pose_N` is the 26 pose bytes of SYNTAX 3.2
which the client already receives and which the codec does not interpret. **The client must retain
`pose_N` alongside the atlas**; that is the only new client obligation.

This is what decouples display from decode: the display pass reads persistent storage and a small
uniform table, so it runs at panel rate with the newest pose whether or not any coded tile arrived.
Decode only refreshes atlas content.

### 6. `NEAR_SKIP` under `ATLAS`

A `NEAR_SKIP` tile (tool bit 28) applies its nine-byte DC-and-ramps correction **to the atlas tile
in place**; `C`, `src_frame`, `gen`, `static` and `res_level` are unchanged. It costs one add per
sample and no warp. The correction was fitted at the current pose and is applied to source-pose
pixels; a DC and two ramps are pose-independent to first order, and the residual error is what the
encoder's own fit already measures. This keeps the tool's value — a cheap refresh that does not
re-code and does not re-warp — and it is the one place a skipped tile touches the atlas.

### 7. Loss, held frames and `ref_sel`

**`ref_sel` SHALL be 0 when `ATLAS` is set.** The atlas holds one generation per tile position.
Keeping four would multiply decoder memory by four (3.4 MB per generation per eye at u16 in the v1
configuration) for a mechanism the atlas makes unnecessary.

It is unnecessary because **loss is already per tile**. A client that missed frame `M` entirely
missed exactly the tiles frame `M` coded; every other tile position's atlas entry is bit-identical
to the encoder's, and continues to be. The client does nothing: its atlas simply still holds the
previous generation for those positions. **There is no concealment kernel under `ATLAS`** — SYNTAX
13.6's `WARP_SKIP`-with-`last_mv` reconstruction becomes the empty operation, because not updating a
tile *is* the correct behaviour and it is free.

The encoder's obligation is symmetric and equally cheap: on a negative receipt for tile `t` of frame
`M`, it rolls tile `t`'s shadow atlas entry back to the generation before `M`, from a one-deep undo
log it keeps for the tiles it coded in the last `K` frames. That replaces today's replay, which runs
`predict_tile` per lost tile. Encoder-side, non-normative, and strictly cheaper.

The residual risk is the same one the current model has: during the RTT before the encoder learns of
a loss, it predicts from a generation the client does not hold. The existing positive-acknowledgement
discipline (`set_frame_held(f, 1)`, `ref_confirm`) carries over unchanged in meaning, but its unit
becomes the tile rather than the frame, which is what the receipt map already is.

`tile_map_reset` clears the whole atlas: every entry `valid := 0`, so every tile must be `INTRA`.
That is the late-joiner and bitmap-gap path, unchanged in role.

### 8. What `ATLAS` excludes in v1

* **`STEREO`.** It predicts from "the decoded first eye of *this* frame", and under `ATLAS` the
  first eye is not reconstructed as a picture — only its coded tiles are. `ATLAS` and `STEREO` are
  mutually exclusive; a stream setting both is `BITSTREAM`. `STEREO` is already Phase 4 and off in
  Lite. Reconciling them means predicting from the left eye's *atlas*, whose tiles are at assorted
  poses, and that is a Phase 2 question.
* **Nothing else.** Chroma format, `res_level`, 4:4:4, `tskip`, `xform_size`, alpha, layers,
  `QUAD_MV`, `INTRA_DIR`, `CTX_V3`, `ENTROPY_LITE` all stay orthogonal. The atlas holds whatever
  plane layout the stream has.

### 9. The syntax delta, in full

That is the whole of it:

1. **Tool bit 31 `ATLAS`** (`NXVC_TOOL_ATLAS = 1ull << 31`), added to `NXVC_TOOLS_V1`.
2. **Constraint:** `ATLAS` requires `INTER`. `ATLAS` and `STEREO` are mutually exclusive.
3. **Constraint:** when `ATLAS` is set, `ref_sel` SHALL be 0 in every tile header.
4. **New normative clause 13.12**, the reconstruction and reference process above.
5. **No new field on the wire.** The per-tile source frame number and composed matrix are
   *derivable* by every conforming decoder from the transmitted `warp_ext()` and the skip/mode maps
   it already parses. Transmitting them would be redundant, and a redundant field is a field that
   can disagree. This is a refinement of the model as briefed, which allowed for "a per-tile
   source-generation field only where needed": the answer measured against the syntax is that it is
   needed nowhere.

Optional, and separable from the atlas:

6. **`row_present`** (tool bit 32), a frame-header bitmap eliding the 12-byte header of every tile
   row with no coded tile (Cheats 9). 5 bytes replacing up to 408 on an idle frame. Orthogonal to
   `ATLAS` and useful without it, behind its own tool bit so it can be measured alone -- and
   **required of a version 1 decoder**, which is a change from this list's first draft.
7. **Tool bit 33 `ATLAS_NBR`**, the neighbour-aware gather of SYNTAX 13.12.8. Implemented and
   measured; see the quality section. It recovers about a dB of an eight-and-a-half dB deficit, so
   it is specified because it exists, not because it fixes anything.
8. A stream constant `gen_max` and a tool bit for `ATLAS_DRIFT` (Cheats 8). Neither is built in v1,
   and the bit is NOT 32 -- 32 is `ROW_PRESENT` and 33 is `ATLAS_NBR`.

## The budget

Per eye per **coded** frame, at 1088x1088, 64x64 tiles, 289 tiles/eye, ~39 coded and ~250 skipped,
using the inherited per-tile measurements:

| stage | today | under `ATLAS` | basis |
|---|---|---|---|
| Pass A entropy | ~1 ms | 39 x 16 us = **0.62 ms** | measured 16 us/tile, coded tiles only |
| coded-tile reconstruction (Pass B) | ~1 ms | 39 x 25 us = **0.98 ms** | measured 25 us/tile |
| skip warp | 250 x 34 us = **8.8 ms** | **0** | removed |
| atlas update (compose + renorm, 289 tiles) | — | **0.0048 ms MEASURED** | 0.0096 ms for 578 entries on the Pico 4 at gpuclk 490 MHz; 10x under the budget it replaces |
| **decode subtotal, per eye** | ~10.8 ms | **1.65 ms** | |
| **decode, per frame pair** | ~21.6 ms | **3.30 ms** | |
| display warp, per frame pair | (in the above) | **1.086 ms MEASURED** | one-tap 8-bit atlas, Pico 4 |

Read that honestly:

* The **8.8 ms measured** skip warp goes to zero. That is the decision's entire content and it is
  arithmetic on measured numbers, not a projection.
* **The display warp is measured**, on the Pico 4 at 1088x1088 per eye, per frame pair
  (`docs/TEXTURE-NATIVE-PATCHES.md`, branch `texture-native`). The previous draft carried a 1.0 ms
  *budget* here and said Phase 0 had to measure it. It did:

  | atlas layout | display warp, per frame pair |
  |---|---|
  | **one tap per output pixel, 8-bit** | **1.086 ms** |
  | one tap per output pixel, R16 | 1.327 ms |
  | three separate R16 planes, which is what section 1 above specified | 2.124 ms |

  **The condition is the LAYOUT, not the format.** What buys the number is *one sampler tap per
  output pixel*; the three-plane form costs three taps and doubles the figure. An 8-bit single-tap
  atlas is available exactly when the coded sample domain is 8-bit, which is every `CT_NONE`
  stream -- and `CT_NONE` is what a live WiVRn NX stream is (SYNTAX 13.12.1). A `CT_YCOCGR` stream
  has 9-bit chroma and pays the R16 line. This is a constraint on the decoder's atlas layout and it
  should be read as one.
* **A frame that is both decoded and displayed costs 3.30 + 1.09 = 4.39 ms against a 4.2 ms
  target** on a `CT_NONE` stream, and 5.42 ms on the three-plane layout. So the arithmetic that
  said "it does not fit" is still the arithmetic, by 0.19 ms rather than by 0.10 ms -- what changed
  is that the display term is now a result, and that the three-plane layout is off the table. Two
  things make the coincident frame fit, and both are in the Cheats section rather than being
  decorations on it:
  * **Amortisation.** Display runs at panel rate, decode at server rate. At 90 Hz server into a
    240 Hz panel the decode cost per displayed pair is 3.30 x 90/240 = **1.24 ms**, plus the
    measured 1.09 ms display = **2.33 ms per displayed pair**, with margin. This is the whole point
    of decoupling display from decode, and both of its terms are now measured.
  * **Spreading.** Cheat 1 (tile streaming) lets a frame's coded tiles be applied to the atlas as
    they arrive, across several display intervals, so the 3.30 ms is never a single serial block in
    front of a vsync.
* The **fps claim**: at 8.8 ms removed per eye the decode of a coded frame pair drops from ~21.6 ms
  to ~3.3 ms, a **6.5x reduction in decode work**. The *displayed* rate becomes the panel rate by
  construction, because display no longer waits for decode. Neither number is a measured frame rate
  and neither should be quoted as one until the Pico runs it.

## Cheats

The mandate is that the codec cheats wherever possible. The atlas makes cheating structural: **the
normative object is the atlas, so anything that affects only the displayed picture and not the atlas
is by definition allowed to be approximate.** That is cheat 0, and the seven below are instances of
it. Each is a first-class mechanism with a switch, not a hack, and each is listed with what it saves
and what it costs so it can be exposed in the GUI.

**1. No whole-frame requirement on the client.** Coded tiles are applied to the atlas as they
arrive; display never waits for a frame to complete. The frame id only orders atlas generations —
which is precisely what step 1's advance and `src_frame` provide. *Saves:* the whole frame-assembly
latency, and it spreads the 3.3 ms of decode across display intervals instead of blocking one.
*Costs:* a tile arriving mid-display-pass shows one frame's worth of tearing at tile granularity
during the pass; the client can double-buffer the per-tile table to avoid even that (25 kB per eye).
*Normative status:* none needed — the order of atlas writes within a frame is unobservable in the
final atlas.

**2. Pose late-latching.** The display warp uses the newest pose at present time, not the pose of
the last decoded frame. The atlas tile's *source* pose is what makes this exact rather than an
approximation: there is a single correct one-step warp from source pose to present pose, and the
client has both. *Saves:* all of the pose-to-photon latency that decode contributes, and it is the
reason the display rate is the panel rate. *Costs:* nothing, for rotation. For translation it
inherits the rotation-only limitation of PAPER 2.1 — this is exactly what Phase 2's depth removes.

**3. Foveated refresh.** The encoder codes tiles in priority order, fovea first, and may leave
periphery tiles skipped for longer, at a higher QP, or at a lower `res_level`. The atlas carries
`res_level` and `gen` per tile from v1 so a client and a rate controller can see it. *Saves:*
directly reduces the ~39 coded tiles per eye, which is the term that does not amortise; this is the
lever that brings the coincident decode-and-display frame under 4.2 ms. *Costs:* periphery detail
and periphery temporal fidelity, which is the trade ADR-0027 already chose. *Status:* fields written
in v1; the encoder's eccentricity policy is a knob, not built this week.

**4. Chroma cheats.** Chroma updating at a different cadence from luma needs a per-plane `C` and
`src_frame`, which the atlas structure permits (the table is per tile, and per-plane costs 3x, still
an SSBO-sized 111 kB). It also needs a tile-header bit to signal a luma-only coded tile, and **v1
syntax has none**. Stated rather than invented: *this cheat is not available in v1*, it costs one
tile-header bit, and the atlas is already shaped for it. What *is* available in v1 is `res_level`,
which already codes chroma coarsely.

**5. Temporal cheats.** A tile whose prediction error is under a perceptual threshold *in motion* is
skipped even where it would not be at rest. The encoder derives head angular velocity from the pose
stream it already receives and scales `skip_thresh` by it. *Saves:* bytes and coded tiles precisely
on the frames that are most expensive today — fast head rotation. *Costs:* smear during fast
rotation, which is where the eye's own contrast sensitivity has collapsed; the artefact appears at
the moment rotation stops, for one refresh. *Status:* pure encoder, zero syntax, one config knob.
Build it.

**6. Display filtering is free.** Bilinear, anisotropic, sharpening, upscaling, tile-edge blending —
all in the display pass, none of it normative, none of it in conformance. *Saves:* it is what lets
the display warp be a sampler gather instead of the 34 us integer kernel. *Costs:* nothing the
conformance vectors can see, by construction.

**7. Hybrid base layer — the idle HEVC ASIC.** The headset's hardware HEVC decoder does nothing
today. Under the atlas it becomes a second *patch source*: an atlas tile may be refreshed either
from an nxvc coded tile or from a rect of a base-layer HEVC frame decoded by the ASIC and converted
into the atlas domain, with the nxvc enhancement layer coding the residual over it (ADR-0014,
PAPER 2.9). The atlas is the natural home for this because a patch source is already per tile.
Bit-exactness has two answers and both must be written down:

* *Option B — the encoder runs the same HEVC decoder.* **Validated on the Pico 4**, and the
  measurements are stronger than the argument was:

  | measurement | result |
  |---|---|
  | `OMX.qcom` HEVC decode vs FFmpeg, 180 frames at 4.7 / 9.6 / 50 Mbit | **byte-identical** |
  | AHardwareBuffer -> Vulkan sampler -> integer YCbCr->RGB->YCoCg-R -> atlas, vs the CPU computation | **0 of 1,183,744 samples differ** |
  | MediaCodec latency at 9.6 Mbit | **2.76 ms mean, 5.63 ms p99** |
  | a base-sourced patch, per 64x64 tile on the Adreno | **1.9 us** (vs ~41 us for an nxvc coded tile) |

  **One trap, and it is normative.** The driver's `samplerYcbcrConversionComponents` is **not
  identity**: the sampled `.r`/`.g`/`.b` came back as Cr, Y, Cb. The conversion clause must therefore
  **consume the reported swizzle** rather than assume a channel order, and conformance needs a
  **non-identity-swizzle device** in the matrix or the bug ships undetected on exactly the hardware
  this is for. *Cost:* an HEVC decoder in the encoder pipeline, and a normative colour-conversion
  clause that reads the swizzle. **This is the preferred option, and it is now measured rather than
  argued.**
* *Option A — base-sourced patches are drift-tolerant.* Mark the patch `base_sourced` (flags bit 2,
  already reserved), exclude it from conformance, and require a scheduled nxvc refresh within `T`
  frames. *Cost:* the encoder's shadow is wrong for those tiles for up to `T` frames, so their
  residuals are computed against a picture the client may not have — the exact failure ADR-0023 and
  the shadow contract exist to prevent, bounded but real. *Use only if a real device diverges under
  Option B.*

*Saves:* an entire idle decode unit, and a base-sourced patch costs **1.9 us a tile against ~41 us**
for an nxvc coded tile -- a 21x reduction on the one term of the budget that does not amortise, which
makes this a *latency* tool and not only a compatibility one. *Costs:* the base layer's own latency,
**measured at 2.76 ms mean / 5.63 ms p99**, not the 8-20 ms PAPER 2.9 carried; that number was
inherited from general MediaCodec lore and is wrong for this decoder at this bitrate.

*Status: **flags bit 2 `base_sourced` is NORMATIVE in v1**, and SYNTAX 13.12.9 is the clause.* The
measurement that promoted it from reserved is section 9 of the hybrid report: for a `CT_NONE`
stream the atlas and the base decoder's output are **the same domain**, so the "normative integer
colour transform" this ADR asked for **does not exist** -- the conversion is a channel mapping and
a widen. What is left, and what 13.12.9 makes normative, is the **channel order**:
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` carries luma in G, Cb in B and Cr in R, so a channel-identity
sampler yields `(Cr, Y, Cb)`, and an implementation must consume the reported swizzle rather than
assume an order. Measured identically on the Pico 4's Adreno 650 through an AHardwareBuffer
external format and on RADV through the plain format, so it is a property of the format and not of
a driver. Three consequences are written into 13.12.9 rather than left as advice: **a base layer is
disallowed when a colour transform is set** (there the domains differ, and a matrix would be back
on the normative path); an externally sourced write **obeys the same `src_frame` monotonicity** as
a coded tile, because the base arrives through a different decoder with a different latency and
out-of-order arrival between the two paths is ordinary rather than exceptional; and the encoder
reproduces the write, which is Option B, now the specified one.

**7b. Texture-native patches (ASTC): measured and REJECTED as a bulk mechanism.** If the atlas held
ASTC blocks the display pass would sample compressed data directly and the atlas would shrink.
Measured on the Pico 4 (`docs/TEXTURE-NATIVE-PATCHES.md`, branch `texture-native`):

| question | measured |
|---|---|
| does the device sample ASTC 4x4 / 6x6 / 8x8 with free bilinear? | **yes**, and with no `STORAGE_IMAGE` usage |
| bytes at matched PSNR, against nxvc | **1.9x to 2.4x** |
| GPU ASTC encoder | **none exists**; CPU `astcenc` is **27 ms per 289 tiles** |
| tiles per frame affordable over Wi-Fi at 43 Mbit/s | **12 to 45** |

So it is refused as the way patches travel: at 1.9-2.4x the bytes it loses the argument the codec
exists to win, and there is no encoder that could produce it in a frame budget. What it may still
be is a **trickle source** -- a few tiles a frame of periphery refresh on a link with headroom,
which is 12 to 45 tiles at the measured rate. **The HEVC base layer stays the fast-motion refresh
mechanism** (cheat 7); ASTC is not a competitor to it and the numbers above are why.

**7c. A note for 13.12 implementers: coalesce the atlas writes.** On this driver an atlas write is
**per-region bound at 3.43 us a tile**, not bandwidth bound, so an implementation that issues one
write per coded tile pays for the region and not for the pixels. Coded tiles are contiguous in row
order (Annex D D-3), so a decoder should coalesce a row's coded tiles into one strip write. This is
non-normative -- the atlas contents are the same either way -- and it is the difference between the
atlas update being below the measurement floor and being a term in the budget.

**8. Drift-tolerant mode (flagged experiment, off by default).** Optional tool bit 32
`ATLAS_DRIFT`: the *display warp output* — non-normative, filtered, fp16 — may be written back as
the atlas content for a skip chain, for at most `gen_max` generations before a mandatory coded
refresh. *Saves:* it lets a tile's pixels track slow content change with no bits at all. *Costs:* it
breaks bit-exactness for those tiles by design, so the encoder shadow can only bound the divergence,
not reproduce it; `gen_max` is that bound and the mandatory refresh is its enforcement. This is
exactly the property ADR-0023 defends, so it is an **experiment with a tool bit and a default of
off**, and it does not ship in v1. `gen` and `gen_max` exist in v1 so the experiment costs no
further syntax later.

**9. Flat UI stays inside the video, and costs nothing.** WayVR panels are the primary content, so
lifting flat UI out into runtime quad layers is explicitly rejected: the codec handles it. That
makes a requirement, not a preference:

> **An atlas patch that is not refreshed must cost the client nothing per frame beyond the sampler
> read at display.**

The design meets it, with this exact accounting:

* **Pixels: zero.** A skipped tile is not reconstructed, not warped, not touched. There is no
  concealment kernel and no `WARP_SKIP` reconstruction under `ATLAS` (7).
* **Metadata, `static` tiles: zero.** A tile whose last coded mode was `STATIC_MV` — a WayVR panel —
  is excluded from the step-1 advance by the `static == 0` guard. Nothing at all happens to it
  between the frame that codes it and the frame that next codes it.
* **Metadata, warped tiles: 9 int64 multiply-adds and 9 divides, touching no pixels**, in one
  dispatch of 578 threads. This is the honest exception to "nothing at all", and it is the price of
  the composed pose. It was budgeted at 0.05 ms per eye and required to be measured rather than
  assumed; it has been. **Measured on the Pico 4: 0.0096 ms for all 578 entries, 0.0048 ms per eye,
  at gpuclk 490 MHz** — an order of magnitude under the budget, and the one term of the atlas's
  per-frame cost that is proportional to the tile grid rather than to what changed. The budget line
  is retired: this is a result.
* **Bits: the row skip bitmap already costs bytes proportional to the grid, not to changes.** A tile
  row with no coded tiles still carries a 12-byte row header: 17 rows x 2 eyes x 12 = **408 bytes
  per frame, 294 kbit/s at 90 Hz**, for a frame in which nothing changed. On a static-panel scene
  that is most of the stream. The fix is a frame-header **`row_present` bitmap** — 34 bits, 5 bytes,
  eliding the header of every row with no coded tile — which reduces the floor to
  **5 bytes per frame, 3.6 kbit/s**, an 80x reduction on an idle frame. It is orthogonal to the
  atlas, and it is what makes "a static scene costs nothing" true on the wire as well as on the
  GPU.

  **It is REQUIRED of a version 1 decoder** (SYNTAX 3.1.2 and 14). The syntax delta below listed it
  as optional; that is now wrong, and deliberately so. Unlike `ENTROPY_LITE`, whose value depends
  on a Pass A time only the decoder knows, there is nothing here for a receiver to weigh: the
  bitmap costs nothing to parse, adds no second code path, and its absence is what makes a static
  scene pay for the tile grid forever. The tool BIT stays, because a sender still has to know
  whether the bytes may be elided.

*Saves:* on a mostly static UI, everything. *Costs:* one optional frame-header field. Phase 2 of
this work adds a **static-panels fixture** — 1088x1088, a mostly static UI with one moving element
under slow head rotation — and reports its bytes per frame and coded-tile count under both models.

### One cheat the atlas gets for free: static skip

`STATIC_MV` content — menus, HUDs, laser pointers — is head-locked, so warping it is exactly wrong.
Under the current model such a tile must be **coded every frame**, because skipping it applies the
warp. Under `ATLAS` its atlas entry carries `static = 1`, its `C` is held at identity, and it may
therefore be **skipped**, holding on screen at zero bits until the content changes. This is
derivable by both sides from the mode that last coded the tile, so it costs zero syntax; it is in
step 1 above as the `static == 0` guard. It is a strict gain that the old model could not express,
and it is called out here because it is the one behavioural change to an existing mode.

## Phase 2: nxModel

The path beyond the atlas is **patches with depth**, so that the display warp becomes a true 6-DoF
reprojection: translation and parallax handled on the headset, patches valid far longer, fewer
refreshes. The v1 atlas is shaped so this needs no change to the pixel layout.

**What v1 reserves.** Per-tile table bytes 44..59: a 4-byte depth-plane handle and a 12-byte
per-tile affine inverse depth `(a, b, c)` with `1/z = a*x + b*y + c` over the tile, plus flags bit 3
`has_depth`. Two representations, deliberately:

* *Per-tile affine inverse depth* — 12 bytes per tile, 6.9 kB per eye at the v1 configuration. This
  is the cheap one, and PAPER 2.1's own argument says it is nearly enough: once depth is
  approximated as constant per tile the plane-induced homography differs from the rotation-only one
  by a term constant within the tile. An affine term is the next order and is what carries a floor
  or a wall that spans a tile.
* *Per-pixel depth as a second atlas image with its own generation* — the same tile grid, a
  single-channel plane, updated per tile exactly as the colour atlas is. This is the one that
  handles a hand in front of a wall, and it is the one that costs bandwidth.

Both are sourced from the compositor's depth when the application submits one:
`XR_KHR_composition_layer_depth` is standard and WiVRn can receive it. When it does not, the tile
falls back to `has_depth = 0` and rotation-only display, per tile, with no stream-level switch.

**How coded-tile prediction uses depth: it does not.** Prediction stays rotation-only and normative;
display uses depth non-normatively. This is the cheaper and the right split, for three reasons.
Prediction is the thing that must be bit-exact on both ends, and adding a depth term to it doubles
the normative surface for a gain that a per-tile motion vector already largely captures (PAPER 2.1).
Display is where the parallax actually matters, because display runs at panel rate with a
late-latched pose while prediction runs once per coded tile. And keeping depth out of the normative
path means a client with no depth and a client with depth decode the same stream to the same atlas
and merely show it differently — which is the same separation that makes cheats 1, 2 and 6 legal.

**What the encoder needs.** Depth in, per tile, from the render pipeline; a fit of the affine
inverse-depth plane per tile (least squares, encoder-side, non-normative); and a
**disocclusion-aware skip decision** — the skip threshold must account for the region a 6-DoF
display warp would tear open, which the rotation-only threshold does not see. That last is the real
Phase 2 encoder work.

**Where learned components sit, cheaply.** Server side: refresh priority and skip-threshold models —
predicting which tiles will need coding, from pose, velocity, depth and past residuals. That is a
small model on a PC GPU, it is entirely non-normative (it only chooses modes), and it is where the
bits are. Client side: a tiny inpainting of disocclusion holes in the display pass, non-normative by
construction because it touches only the displayed picture. **Not** a neural full-frame decoder on
this chip: the 4.2 ms budget for two eyes at 2.4 Mpix rules it out, and it would put a learned
component in the normative path, which ADR-0010 forbids.

None of this enters the v1 syntax. The reserved fields are the whole of the forward commitment.

## Consequences

* The skip warp, 80 % of the measured decoder, ceases to exist. This is the decision.
* The normative output of the codec changes: it is the atlas, not the picture. `nxvc_decoder_decode_frame`'s
  output image becomes non-normative under `ATLAS`, produced by a display helper. New accessors are
  needed for the atlas and the per-tile table, on both encoder (shadow) and decoder.
* Conformance changes shape: a vector's expected output is the atlas + table, not a picture. The
  existing vector format carries a per-frame hash of the reference; it carries the atlas hash the
  same way. Phase 3 of this work delivers those vectors so the GPU decoder has a target.
* Concealment code disappears under `ATLAS`. So does `ref_sel` and the replay in
  `nxvc_encoder_set_received_tiles`, replaced by a one-deep per-tile undo.
* Skip decisions should hold longer, because the encoder now measures a one-step warp instead of a
  chained one and there is no accumulating resampling blur. **Measured: true at low angular
  velocity** (79.8 % skip at +1.30 dB and -7.1 % bytes), **false at high** (72.3 % skip against the
  picture model's 80.1 %), for the reason set out above. Phase 2 of this work measures bytes per frame and PSNR of the *displayed*
  picture against the old model, on the same clips at the same bytes. If skip runs do not lengthen,
  the model still wins on GPU time and loses nothing on rate; if displayed PSNR falls, the seam
  effect below is why.
### The measured quality result, and the defect it found

Measured on this branch, 1088x1088, 16-frame synthetic fixtures, luma PSNR at
equal QP, encoder shadow byte-identical to the decoder throughout:

| fixture | angular velocity | atlas | picture model | delta |
|---|---|---|---|---|
| near-still | 5.2 deg/s mean, 8.4 peak | **41.99 dB @ 7131 B/f**, 79.8 % skip | 40.69 dB @ 7675 B/f, 78.7 % skip | **+1.30 dB, -7.1 % bytes** |
| fast turn | 71.4 deg/s mean, 151.7 peak | 32.99 dB @ 11857 B/f, 72.3 % skip | 40.46 dB @ 8225 B/f, 80.1 % skip | **-7.47 dB, +44 % bytes** |
| identity (`H = I`) | 0 | 39.85 dB @ 11347 B/f | 39.81 dB @ 11186 B/f | +0.04 dB, +1.4 % bytes |

**At low angular velocity the atlas does exactly what this ADR claimed**: it
wins on both axes at once, +1.30 dB *and* 7 % fewer bytes, because the one-step
warp from the source pose carries none of the chained-bilinear blur the picture
model accumulates. At high angular velocity it collapses.

**The mechanism is cross-tile gather, and it is a defect in 13.12.4's
co-located-`C` rule, not in an implementation.** A skipped tile is displayed and
predicted by reading the atlas at `C(x)`. For a displacement `d`, samples land
`d` pixels outside the tile's own position, in *neighbouring* atlas entries
whose content was coded at a different frame and therefore belongs to a
different pose. `d` grows as angular velocity times skip-run length, so:

* at `H = I`, `d = 0` and the penalty is **0.04 dB** -- the model is exactly
  sound when nothing moves;
* excluding a border strip of width `b` around every tile recovers the penalty
  monotonically -- **32.99 dB at b=0, 33.89 at 4, 34.69 at 8, 36.20 at 16,
  37.38 at 24, 37.42 at 30** -- while the picture model is **flat** across the
  same sweep (40.46 -> 40.94), which is the signature of an error that lives at
  tile boundaries and nowhere else;
* once `d` exceeds the 64-sample tile, a skipped tile displays *mostly its
  neighbours' content*, which is the fast-turn column above.

This was written up as "the seam", a border artefact bounded by the encoder's
skip threshold. That was wrong in degree: it is not a strip, it is
displacement-proportional contamination that consumes the whole tile once the
head turns fast enough, and the encoder's threshold does not bound it because
the encoder measures the same contaminated predictor and therefore cannot see
that it is contaminated.

**Bounding staleness does not fix it.** At `atlas_gen_max = 1` -- a tile may be
skipped for a single frame -- the fast-turn case is still 37.49 dB at
16910 B/frame, because invalidating an entry forces `INTRA` rather than
shortening the gather distance. The knob trades the defect for intra bits.

### The two fixes, priced

Both candidates are now implemented and measured. Three motion rates,
1088x1088, 16 frames, luma PSNR of the DISPLAYED picture against the source
over frames 1..15:

* **(a) neighbour-aware gather** -- tool bit 33 `ATLAS_NBR`, SYNTAX 13.12.8.
  Resolve which entry a sample lands in with the co-located `C`, fetch through
  that entry's `C`; four corners per (tile, entry) pair, no per-sample divide.
* **(b) displacement-bounded skip** -- encoder only, no syntax. A tile may be
  skipped only while the composed displacement at its four corners is under a
  margin.

**Equal QP** (PSNR / bytes per frame):

| fixture | QP | picture | atlas | (a) atlas+nbr | (b) margin 4 | (b) margin 8 | (b) margin 16 |
|---|---|---|---|---|---|---|---|
| near-still, 4.2 deg/s | 22 | 41.78 / 8956 | **42.85 / 8152** | 42.90 / 8087 | 42.85 / 8152 | 42.85 / 8152 | 42.85 / 8152 |
| near-still | 26 | 38.77 / 5471 | **39.93 / 5416** | 39.93 / 5296 | 39.93 / 5416 | 39.93 / 5416 | 39.93 / 5416 |
| near-still | 30 | 35.91 / 3404 | **36.83 / 3384** | 36.79 / 3352 | 36.83 / 3384 | 36.83 / 3384 | 36.83 / 3384 |
| mid, 25.2 deg/s | 22 | 41.52 / 10196 | 37.12 / 12824 | 36.55 / 11548 | 41.76 / 20696 | 38.75 / 16191 | 37.26 / 13961 |
| mid | 26 | 38.62 / 6092 | 34.68 / 8562 | 35.51 / 7841 | 39.27 / 15566 | 36.99 / 11795 | 35.66 / 9707 |
| mid | 30 | 35.91 / 3690 | 33.54 / 5320 | 33.13 / 4993 | 36.63 / 12077 | 35.00 / 8614 | 33.86 / 6599 |
| fast turn, 75.6 deg/s | 22 | 41.38 / 10483 | 30.46 / 15061 | 31.32 / 13799 | 42.32 / 24084 | 39.70 / 22178 | 36.59 / 18340 |
| fast turn | 26 | 38.50 / 6582 | 29.94 / 10518 | 31.36 / 9345 | 39.61 / 18301 | 37.63 / 16607 | 35.50 / 13362 |
| fast turn | 30 | 35.82 / 4302 | 28.61 / 6727 | 29.52 / 6372 | 36.75 / 14187 | 35.40 / 12806 | 33.80 / 9859 |

At the margin the atlas *beats* the picture model on quality -- +0.94 dB at
fast turn, QP 22 -- and pays 2.3x the bytes for it. The shape of the table is
the same at all three quantisers: the atlas wins at 4.2 deg/s by ~1 dB at
slightly fewer bytes, and the deficit at speed is 7.2 dB (QP 30) to 10.9 dB
(QP 22) with neither fix closing it inside its own byte budget.

**At 4.2 deg/s the margin does not bind at 4, 8 or 16** -- every cell is the
plain atlas to the byte -- so the low-velocity win is untouched by it. Margin 2
is the first that binds there, and it binds the wrong way: 42.91 dB at
9192 B/frame against the plain atlas's 42.85 at 8152 (QP 22), 0.06 dB for
12.8 % more bytes. Margin 2 is not carried in the table for that reason.

**Equal rate** is the test that decides, and it is unambiguous. Each
configuration is re-quantised to the bytes the picture model spends at QP 26:

| fixture | anchor | picture | atlas | (a) | (b) m8 | (b) m16 | (a)+(b) m8 |
|---|---|---|---|---|---|---|---|
| near-still | 5471 B/f | 38.77 | **39.93** | **39.93** (-3.2 % bytes) | 39.93 | 39.93 | 39.93 |
| mid 25 deg/s | 6092 B/f | **38.62** | 35.07 | 34.11 | 32.77 | 33.42 | 32.05 |
| fast turn | 6582 B/f | **38.50** | 28.61 | 29.52 | 27.12 | 30.68 | 28.12 |

**Neither fix works.** (a) recovers 0.9 dB of a 9.9 dB deficit at fast turn and
*loses* 0.96 dB at 25 deg/s. (b) buys its quality with forced refresh, and once
the bytes are held fixed the QP it has to pay for that refresh gives back more
than the refresh gained: at fast turn margin 8 lands at **QP 43** and 27.12 dB,
worse than the plain atlas. Margin 16 is the best of them at 30.68 dB, still
**7.8 dB behind the picture model at the same bytes**. Combining them is worse
than either.

One thing (b) did establish, and it is a real finding rather than a null one:
the first implementation of the bound bought 0.1 dB because forbidding
`WARP_SKIP` without also forbidding `NEAR_SKIP` moves 2073 of 4624 tiles from
one to the other. **A `NEAR_SKIP` tile is still a skipped tile** -- still
displayed by warping from its own source pose -- so a DC and two ramps cannot
undo samples fetched out of a neighbour captured at another pose. With that
closed the bound works, exactly as far as the table says.

Also measured and rejected: iterating (a)'s resolution toward the fixed point
it is approximating (`C_N(x)` inside `N`) is **worse** at every rate --
30.28 / 29.74 / 28.32 dB against one step's 31.32 / 31.36 / 29.52. The one-step
form is normative because it measured better, not because it is simpler.

**So the honest scope of this ADR is unchanged and now bounded by measurement
rather than by argument: the atlas is a win at low angular velocity -- +1.16 dB
at 3 % fewer bytes, equal rate -- and a loss at high, and neither of the two
fixes proposed for the loss recovers it.** The 8.8 ms the atlas removes is
unaffected either way, which is why the decision above still stands; what does
not stand is any claim that the quality loss at speed is a detail with a known
remedy. It is an open problem, and the next place to look is not the gather
rule but the *skipped tile's displayed reconstruction*: the picture model wins
because it warps a coherent picture, and every mechanism tried here still warps
each tile out of storage that its neighbours do not agree with.

### Acting on the mosaic: two more fixes, both measured, both negative

The diagnosis above -- the defect is the MOSAIC of capture times, not the
gather rule -- names two mechanisms that act on the mosaic itself. Both are
implemented, both are priced on the same three fixtures at the same equal-rate
anchor (the picture model at QP 26), and both fail.

**(c) ROLLING REBASE.** Re-pose the `N` most displaced entries every frame
(13.12.10), `N` per eye, so the per-frame warp is bounded at `N x 34 us`
instead of `289 x 34 us` and there is no spike. Entries with zero displacement
are ineligible, so nothing fires at rest.

| fixture | N=24 (0.82 ms) | N=48 (1.63 ms) | N=96 (3.26 ms) | plain atlas | picture |
|---|---|---|---|---|---|
| mid 25 deg/s | 34.48 | 34.62 | 36.54 | **35.07** | **38.62** |
| fast turn | 29.34 | 29.82 | 30.95 | 28.61 | **38.50** |

At `N = 24` and `N = 48` the rolling rebase is **worse than doing nothing** at
25 deg/s -- 34.48 and 34.62 against the plain atlas's 35.07. Re-posing a
fraction of the mosaic still costs bytes while leaving the disagreement between
neighbours in place, so it buys a partial fix and pays a full price. Only
`N = 96` -- a third of the grid, every frame -- turns positive, and it is still
2.08 dB behind the picture model. The cost axis works exactly as designed; the
quality axis never arrives.

**(d) BASE-LAYER REFRESH.** Use an HEVC base layer decoded outside the codec
(13.12.9, cheat 7) as the refresh source instead of nxvc intra: every position
whose corner displacement passes a margin is patched from the base picture of
that frame. Bytes are `nxvc + the base's own HEVC bytes`, because a base layer
is a whole-picture stream whose cost is paid every frame whether one tile takes
a patch or all of them do.

| fixture | best result | at | picture model | plain atlas |
|---|---|---|---|---|
| near-still | 39.19 (0 patches ever) | crf26/30, m8 | 38.77 | **39.93** |
| mid 25 deg/s | 34.82 | crf30, m8 | **38.62** | 35.07 |
| fast turn | 30.99 | crf26, m8 | **38.50** | 28.61 |

It loses at every velocity. At 25 deg/s the best point is **below the plain
atlas**. At rest it is worse than useless: the margin never binds, **zero
patches are applied on any frame**, and the stream has simply paid 509 to 1944
B/frame for a base layer it never reads. At fast turn the best point is 7.5 dB
behind the picture model, and the result is flat across four operating points
(29.28 / 30.48 / 30.99 / 30.91 at CRF 18 / 22 / 26 / 30) because cheapening the
base frees nxvc budget at exactly the rate it degrades the patches.

Two things must be said about this result so it is not read as contradicting
the hybrid gate:

* **The base's bytes count against Wi-Fi.** They are not free because they
  travel on a different decoder; they are bytes on the same link, and the
  equal-rate table charges them.
* **The gate's 45-71 % saving was measured against nxvc INTRA refresh**, not
  against inter prediction. Against a working inter predictor the base has to
  beat *prediction*, not beat an I-frame, and on this corpus it does not. Both
  results are correct and they are answers to different questions.

One caveat runs in the base layer's favour and so strengthens the negative:
these synthetic fixtures are bandlimited and unusually cheap for HEVC -- CRF 26
costs 1795 B/frame here against the gate's 5915 B/frame on real content -- so a
real base layer would take a larger share of the same budget, not a smaller
one.

### What the whole sweep actually established

Set the four fixes beside the one measurement that explains them. At fast turn,
re-posing the *whole* atlas *every* frame recovers 7.31 dB of the atlas's
9.89 dB deficit -- the mosaic is the mechanism, and collapsing it is what fixes
the quality. It costs 9.83 ms per eye per frame to do, which is the entire
8.8 ms the atlas exists to save, plus interest.

**The atlas's quality loss at speed and its GPU win are the same fact.** Every
point on every sweep above is a different exchange rate between them, and none
of them is an escape from the trade. That is why four mechanisms aimed at the
quality side all failed in the same shape: each one bought back some of the
loss by paying back some of the win.

Which settles what the atlas *is*. It is not a profile a stream picks once and
lives with. It is an **operating point**, worth having when the head is slow
and not worth having when it is fast, and both of those are true within one
second of the same session. The decision that follows is 13.12.11: the atlas
becomes a per-frame MODE.

### The decision: the atlas is a per-frame mode (13.12.11)

Every frame is coded either as an **ATLAS frame** (13.12 as written: skipped
tiles untouched, per-tile `C` advance, no warp) or as a **PICTURE frame** (the
ordinary non-`ATLAS` process in full, the atlas rebuilt from its result). One
frame-header bit carries which.

**A PICTURE frame is the picture model, not an approximation of it.** This is
verified rather than argued: an all-PICTURE stream is **byte-identical** to the
same clip coded with no atlas at all -- 129226 bytes, frame for frame, on the
fast-turn fixture. That is what the earlier full-rebase experiment could not
do. It paid *atlas* bytes at QP 30 for what the picture model gets at QP 26,
which is why it read as 2.58 dB behind rather than as equal; running the
ordinary process instead of an atlas frame that happens to rebase closes that
gap by construction. **The mode can therefore never be worse than not having
it**, at any velocity, which is the property that makes it safe to ship.

The encoder chooses per frame from the trigger of SYNTAX 13.12.11.1: the worst
corner displacement in the atlas **including this frame's advance**, against a
threshold `D` in luma samples and an optional minimum spacing `S`.

**Measured, equal rate**, anchor = the picture model at QP 26, GPU from the
encoder's own per-frame coded-tile counts at the measured 34 us a warped tile,
per eye:

| policy | near-still 4.2 deg/s | mid 25.2 deg/s | fast turn 75.6 deg/s |
|---|---|---|---|
| | dB / PICTURE % / ms | dB / PICTURE % / ms | dB / PICTURE % / ms |
| all-ATLAS | **39.93** / 0 / 0.00 | 35.07 / 0 / 0.00 | 28.61 / 0 / 0.00 |
| all-PICTURE (= no atlas) | 38.77 / 0 / 0.00 | 38.62 / 0 / 0.00 | 38.50 / 0 / 0.00 |
| **D=4** | **39.93** / 0 / 0.00 | **38.62** / 100 / 8.35 | **38.53** / 87 / 7.15 |
| **D=8** | **39.93** / 0 / 0.00 | **38.61** / 47 / 3.98 | **38.52** / 73 / 6.03 |
| D=16 | 39.93 / 0 / 0.00 | 37.40 / 27 / 2.21 | 36.88 / 60 / 4.88 |
| D=32 | 39.93 / 0 / 0.00 | 35.26 / 13 / 1.07 | 33.34 / 33 / 2.65 |
| D=8, S=2 | 39.93 / 0 / 0.00 | 38.61 / 47 / 3.98 | 33.78 / 40 / 3.18 |
| D=8, S=4 | 39.93 / 0 / 0.00 | 36.77 / 20 / 1.65 | 31.34 / 20 / 1.55 |
| D=16, S=4 | 39.93 / 0 / 0.00 | 36.77 / 20 / 1.65 | 31.34 / 20 / 1.55 |

**Recommended: `D = 8` luma samples, `S = 0` (no minimum spacing).**

Read the three columns, because each answers a different question.

* **At rest the trigger never fires, at any threshold.** Every `D` gives
  exactly the all-ATLAS row -- 39.93 dB, 1 % fewer bytes than the picture
  model, and **0.00 ms of warp**. The atlas's whole reason for existing is
  untouched, and it is untouched by construction rather than by tuning: a head
  that is not moving displaces nothing, so there is nothing to re-pose. The
  mode switch also costs **zero bytes** here -- an atlas stream with the tool
  enabled and never firing is byte-identical in its payload to one without it,
  differing in exactly one byte of the stream header's tool field.
* **At 25 deg/s, `D = 8` is within 0.01 dB of the picture model at 3.98 ms an
  eye**, spending 47 % of frames as PICTURE frames. This is the case the whole
  ADR has been failing to solve since the cross-tile gather was attributed:
  four mechanisms landed between 1.5 and 4 dB short, and this one is level.
* **At 75.6 deg/s, `D = 8` matches the picture model** (+0.02 dB) at 6.03 ms an
  eye. Six milliseconds is over the 4 ms the atlas was supposed to buy -- and
  it is **below the 8.8 ms the picture model spends to get the same quality**,
  because the 27 % of frames that stay ATLAS frames warp nothing at all. At
  this velocity no mechanism can be both under 4 ms and equal to the picture
  model, since the picture model itself costs 8.8; the honest statement is that
  the mode switch is strictly better than the status quo on both axes.

`D = 4` is very slightly better on quality and materially worse on cost (8.35
against 3.98 ms at 25 deg/s, for 0.01 dB), because it spends every frame as a
PICTURE frame at mid velocity and gives the atlas up entirely. `D = 16` and
above give back 1.2 to 5.2 dB to save GPU that is not scarce at those
velocities.

**The minimum spacing is rejected, and the table is why.** `S` was proposed to
bound the PICTURE rate, and it does -- by throttling refresh exactly when
refresh is what is needed. At fast turn `S = 2` costs **4.72 dB** against
`S = 0` and `S = 4` costs **7.16 dB**, for 2.85 and 4.48 ms of saved warp that
the frame budget did not need. `S = 4` also makes `D` irrelevant (`D = 8` and
`D = 16` give identical results), which is the signature of a constraint that
has stopped tracking the content. A rate controller that must bound the PICTURE
rate should raise `D`, which throttles by *staleness*, not by the clock.

**So the scope of this ADR changes.** The earlier conclusion -- "a win at low
angular velocity and a loss at high, and neither fix recovers it" -- is
superseded. With the mode switch the atlas is a win at low velocity (+1.16 dB
at 1 % fewer bytes and no warp at all) and **level with the picture model at
every higher velocity measured**, at less GPU than the picture model spends.
The quality loss at speed is not fixed; it is **avoided**, by not being in
atlas mode when the atlas is the wrong trade. That is a smaller claim than
"the atlas is better everywhere" and it is the one the measurements support.

### Two-level refresh: coarse first, refine later -- REJECTED

**It needs no syntax, and establishing that is half the result.** The atlas
already carries `res_level` per entry, and 13.12.1 upsamples a `res_level > 0`
tile into the atlas so the pixel layout never depends on a per-tile choice.
Under `ATLAS` a coded tile predicts from its own entry. Put those together and
a "refinement" is not a new mechanism at all: coding the tile again at
`res_level 0` predicts from the upsampled coarse pixels, so the bits it sends
**are** a residual on the coarse tile rather than a re-code of it. The whole
proposal is therefore a per-frame `res_map`, which the library has always
taken, and it was measured without changing one line of the codec.

So the question is purely economic, and the answer is no. Measured on the two
motion fixtures, four quantisers each, with every non-baseline curve read at
the baseline's byte rate by interpolation in (log rate, PSNR):

| | mid 25.2 deg/s | fast turn 75.6 deg/s |
|---|---|---|
| coarse `R=1`, refine when worth it | **-1.85 to -1.99 dB** | **-0.30 to -0.64 dB** |
| coarse `R=2`, refine when worth it | -4.19 to -5.27 dB | -1.31 to -1.65 dB |
| coarse `R=1`, at most 16 refinements a frame | -2.29 to -3.28 dB | +0.52 to +0.60 dB |

At equal QP it is worse still and more obviously so: at 25 deg/s the
unbudgeted two-level costs **20 % more bytes for 1.9 dB less** than coding the
tile properly once. It is *dominated* -- worse on both axes at the same time --
at every quantiser on both fixtures, which is a stronger negative than losing
at equal rate.

**Why it loses is the useful part.** The refinement's predictor is the
upsampled coarse tile, which is missing exactly the high frequencies the
refinement has to send. A residual against a blurred prediction is not cheap;
it costs more than the original tile would have, so a landing plus a refinement
is more total bytes than one full-resolution coding. The two-level split does
not divide the cost of a tile in two. It pays for the tile twice.

**The one configuration that wins is not two-level.** At fast turn the budgeted
rows gain 0.5 to 0.6 dB -- and they do it with **0 refinements out of 1179
landings**, with **75.5 %** of coarse landings replaced by the next refresh
before anything refined them (70.8 % at 25 deg/s). That is not coarse-then-fine;
it is *single-level coarse refresh*, and it wins for a reason that has nothing
to do with the proposal: during a fast turn, full-resolution detail in a tile
that will be re-refreshed within a few frames is detail nobody sees. The half
of the idea that pays is the coarse landing. The half that costs is the
refinement -- the half the proposal was actually about.

Two caveats keep that from becoming a recommendation here. It is measured in
**pure ATLAS mode**, at a velocity where the mode switch's recommended policy
spends 73 % of frames as PICTURE frames instead, so the regime it wins in is
largely one 13.12.11 now avoids; and it *loses* 2.3 dB at 25 deg/s, so it is
velocity-dependent in the direction that needs a controller rather than a
constant. **Resolution-adaptive refresh is an open rate-control question, it is
already expressible with today's syntax, and it is not this ADR's.**

**Decision: no normative text.** Two-level refresh is recorded as measured and
rejected. Nothing in 13.12 changes, because nothing in 13.12 would have had
to.

### Single-level coarse refresh under the mode switch: REJECTED

The half of the two-level idea that paid -- landing a stale tile at
`res_level 1` and never refining it -- gained 0.5 to 0.6 dB at fast turn *in
pure ATLAS mode*. That measurement is not wrong, and it does not survive the
mode switch.

Priced as a rate-control policy **under `D = 8`**, driven by the same corner
displacement 13.12.11.1's trigger reads (including this frame's advance, for
the same reason), applied to both a PICTURE frame's coded tiles and an ATLAS
frame's refreshes. Equal rate, four quantisers, dB against the `D = 8`
baseline:

| T (luma samples) | near-still | mid 25.2 deg/s | fast turn 75.6 deg/s |
|---|---|---|---|
| 2 | -0.07 .. 0.00 | **-1.49 .. -5.29** | **-1.33 .. -3.17** |
| 4 | 0.00 | -0.74 .. -2.81 | -1.25 .. -2.83 |
| 8 | 0.00 | -0.06 .. -0.22 | -0.95 .. -1.96 |
| 16 | 0.00 | 0.00 | -0.21 .. -0.95 |
| 32 | 0.00 | 0.00 | -0.02 .. +0.04 |

**There is no `T` that gains at fast turn.** Every threshold that engages
loses; the only thresholds that do not lose are the ones that stop firing
(at `T = 16` and above the policy is off at rest and at 25 deg/s, and at
`T = 32` it lands 8 tiles out of 988 at fast turn). So the answer to "constant
`T` or velocity hysteresis" is neither: **under the mode switch this policy has
no operating point at all.**

The reason is that the two mechanisms are **substitutes, not complements**.
Coarse refresh won in pure ATLAS mode because a tile refreshed during a fast
turn is replaced again before anyone looks at it, so its detail is wasted. At
`D = 8` a fast turn is 73 % PICTURE frames, and a PICTURE frame's output *is*
what the viewer sees and *is* what the atlas becomes -- there is no
soon-to-be-discarded refresh left to cheapen. 13.12.11 had already collected
that win, by a route that does not cost quality.

**The decoder-side saving is real and badly priced.** Coded samples per frame
at fast turn, QP 26, against 268 698 for the baseline: 255 386 at `T = 16`
(-5.0 %), 230 468 at `T = 8` (-14.2 %), 208 964 at `T = 2` (-22.2 %). Pass B is
proportional to this, and Pass B is 0.98 ms of a 3.30 ms decode -- so the
largest saving on offer is **0.22 ms for 3.17 dB**, and the mildest is 0.05 ms
for 0.95 dB. That is roughly 0.2 dB per 1 % of Pass B, which is not a trade
worth having at any of these thresholds.

**And it looks wrong, which is the reason that would have settled it anyway.**
The stated preference is that degradation read as soft or low-poly rather than
blocky. Measured on the decoded luma of a fast-turn frame -- mean absolute
difference across sample pairs that straddle the 64-sample tile grid, over the
same within tiles, so 1.0 means a tile edge looks like any other pair:

| | seam ratio | HF energy |
|---|---|---|
| source | 0.93 | 3.24 |
| `D = 8` baseline | 1.14 | 3.04 |
| `T = 8` | **1.53** | 2.76 |
| `T = 2` | **1.81** | 2.61 |

Both numbers move at once, and that combination is the failure mode: high-
frequency energy falls (the tiles really do get softer inside) while the seam
ratio rises by 34 to 59 % over the baseline (the 64-sample grid becomes
visible). Soft interiors separated by hard tile-aligned edges is the definition
of blocking, not of low-poly. The rendered frames agree -- object silhouettes
that are round in the baseline acquire straight, tile-aligned cuts. Even had
the rate-distortion result been neutral, this is the wrong kind of artefact to
spend it on.

**Decision: no normative text, and no encoder default.** The policy stays
expressible -- it is a `res_map`, and any encoder can choose it -- but it is
recorded here as measured and rejected under the mode switch. Its earlier win
in pure ATLAS mode is retained in the record above as the reason it was worth
testing, and as the explanation for why it stopped winning.

* **The seam, as originally written.** Two adjacent tiles with different source frames are each
  individually correctly reprojected, so static distant content is seamless. They diverge on moving
  content and on near parallax, growing with the age difference — a tile coded 30 frames ago beside
  one coded this frame, during a fast turn. The encoder's skip threshold bounds it, and bounds it
  more directly than before because the threshold now measures exactly this error. It must be
  measured, not assumed.
* Decoder memory: one atlas per eye (the same size as one ring slot, and the ring shrinks from four
  slots to one) plus 37 kB of table. This is a **reduction**.
* `STEREO` is excluded in v1, and reconciling it is Phase 2 work.

## Alternatives considered

**Assemble a pose-aligned reference picture before decoding coded tiles.** Warp every atlas tile
into current-frame coordinates, then predict coded tiles from it with the identity transform. It is
the cleanest semantics — every prediction sample is at the right pose — and it is rejected because
it reintroduces exactly the cost being removed: a coded tile with a full-range motion vector needs a
196x196 source region, up to 9 tiles of assembly each, so 39 coded tiles can demand more warping
than the 289 tiles do today. The per-tile-`C` rule accepts a bounded, residual-corrected border
error to keep the cost proportional to coded tiles.

**Per-pixel selection of the source tile's matrix.** For each source sample, use the matrix of the
atlas tile it lands in. Circular: which tile it lands in depends on the matrix. Not decodable.

**Transmit the per-tile source frame number and/or the composed matrix.** Rejected: both are
derivable from data every conforming decoder already parses, so the field is redundant, and a
redundant field is one that can disagree with its derivation. It also costs bytes on every frame for
information the decoder computes in a dispatch too small to measure.

**Keep four atlas generations, so `ref_sel` retains meaning.** Rejected: 3.4 MB per generation per
eye on a headset, to solve a problem the per-tile update rule already solves for free. Loss under
the atlas invalidates exactly the tiles a lost frame coded, and nothing else.

**Compose the corner coordinates instead of the matrices.** Push the four Q.6 tile corners back
through the chain each frame. It avoids matrix arithmetic entirely, but the corner at frame `N` is a
different point from the corner at frame `N-1`, so the chain cannot be advanced incrementally and
costs `N - S` matrix applications per tile per frame. The matrix composition is `O(1)` per tile per
frame; this is `O(age)`.

**128-bit accumulation in the composition.** Rejected in favour of two independently rounded partial
sums, which keeps every intermediate in `int64` at a cost of ~10 ulp of Q21 over a 100-frame chain
(0.003 samples) and keeps the operation implementable on the GPU with the arithmetic SPIR-V already
requires.

**Tighten `warp_ext()`'s legality envelope for `ATLAS` streams so the naive composition fits
`int64`.** Rejected: it makes an `ATLAS` stream's matrices a different object from a non-`ATLAS`
stream's, for no gain over the two-sum form.

**Make the display warp normative.** Rejected, and this is the load-bearing rejection of the whole
ADR: it is what makes the sampler, fp16, filtering, late latching, foveation, inpainting and every
other cheat legal. ADR-0010 keeps the *reference* integer-only; it never required the *display* to
be, and the atlas is the first model in which those are different objects.

## References

- PAPER 2.1, 2.2, 2.6, 2.7, 2.8, 2.9 — the predictor, determinism, the reference model, concealment,
  temporal decoupling, hybrid mode
- docs/SYNTAX.md 3.1.1 (`warp_ext()`), 4.1 (tile header), 13.1-13.10 (inter prediction)
- docs/WARP.md — the normative predictor, unchanged by this ADR
- ADR-0010 — integer-only normative path; the reference decoder is the specification
- ADR-0014 — layered bitstream, hybrid mode (cheat 7)
- ADR-0023 — bit-exactness stays (cheat 8 is the flagged exception)
- ADR-0027 — no spatial hybrid; foveation inside the codec is the lever (cheat 3)
- ADR-0028 — the GPU encoder's integer mode decision, which the atlas skip decision extends
