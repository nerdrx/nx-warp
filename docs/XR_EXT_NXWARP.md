# XR_NX_render_hints: a vendor OpenXR extension for codec-facing render data

Status: proposal. Not registered with Khronos, not implemented, no allocated extension number or
structure-type values.

This specifies the interface through which an application — or Monado/WiVRn NX acting on its
behalf — hands the encoder the four things a renderer knows and a video codec has to guess:
**screen-space velocity, depth, a UI/text stencil, and per-layer static flags**, plus a foveation
hint. PAPER 2.3 assumes this extension exists in one paragraph; this document is that paragraph
written out, including what the codec does when it is absent, which is the case that has to work
first.

Cross-references: PAPER 2.3 (per-tile vectors and the search), 2.5 and `docs/STEREO.md` (what depth
buys the STEREO mode), 4.6.1 (the degradation ladder the stencil steers), 5.1 (foveation).

Descriptions of existing OpenXR extensions below are **from memory and must be verified against the
registry** before any of this is proposed anywhere. They are marked where the uncertainty matters.

---

## 1. Why not just use what exists

Three existing extensions already cover part of this ground, and the proposal is shaped by them.

**`XR_KHR_composition_layer_depth`.** Chains `XrCompositionLayerDepthInfoKHR` to
`XrCompositionLayerProjectionView`, carrying a depth sub-image plus `minDepth`, `maxDepth`, `nearZ`,
`farZ`. Monado exposes it; several engines can submit it. **This is sufficient for everything the
codec wants depth for**, and the proposal below reuses its semantics verbatim rather than inventing
a second depth path. Believed to have fallen out of favour with runtimes (it was intended for
positional timewarp, which runtimes stopped doing); availability in practice needs checking per
runtime and per engine.

**`XR_FB_space_warp`.** The Application SpaceWarp extension: chains
`XrCompositionLayerSpaceWarpInfoFB` to each projection view with a **motion vector sub-image** and a
**depth sub-image**, plus `appSpaceDeltaPose`, `nearZ`/`farZ`, and a frame-skip flag; system
properties (`XrSystemSpaceWarpPropertiesFB`, believed) advertise a recommended motion-vector image
size, typically a quarter of the eye resolution in each dimension. Unity and Unreal both ship
support because Quest's ASW needs it.

**This is the important one.** The expensive part of what we are asking for — persuading engines to
export a screen-space velocity buffer at all — has already been done by Meta, is already in the
major engines, and already arrives through a composition-layer chain on the projection view. A
proposal that ignores it and invents a parallel velocity buffer would be asking the ecosystem to do
the same work twice.

**Decision: `XR_NX_render_hints` does not define a velocity buffer of its own where
`XR_FB_space_warp` is available.** If the application chains `XrCompositionLayerSpaceWarpInfoFB`,
the encoder consumes its motion-vector and depth images directly. The new extension carries only
what nothing else provides — the stencil, the static flags, the per-tile depth-plane shortcut, the
foveation hint and the frame-level scene-cut hint — and defines a velocity image only as a fallback
for runtimes without the FB extension. This costs us a dependency on a vendor extension from a
different vendor, which is not elegant, and buys ecosystem support that would otherwise take years.

**`XR_FB_foveation` / `XR_META_foveation_eye_tracked` / `XR_VARJO_foveated_rendering`.** These tell
the *runtime* how the application wants to render foveated. What the codec wants is the inverse
direction and a different thing: where the application believes detail matters. Hence a hint, not a
control.

---

## 2. What the codec does with each input

The whole argument for the extension, in one table. "Absent" is the column that has to be
acceptable, because it is what every application does today.

| Input | Codec use | Paper | If absent |
|---|---|---|---|
| Velocity (screen-space, px/frame) | Replaces the ±16 px coarse motion search with a single candidate, verified by SATD and never trusted blindly. Recovers motion beyond the search range (fast objects, thrown items, the player's own hands at speed). | 2.3 step 1 | Coarse search: 1089 candidates x 64 samples per tile, about 0.6 Gop per eye-frame, under 0.2 ms on an RX 580. Correct, but bounded at ±16 px: motion faster than that falls back to INTRA. |
| Depth | Seeds the parallax vector from head translation; seeds the STEREO disparity; supplies the **disocclusion guard**, which nothing else can provide. Also feeds `dQ_depth` if perceptual quantisation ever uses it. | 2.3 step 1, `docs/STEREO.md` 3 and 7.3 | Parallax is found by the coarse search. STEREO disparity is found by a dedicated horizontal coarse search that recovers 95 percent of the depth-seeded gain (`stereo/RESULTS.md`) but is wrong by >16 px on 5.3 percent of tiles. **The disocclusion guard is simply unavailable**, and the encoder pays for its absence in RD. |
| UI/text stencil | Forces `STATIC_MV` on head-locked pixels; forces lossless/near-lossless intra and 4:4:4 on text; raises lambda tolerance on alpha; forbids foveation blur inside UI; forbids STEREO on view-dependent surfaces. | 2.3, 4.6.1, 5.5 | Heuristics only: the compositor knows which *layers* are quads (Section 6), but within a projection layer an engine-drawn HUD is invisible to us. Text is detected, badly, by activity metrics. **This is the input with no adequate substitute.** |
| Per-layer static flags | A layer marked static for N frames skips analysis entirely and codes `WARP_SKIP`; the encoder's shadow model can freeze it. Directly cuts encoder time and bits on desktop/overlay panels. | 5.5, 4.6 | Detected after the fact by measuring a near-zero residual, i.e. one frame late and after paying for the analysis. |
| Foveation map hint | Biases the per-tile quantiser and per-tile resolution when eye tracking is absent or its gaze is stale. | 5.1.3, 5.1.4 | The fixed foveation model from the lens geometry, which is what ships today. Low value; included because it is nearly free once the chain exists. |
| Scene-cut / teleport hint (per frame) | Forces a full intra frame *on the frame it happens* instead of discovering the scene changed by spending 4 Mbit of failed prediction. Also resets the shadow model. | 2.6, 4.6 | One frame of very expensive prediction failure, then rolling intra refresh catches up over 2 s. Visible as a quality dip after every teleport. Cheap to provide, unusually valuable. |

Honest summary of the value ordering, which is **not** the order PAPER 2.3 implies: the stencil and
the scene-cut hint are the ones nothing can replace; the static flags save real encoder time; the
velocity buffer is a quality improvement in the tail (fast motion) rather than an average-case win;
and depth's unique contribution turns out to be the STEREO disocclusion guard rather than the
disparity itself. `stereo/RESULTS.md` is where that last conclusion comes from and it weakened the
case for depth relative to what PAPER 2.3 assumed.

---

## 3. The extension

### 3.1 Name and registration

```c
#define XR_NX_render_hints_SPEC_VERSION 1
#define XR_NX_RENDER_HINTS_EXTENSION_NAME "XR_NX_render_hints"
```

`NX` is not a registered Khronos vendor tag. Until one is allocated this is a private extension
between WiVRn NX's client and Monado, which is legal (runtimes may expose extensions the registry
does not know about; applications must query `xrEnumerateInstanceExtensionProperties` and must not
assume the numeric values). Structure type values are runtime-private in the
`XR_TYPE_...` vendor range and must be treated as unstable until registration. **Nothing in this
document may be shipped to third-party applications under this name without registration.**

### 3.2 System properties

```c
typedef struct XrSystemRenderHintsPropertiesNX {
    XrStructureType    type;                            // XR_TYPE_SYSTEM_RENDER_HINTS_PROPERTIES_NX
    void*              next;
    XrRenderHintsCapabilityFlagsNX supportedHints;      // which of the below the runtime consumes
    uint32_t           recommendedVelocityImageWidth;   // per eye; 0 if velocity is not consumed
    uint32_t           recommendedVelocityImageHeight;
    uint32_t           recommendedStencilImageWidth;    // typically the tile grid, e.g. 32x32
    uint32_t           recommendedStencilImageHeight;
    uint32_t           tileWidth;                       // 64 for this codec; 0 if tile planes unsupported
    uint32_t           tileHeight;
} XrSystemRenderHintsPropertiesNX;

typedef XrFlags64 XrRenderHintsCapabilityFlagsNX;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_VELOCITY_BIT_NX      = 0x01;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_DEPTH_BIT_NX         = 0x02;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_TILE_PLANES_BIT_NX   = 0x04;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_STENCIL_BIT_NX       = 0x08;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_FOVEATION_HINT_BIT_NX= 0x10;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_LAYER_FLAGS_BIT_NX   = 0x20;
static const XrRenderHintsCapabilityFlagsNX XR_RENDER_HINTS_CAPABILITY_SCENE_CUT_BIT_NX     = 0x40;
```

Chained to `XrSystemProperties` in `xrGetSystemProperties`. An application must submit only what is
advertised; a runtime must ignore, without error, anything it does not consume. **A runtime that
stops using a hint mid-session (the user switched the encoder to the Lite profile) must keep
accepting it.** Hints are advice; dropping one is never an error.

### 3.3 Per-view hints

Chained to each `XrCompositionLayerProjectionView`, alongside (not instead of)
`XrCompositionLayerDepthInfoKHR` or `XrCompositionLayerSpaceWarpInfoFB`.

```c
typedef struct XrCompositionLayerRenderHintsNX {
    XrStructureType         type;               // XR_TYPE_COMPOSITION_LAYER_RENDER_HINTS_NX
    const void*             next;
    XrRenderHintsLayerFlagsNX layerFlags;
    XrSwapchainSubImage     velocitySubImage;   // may be {0}; see 4.1
    XrRenderHintsVelocityEncodingNX velocityEncoding;
    float                   velocityScale;      // multiply texel values by this to get the unit below
    XrSwapchainSubImage     stencilSubImage;    // may be {0}; R8_UINT, see 4.3
    XrSwapchainSubImage     foveationHintSubImage; // may be {0}; R8_UNORM, see 4.5
    uint32_t                staticFrameCount;   // see 4.4; 0 = unknown/changing
} XrCompositionLayerRenderHintsNX;

typedef enum XrRenderHintsVelocityEncodingNX {
    XR_RENDER_HINTS_VELOCITY_ENCODING_PIXELS_PER_FRAME_NX   = 1, // preferred: screen-space px, this eye
    XR_RENDER_HINTS_VELOCITY_ENCODING_NDC_PER_FRAME_NX      = 2, // TAA-native; runtime converts
    XR_RENDER_HINTS_VELOCITY_ENCODING_UV_PER_FRAME_NX       = 3,
} XrRenderHintsVelocityEncodingNX;

typedef XrFlags64 XrRenderHintsLayerFlagsNX;
static const XrRenderHintsLayerFlagsNX XR_RENDER_HINTS_LAYER_STATIC_BIT_NX          = 0x01;
static const XrRenderHintsLayerFlagsNX XR_RENDER_HINTS_LAYER_HEAD_LOCKED_BIT_NX     = 0x02;
static const XrRenderHintsLayerFlagsNX XR_RENDER_HINTS_LAYER_TEXT_BIT_NX            = 0x04;
static const XrRenderHintsLayerFlagsNX XR_RENDER_HINTS_LAYER_VIEW_DEPENDENT_BIT_NX  = 0x08;
static const XrRenderHintsLayerFlagsNX XR_RENDER_HINTS_LAYER_NO_FOVEATION_BIT_NX    = 0x10;
```

### 3.4 Per-tile depth planes: the cheap alternative to a depth image

A depth image at full resolution is 8 MB per eye per frame at 2048x2048 fp16, and the encoder
immediately reduces it to one number per tile. An application that already knows its geometry (a
compositor drawing quads, a UI toolkit, a video player) can skip the image:

```c
typedef struct XrTileDepthPlanesNX {
    XrStructureType type;              // XR_TYPE_TILE_DEPTH_PLANES_NX
    const void*     next;
    uint32_t        tileCountX;        // ceil(view width  / tileWidth)
    uint32_t        tileCountY;
    const float*    tileDepthMeters;   // row-major, tileCountX*tileCountY, eye-space metres
    const float*    tileDepthSpreadMeters; // optional, may be NULL: max-min within the tile
} XrTileDepthPlanesNX;
```

Chained to `XrCompositionLayerRenderHintsNX`. `tileDepthMeters` is the **median** eye-space depth
over the tile (median, not mean, for the reason in `docs/STEREO.md` 2.3). `tileDepthSpreadMeters`,
when supplied, is what drives the STEREO disocclusion guard without a depth image: a large spread
means a depth discontinuity crosses the tile. The pointer is read during `xrEndFrame` and need not
outlive the call.

This is the highest value-per-byte item in the whole proposal — 2048 floats per eye against 8 MB —
and it is the one an application can supply most easily.

### 3.5 Per-frame hints

```c
typedef struct XrFrameRenderHintsNX {
    XrStructureType  type;             // XR_TYPE_FRAME_RENDER_HINTS_NX
    const void*      next;
    XrBool32         sceneCut;         // this frame is discontinuous with the last
    XrBool32         teleport;         // pose is discontinuous; the pose warp is meaningless
    float            contentFrameRate; // Hz the content actually updates at; 0 = unknown
} XrFrameRenderHintsNX;
```

Chained to `XrFrameEndInfo`. `sceneCut` forces an intra frame and resets the encoder's shadow model
(PAPER 2.6). `teleport` additionally tells the client's reprojection not to extrapolate across the
discontinuity. `contentFrameRate` lets the frame-rate governor (PAPER 2.8) stop sending 90 unique
frames per second for a 24 fps video panel.

---

## 4. Semantics

### 4.1 Velocity

- **Preferred encoding** is `PIXELS_PER_FRAME_NX`: for each texel, the 2D screen-space displacement
  **in this eye's streamed pixels** from the previous frame's position to this frame's, i.e. the
  same sign convention as a motion vector pointing from the current frame back to the reference.
  This is the direction the encoder wants and it is the *opposite* of what most TAA buffers store,
  so the conversion has to be stated: a TAA buffer storing "current minus previous in NDC" is
  negated and scaled by `viewWidth/2`, `viewHeight/2`.
- Format: RG16F, or RGBA16F where the engine already has one (extra channels ignored). RG16_SNORM
  with `velocityScale` is permitted and preferred on bandwidth-limited paths.
- **Resolution: quarter of the eye resolution in each dimension is enough**, i.e. 512x512 for a
  2048x2048 eye. The encoder consumes a per-tile median, so a 64x64 tile still gets 256 samples.
  This matches the recommended size in `XR_FB_space_warp` (believed) and keeps the buffer at 1 MB.
- **The velocity buffer must describe the same instant as the colour image**, including the same
  jitter offset if TAA jitter is applied. A velocity buffer from a different jitter phase produces a
  systematic sub-pixel bias that the refinement then has to undo, which is worse than no buffer.
- The codec uses the per-tile **median**, not mean, and always verifies the candidate by SATD
  against the zero and previous-vector candidates before accepting it (PAPER 2.3). A velocity buffer
  that is wrong costs at most the SATD evaluation.
- Velocity for the *disoccluded* parts of a tile is meaningless in every engine. The codec does not
  care: it is one candidate seed among several.

### 4.2 Depth

Reuses `XrCompositionLayerDepthInfoKHR` semantics exactly: sub-image, `minDepth`/`maxDepth` (the
range of values in the image, normally 0 and 1), `nearZ`/`farZ` (the projection's clip distances,
possibly with `farZ` infinite). The encoder converts to eye-space metres itself. Reversed-Z
(`nearZ > farZ`) must be supported; it is the common case in modern engines.

Resolution may be lower than the colour image; half in each dimension is ample. Depth is read once
per eye-frame by a reduction pass and never stored.

If both `XrTileDepthPlanesNX` and a depth image are supplied, the tile planes win and the image is
ignored — the application knows better than a reduction does.

### 4.3 Stencil

An R8_UINT image, one texel per **tile** by default (32x32 for a 2048x2048 eye at 64x64 tiles), or
at any resolution the application finds convenient; the encoder reduces by bitwise OR over each
tile, so a bit set anywhere in a tile is set for the tile. Bit assignments:

| Bit | Meaning | Codec response |
|---|---|---|
| 0 | head-locked | force `STATIC_MV`, exclude from motion smoothing (PAPER 2.8) |
| 1 | text / hard-edged UI | near-lossless, 4:4:4 if the profile allows, never foveation-blurred (4.6.1) |
| 2 | transparent / additive | raise lambda, do not trust the velocity buffer here |
| 3 | view-dependent shading (mirror, strong specular, refraction) | **forbid STEREO** (`docs/STEREO.md`) and lower confidence in the temporal predictor |
| 4 | do not foveate | pin the tile to the fovea quantiser regardless of gaze |
| 5 | dynamic / newly spawned | skip `WARP_SKIP`, go straight to search |
| 6-7 | reserved, must be zero | — |

Bit 3 exists because of Package A: view-dependent shading is the one content class where inter-view
prediction is systematically wrong, and no analysis of the two images can distinguish it from
ordinary detail cheaply. This is the single most useful bit in the byte for a VRChat-class workload
and it cannot be derived.

The OR reduction is deliberately conservative: a stencil is a promise that something *is* text, not
a promise that everything else is not.

### 4.4 Static flags and `staticFrameCount`

`XR_RENDER_HINTS_LAYER_STATIC_BIT_NX` asserts the layer's content is **bit-identical** to the
previous frame's submission of the same layer. `staticFrameCount` is the number of frames the
application expects it to remain so (0 = unknown). The encoder codes the whole layer `WARP_SKIP`
for a head-locked layer, or pose-warp-only for a world-locked one, and skips analysis.

**The promise must be exact.** An application that sets the bit and then changes a pixel produces a
frame the encoder never looks at, and the error persists until the rolling intra refresh reaches
those tiles — up to two seconds. Runtimes should treat this bit as untrusted from unknown
applications and verify it with a cheap hash for the first few frames. WiVRn NX will.

### 4.5 Foveation hint

R8_UNORM at any resolution, 255 = "spend bits here", 0 = "spend none". Combined multiplicatively
with the lens-driven fixed foveation map and, when eye tracking is present, with the gaze map; it
never *overrides* gaze, because a hint that could blur the fovea is a hint that can make the image
worse. Lowest-value item in the proposal; specified because the chain costs nothing once it exists.

### 4.6 Lifetime and synchronisation

- All sub-images reference swapchains created by the application with the usage flags the runtime
  requires; the runtime documents which. The velocity, depth, stencil and foveation swapchains
  follow the same acquire/wait/release discipline as the colour swapchain and must be **released
  before `xrEndFrame`**.
- The runtime reads them during `xrEndFrame` and may hold them until the encode completes; the
  application must not assume the image is free again until the swapchain's next acquire.
- Sub-image `imageRect` and `imageArrayIndex` follow the projection view's, scaled by the ratio of
  the two images' sizes. Non-uniform scaling between the colour and hint images is an error.
- All hint structures are optional and independently omittable. Chaining a structure with all
  sub-images zeroed is legal and means "no hints this frame".

---

## 5. Validation

- `XR_ERROR_VALIDATION_FAILURE` if a sub-image references a swapchain whose format is not one the
  runtime advertised, or if `tileCountX/Y` do not match the view size and the advertised tile size.
- `XR_ERROR_FEATURE_UNSUPPORTED` at `xrCreateSession` if the extension was not enabled at instance
  creation.
- Everything else is advice. A runtime must never fail a frame because a hint was wrong; wrong
  hints cost quality, not correctness, and the codec verifies every one of them that it can.

---

## 6. How WiVRn NX exposes it through Monado

The path, and the thing worth noticing is how much of it works with **no application support at
all**.

1. **Loader and instance.** WiVRn NX's server links Monado; the extension is advertised by Monado's
   `xrEnumerateInstanceExtensionProperties` when the WiVRn NX encoder is the active compositor
   target. For the private-tag period, it is advertised only when an environment variable or config
   key is set, so unregistered names never reach unsuspecting applications.
2. **Composition layers.** Monado parses the `XrCompositionLayerProjectionView` next-chain in its
   `xrEndFrame` implementation and stores the hints in `xrt_layer_data` alongside the existing
   depth fields. This is where the work is: `xrt_layer_data` needs new fields and the layer
   accumulator needs to carry them to the compositor's sink. Swapchain images become additional
   `xrt_swapchain` handles imported into the encoder's Vulkan device — the same import path the
   colour swapchain already uses, so no new interop code.
3. **To the encoder.** The WiVRn NX video sink receives, per eye per frame, the colour image plus a
   `nxvc_frame_hints` struct holding image handles and the per-tile plane array. The encoder's E0
   analysis dispatch reduces velocity, depth and stencil to per-tile values in one pass over the
   tile grid, and everything downstream sees only per-tile numbers. The bitstream is unchanged —
   this is the property PAPER 2.3 insists on and it must stay true.
4. **Compositor-derived hints, no application involvement.** Monado already knows things the
   application never has to tell it, and WiVRn NX should synthesise these before it asks anyone to
   adopt an extension:
   - **Quad layers are head-locked or world-locked with known pose and known depth.** Every quad
     layer trivially yields stencil bit 0 or a `XrTileDepthPlanesNX` array over the tiles it covers,
     and its "static" flag from comparing the swapchain image index between frames.
   - **A quad layer whose swapchain index did not change is bit-identical**, which is
     `XR_RENDER_HINTS_LAYER_STATIC_BIT_NX` for free and verified rather than promised.
   - **Layer count and z-order changes** are a scene cut for the composited result.
   - The desktop/overlay panels that PAPER 5.5 cares most about are exactly these quad layers.
   This covers the UI half of the stencil for the entire ecosystem with zero application changes,
   and it should ship first.
5. **What still needs the application**: velocity, depth and the stencil *inside* a projection
   layer — the engine-drawn HUD, the text on an in-world screen, the mirror. Those are the
   XR_FB_space_warp path (velocity, depth) and the genuinely new part (stencil).
6. **Client side.** Nothing. Every hint is consumed by the encoder; none of it is transmitted, so
   the headset's decoder is untouched and no capability negotiation is needed.

---

## 7. The cheaper fallback: deriving velocity from depth and pose

If an application supplies depth but not velocity — or supplies neither and the runtime supplies
depth for quad layers — the encoder can compute part of the velocity field itself.

**What the derivation gives exactly.** For a static world point at eye-space depth `z` seen at pixel
`x` in frame N, its position in frame N-1 follows from the pose delta and `z` with no ambiguity:

    x_prev = K R_{N-1}^T ( R_N K^-1 x z + (p_N - p_{N-1}) ) / z'

The *parallax* field — the part of the motion caused by the head translating — is therefore exactly
recoverable from depth plus pose, per tile, from the tile's plane depth alone. This is a handful of
arithmetic per tile: 2048 tiles x about 40 flops = under 0.1 Mflop per eye-frame, unmeasurably
cheap, and it is already what PAPER 2.3's "parallax seed" does.

**What it cannot give.** Object motion. A moving avatar's velocity is not a function of depth and
pose; recovering it needs matching, which is the coarse search we were trying to avoid. Depth
*differencing* between consecutive frames does not rescue this: it detects that a tile's depth
changed, which is a useful "this tile is dynamic" signal (a cheap substitute for stencil bit 5), but
it does not yield a vector. An object moving parallel to the image plane at constant depth is
invisible to it entirely.

**Costs, PC-side, per eye-frame at 2048x2048 and 90 Hz** (RX 580 class, 256 GB/s):

| Step | Cost |
|---|---|
| Read a half-resolution depth image (1024x1024 fp16, 2 MB) | 2 MB / 256 GB/s = **8 us**; 0.36 GB/s of bandwidth at 90 Hz |
| Read a full-resolution depth image (2048x2048 fp16, 8 MB) | **31 us**; 1.4 GB/s at 90 Hz for two eyes |
| Reduce to 2048 per-tile plane depths (median of an 8x8 subsample) | fused into the read, negligible |
| Per-tile parallax vector from pose delta | under 1 us |
| Depth-difference "tile is dynamic" flag (needs last frame's plane depths, 8 kB) | under 1 us |
| **Total, half-res depth** | **about 10 us per eye-frame** |
| For comparison: the coarse motion search it partially replaces | about 200 us per eye-frame (PAPER 2.3) |
| For comparison: the STEREO horizontal coarse search it replaces | about 200 us per eye-frame (`stereo/RESULTS.md`) |

**Verdict on the fallback.** Depth alone, at 10 us per eye-frame, removes the need for a coarse
search on the *static* world and for the STEREO disparity search, and it enables the disocclusion
guard. That is most of what PAPER 2.3 wanted from the extension, for the cost of an application
setting one already-standard chain — or for free on quad layers, from the compositor. The velocity
buffer's remaining unique contribution is object motion beyond ±16 px, which is real (thrown
objects, fast avatars, the player's own hands) but is a tail case.

**Therefore the adoption order is: (1) compositor-derived hints for quad layers, no application
changes; (2) `XR_KHR_composition_layer_depth` or `XrTileDepthPlanesNX`, one chain, large benefit;
(3) `XR_FB_space_warp` velocity where the engine already has it; (4) the new stencil, which needs
real engine work and is worth asking for only once the rest is shipping.** Writing the extension
before doing (1) would be building the interface before the thing that needs it.

---

## 8. Open questions

1. **Is `XR_FB_space_warp` available on the runtimes we care about**, or only on Meta's? If SteamVR
   and Monado do not expose it, engines will not populate it on those paths and the dependency
   buys nothing. Needs checking before any of Section 1's decision is committed to.
2. **Does the velocity buffer's sign and jitter convention actually match across Unity and Unreal?**
   Section 4.1 states a convention; whether engines honour it under ASW is unverified.
3. **Multi-layer composition.** The stencil is per projection view, but the composited result mixes
   layers. If the encoder encodes the *composited* frame (it does), the stencil has to be composited
   too, with the same z-order. Specified nowhere above; needs a rule.
4. **Foveation grids and STEREO.** `docs/STEREO.md` 10.1 needs the two eyes' grids to be relatable.
   If the extension carried the grids explicitly this would be easier — but the grids are the
   runtime's, not the application's, so it belongs in the internal interface rather than here.
5. **Trust.** `staticFrameCount` and the static bit are promises an application can break. Section
   4.4 says verify; the verification cost has not been measured.
6. **Registration.** If this ever goes to Khronos, the right shape is probably not a vendor
   extension at all but an `EXT` covering "render hints for streaming compositors", with Valve,
   Meta and Collabora as the parties who would have to care. That is a much longer road and this
   document is the argument one would take down it.
