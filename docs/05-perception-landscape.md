# 5. Perception, foveation, future tools and the competitive landscape

## 5.1 Foveation model

### 5.1.1 One map, three consumers

Decision: a single per-frame **foveation map** is the source of truth. It is a small R8 texture, one texel per codec tile (for 2160x2160 at 64x64 tiles: 34x34 texels), generated on the server from (gaze or lens center, lens model, head velocity, content class). Three consumers read it:

1. the app's render pass, as a `VK_KHR_fragment_shading_rate` attachment (VRS), handed over through the OpenXR foveation extension (`XR_FB_foveation` family on Meta; on Monado we add a vendor extension that exposes the same map),
2. the encoder: per-tile sample scale, QP offset, chroma mode,
3. the client reprojection shader: the existing WiVRn NX defoveation pass, which now reads per-tile scale instead of only the global remap parameters.

If the app renders periphery tiles at VRS 2x2, the render target already contains 2x2-replicated shading; the encoder's 1/2 downsample of that tile is then lossless, so foveation is paid once, not twice. If the app ignores VRS (most do), the encoder still downsamples; the only loss is wasted GPU shading on the PC, not quality.

### 5.1.2 The grid and the lens

The tile grid stays axis-aligned and uniform in the encoded image (workgroup mapping must be trivial: tile id = (x/64, y/64)). Foveation is expressed per tile as:

- sample scale `s` in {1, 1/2, 1/4} (tile of 64x64 render pixels coded as 64, 32 or 16 samples per side),
- chroma mode in {4:4:4, 4:2:0, 4:1:0-like (chroma at 1/4 both axes)},
- QP offset `dQ` in [-8, +12] on the codec's own step ladder (Section 3),
- refresh priority (feeds the transport FEC class).

"Lens space" enters through a density function, not through warped tiles. For a rectilinear render target with half-FOV `F` the pixels per degree at off-axis angle `theta` is `ppd_render(theta) = ppd_center / cos^2(theta)`; at 45 degrees a tan-projected image spends 2x the pixels per degree that it spends in the center. The eye needs `ppd_needed(e) = 60 / (1 + e/e2)` with `e2 = 2.3` degrees (the standard cortical magnification / minimum-angle-of-resolution model, Geisler and Perry 1998 use the same constant), `e` = eccentricity from gaze in degrees, 60 ppd = 30 cycles/degree = 20/20 acuity. The scale for a tile is

```
s_tile = clamp_to_ladder( margin * ppd_needed(e_tile) / ppd_render(theta_tile) )
```

with `margin = 1.5` (contrast-detection thresholds are measured with gratings; natural images need less, but 1.5x keeps us on the safe side of the studies that report "indistinguishable" rather than "acceptable").

Pico 4 numbers: 2160 px across roughly 100 degrees of horizontal FOV, about 21 ppd average, about 24 ppd in the center after distortion (WiVRn default 1.0x render scale gives ppd_center ~ 22). The panel is therefore below foveal acuity everywhere, which caps what foveation can win in the center: the fovea tile is coded at `s = 1` and there is no headroom above it.

| eccentricity from gaze | ppd_needed x1.5 | ppd_render at 22 center | raw ratio | ladder | notes |
|---|---|---|---|---|---|
| 0 to 8 deg | 90 to 20 | 22 | > 0.9 | 1 | fovea + pad |
| 8 to 18 deg | 20 to 10 | 23 | 0.9 to 0.45 | 1 then 1/2 at 14 deg | mid ring |
| 18 to 35 deg | 10 to 5.5 | 25 to 33 | 0.4 to 0.17 | 1/2 | |
| 35 to 50 deg | 5.5 to 4 | 33 to 53 | 0.17 to 0.08 | 1/4 | lens over-sampling helps here |

Sample count with eye tracking: roughly 8% of the image at s=1, 25% at 1/2, 67% at 1/4 gives 0.08 + 0.25/4 + 0.67/16 = 0.18 of the full-resolution samples. That is the win the bit budget in Section 3 assumes for the Pro profile.

### 5.1.3 Fixed foveation (Pico 4, no eye tracking)

Without gaze the map is centered on the lens axis and eccentricity is replaced by `e' = max(0, e - R_box)`, where `R_box` is the region the eye visits often. VR gaze statistics (Sitzmann et al. 2018, "Saliency in VR", and later head/eye datasets) put roughly 90% of fixations within about 15 degrees horizontally and 10 degrees vertically of the head direction; users turn their head rather than their eyes for larger offsets. Decision: `R_box = 20` degrees horizontally, 15 vertically, elliptical. Beyond that the same ladder applies. The Pico 4's pancake lenses are sharp to the edge, unlike Fresnel optics whose blur beyond about 30 degrees would have hidden a further step, so no extra lens-blur credit is taken.

Result for the Pico 4: about 40% of tiles at s=1, 35% at 1/2, 25% at 1/4: 0.40 + 0.09 + 0.016 = 0.50 of the samples. Fixed foveation halves the work; it does not give the 5x of eye tracking. This matches field experience with the current continuous remap in WiVRn (users tolerate roughly a 2x reduction and complain above it). A user "foveation strength" slider scales `R_box` and `margin` together; the defaults above are the conservative point.

### 5.1.4 Eye-tracked foveation: latency budget and padding

Gaze-to-photon budget on a Quest Pro / Pico 4 Enterprise class device:

| stage | ms |
|---|---|
| eye camera exposure + tracker inference (90 to 120 Hz) | 8 to 11 |
| headset to PC (piggybacked on the pose packet) | 2 to 4 |
| render of the next frame with the new map | 5 to 11 |
| encode + transport + decode (this codec, Section 4 target) | 12 to 20 |
| scanout | 4 to 11 |
| total | 31 to 57 |

Padding rule: the s=1 region must still contain the fovea when the frame lands. Saccades are covered by saccadic suppression (vision is degraded for about 50 ms around a saccade) and handled by prediction below; the padding is for smooth pursuit, whose comfortable ceiling is about 30 deg/s with bursts to 100 deg/s. Decision: `pad = 0.05 deg per ms of gaze-to-photon latency + 1 deg tracker error`, so 40 ms gives 3 degrees and the s=1 radius is 5 + 3 = 8 degrees, which is the "0 to 8" row above. Albert et al. 2017 ("Latency requirements for foveated rendering in virtual reality", ACM TAP) found 50 to 70 ms tolerable with a gentle falloff and detection around 20 to 40 ms with an aggressive one; our ladder is the gentle kind, so 57 ms worst case is inside the tolerated range, and the map generator widens the pad automatically from measured latency telemetry (Section 4 stamps).

Saccade prediction: the main sequence relates saccade amplitude to peak velocity and duration (duration about 2.2 ms per degree + 21 ms). From the first 15 to 20 ms of a saccade's velocity profile the landing point can be predicted to within 10 to 20% of amplitude (Arabadzhiyska et al. 2017, "Saccade landing position prediction for gaze-contingent rendering", SIGGRAPH). Decision: the client runs the predictor (it has the raw samples at full rate) and sends a predicted landing point with a confidence; the server places the s=1 region at the landing point one to two frames early. During the saccade the fovea tiles at the old position degrade to 1/2 without penalty. Refresh cost: a tile jumping from 1/4 to 1 has a low-resolution warped reference, so its residual is nearly intra-sized; the predictor gives the encoder two frames to spread that cost through the enhancement layer instead of one spike. Without prediction, expect a 15 to 25% bit spike on the frame after each saccade at 3 saccades/s; with prediction, half of that.

Open risk: WiVRn NX's current foveation is a continuous separable remap (the render itself is squeezed, the client unwarps). Keeping it in v1 reduces app render cost, which per-tile scale cannot. But pose-warped prediction (Section 2) then operates in the remapped domain: the reprojection of the reference must unwarp, rotate, rewarp. That is a per-pixel function evaluation, cheap, but Section 2 must accept the remap as an input rather than assuming a rectilinear reference. Cross-section conflict flagged. Long term the per-tile scale should replace the continuous remap for the encoded image, with VRS carrying the render-cost win instead.

## 5.2 Perceptual quantization

All perceptual terms collapse into a per-tile QP offset and a chroma mode. The encoder does not carry a full HVS model per coefficient; the tile is the unit.

```
dQ_tile = dQ_ecc(e) + dQ_motion(v_slip) + dQ_lum(Ybar) + dQ_act(sigma) + dQ_class
QP_tile = clamp(QP_base + dQ_tile, QP_min_class, QP_max)
```

**Eccentricity** (`dQ_ecc`): on top of the sample scale, +0 inside the fovea, +2 in the mid ring, +4 at 1/2 tiles, +6 at 1/4 tiles. The scale removes resolution the eye cannot see; the QP offset removes contrast it cannot see: peripheral contrast sensitivity at the surviving frequencies is 2 to 4x lower than foveal.

**Motion** (`dQ_motion`): head rotation alone does not blur the retinal image; the vestibulo-ocular reflex counter-rotates the eye and the world stays stable. What matters is retinal slip: content motion that the eye does not track. The codec knows it exactly: the per-tile residual motion after pose warp (Section 2's correction vectors) is the slip for a fixating user. The spatio-temporal contrast sensitivity surface (Kelly 1979) shows the high-spatial-frequency limb falling by roughly a factor of 3 between 0 and 30 deg/s and effectively vanishing above 15 cycles/deg at 100 deg/s. Decision: `dQ_motion = 0` below 10 deg/s slip, +2 at 30, +4 at 60, +6 above 100 deg/s, per tile, with two safeguards: it decays over one frame when motion stops (masking after motion offset lasts about 50 to 100 ms, so one 11 ms frame is well inside it), and during fast head rotation (> 120 deg/s yaw) an additional +2 applies globally because reprojection blur and pursuit errors dominate anyway. Intra-refresh tiles ignore `dQ_motion` (their job is to be a clean reference).

**Luminance** (`dQ_lum`): threshold contrast rises in dark regions on an LCD with a finite black level, and the display's EOTF spreads codes thin in the shadows. Using Watson's DCTune luminance term, threshold scales as `(Ybar/Y_ref)^0.65`: a tile at 12% of reference luminance tolerates a step 4x smaller in absolute terms but its coded values sit in the dark end of the transfer curve where quantization is coarser per nit. Net rule: `dQ_lum = -2` for tiles with mean luma below 16/255 (protect shadows, this is where banding lives), 0 in the mid range, +2 for tiles above 220/255 (bright, saturated highlights mask well).

**Activity / contrast masking** (`dQ_act`): the x264 adaptive-quantization rule survives because it works: `dQ_act = -strength * (log2(sigma^2_tile) - log2(sigma^2_avg))` with strength 1.0 and clamp [-4, +4]. Flat tiles get finer steps, busy tiles coarser; the Watson contrast-masking exponent of about 0.7 is what the log-variance rule approximates.

**Class** (`dQ_class`): from the UI stencil / quad-layer metadata: text panels lock to lossless (QP_min_class forces it), passthrough tiles get +2 (camera noise masks), skip tiles have no QP.

**Chroma per eccentricity**: chromatic contrast sensitivity cuts off around 10 to 12 cycles/deg for red-green and 5 to 6 for blue-yellow (Mullen 1985), a third to a quarter of luminance. Decision: 4:4:4 in the fovea (s=1 tiles; text fringes are what 4:2:0 breaks), 4:2:0 in s=1/2 tiles, and chroma at 1/4 in both axes for s=1/4 tiles (the 4:1:0-like mode; the tile still stores Co and Cg, at 4x4 samples for a 16x16 luma tile). Additionally, the Cg (roughly blue-yellow) plane takes +2 QP relative to Co everywhere.

**Banding**: 8-bit output with an sRGB-like curve steps 1/255 per code, above the Weber fraction in dark gradients (sky, fog, menus). Decisions: the internal pipeline is 10-bit luma always, 8-bit only as a wire-format choice for the Lite profile; the decoder's output stage adds 1 LSB blue-noise dither (a 64x64 precomputed tile, offset per frame for temporal decorrelation) when writing to an 8-bit storage image; the existing 10-bit toggle becomes "keep 10-bit through to the panel where the compositor supports it". Dither costs 2 ops per pixel.

## 5.3 Quality metrics for VR

PSNR is the wrong tool for four reasons: it weighs every pixel equally when 80% of them are in the periphery at 1/4 sampling by design; it measures the encoded image, not what is displayed after lens warp, reprojection and defoveation; it is blind to temporal artifacts (tile pop-in on saccades, refresh flicker, warped-reference hole fill) which are the artifacts this codec actually produces; and it cannot compare a 4:4:4 fovea against a 4:2:0 periphery.

Decisions:

- Primary objective metric: **FovVideoVDP** (Mantiuk et al. 2021, SIGGRAPH), which takes gaze, display geometry (ppd), luminance and temporal content and outputs a JOD score; ColorVideoVDP (2024) as the color-aware successor once its VR tuning is stable. It is run in **display space**: the PC simulator decodes bit-exactly, runs the real client reprojection shader with the recorded poses, and compares against the same shader run on the uncompressed frames. This is the only way the warped-reference concealment is charged for what it actually shows.
- Secondary, cheap, per-tile: eccentricity-weighted SSIM (in the spirit of FWQI, Wang and Bovik 2001, and the foveated SSIM variants used in the foveated-streaming literature) computed in the same display space, used inside the encoder's rate control loop where a VDP is too slow. VMAF is kept only as a sanity number for the base layer when compared with HEVC.
- Temporal: a dedicated "pop-in" metric: per-tile JOD delta between consecutive frames in the fovea ring after a scale change, thresholded; tracked as a distribution, not a mean.
- Latency is a quality metric: motion-to-photon measured with a photodiode on the panel and an IMU on the headset, reported alongside every quality number; a codec that gains 1 JOD by adding 8 ms has lost.

Subjective methodology: sessions recorded from real games (poses, eye tracking where available, render targets at full rate) and replayed through the simulator to produce candidate streams, then presented on the actual headset with the recorded head motion re-driven through the compositor (the viewer's own head is tracked for comfort, the content path is the recorded one). Paired comparison (2AFC, "which is sharper / has fewer artifacts") against the uncompressed replay and against HEVC at matched bitrate, ITU-R BT.500 DSIS for absolute impairment, 15 to 20 participants, three content classes (fast game, text panel, social scene), and two task conditions (free viewing, and a reading / tracking task that forces gaze to the periphery). Report DMOS with confidence intervals and the fraction of trials where participants saw refresh or pop-in artifacts. Fixed foveation defaults (`R_box`, `margin`) come from this study, not from the model.

## 5.4 Future tools (versioned, optional, capability-gated)

Rule: every learned tool is **out of loop** (a post-filter on the decoded image the reference never sees) unless both sides can run integer-exact inference and exchange a weights hash at connect. This keeps drift impossible and lets the client refuse a tool with no effect on the bitstream. Capability bits: `TOOL_LEARNED_UPSAMPLE_V1`, `TOOL_LEARNED_DEBLOCK_V1`, `TOOL_QUANT_TABLE_V1`, `TOOL_NEURAL_SR_V1`; the bitstream carries hint flags only.

| tool | where it runs | size | cost | gate |
|---|---|---|---|---|
| peripheral upsampler: 3 conv layers 3x3, 8 channels, FP16, input 1/4 tile + warped ref, output 1/2 | headset GPU, s=1/4 tiles only | ~1.5k params | ~1.1k MAC per output pixel; on the 1/4-scale ring at 1/2 output that is ~60 GMAC/s per eye at 90 Hz | Adreno 740 (XR2 Gen 2) and up; Adreno 650 fails the 5.2 budget |
| learned deblocking / ringing filter, 4 layers, 16 channels, fovea tiles only | PC-class or XR2 Gen 2 GPU | ~7k params | ~4k MAC/px on 8% of pixels | strong headsets, Pro profile |
| content-adaptive quant tables: 64 coefficient weights x 3 planes x 8 tile classes, learned offline per game from opt-in sessions | encoder, table sent at connect | 1.5 KB | zero decode cost | any client (decoder just reads the table) |
| neural SR enhancement layer: ESPCN-style 4 layers x 32 channels int8, 1/2 to 1, replaces the coded enhancement layer for the mid ring | Hexagon NPU on XR2 Gen 2 (int8 TOPS figure vendor-marketed; treat as 2 to 3 usable TOPS, uncertain) | ~30k params | ~10k MAC/px; restricted to the mid ring (~25% of pixels) at 60 Hz with warp filling the other frames | NPU present and a Vulkan-to-NPU zero-copy path (AHardwareBuffer) proven |

The in-loop variant of the SR layer (server encodes residual on top of the SR prediction) would be a large gain but requires bit-exact int8 inference on both a desktop GPU and the Hexagon; this is deferred until such parity is demonstrated, and marked as the highest-risk item in this section.

## 5.5 Content types

| content | dominant property | codec response |
|---|---|---|
| games | fast motion, full-frame change on head turn, specular noise | pose-warped prediction carries most of it; `dQ_motion`; depth stream helps parallax; skip tiles rare |
| desktop / overlay panels (quad layers) | text, hard edges, static for seconds | lossless mode (RLE + rANS on YCoCg-R residual against the previous frame, most tiles skip), 4:4:4, no foveation on the panel while it is in the fovea box |
| passthrough MR | camera noise, alpha, composite with rendered content | alpha plane first class, +2 QP on camera tiles, chroma 4:2:0 everywhere (camera content has no text); the camera composite is done on the client, only the rendered layer with alpha is streamed |
| video players | content already compressed, 24 to 60 fps in a static frame | pass-through mode: the player's HEVC/AV1 bitstream travels untouched to the hardware decoder as a quad layer, and the codec only carries the surrounding scene; the compositor composes; this avoids double compression and frees the compute budget |
| social VR (VRChat) | dozens of animated avatars, high entropy, particles, mirrors | the hard case: pose warp helps the world, not the avatars; skip is rare; relies on the rate controller and on the periphery ladder; expect this to set the bitrate floor for a given quality |

## 5.6 Competitive landscape

**Hardware H.264/HEVC/AV1 paths (ALVR, WiVRn, Virtual Desktop, Steam Link, Meta Air Link).** Mature, free in engineering effort, hardware-decoded at low power, with excellent inter prediction for camera-like motion. ALVR and WiVRn add a continuous foveated remap before encode; Virtual Desktop adds client-side synchronous spacewarp, 10-bit HEVC, AV1 on Quest 3 and a Snapdragon super-resolution upscale; Meta Link exposes a "distortion curvature" knob that is fixed foveated encoding, plus a sharpening pass. What none of them can do: take the head pose into the predictor, refresh a single lost tile without an IDR or a slice trick, code alpha, code 4:4:4 on a mobile decoder, present a partial frame, or exceed the decoder's fixed pixel rate (the XR2 Gen 1 decoder is the reason these systems encode below panel resolution). Their latency floor is frame-granular by construction.

**JPEG XS (ISO/IEC 21122) and VC-2 (SMPTE 2042).** Intra-only wavelet codecs with line-based latency (32 lines for XS), visually lossless at 6:1 to 10:1, and simple enough to run in compute. They prove the intra tile design point this codec's Phase 1 resembles. They cannot use temporal redundancy, so 4K90 stereo sits at 500 Mbit to 1 Gbit, USB only. JPEG XS is patent-licensed (intoPIX, Fraunhofer and others); VC-2 is royalty-free by the BBC's declaration and is the safer reference for wavelet tools.

**LCEVC (MPEG-5 Part 2, V-Nova).** Base codec plus a compute-decoded enhancement of small-transform residuals with temporal residual prediction. It is the closest existing shape to the hybrid decode path here, and the strongest patent overlap: V-Nova licenses it commercially. Our differentiator is that the enhancement is predicted from a pose-warped reference and carries foveation and tiles, which LCEVC's residual layer does not. The FTO review must examine the enhancement-over-hardware-base structure specifically.

**GPU texture and lossless codecs (Oodle Kraken/Mermaid, Oodle Texture, GDeflate, BCPack; ASTC/BC7 hardware formats).** They demonstrate lane-interleaved entropy decoding at tens of GB/s on GPUs (GDeflate's 32-way interleave is the model for the per-lane rANS substreams), and RDO texture encoding (Oodle Texture) is the model for rate-distortion decisions on fixed-rate blocks. They have no perceptual model and no temporal prediction. One idea is worth keeping: a zero-compute mode where each tile is an ASTC 8x8 block set (2 bpp fixed) decoded for free by the sampler. At 2160x2160x2x90 that is 1.7 Gbit/s, viable over USB 3 with foveation and useful as a fallback when the compute budget is exhausted.

**Apple Vision Pro Mac Virtual Display.** Publicly known: a direct link to the Mac, eye-tracked foveated streaming (Apple stated foveation is used in the visionOS 2 ultrawide mode), hardware codec on both ends (HEVC assumed, unconfirmed). Excellent text quality, latency figures unpublished, closed. It validates eye-tracked foveated streaming as a product.

**PSVR2.** Wired DisplayPort, no compression, eye tracking with in-engine foveated rendering. Zero codec latency, zero flexibility, a cable. Its existence is the argument that the codec's success is measured against "uncompressed over a wire", which is the near-lossless USB4 end of our bitrate range.

**Meta Quest Link / Air Link foveated encoding.** Fixed foveated encoding via the distortion-curvature warp, dynamic bitrate, Link sharpening on the client; whether eye-tracked encoding is used with Quest Pro over Link is not publicly documented (uncertain). Same hardware-codec ceilings as above.

**NVIDIA CloudXR.** HEVC/AV1 via NVENC with server-side pose prediction and a client SDK; NVIDIA-only on the server. Recent versions advertise foveated encoding (uncertain on mechanism). Its pose handling is prediction of the pose for rendering, not pose-based prediction inside the codec.

**Google split rendering.** Android XR's split-rendering work is not documented in detail; the lineage runs through Seurat (2018, offline light-field simplification) and the Daydream-era work rather than a codec. Nothing to reuse, but a likely future platform for the client.

**Academic work.** Three threads matter. (1) Pose-based prediction: Levoy 1995 ("Polygon-assisted JPEG and MPEG compression of synthetic images") is the original render-locally-stream-the-residual idea; MPEG-4 Part 2 global motion compensation (1999) is prior art for warped prediction; Furion (Lai et al., MobiCom 2017), "Cutting the Cord" (Liu et al., MobiSys 2018) and Coterie (Meng et al., ASPLOS 2020) are the mobile-VR systems that split or predict; Shading Atlas Streaming (Mueller et al., SIGGRAPH Asia 2018) and QuadStream (Hladky et al., SIGGRAPH Asia 2022) stream object-space shading instead of video and are the strongest alternative architecture (they need engine integration, which we refuse to require). (2) Foveated coding and rendering: Guenter et al. 2012, Patney et al. 2016, Kaplanyan et al. 2019 (DeepFovea, the learned peripheral reconstruction our 5.4 upsampler descends from), Illahi et al. 2020 (foveated video encoding for cloud gaming), Ryoo et al. 2016 (foveated streaming on commodity devices), Lungaro et al. 2018 (gaze-aware 360 streaming), Tursun et al. 2019 (luminance-contrast-aware foveation). (3) Perception models we lean on: Albert et al. 2017 (latency), Arabadzhiyska et al. 2017 (saccade landing), Krajancich et al. 2021 (eccentricity-dependent flicker fusion; relevant to per-tile refresh flicker), Denes et al. 2020 (motion quality vs refresh and resolution), Mantiuk et al. 2021 (FovVideoVDP). Years and venues are from memory and should be checked before publication.

## 5.7 Patent and royalty summary

Not legal advice; an engineering map of where the mines are.

- H.264: the bulk of Baseline/Main patents (2003 filings) have expired or expire by 2027; pools (Via LA) still license. Not used here except as a hybrid base through the device's own licensed decoder.
- HEVC: three pools (Access Advance, Via LA, Velos Media) plus unaffiliated holders; the messiest landscape in video. Same position: only ever used through the licensed hardware decoder.
- AV1: royalty-free under the AOM license with defensive termination; Sisvel asserts a pool against it. Not used in v1 (no decoder on the Pico 4).
- LCEVC: commercially licensed by V-Nova; our hybrid layer must be reviewed against its claims.
- JPEG XS: licensed. VC-2: royalty-free; wavelet lifting (5/3) predates it and is safe.
- Entropy coding: rANS (Duda 2013) is published without patent; note the controversial Microsoft patent on specific rANS variants (granted 2022) and steer the implementation to the plain published construction. CABAC is patented and avoided.
- Transforms: 4x4/8x8 integer DCTs of the H.264 style have expiring patents; Haar/5-3 are clear. YCoCg-R (Malvar and Sullivan 2003) had Microsoft patents from that era, now at or past expiry (verify).
- Pose-warped prediction: Levoy 1995 and MPEG-4 GMC are prior art for the concept; Meta and Microsoft (Holographic Remoting) hold patents around reprojection-based remoting details. Foveated encoding has patents from Meta, NVIDIA, Sony and Apple on specific mechanisms.

Decision: no FTO cost before Phase 2 (research code, no distribution). A proper FTO review is required before Phase 3 ships in a WiVRn NX release, scoped to: pose-warped prediction with per-tile corrections, per-tile foveated quantization driven by eye tracking, the enhancement-over-hardware-base structure, and the entropy coder. Keep a written record of the public-domain sources for every tool from day one.

## 5.8 Why this is the future

The hardware trends all move the same way. WiFi 7 (320 MHz channels, MLO, 4K QAM) puts 1 to 2 Gbit/s at a headset in practice; USB4 makes 10+ Gbit/s a cable choice. At those rates the required compression ratio for 4K-per-eye 120 Hz stereo (about 2 x 4096 x 4096 x 120 x 15 bits, roughly 60 Gbit/s raw) is 30:1 to 60:1 over WiFi 7 and under 10:1 over USB4. Intra wavelet codecs already do 10:1 visually lossless; add pose-warped prediction and foveation and 40:1 near-lossless is credible. The problem stops being "squeeze the bits" and becomes "spend the least latency and the least headset power per delivered perceptual quality", which is a scheduling and perception problem, not a transform problem.

Every headset SoC generation multiplies compute (XR2 Gen 2: about 2.5x the GPU of Gen 1 plus an NPU; the next one more) while hardware video decoders stay bound to a fixed pixel rate, a fixed format list (no 4:4:4, no alpha, no tiles-as-packets), and a fixed feature set that changes only with new silicon. The XR2 Gen 1 decoder already cannot decode two 2160x2160 streams at 90 Hz, which is why every hardware-codec streamer encodes below the panel. A compute codec's ceiling rises with every SoC and its tools change with a software update; the 5.4 tools are the proof: none of them is a wire-format change.

Eye tracking is spreading (Quest Pro, PSVR2, Vision Pro, Pico enterprise models) and it converts the periphery, 90% of the pixels, into a near-free region for whoever can put per-tile scale and quantization into the bitstream and refresh a region in one frame. Hardware codecs can approximate this only through a global warp of the input image. 4K per eye and 120 Hz double down: pixel counts grow 3.6x and frame counts 1.3x, but the fovea does not grow at all, so the foveated bit budget grows far slower than the panel.

Finally, the content is synthetic and the streamer owns the render. It has the pose, depth, engine motion vectors, layer composition and UI stencil, information a camera-video codec is built without. A codec that takes those as inputs is not competing with HEVC and AV1 on their turf; it is a different tool, and the only one whose design space expands rather than shrinks with the hardware roadmap. Fixed-function decoders will keep the base layer of the hybrid path on weak devices for years; the compute path is where every new capability lands.
