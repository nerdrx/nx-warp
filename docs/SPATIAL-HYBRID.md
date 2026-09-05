# The spatial hybrid: an HEVC periphery with an NX Warp fovea inset

Status: design study, 2026-09-05. Measurements in
[`hybrid/RESULTS-SPATIAL.md`](../hybrid/RESULTS-SPATIAL.md), simulator in
`hybrid/sim/nxvchybrid/spatial.py`.

[ADR 0022](adr/0022-hybrid-mode-is-not-a-quality-tool.md) closed the *layered*
hybrid: a full-frame HEVC base with a pose-warped enhancement layer stacked on
top of every pixel. This document asks a different question, which that ADR
does not reach and says so in as many words: what if the two codecs divide the
**image** instead of the **quality**?

> The hardware HEVC decoder carries the periphery. NX Warp carries a square
> inset over the fovea, composited on top of it with a feathered boundary.

Nothing is coded twice. There is no "which layer earns the marginal bit"
question, because no pixel is available to both coders. The trade is a
different one, and it is bounded by a different resource: the Adreno 650's
tile throughput.

**The answer, measured, is no.** The arrangement foveates genuinely well --
its best configuration opens a 12 dB fovea-over-periphery gap and matches a
foveated HEVC encode's fovea to within 0.2 dB -- and it is still beaten at
every budget and every configuration by an ordinary HEVC encoder with a
delta-QP map, by 0.14 to 0.43 JOD on FovVideoVDP, which needs no second
decoder on the client. The numbers and the recommendation are in
[`hybrid/RESULTS-SPATIAL.md`](../hybrid/RESULTS-SPATIAL.md); this document is
the design that was measured, kept because the *reason* it loses is specific
and points at the one measurement still worth doing (RESULTS-SPATIAL.md,
"What would change this answer", item 1).

---

## 1. What ADR 0022's data does and does not say about this

ADR 0022 measured a 68-point sweep and reached three findings. Two of them are
about the *layered* arrangement specifically and do not transfer; one of them
transfers with full force and is the thing this design has to survive.

**Does not transfer — "the optimum is at the boundary."** The sweep's optimum
sat at the largest base share tested and kept improving past it, converging on
plain HEVC from below. RESULTS.md is explicit about the mechanism: *"the
optimiser is minimising exposure to the model's own weakness, not discovering
something about hybrid decoding."* That optimiser had a knob the spatial
hybrid does not have. In the layered arrangement every pixel is coded by both
coders and a scalar `base_frac` slides the *same* pixels between them, so
driving `base_frac` to 1 is always available and always removes the weaker
coder from the picture. In a spatial split, driving the periphery share to 1
does not hand the fovea to x265 — it starves the fovea and leaves it at
whatever the periphery's flat allocation gave it. The two arrangements share a
parameter name and not its meaning.

**Does not transfer — "a reduced-resolution base loses 3 to 4.6 dB."** That is
a statement about an *upsampled* base being asked to carry detail the
enhancement layer then has to repair. The periphery here is full resolution
(ADR 0022 item 3 is honoured, not contradicted) and is never upsampled; it is
simply given fewer bits per pixel over the area the eye cannot resolve. The
mechanism that cost 3 to 4.6 dB is absent.

**Transfers, and is the real threat — the codec is 4 dB behind x265 in that
model, and PAPER 1.10 estimates parity.** Every bit that goes through NX Warp
instead of x265 is charged whatever the real gap is. The spatial hybrid does
not escape this; it *concentrates* it, putting the whole gap on the one region
of the image where the eye is sharpest. This is why the simulation below runs
the **real codec** from `build-ref` rather than the rate-distortion model:
ADR 0022's own "what would change this answer" item 1 asks for exactly that
re-run, and a spatial split cannot be argued from the model's numbers.

Two things ADR 0022 listed as unmeasured are load-bearing here and are still
unmeasured. **Loss** is not modelled: a lost inset is a hole over the fovea,
which is the worst possible place for one, and the mitigation (section 2.3's
fallback to the plain HEVC frame) is designed but not measured. **Frame-rate decoupling** is not modelled either.

---

## 2. Architecture

### 2.1 Two streams per eye

```
server                                     client (Pico 4)
------                                     ----------------
composited view (2048^2 per eye)
  |
  +-- crop the inset -----> nxv encoder ---> tile-run datagrams
  |                          (intra+inter,      |
  |                           own rate control) |
  |                                             v
  +-- whole frame --------> HEVC encoder ---> access units --> MediaCodec
                             (P-only,                             |
                              zerolatency)                        v
                                                          AHardwareBuffer
                                                          imported (PAPER 3.5)
                                                                  |
                                            Pass A/B --------+    |
                                            (inset tiles)    |    |
                                                             v    v
                                            reprojection pass: sample the
                                            periphery, sample the inset,
                                            cross-fade over the feather ring
```

The stream carries two `layer_desc` entries exactly as ADR 0014 already
allows: layer 0 `HEVC_NAL`, layer 1 `NATIVE`. The one field that does not
exist yet is the inset's **rectangle** — origin and extent in the frame — and
whether layer 1 covers the whole picture or a sub-rectangle of it. That is a
`layer_desc` extension, not a new container.

The two streams are not a second transport. They share the datagram format,
the FEC classes (ADR 0024), the feedback bitmap and the telemetry. What they
do not share is the reference model: the periphery is HEVC with HEVC's own
reference invalidation, and the inset keeps NX Warp's acknowledged-3x3-
neighbourhood rule (ADR 0006).

### 2.2 Aligning the inset on the tile grid

The inset must cover **whole codec tiles of the full frame**, so that a tile
index in the inset stream maps to a tile index in the frame by an integer
offset and the reprojection pass's per-tile pose lookup (INTEGRATION 2.3)
needs no second coordinate system.

For a centred inset of side `I` in an eye of width `E` the origin is
`(E - I) / 2`, which is a multiple of 64 exactly when `I ≡ E (mod 128)`. For
the Pico 4's 2048-px eye that means **the inset side must be a multiple of
128**: 512, 640, 768, ... 1408, 1536. It is not a serious restriction — the
grid it leaves is 128 px, about 2.9 degrees at the panel's centre density.

Off-grid sizes are reachable by snapping the origin down to the tile grid,
which puts the inset centre up to 32 px (0.7 degrees) off the optical axis.
That is well inside the eye box of section 4 and is the right escape hatch if
a device's eye width is not a multiple of 128, but the sweep does not use it:
an asymmetric inset would make the two eyes' scores incomparable.

Chroma: at 4:2:0 the inset origin and side are also even in chroma samples for
any multiple of 128, so no chroma-siting correction is needed at the seam.

### 2.3 Compositing and the feather

The composite lives in the reprojection pass, which is already the client's
one full-frame pixel pass (INTEGRATION 2.2, `client/shaders/reprojection.glsl`).
It becomes:

```glsl
vec3 c = texture(periphery, uv_p);              // existing tap
if (inside_inset_rect(uv)) {
    float a = feather(uv);                      // 0 at the border, 1 inward
    c = mix(c, texture(inset, uv_i), a);        // one extra tap
}
```

The feather is a smoothstep on the distance to the inset border, `F` pixels
wide, applied inward so the inset's own outer ring is what fades:

```
t = clamp(d_border / F, 0, 1)        a = t * t * (3 - 2t)
```

taken as the minimum over the two axes so corners behave. `F = 32` (half a
tile) is the default; `F = 0` is a hard seam and is measured as an A/B, as is
`F = 64`.

Three properties the feather has to have, and this one does:

1. **It is a gradient, not a step.** A step at the boundary is a
   high-contrast edge at a fixed retinal position, which is precisely the
   stimulus foveated-rendering studies report as the detectable artefact.
2. **It costs one lerp.** The extra tap is the cost; the blend is free. The
   branch is coherent across a warp because the inset is a rectangle, which is
   why the inset is a rectangle and not a disc.
3. **It degrades to the periphery.** If the inset stream is late or lost,
   `a = 0` everywhere and the frame is the plain HEVC frame WiVRn shows
   today. There is no third state to write a concealment path for.

The periphery is sampled through the existing `VkSamplerYcbcrConversion` at
4:2:0 (INTEGRATION 1.3 option 1). The inset can be 4:4:4 — the fovea is
exactly where PAPER 5.2 wants chroma fidelity — at the cost of a second
sampler and a chroma upsample of the periphery inside the feather ring so the
two agree where they meet.

### 2.4 Latency asymmetry, and how to present it

This is the part that makes the arrangement work, and it is the opposite of
what it looks like.

| path | decode latency | source |
|---|---|---|
| periphery, MediaCodec HEVC 2x2048^2, low-latency mode | 8 to 12 ms | PAPER 3.5 |
| inset, Pass A + Pass B over `T` tiles | section 3 | bench/README.md |
| composite in the reprojection pass | one extra tap | INTEGRATION 2.2 |

The two decoders run **concurrently** and the frame cannot be presented until
both have landed, so the composite's latency is `max(periphery, inset)`, not
their sum. The periphery is the long pole at 8 to 12 ms. **Any inset that
decodes in under about 8 ms is free**: it hides entirely under the base
decoder's own latency, and the composite is exactly as late as the plain HEVC
frame WiVRn presents today.

That reframes the budget question. The 5 ms of PAPER 3.4's K5 threshold is the
budget for the *pure compute* path, where the compute decoder is the only
decoder and the whole motion-to-photon chain waits on it. In a spatial hybrid
the relevant budget is "no slower than the periphery", which on this hardware
is 8 to 12 ms. Both numbers are reported in section 3 because both are real
questions, but the 5 ms figure is the wrong one to design against here.

The asymmetry that does bite is a different one. The inset can be *decoded*
before the periphery and cannot be *shown* before it. Two consequences:

* **Do not present them separately.** A tempting optimisation is to reproject
  the inset at its own, earlier deadline and let the periphery catch up. That
  is two different poses in one frame with a hard boundary between them, which
  is a shear at the inset border — visible, and worse than the latency it
  saves. One deadline, one pose, one composite.
* **The inset can afford a later deadline.** Because it is decoded after the
  periphery is already waiting, the inset's tile runs can be scheduled *last*
  in the frame's transmission order, which gives the encoder the freshest pose
  for the region where pose error is most visible. This is the one genuine
  latency win in the arrangement and it is free.

### 2.5 IDR behaviour of the periphery

ADR 0006 removed the IDR from NX Warp. The periphery is HEVC and brings it
back, so the two halves of the image have incompatible refresh models. Three
options, in order of preference:

1. **Periodic intra refresh on the periphery, no IDRs after the first.** This
   is what `x265-p-refresh` does (`--intra-refresh`, and the real
   `VK_KHR_video_encode_intra_refresh` on hardware that has it) and it is what
   the foveated anchor in the sweep uses. A refresh column sweeps the picture
   instead of a keyframe spiking the rate. The inset keeps ADR 0006's rolling
   1/180 refresh independently. The two refresh schedules are unrelated and
   must be *deliberately* unrelated: aligning them stacks two rate spikes on
   one frame.
2. **IDR on the periphery only on stream start and profile change**, matching
   ADR 0006's own exceptions. Acceptable, but a mid-stream IDR is a rate spike
   over the whole frame at a moment the inset also needs its bits.
3. **Periodic IDRs.** Rejected. It reintroduces exactly the
   invalidate-refresh-IDR ladder ADR 0006 retired, and it does so on the
   larger of the two streams.

The rate controller has to know which of these it is on, because a periphery
IDR is the one event that can starve the inset. The split of section 5 is a
*mean* over the sequence; on an IDR frame the periphery takes what it needs
and the inset drops a QP step rather than the inset dropping tiles.

### 2.6 Bitrate split

`base_frac` is the periphery's share of the total. It has a floor and a
ceiling that are structural rather than empirical:

* **Floor.** The periphery still covers about 90 percent of the picture and
  all of the motion the head produces. Below roughly half the total it visibly
  blocks, and blocking in the periphery is a motion cue the eye is *more*
  sensitive to than a static acuity loss.
* **Ceiling.** Past the point where the inset can no longer spend its share —
  a small inset at a high total simply runs out of things to code and hits
  QP 0 — extra inset bits are wasted. The simulator reports the shortfall
  rather than silently rebalancing, because a real rate controller would give
  those bits back to the periphery and the sweep should not pretend it did.

The measured optimum per budget is in `hybrid/RESULTS-SPATIAL.md`.

---

## 3. Decoder cost on the Pico 4

The gate kernel K5 (Pass A + Pass B, PAPER 3.4) measured **28.0 ms p50** for
the 2048 tiles of a 2 x 2048^2 frame on the Pico 4, with the GPU at 441.6 MHz
and the co-tenant reprojection pass running (`bench/README.md`). The run-to-run
spread in the same session was 28.0 to 33.3 ms; 28.0 is used throughout, which
is the optimistic end of the measurement.

**The assumption, stated plainly: the cost is linear in the tile count.** K5 is
per-tile work — one workgroup cluster per tile in Pass A, one in Pass B, no
frame-level stage between them — and `bench/README.md`'s own framing is that
"cost scales with tiles". Two things could break the linearity and neither is
measured: a small dispatch may not fill the GPU (which would make small insets
*worse* than linear), and a small working set may fit better in cache (which
would make them better). The model is the linear one and the error bars are
unknown.

Per-tile cost:

| | at 441.6 MHz (as measured) | at 587 MHz (the part's clock) |
|---|---|---|
| luma only, as the bench measures it | 13.67 us | 10.28 us |
| plus 4:2:0 chroma (+50%, bench "One transform plane, no chroma") | 20.51 us | 15.43 us |

The chroma correction is not optional for a shipping decoder and the bench
says so; both columns are carried because the measurement is the luma one.

### 3.1 What fits

| inset per eye | tiles/eye | tiles | 441.6 MHz | +chroma | 587 MHz | +chroma |
|---|---|---|---|---|---|---|
| 512 | 64 | 128 | 1.75 ms | 2.62 ms | 1.32 ms | 1.97 ms |
| 640 | 100 | 200 | 2.73 ms | 4.10 ms | 2.06 ms | 3.09 ms |
| 768 | 144 | 288 | 3.94 ms | 5.91 ms | 2.96 ms | 4.44 ms |
| 896 | 196 | 392 | 5.36 ms | 8.04 ms | 4.03 ms | 6.05 ms |
| 1024 | 256 | 512 | 7.00 ms | 10.50 ms | 5.27 ms | 7.90 ms |
| 1088 | 289 | 578 | 7.90 ms | 11.85 ms | 5.94 ms | 8.92 ms |
| 1216 | 361 | 722 | 9.87 ms | 14.81 ms | 7.43 ms | 11.14 ms |
| 1344 | 441 | 882 | 12.06 ms | 18.09 ms | 9.07 ms | 13.61 ms |
| 1408 | 484 | 968 | 13.23 ms | 19.85 ms | 9.96 ms | 14.93 ms |
| 2048 (whole frame) | 1024 | 2048 | 28.00 ms | 42.00 ms | 21.06 ms | 31.60 ms |

**The inset sizes that fit 5 ms**, both eyes, tile-aligned:

| budget | 441.6 MHz | +chroma | 587 MHz | +chroma |
|---|---|---|---|---|
| 2 ms (Pass C's own K6 threshold) | 512 px | 384 px | 576 px | 512 px |
| **5 ms (the K5 gate)** | **832 px** | **704 px** | **960 px** | **768 px** |
| 7 ms (K5's p99 threshold) | 1024 px | 832 px | 1152 px | 960 px |
| 10 ms (hidden under MediaCodec) | 1216 px | 960 px | 1408 px | 1152 px |
| 12 ms (hidden under a slow MediaCodec) | 1280 px | 1088 px | 1536 px | 1216 px |

The row above is unrestricted; section 2.2 requires a multiple of 128, so the
answers that can actually be built are one step coarser:

| what has to fit | 441.6 MHz (measured) | 587 MHz (the part) |
|---|---|---|
| 5 ms, luma only | **768 px** (3.94 ms; 896 is 5.36, over) | **896 px** (4.03 ms) |
| 5 ms, with 4:2:0 chroma | **640 px** (4.10 ms; 768 is 5.91, over) | **768 px** (4.44 ms) |
| 10 ms, luma only | **1152 px** (8.86 ms) | **1408 px** (9.96 ms) |
| 10 ms, with chroma | **896 px** (8.04 ms) | **1152 px** (10.00 ms) |

So the 5 ms question has a one-line answer: **640 to 896 px per eye**,
depending on which clock and whether chroma is charged. Hold that against
section 4.

---

## 4. The perceptual argument, and why the inset has to be big

The whole case rests on the inset covering the region the eye can resolve. On
a headset with eye tracking that region is small. **The Pico 4 has no eye
tracker**, and that changes the answer by more than a factor of two in area.

### 4.1 The geometry

`docs/RATECONTROL.md` 6.3 `pico4_eye()`: 2160 px per eye over tangents of
+/-0.8568 (+/-40.6 degrees), the render FOV WiVRn asks for at 1.0x scale. In a
rectilinear projection the on-axis density is `ppd_center = 22.0` and the focal
length is `f = 22.0 * 180/pi = 1260.5 px/rad`. A half-angle `theta` off axis is
`f * tan(theta)` pixels from the centre.

### 4.2 The eye box, and the thing that is easy to get wrong

PAPER 5.1.3 fixes the fixed-foveation eye box at **20 degrees horizontally and
15 vertically**, elliptical, from the VR gaze statistics: about 90 percent of
fixations land inside it and users turn their head rather than their eyes for
more.

The mistake is to size the inset to the eye box. The eye box is where the
**fovea's centre** goes, not where the fovea *is*. When the gaze sits at the
edge of the box, the `s = 1` region has to extend a further foveal radius
beyond it or the sharp region has a soft rim exactly where the user is looking.
PAPER 5.1.4 sets that radius at 5 degrees of fovea plus a pad of
`0.05 deg/ms * gaze_to_photon + 1 deg` — 3 degrees at a 40 ms budget, 5.85 at
the 57 ms worst case of its own table.

So the inset half-angles are `box + fovea + pad`:

| what | h half-angle | h px | on the 64 grid | v half-angle | v px | on the grid |
|---|---|---|---|---|---|---|
| eye box alone (the wrong answer) | 20.0 | 918 | 960 | 15.0 | 676 | 704 |
| box + 5 deg fovea | 25.0 | 1176 | 1216 | 20.0 | 918 | 960 |
| **box + fovea + 3 deg pad (40 ms)** | **28.0** | **1340** | **1344** | **23.0** | **1070** | **1088** |
| box + fovea + 5.85 deg pad (57 ms) | 30.85 | 1506 | 1536 | 25.85 | 1221 | 1280 |

(`px` is the full width, `2 * f * tan(half)`, at `ppd_center = 22.0`.)

**The inset that a Pico 4's fixed foveation actually asks for is 1344 x 1088
px per eye**, or 1344 square if the implementation wants one number. That is
357 tiles per eye, 714 tiles, **9.8 ms luma-only and 14.7 ms with chroma at
441.6 MHz** — 7.4 and 11.0 ms at the part's clock.

### 4.3 Reading that against section 3

Three verdicts, and they are different verdicts:

* **Against the 5 ms K5 gate: it does not fit, by a factor of two.** The
  largest 5 ms inset is 832 px (704 with chroma) and the eye box needs 1344.
  A spatial hybrid does not make the Pico 4 pass PAPER 3.4's gate. Nothing
  makes the Pico 4 pass PAPER 3.4's gate; K5 missed it by 5.6x.
* **Against the periphery's own latency: it fits, at the part's clock, and
  is marginal at the clock the bench measured.** 7.4 ms (luma) or 11.0 ms
  (chroma) at 587 MHz against a MediaCodec periphery at 8 to 12 ms. The luma
  number hides completely; the chroma number is at the edge and the frame
  becomes as late as its worst case.
* **A smaller inset is not a free compromise.** Dropping to 896 px (5.4 ms
  luma) covers +/-19.6 degrees, which is the eye box's horizontal half-angle
  minus 0.4 degrees and *none* of the foveal radius. The eye reaching the edge
  of its habitual range would find the sharp region ending at its fixation
  point. That is the artefact the whole scheme exists to avoid.

The scaling that saves it, if anything does, is the one PAPER 5.1.2 already
describes: the inset does not have to be coded at `s = 1` throughout. The
outer ring of the inset — past 14 degrees, where the ladder drops to `s = 1/2`
— is a half-scale tile, which is a quarter of the samples and, on the
assumption of section 3, a quarter of the tile cost. An inset coded at the
paper's own ladder rather than flat is roughly 0.4 to 0.5 of the flat tile
count, which puts 1344 px back at 4 to 5 ms. **That is the single largest
unmeasured lever in this document**, and it is unmeasured because the
simulator codes the inset flat, exactly as ADR 0022's sweep did.

---

## 5. What the simulation measures

`nxvc-hybridsim spatial` (`hybrid/sim/nxvchybrid/spatial.py`), on the
`tools/quality` v2 band-limited sequences:

| stage | what runs | modelled? |
|---|---|---|
| periphery | ffmpeg/libx265, zerolatency P-only, `ref=1`, tight VBV, full resolution, ABR at `base_frac * total` | a model of a hardware encoder, and a generous one |
| inset | `nxv-enc --eyes 2 --inter on --poses ... --fov ...` from `build-ref`, QP bisected to the remaining budget, then `nxv-dec` | **not modelled — this is the codec** |
| composite | smoothstep feather, `F` px inward, in the sequence's own 4:2:0 domain | as section 2.3 |
| quality | eccentricity-weighted PSNR/SSIM at a centre fixation (`tools/quality/foveated_metrics.py`, PAPER 5.1.2's acuity model) and FovVideoVDP JOD in the Pico 4 display model (`nxq/fvvdp.py`, PAPER 5.3's primary metric) | the metrics the paper names |
| anchors | `x265` flat at the full budget, and `x265-p-refresh` — intra refresh plus the concentric delta-QP map of `nxq/qpmap.py` — bisected on CRF to the same budget | the hardware-class opponent |

The inset is cropped from the eye with the principal point and focal length
unchanged, so the crop is a valid pinhole camera and the codec's rotation-only
homography stays exact for it; its FOV is computed rather than assumed
(`crop_fov_deg`), because a wrong FOV is a silently wrong warp (WARP 2.1).

Bitrates are quoted, as everywhere in `hybrid/`, as their **2 x 2048^2 x 90 Hz
equivalent**. The simulator's eye is 1024 px over 95 degrees and the headset's
is 2160 over 81.2, so a simulator inset translates to a headset inset by
**angle, not by a pixel scale** (`device_inset_px`); the sweep's inset column
carries both.

### 5.1 What it does not measure

Everything ADR 0022 could not measure, plus two of its own:

* **Loss.** A lost inset run is a hole over the fovea. Section 2.3's fallback
  makes it a soft hole rather than a black one, which is the right design, but
  no number here says how it looks.
* **Foveated coding inside the inset.** Flat quantisation, flat sample scale.
  Section 4.3 says this is the largest lever in the document.
* **The integer warp, entropy-coder engineering on the periphery's side, and
  the YCoCg-R round trip** — the same list as `hybrid/README.md`.
* **The reprojection pass.** FovVideoVDP is scored on the composite against
  the source, not on the output of the client's reprojection shader, which is
  what PAPER 5.3 asks for. The gap is the harness's, and it is stated wherever
  a JOD is quoted.
* **One sequence.** `vr-mixed-1024-v2`, 36 frames, one pose profile.

---

## 6. If it were built: what is touched

| component | change | size |
|---|---|---|
| `spec/`, `SYNTAX.md` | `layer_desc` gains an inset rectangle (origin, extent) and a "covers a sub-rectangle" flag | a syntax addition, no new container |
| `ref/` encoder | crop, a second rate-control target, the inset's own FOV | the encoder already takes `--fov`; the crop is a wrapper |
| `rc/` | two coupled rate targets instead of one, and the periphery-IDR interaction of section 2.5 | new control law |
| server | a second encoder instance per eye, HEVC over the frame and nxv over the crop | WiVRn already runs multiple encoders per view |
| `client/decoder/nxwarp/nxwarp_hybrid.{h,cpp}` | contains an `android::decoder` for the periphery, Pass A/B over the inset tiles only | INTEGRATION 2.4's ~350 lines, mostly unchanged |
| `client/shaders/reprojection.glsl` | one extra tap, a rectangle test, a feather lerp | ~20 lines behind a specialization constant |
| `stream_defoveator.cpp` | a second sampler and its descriptor binding | ~60 lines |
| transport | inset runs scheduled last in the frame (section 2.4); FEC class for the inset | scheduling, not format |
| `android_decoder.{h,cpp}` | `KEY_LOW_LATENCY` and the Qualcomm vendor key, `acquireLatestImageAsync` + sync fd | INTEGRATION 2.4's ~40 lines, and worth doing for HEVC today regardless |

Nothing on that list is a second codec. Every item is an extension of
something ADR 0014 and INTEGRATION already put in place for the layered
hybrid — which is the engineering argument ADR 0022 made for keeping hybrid
mode in the bitstream, still standing, now attached to an arrangement that
might have a quality case.

---

## 7. Reproducing

```sh
cd hybrid/sim
export NXVCH_CPUS=20-21 NXVCH_THREADS=2 NXQ_CPUS=20-21 NXQ_THREADS=2
export NXQ_SCRATCH=$NX_SCRATCH NXVCH_CODEC_DIR=$CHECKOUT/build-ref/bin

./nxvc-hybridsim spatial \
    --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2.yuv420p.json \
    --totals 40,80,150 --insets 128,256,384,512,640 \
    --fracs 0.40,0.55,0.70,0.85 --feather 32 --workers 2 \
    --extra-point 'total=80:inset=384:frac=0.7:feather=0:tag=noseam' \
    --extra-point 'total=80:inset=384:frac=0.7:feather=64:tag=feather64' \
    --extra-point 'total=80:inset=384:frac=0.7:hole=8:tag=hole8' \
    --extra-point 'total=80:inset=384:frac=0.7:hole=16:tag=hole16' \
    --out $NXVCH_SCRATCH/results/spatial-main.json

./nxvc-hybridsim spatial-report $NXVCH_SCRATCH/results/spatial-main.json \
    --out ../RESULTS-SPATIAL.md
```

70 points, 1186 s on a two-core slice with FovVideoVDP on the GPU. The tables
of `hybrid/RESULTS-SPATIAL.md` are what that command writes; the two prose
sections in that file are authored on top and a regeneration does not
reproduce them.
