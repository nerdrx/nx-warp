# Pass B on the Adreno 650: where the 23 ms goes, and what to try

Phase 1, desktop only. Nothing in this document has been measured on a device;
every number quoted is either already in the tree (cited) or derived from source
by counting. The variant plan at the bottom is what Phase 2 prices.

The standing rule from `vk/decoder/passB/README.md` governs all of it:

> on this driver, for this kernel, no metric available to us predicts the
> outcome, and every candidate has to be measured.

So nothing below is a prediction. It is a ranked list of things worth the device
time, with the evidence that earned each its rank.

## 0. The headline: the 23 ms is not the kernel we were told to look at

The brief describes `gpu 30.7 ms (passA 7.6, passB 23.1)` at 1088x1088 stereo and
asks where Pass B's 23 ms goes. The first thing to establish is which module is
spending it, because the answer changes the whole search.

**`pass_b_ms` is not Pass B.** `vk/decoder/nxvc_vkdec.cpp:1963-1969`:

```
d->stats.pass_a_ms = (ts[1] - ts[0]) * k;
d->stats.pass_b_ms = (ts[2] - ts[1]) * k;
if (nq >= 6) d->stats.pass_w_ms = (ts[5] - ts[4]) * k;
```

Timestamps 4 and 5 are written *inside* the `[ts1, ts2]` interval — Pass W is
dispatched in the same loop, before the Pass B segments. So `pass_b_ms` spans
**Pass W plus all three Pass B segments**, and the WiVRn HUD, which shows only A
and B, folds the warp into B. That is why `7.6 + 23.1` is exactly `30.7` with no
third term.

**The live stream is inter.** The session config
(`nx-scratch/live/xdg/wivrn/config.json`) is `"inter": "true"`, `backend vk`,
`intra-period 180`, `coded-vectors none`. At 90 fps with a 180-frame intra
period, almost every frame is inter and most tiles are `WARP_SKIP`.

**So the 23.1 ms is, to within the noise, the number `docs/ATLAS-DECODER.md`
already records.** That file measures 289 tiles per eye on the Pico at 490 MHz:
Pass A 1.534 ms, Pass W 0.661, Pass B 10.760 — and says

> 8.889 of the 10.760 ms is `reconstruct_skip_store` running the normative
> integer warp over ~250 skipped tiles.

Two eyes of `(0.661 + 10.760)` is **22.8 ms** against the reported 23.1.

The target is therefore `reconstruct_skip_store`'s warp predictor, which
`vk/decoder/passB/README.md` already prices at **86 %** of that module (34.75
us/tile against 4.73 with the predictor ablated). It is *not* the intra
reconstruct kernel, which the same tree measures at **8.96 us/tile, perfectly
linear, 5.15 ms for 578 tiles** (`vk/decoder/README.md` L1506-1518).

This matters because twelve levers on that warp have already been priced and
lost (`vk/decoder/passB/README.md`, "What does NOT work"). Any plan that
re-proposes staging the source in LDS, factoring the bilinear, hoisting the
quadrant, software pipelining, more independent chains, or 16-bit packing is
proposing a measurement that has already been made.

## 1. Ranked hypotheses

### H1. The warp coordinate pipeline runs once per PLANE, not once per tile

**The one structural redundancy the failed-lever table never covers.** Every
lever in that table is a change *inside* one `nxvwWarpPlane` call; none of them
asks how many times it is called.

`reconstruct.comp` calls `nxvwWarpPlane(tid, p, ...)` from inside the per-plane
loop `for (int p = 0; p < 4; ++p)`. At `res_level` 0, 4:2:0
(`passB_layout.h:120-129`) the plane edges are luma 64 and chroma 32, so one
skipped tile runs the full coordinate pipeline over

| plane | edge | samples |
|---|---|---|
| Y | 64 | 4096 |
| Co | 32 | 1024 |
| Cg | 32 | 1024 |
| **total** | | **6144** |

The chroma coordinate field is the luma field scaled by
`factor = full / size` — the function computes it itself, from the same tile
warp record, at a third of the density. The corner interpolation, the `xq6` and
`xq4` clamps and the quadrant select are re-derived for 2048 samples whose
positions are a shift of positions already computed.

Evidence this is the right half of the kernel to attack: `vk/decoder/README.md`
L1750-1756 measured the predictor's *arithmetic* against its *loads* and found
"the predictor is a load-count problem, not an arithmetic one" — but the skip
module's own table contradicts that for this module, at
"taking essentially every tap load out of the ring is worth **7.2 %**" and
"the arithmetic is the cost". Both can be true: the loads are cheap and the
coordinate arithmetic is what is left, and the coordinate arithmetic is exactly
what is being done three times.

Ceiling if the coordinate half were free for chroma: chroma is 33 % of the
warped samples. Not 33 % of the module — the taps and the store stay — so this
is a candidate for a double-digit percentage, not a halving.

### H2. Barrier density inside the per-plane loop

Counted from the preprocessed shipped module (`NXVW_INTRA_DIR=0`,
`NXVW_XFORM_LARGE=0`): **11 `barrier()` sites survive, and every one of them is
inside `for (int p = 0; p < 4; ++p)`.** On the common 8x8-DCT intra path a
4:2:0 tile executes

| plane | barriers | why |
|---|---|---|
| Y (nb == 8) | 7 | +2 for the second-level DC IDCT |
| Co (nb == 4) | 5 | |
| Cg (nb == 4) | 5 | |
| **per tile** | **17** | |

with 256 threads, which `vk/decoder/passA/README.md` L447-451 notes is "four
subgroups and `barrier()` is a real barrier here". The skip module is 9 sites.

The tree already has a price for barriers on this part: the LDS-staging lever
lost 26.3 %, and the post-mortem attributes it to "two more barriers a plane and
8 KB of shared traffic". Two barriers a plane costing a visible fraction of 26 %
puts the standing 17-per-tile in play as a first-class cost.

Rank 2 rather than 1 because the *direction* is unclear: `vk/decoder/README.md`
L1863-1892 records that removing barriers from Pass A was a **3.5x regression**
on Adreno. Barrier count on this part is not monotonic and this hypothesis could
easily go the wrong way. That is a reason to measure it, not a reason to skip it.

### H3. The chroma planes pay luma-shaped fixed costs

Both chroma planes are 32x32 = 1024 samples, a quarter of luma's 4096, driven by
the same 256-thread workgroup. Every `for (i = tid; i < n; i += 256)` loop over
a chroma plane runs 4 iterations against luma's 16, so the per-plane fixed costs
— the barriers of H2, the loop prologues, the `refElemBase`/`refStride` setup —
are amortised over a quarter of the work. Two of the three plane passes are the
inefficient ones.

This is the same observation as H1 from the other end and the two variants
interact, which is why they must be priced separately before any combination.

### H4. Per-eye serialisation is NOT the problem here, and should be struck

`vk/decoder/nxvc_vkdec.cpp:2259`: `eyePasses = fp.any_stereo_tile ? d->si.eyes : 1u`,
with a `buffer_barrier` between passes. **The eye split only happens when the
frame contains a STEREO tile** — a tile predicting across eyes. `coded-vectors`
is `none` and nothing in the live config enables stereo prediction, so this
frame almost certainly runs one dispatch over all 578 tiles already, which is
what `vk/decoder/README.md` L1546-1552 calls "the cheapest real win" and records
as **already taken** (18 %, one dispatch over 578 beats two over 289).

Listed so it is not proposed again. Worth one line of confirmation in Phase 2
(`NXVC_VKD_SEG_MS` prints the segment split) and no variant.

### H5. Private-array scratch is a closed question, not an open one

A `-O` build of the shipped module disassembles to 85 function-scope
`int[64]` arrays and 54 `int[16]`, every one dynamically indexed — which is
`docs/ADRENO-RULES.md` rule 1 on its face, and looks alarming.

**It is not the lead it appears to be, for two reasons.** First, that build is
not the shipped one: `docs/ADRENO-RULES.md` rule 4 forbids `glslc -O` for any
shader that reaches a device, because `spirv-opt`'s redundancy passes miscompile
on this driver; the shipped pipeline uses the curated list in
`NxvcShaderPasses.cmake`. Second, the *driver's own* statistics for the shipped
kernel already read **register footprint 6, scratch 342 B** after the 2026-09-05
work (`vk/decoder/README.md` L1669-1700), against 328 and 840 B before it — and
the same file records that hand-scalarising further made it **worse** (1477
instr / 342 B / 22.25 ms became 3062 / 448 B / 24.93). `vk/decoder/passA/README.md`
L660-676 adds that ~400 B of scratch is what this driver emits for every module
in the codebase, that RADV emits zero for the same source, and that it "costs
nothing measurable".

Recorded here only so the next reader who disassembles with `-O` and finds 85
spilled arrays does not spend a day on it. Not a variant.

## 2. Corrections to the brief

Three of the landmines in the brief are not what the tree records, and acting on
them would waste device time.

* **"sampler over R16_UNORM is 41x faster than `imageLoad` bilinear"** — no such
  finding exists anywhere in this repo. `docs/TEXTURE-NATIVE-PATCHES.md` §2.1
  measures the opposite: `R16_UNORM` is the *slowest* sampled format on the Pico
  and the only one that pays a bilinear surcharge at all (+0.180 ms, 16 %); ASTC
  and `R8_UNORM` filter for free. Separately,
  `docs/adr/0010` forbids the hardware sampler in the normative predictor
  outright, because sampler weight precision is undocumented on Adreno and the
  path has to be bit-exact. If the 41x is real it is on another branch
  (`texture-native` and `atlas` both exist); it is not applicable to a
  reconstruction kernel either way.
* **"TPG <= 32"** — the recorded rule is "32 is the largest working value; do not
  try 40, 48 or 64", and the unit is *tiles* per group in **Pass A**, which is
  256 threads. It is an empirical hang boundary, not a thread-count limit, and it
  does not constrain Pass B, whose workgroup is one tile of 256 threads.
* **"push <= 128 B"** — no Adreno-specific push-constant rule is recorded. The
  only 128 in the tree is Vulkan's guaranteed minimum, noted in the encoder.
  Pass A's push block is 20 bytes.

The landmines that *are* real and that constrain this work: `glslc -O`
miscompiles on this driver; dynamically-indexed function-scope arrays go to
scratch and in one measured case were **read back wrong**; subgroup arithmetic
scans are wrong *and* slower on this part; `XFORM_LARGE` hangs the device;
`VK_EXT_global_priority` HIGH is a 10x regression on a headset because priority
is per process; and the two eyes do not overlap on the GPU (concurrent/sequential
ratio 0.973-0.977).

## 3. Variant plan

Every variant must decode bit-identically. Pass B is reconstruction: the gate is
`vk.passB.*` and `vk.decoder.*` on **both** RADV and lavapipe, plus
`nxvc-passB-test`'s CPU-vs-GPU compare, before anything is pushed to a device.
`passB_model.cpp` is the line-for-line CPU model and moves with the kernel or the
compare is meaningless.

| # | variant | hypothesis | shape | byte-identity argument |
|---|---|---|---|---|
| V0 | control | — | the shipped binary, interleaved with every row | — |
| V1 | `NXVW_ABL_WARPONCE`: compute the luma coordinate field once into LDS, derive chroma by shift | H1 | ablation switch, off by default | the chroma coordinate is defined as the luma coordinate scaled by `full/size`; deriving it by shift is the same integer when the scale is a power of two, which 4:2:0 guarantees. Fails the compare on any non-power-of-two ratio, so the variant refuses those and falls through to the shipped path |
| V2 | merge the two chroma planes into one pass over 2048 samples | H3 | ablation switch | the planes are independent; interleaving their loops changes iteration order, not arithmetic |
| V3 | hoist the four per-plane setup barriers to one per tile where the schedule allows | H2 | ablation switch | needs a written argument per barrier removed; three of the 11 sites are already marked "keep the barrier count uniform", so those stay |
| V4 | V1 + V2 together | H1+H3 | — | as above |

V1 is the one worth the device time first. V3 is the one most likely to regress —
Pass A's barrier removal was a 3.5x regression on this part — and is included
because that is exactly the kind of thing this tree measures rather than assumes.

Each row is priced with `scripts/passb-device-rows.sh`, which already gates on
the headset being idle, checks the sha256 either side of the push, samples
`gpuclk` during the run and reports the temperature either side. Rows are
interleaved; the ratio is the measurement and the absolute is not. Two tile
counts, 289 and 578. Between rows, the headset stays below 50 C.

## 3b. The identity fast path (`NXVW_ABL_IDENTITY`)

Attacks the tile COUNT rather than the kernel: a tile whose prediction is a
straight copy need not run the warp at all.

### It is byte-identical, and here is the proof rather than the claim

Three conditions, all tested per tile in `nxvwWarpPlane`:

1. **The corners are the identity grid.** With `c0 = (tox, toy) << 6` and the
   other three one `kWarpTile` away in each axis, `dTopX = dBotX = 4096` and
   `c2.x == c0.x`, so `nTopX = 4096(tox + u) + 32`, whose `>> 6` is
   `64(tox + u)` because 32 < 64 — and `nBotX >> 6` is the same integer. Stage
   two's weights sum to `kWarpTile`, so

       bx = (64(tox+u)*wv1 + 64(tox+u)*wv0 + 32) >> 6 = 64(tox + u)

   exactly, for every `u`, **whatever the row weights are**. Likewise `by`. The
   geometry contributes no fractional part.
2. **Every active vector is a whole sample.** `mqx` is the plane's own vector in
   quarter samples shifted into Q.6, so `mqx = 16*vx` and
   `xq4 = (64(tox+u) + 16vx + 2) >> 2 = 16(tox+u) + 4vx`, giving
   `fx = xq4 & 15 = (4vx) & 15` — zero exactly when `vx` is a multiple of four.
   Testing `mq & 63` asks the same question one step earlier and covers both
   axes and all four quadrant vectors at once. Chroma asks about the vector it
   already halved for `sub == 2`, not luma's.
3. **Nothing saturates.** `sat_add_i32` and the clamp are not linear, so a copy
   cannot reproduce them. The corners being the grid means the extreme
   coordinates are the tile's own opposite corners plus the extreme vector, so
   testing those four bounds covers every interior sample.

Then `sample_bilinear(ix, iy, 0, 0)` has `gx = gy = 16`, `acc = 256*t00`, and
`(256*t00 + 128) >> 8 == t00` for every `t00 >= 0`. Every tap is a reconstructed
sample already clamped to `[0, maxval]`, so `t00 >= 0` always. **One fetch, and
it is the same integer the four-tap path produces.**

### The reference does not special-case it, and does not need to

`warp/ref/warp_ref.cpp:224` gives `kModeStatic` the identity corners directly —
"STATIC_MV: the identity predictor, exactly. No homography, no divide." — and
then runs the ordinary bilinear over them. So the reference *derives* the copy
rather than shortcutting to it, which is why the fast path is a decoder-side
optimisation and not a syntax change: there is nothing to agree with.

### Detection is cheap, and the hit rate is not luck

The corners are already in `sCorner[0..3]` before the sample loop, so the test
is a handful of integer compares on values the kernel has in hand, once per
plane, against 1024 to 4096 samples of work. There is no new traffic and no new
barrier.

**`kWarpModeStatic` produces the identity grid by construction**
(`warp_pred.glsl:152`, `inter_layout.h:87`), so a STATIC_MV tile qualifies
whenever its vector is a whole sample — deterministically, not by accident. A
WARP_SKIP tile qualifies only if its homography happens to come out as the
identity, which at rest it may or may not.

`warp_pred.glsl` is included by **both** `warp_pred.comp` (Pass W) and
`reconstruct.comp`'s skip module, so one edit covers the coded tiles and the
skipped ones together.

### Ceiling

`NXVW_ABL_NOWARP` already prices the predictor's whole share of the skip module
at **86 %** (34.75 us/tile against 4.73 ablated). The identity path keeps the
fetch and the store, so it lands between those two rather than at the floor.
`NXVW_ABL_COPYWARP` — one fetch, no interpolation, coordinate pipeline still
running — is the other bracket and its number has never been published.

### The byte-identity test, and why it needed a second switch

A conformance pass proves nothing on its own here: if no fixture tile has a
non-identity warp, the predicate is never gating and the pass is vacuous.
`NXVW_ABL_IDENTITY_FORCE` answers that by forcing the predicate true, which
produces a wrong picture on any tile whose warp is not already the identity. A
failure is the result being looked for.

| build | `vk.passB.*` | the warp-exercising set |
|---|---|---|
| forced true | **4/4 pass** — the passB fixtures contain no warped tile at all, so this suite cannot validate the path | **3 fail**: `vk.encoder.inter.cv1088`, `vk.decoder.conformance`, `vk.decoder.loss` |
| honest predicate | pass | **28/28 pass** |

The middle column is the reason the switch exists: `vk.passB.*` alone would have
gone green either way. The right column is the actual gate — the same three
tests that detect a forced identity accept the real one, on both ICDs.

### What is NOT established

**How many live tiles qualify.** That is the device question, and it is the one
that decides whether this is worth shipping: the arithmetic above says each
qualifying tile is nearly free, and says nothing about how many there are. A
frame of pure head rotation may have none. Worth reading alongside the segment
split, not before it.

## 3c. V2, the chroma pair (`NXVW_ABL_CHROMAPAIR`)

The two chroma planes of a 4:2:0 picture share **every input to the coordinate
pipeline** — the same tile corners, the same subsampled vector, the same
extent, the same quadrant split — and differ only in which plane of the
reference ring the taps come from. So the coordinate is computed once and used
twice, which removes a whole plane's worth of corner interpolation, clamping
and Q.6 -> Q.4 conversion: 1024 of the tile's 6144 warped sample-coordinates.

**It needs no shared memory and no extra barrier**, because both taps happen in
the same thread at the same loop iteration. That is what separates it from the
LDS-staging lever this tree measured at +26 %: nothing is published between the
planes, so there is nothing to publish it *through*. It was the reason to
prefer this over H1's luma-to-chroma sharing, which cannot avoid LDS.

Scoped to the module whose scratch is per-plane. `NXVW_WARP_EMIT_IS_SCRATCH`
says the destination is the tile's own plane slot, so plane 2's slot already
exists; Pass W reuses one `sFull` across planes and would need a second buffer,
which is LDS spent on the smaller term.

Paired only when the tail after the sample loop is the trivial one — `factor ==
1` and no near-skip field — because the sharing covers the loop and nothing
after it. That is res_level 0 without a near-skip record, which is the live 1088
case; anything else falls through unpaired. Plane 2's stride is compared against
plane 1's rather than assumed equal.

The refactor underneath is `fetchRefAt` / `fetchRefPairAt` / `sample_bilinearAt`,
taking the plane base as a parameter, with the existing no-base forms kept as
thin wrappers on `refElemBase` — so every existing call site produces the
integer it always did, in the default build as much as the ablated one.

### The proof that the test gates it

`NXVW_ABL_CHROMAPAIR_FORCE` fills plane 2 from plane 1's ring base, so the two
chroma planes come out equal — a wrong picture on any tile that is actually
paired.

| build | result |
|---|---|
| forced wrong base | **3 fail**: `vk.decoder.conformance`, `vk.decoder.loss`, `vk.encoder.inter.cv1088` |
| honest | **28/28 pass**, both ICDs |

The same three tests that detect the forced identity detect this, which is what
says the fixtures reach the paired path rather than skipping it.

## 4. What Phase 1 did not establish

* The split of the 23.1 ms between Pass W, the skip module, the non-directional
  module and the directional module **on this stream**. `docs/ATLAS-DECODER.md`
  gives it for a different fixture at `ENTROPY_LITE`; the live stream is `auto`.
  `NXVC_VKD_SEG_MS` prints exactly this and it is the first thing Phase 2 should
  read, before any variant is built — if the split is not what the atlas
  document implies, H1 through H3 are aimed at the wrong module and the plan
  changes.
* Whether the frame contains STEREO tiles (H4). Same command answers it.
* Any device number at all. Nothing here has run on a headset.
