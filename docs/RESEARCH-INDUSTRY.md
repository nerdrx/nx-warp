# Industry and open source survey, 2022 to 2026

What everyone else is shipping, what they claim, and what NX Warp should take,
prototype, watch or reject. A scouting report, not a specification. It extends
[PAPER.md 5.6](PAPER.md#56-competitive-landscape) with sources and corrects it
where the world has moved.

**How to read the sourcing.** *Sourced* means a page was opened and the claim is
on it. *Reported* means a reputable outlet states it but the primary source was
not opened. *Unverified* means the claim could not be confirmed from an opened
page and must not be repeated as fact.

Survey date: 2026-09-04.

---

## 1. Headset streaming stacks

### 1.1 Meta Quest Link and Air Link

Meta's PC link path runs hardware codecs through MediaCodec. The publicly
discussed knobs are sliced encoding, dynamic bitrate, distortion curvature and a
client-side sharpening pass. No primary Meta developer page for these settings
was opened during this survey; sliced encoding defaulting to five slices,
distortion curvature defaulting to low, dynamic bitrate ceilings around 750 to
960 Mbit, and automatic codec and slice selection are therefore **reported, not
sourced**. Application SpaceWarp, which extrapolates a frame from the depth
buffer plus engine motion vectors, is widely documented (reported); since March
2024 an app using ASW may render at half the display refresh rate.

**Take:** the slice is the closest a standard codec comes to our tile run, and it
exists for our reason, to overlap encode, transfer and decode; it stops at
frame-level references, so a lost slice still costs an IDR. ASW is the strongest
argument for the pose-warp predictor, since Meta already trusts depth plus motion
vectors to synthesise a whole frame for display and we use the same signal one
layer earlier as a predictor whose residual is transmitted. **Reject:** the
sharpening pass, a cosmetic apology for blocking that the degradation ladder is
supposed to make unnecessary.

### 1.2 Valve: Steam Link and Steam Frame

Valve announced Steam Frame in November 2025 as a streaming-first headset:
2160x2160 LCD per eye, Snapdragon 8 Gen 3, 16 GB LPDDR5X, and two radios, one for
home Wi-Fi and a dedicated 6 GHz Wi-Fi 6E radio pairing point to point with a USB
adapter on the PC, giving Valve firmware-level control of the network stack; its
foveated streaming is said to have lower latency and greater precision than Steam
Link on Quest Pro because Valve controls the whole headset stack (sourced,
UploadVR). Mechanism per press: the GPU renders the full frame sharply and the
*encoder* varies quality by gaze, so it applies to all streamed Steam content
with no per-title work (reported, PC Gamer). Valve says foveated streaming will
work on any eye-tracked headset compatible with the Steam Link app, but the 6 GHz
adapter will not, needing OS support they only have on SteamOS (sourced,
TechBuzz quoting a Valve hardware engineer).

One conflict to record: a low-quality buyer's guide claims Steam Frame ships
without eye tracking, against every reputable source consulted; treat the denial
as **wrong until a primary source says otherwise**. Ship date is reported as both
early 2026 and Q4 2026, and codec choice is undisclosed in everything opened
here, with H.264 and HEVC the long-standing assumption. Both **unverified**.

**Take:** Valve has validated, in a shipping consumer product, two bets this
project is built on: that owning the whole stack from encoder to radio beats
optimising inside someone else's pipeline, and that foveation belongs in the
transport rather than only in the renderer. Their point-to-point 6 GHz link is
the honest statement that home Wi-Fi is the enemy. WiVRn NX should treat a
dedicated AP or direct link as a supported deployment, not an exotic one.

**Differ:** Valve foveates a standard codec by modulating quality regions.
NX Warp puts per-tile scale and quantization in the bitstream and can refresh a
single region in one frame. That is a capability difference, not a tuning
difference, and it is the cleanest line to draw against them.

### 1.3 Apple Vision Pro and the visionOS Foveated Streaming framework

The most important development in the survey, and it postdates the paper.
visionOS 26.4 introduced **Foveated Streaming**, a system framework for receiving
streamed OpenXR content, documented in Apple's WWDC 2026 session 286 (sourced). A
receiver app uses the `FoveatedStreaming` framework and presents an
`ImmersiveSpace(foveatedStreaming:)`; the streaming endpoint implements the
**Foveated Streaming Protocol**, a lightweight TCP pairing and authentication
layer with JSON messages, and runs the **NVIDIA CloudXR runtime**, which is built
into visionOS. Apple ships open source reference implementations and Windows
OpenXR samples. Notable engineering details: a depth buffer is recommended, an
**alpha channel is required** so streamed content can be occluded by and blended
with RealityKit content, a Message Channels API carries opaque blobs
bidirectionally, and Xcode gains an instrument reporting stream bandwidth, pose
latency and frame rate. Gaze data stays on device behind the API. No codec and no
bandwidth figures appear in the session (sourced absence). Samsung Galaxy XR
reportedly gains comparable capability through Virtual Desktop rather than a
first-party framework (reported, UploadVR).

NVIDIA's side: CloudXR 6.0, announced at GTC 2026, is a universal OpenXR
streaming runtime with clients for Vision Pro, browsers via CloudXR.js over
WebRTC and WebXR, and standalone Quest 3 and PICO 4 Ultra (sourced, NVIDIA
developer blog). Its headline foveation claim is concrete and worth measuring
against: the system "transmits at 1K resolution while maintaining the same pixel
density at the fovea", delivering a 4K-like experience at 90 FPS over standard
5 GHz Wi-Fi, with a 200 Mbit/s minimum for local streaming. A separate report of
4K at 120 Hz untethered on visionOS is **marketing, mechanism unclear**.

**Take, urgently:** the required alpha channel. Apple has made alpha a hard
requirement for compositing streamed content with passthrough and local content,
and no mobile hardware decoder does alpha. NX Warp already lists alpha and 4:4:4
as capabilities the hardware lacks; that has stopped being a nice-to-have and
become the entry ticket for passthrough-composited streaming.

**Take:** the ecosystem shape. Apple defined a protocol and let NVIDIA supply the
runtime, which argues for NX Warp being a *codec plus wire format* other runtimes
adopt, the direction [XR_EXT_NXWARP.md](XR_EXT_NXWARP.md) reaches for. Also the
instrumentation: Apple ships a profiler reporting pose latency as a first-class
metric, so our harness should print pose latency and bandwidth alongside PSNR.
**Do not fight:** Vision Pro is closed and there is no client story there; its
value is as a specification of what the market considers table stakes.

### 1.4 ALVR

Hardware H.264, HEVC and AV1; a TCP control socket plus UDP or TCP stream
sockets; foveation by a reimplementation of Oculus's AADT warp compressing the
lateral and vertical image edges before encode; pose prediction by measuring and
averaging the tracking-poll-to-submit interval; and, on packet loss, **an IDR
request** (sourced, ALVR wiki). AV1 needs RTX 40 or RX 7000 plus a Quest 3, and
eye-tracked foveated encoding is not supported (reported).

**Take:** the pose prediction filter is simple and correct and the client shadow
needs the same measurement; the AADT warp is the right cheap fallback for
eye-tracking-less headsets and is what our fixed foveation map should reduce to.
**Differ, loudly:** the IDR request on loss is the exact failure NX Warp exists to
delete, which makes ALVR the cleanest before-picture for the acknowledged-
reference design and the right benchmark opponent on a lossy link.

### 1.5 WiVRn upstream

Upstream WiVRn now defaults to Vulkan video H.265 on AMD and Intel and NVENC AV1
or H.265 on NVIDIA, supports `vulkan` as an experimental encoder for any GPU
with Vulkan video encode, offers h264, h265, av1 and `raw`, and supports 10-bit
via VAAPI and NVENC (reported from search summaries of the WiVRn configuration
doc and release notes; the configuration page itself was not opened, so treat
specific defaults as **reported**). Recent fixes mentioned include Pico eye
tracking crashes and manual foveation centre configuration.

**This matters more than it looks.** Upstream moving its default encoder to
Vulkan video means the integration target already links Vulkan video and already
hands a Vulkan image to an encoder, so the NX Warp server slots in beside that as
another backend rather than as an alien subsystem. The `raw` codec option proves
the project tolerates a non-standard wire format, and manual foveation centre
configuration is the hook where per-tile foveation metadata should enter.

### 1.6 Virtual Desktop and the indie tier

Virtual Desktop remains the quality leader among third-party streamers: 10-bit
HEVC and AV1 10-bit on Quest 3 with RTX 40 or RX 7000, client-side Synchronous
Spacewarp extrapolating on the headset rather than the PC, VDXR as an optimised
OpenXR runtime, and recent work on full-range RGB and AMD adaptive quantization
(**reported**; no primary changelog opened). PhoneVR now recommends the ALVR
server for hardware encode, reporting roughly 150 ms latency against roughly
1500 ms for its own legacy x264 server (sourced, README). Immersed produced no
usable technical sources; **unverified**, excluded.

**Take:** SSW running on the headset rather than the PC is the correct division
of labour and mirrors our deadline presentation. Content-driven adaptive
quantization is the standard-codec approximation of our tile classifier.
**Note for honesty:** 10-bit AV1 on Quest 3 sets a real quality bar, and any
picture-quality claim we make must be measured against that configuration rather
than against 8-bit HEVC.

---

## 2. Silicon, transport and eye tracking

### 2.1 Qualcomm

Qualcomm announced **Snapdragon Reality Elite** at AWE 2026 (primary Qualcomm
page not opened, so **reported**). Against XR2+ Gen 2 it claims up to 60 percent
higher Adreno, 30 percent higher CPU, 160 percent higher NPU at 48 TOPS, up to
4.4K per eye at 90 FPS, 8K60 decode and 8K30 encode, **low-latency slice-based
decoding**, decode of AVC, HEVC, VP9 and AV1, and 20 percent longer battery life.

This validates and slightly complicates PAPER 5.8. Validation: GPU and NPU
compute keep multiplying while the video block gains a generation of formats and
a fixed pixel rate. Complication: 8K60 decode and explicit low-latency
slice-based decode are a meaningful raise on the fixed-function side, so on a
Reality Elite headset the argument "the hardware decoder cannot keep up" weakens.
What survives intact is alpha, 4:4:4, per-tile references, partial frame
presentation and single-region refresh, which no pixel rate improvement provides.
**The paper's framing should shift from "the decoder is too slow" to "the decoder
has the wrong interface", because that is the durable claim.** The Pico 4's
Adreno 650 remains today's constraint and none of this changes the hybrid plan.

### 2.2 Vulkan Video

Ratified (sourced, RasterGrid): decode for H.264 and H.265 (2022), AV1 (Feb 2024)
and VP9 (June 2025); encode for H.264 and H.265 (Dec 2023) and AV1 (Nov 2024).
Two feature extensions matter directly:

- **`VK_KHR_video_encode_quantization_map`** (announced Nov 2024, in SDK
  1.4.304, CTS 1.4.1.1) lets the application supply a per-coding-block map with
  each input picture, in two flavours: *delta quantization maps* for explicit
  codec-specific per-block QP, and *emphasis maps* for a codec-independent
  relative-importance hint (sourced, Khronos blog and registry).
- **`VK_KHR_video_encode_intra_refresh`** (announced 9 July 2025, Vulkan
  1.4.321) performs intra refresh in encode operations, aimed at robustness
  under network errors in wireless video (sourced, Khronos blog). RADV has
  landed it (reported, Phoronix).

Open-source driver support is documented for AMD RADV, Intel ANV and NVIDIA NVK
on desktop, with FFmpeg and GStreamer integration. **No Android, Qualcomm Adreno
or ARM Mali Vulkan video support was found on any page opened**; treat mobile
Vulkan video as absent.

**This is the most consequential technical finding for strategy.** The industry
has just standardised, in Vulkan, two things NX Warp claims as differentiators:
per-block quantization control, which is foveated encoding, and intra refresh,
which is IDR-free loss recovery. A competitor can now build a foveated, IDR-free
hardware-encoded VR streamer on portable Vulkan without our codec. The remaining
moat is what the *decoder* cannot do: per-tile references keyed to
acknowledgement, partial frame presentation, alpha, 4:4:4 and per-tile
resolution. Quantization maps and intra refresh do not touch any of those. Say
so in the README rather than waiting to be told.

Practically: the encode quantization map is the correct comparison baseline for
our foveation experiments, the emphasis map is a better abstraction than raw QP
delta and worth copying in spirit, and Vulkan video H.265 is now a portable,
vendor-SDK-free base layer for the hybrid path on non-NVIDIA servers.

### 2.3 Radio and cable

Wi-Fi 7 is IEEE 802.11be, approved 2024, with 320 MHz channels, 4096-QAM and
Multi-Link Operation (reported). UploadVR has argued Wi-Fi 7 will not mean much
for wireless VR *latency*, which matches our own model: the bottleneck is codec
and compositor phase, not airtime. 802.11ad delivers up to about 7 Gbit/s and
802.11ay 20 to 40 Gbit/s (reported, Wikipedia), but no 2026 headset with a 60 GHz
radio was found, so **60 GHz remains a dead end for consumer headsets**; Valve's
answer was a dedicated 6 GHz point-to-point link. USB 3 at 5 Gbit/s nominal is
the practical wired ceiling; no headset opened here uses USB4 for video.

**Take:** temper the paper's Wi-Fi 7 optimism in 5.8. The realistic 2026 target
is a dedicated 6 GHz point-to-point link at a few hundred Mbit/s to low gigabits,
not multi-gigabit home Wi-Fi. The transport already budgets at 150, 300 and
600 Mbit; keep those as the headline numbers and stop leading with 1 to 2 Gbit/s.

### 2.4 Eye tracking

Shipping with eye tracking: Apple Vision Pro, Quest Pro, PSVR2, Pico enterprise
and Pico 4 Ultra variants, Steam Frame, and Samsung Galaxy XR, which has four
inner eye cameras. Quest 3, Quest 3S and the Pico 4 consumer unit that is our
first target do not. Per-model detail is **partly unverified**; the safe planning
assumption is that eye tracking is premium-tier only and that a codec must
degrade gracefully to a fixed foveation map, which NX Warp already does.

---

## 3. Codec standards activity

**AV2.** AOMedia finalised v1.0.0 on 28 May 2026 and announced it on 9 June 2026
under the royalty-free AOMedia patent policy, with `libavm` as reference software
and a `dav2d` decoder in progress; the release mentions scalable bitstreams,
multi-view including stereoscopic video, and a planned 12-bit profile (sourced,
AOMedia press release), and makes **no mention of low latency, screen content or
immersive use** (sourced absence). Gains of roughly 30 to 40 percent over AV1 are
reported and vary by outlet. **Verdict: watch, do not chase.** Stereoscopic
multi-view is the one part worth tracking, since it targets the inter-eye
redundancy our stereo work does, but hardware AV2 decode in a headset is years
away and AOMedia's grant does not extend to us, so its tool set is not a safe
source to copy from.

**VVC / H.266.** Two relevant features: **gradual decoding refresh**, giving
tune-in points without a refresh picture, and **subpictures**, independently
decodable spatial regions for viewport-dependent delivery (reported; ITU text not
opened). Licensing is the problem: Access Advance and Via-LA pools plus a
reported 17-plus essential holders outside both as of March 2026 (reported).
**Verdict: reject as a source of tools, cite as validation.** GDR and subpictures
are a conventional codec reinventing what tile-independent coding and IDR-free
refresh give us natively. Do not read the spec for implementation ideas; the
patent exposure is the worst in the survey.

**MPEG-5 EVC.** The Baseline Profile contains only technologies that are
royalty-free or whose patents have expired (reported). **Verdict: adopt as a
patent-hygiene reference, not as a codec.** It is a curated, published list of
tools a standards body's lawyers believe are clear, and Phase 3's
freedom-to-operate review should cross-reference it against our provenance
record.

**LCEVC (MPEG-5 Part 2).** The paper's assessment stands: closest structural
analogue to our hybrid path, strongest patent overlap, licensed commercially by
V-Nova. 2026 adoption status **unverified**, no usable source opened.

**JPEG XS (ISO/IEC 21122).** Part 2 second edition is at DIS stage, adding raw
Bayer, mathematically lossless up to 12-bit and 4:2:0 content, and permitting
faster decoder implementations (reported). 2026 deployment is concentrated in
SMPTE ST 2110-22 professional environments; patent-licensed via intoPIX,
Fraunhofer and others. **Verdict: keep as the intra design-point reference,
reject the tools.** The 4:2:0 profile work confirms our 4:2:0 passthrough
decision matches where low-latency intra coding is heading.

**JPEG AI.** No usable status source was opened. **Unverified**, excluded.

---

## 4. GPU compression and entropy coding

**GDeflate, nvCOMP and DirectStorage.** GDeflate splits the stream into 64 KiB
tiles compressed independently, each decompressed by one GPU thread group
(reported, Microsoft DirectX blog). DirectStorage throughput with GDeflate is
quoted around 13 GB/s against about 5.85 GB/s uncompressed; the academic point of
comparison is GSST at 191 GB/s on an A100, 2.7 to 4x ratio (reported).

**This is the strongest direct confirmation of the Pass A design.** One
independent block, one workgroup, no cross-block state, is exactly GDeflate's
structure and exactly our tile structure, and the numbers say the shape scales.
The lesson is that GDeflate chose a *fixed, generous* 64 KiB block so per-block
setup amortises, whereas our tiles are far smaller. Our per-tile fixed overhead,
meaning header parse, table load and lane startup, is proportionally much larger
and is the thing most likely to make Pass A miss its budget. Measure it
explicitly.

**Basis Universal.** v2.5 supports ETC1S and UASTC LDR 4x4, HDR 4x4 and HDR 6x6
with optional RDO, transcoding to essentially any GPU texture format via KTX2,
under an Apache-2.0 encoder library; ASTC HDR 6x6 lands at 3.56 bpp (sourced,
README and wiki). **Take:** the practical route to the paper's zero-compute
fallback mode. The paper budgeted 2 bpp for ASTC 8x8; 6x6 at 3.56 bpp is the
nearer, better-tooled option to prototype first.

**Oodle Texture and Kraken** are proprietary; RDO on fixed-rate blocks remains
the model for the ASTC fallback's rate control, but the code is unusable and 2026
terms are **unverified**. **AMD Compressonator**: no source opened, excluded.

---

## 5. Academic work worth reading

**EyeNexus** (arXiv 2509.11807) combines gaze-driven spatial compression with
gaze-driven video encoding, sizing foveation regions from both bandwidth and
gaze, reporting up to 70.9 percent latency reduction and up to 24.6 percent
perceptual quality improvement (sourced, abstract only; the PDF exceeded the
fetch size limit, so methodology and baselines are **unverified**).

**NajVR** (Springer, *Computing*, 2025) is plug-and-play remote rendering for
unmodified VR apps using **LHE**, a GPU-accelerated codec built for minimal
encode latency and loss robustness, reporting up to 75 percent lower encode
latency than commercial H.264 and round trips slightly above 20 ms on LAN, 50 to
60 ms on WAN (reported; paper not opened).

NajVR is the closest published thing to NX Warp's thesis, a GPU-side custom codec
beating hardware H.264 on latency and loss. Read it in full and, if the numbers
hold, cite it as independent support for the core bet. EyeNexus is the reference
for adaptive foveation region sizing under bandwidth pressure, a gap in our rate
control today: our foveation map responds to gaze but not to available bitrate.

---

## 6. Patents and licensing

Three findings change the risk picture.

1. **Intel US12348767B2, "Adaptive foveated encoder and global motion
   predictor"**, granted 1 July 2025, priority 23 November 2017 (sourced, Google
   Patents). The claims cover higher quality for macroblocks inside a gaze focus
   region than outside, *and* deriving a global motion predictor for a macroblock
   encode from HMD position information rather than a full motion search. **This
   is a direct read on the combination NX Warp is built from**, and it must lead
   the Phase 3 freedom-to-operate agenda. Mitigating arguments exist, since our
   prediction is a per-tile homography warp of a reference frame rather than a
   motion vector predictor for a macroblock search, and our quantization is
   per-tile in the bitstream rather than a macroblock quality parameter. Those
   are arguments for counsel, not for a README.
2. **AMD / ATI US10432988B2**, priority 15 April 2016, covering viewpoint
   prediction before render, rendering only the predicted portion, dedicated
   wireless channels, and DMA paths bypassing the CPU (sourced). Reads on the
   *system*, not the codec, and touches our no-CPU-on-the-hot-path claim. Lower
   risk, but log it.
3. **Varjo US11568574B1, "Foveation-based image encoding and decoding"**
   (sourced assignee). Foveated coding is a dense, active thicket with at least
   Intel, Varjo and Qualcomm in it. Expired prior art exists, notably US6252989B1
   (2001) on foveated image coding for bandwidth reduction, usable as a cited
   public-domain basis for the *idea* in our provenance record.

The conclusion is unchanged in direction and sharper in degree:
foveation-in-the-encoder is patented territory, our defence is that our tools are
specific, published and either expired or public domain, and AV2's royalty-free
policy and EVC Baseline are the external references for how that record looks.

---

## 7. Top 10 things to steal, ranked

1. **Alpha as a first-class requirement.** Apple's framework *requires* alpha to
   composite streamed content with passthrough and RealityKit content, and no
   mobile hardware decoder does alpha. **Implement now:** promote alpha from a
   capability bullet to a headline feature and check the syntax reserves it.
2. **Vulkan encode quantization maps and intra refresh as the honest baseline.**
   Two features we call differentiators now exist portably for hardware encoders.
   **Implement now** as a benchmark target and a README correction: measure
   against Vulkan video H.265 with a delta QP map, not against naive HEVC.
3. **GDeflate's per-block amortisation lesson.** One workgroup per independent
   block is confirmed at scale, but their block is 64 KiB and ours is a tile.
   **Implement now** as an explicit Pass A measurement of fixed per-tile overhead,
   before optimising anything else.
4. **ALVR's pose prediction filter.** Measure poll-to-submit, average, predict
   forward. Simple, proven, and the client shadow needs it. **Implement now.**
5. **Dedicated point-to-point 6 GHz link as a first-class deployment.** Valve
   shipped it because home Wi-Fi is unreliable, and it is the difference between
   our 150 and 600 Mbit budgets. **Implement now** in docs and test configs;
   no code, just an assumption to stop hiding.
6. **Bandwidth-adaptive foveation region sizing (EyeNexus).** Our map follows
   gaze but not the link. **Prototype behind a tool bit.**
7. **A zero-compute ASTC tile mode via Basis Universal.** Apache-2.0 encoder,
   sampler-decoded, a defined escape hatch when the GPU budget is gone; 6x6 at
   3.56 bpp before 8x8 at 2 bpp. **Prototype behind a tool bit.**
8. **Emphasis maps rather than raw QP deltas as the rate control interface.**
   The tile classifier should emit importance and let a later stage turn it into
   quantization. **Prototype behind a tool bit**, an internal refactor with no
   wire cost.
9. **A protocol-plus-runtime split, following Apple and NVIDIA.** A template for
   how `XR_EXT_NXWARP` is positioned. **Watch**, and let the extension draft
   absorb the lesson without chasing Apple's actual protocol.
10. **AV2 stereoscopic multi-view coding.** The only part of AV2 aimed at our
    problem. **Watch**, revisit after the FTO review, since AOMedia's grant does
    not extend to us.

**Explicitly rejected:** VVC tools as an implementation source, on patent
exposure, despite GDR and subpictures being conceptually aligned; Link-style
sharpening; 60 GHz radios, a dead end no 2026 headset pursues; and chasing AV2
compression efficiency, since a 30 percent bitrate win is not our axis.

---

## 8. Positioning

Say this, and nothing stronger. **Against Meta Link**, NX Warp is slice-level
pipelining taken to its end: the tile is the loss, reference and concealment
unit, so there is no IDR and no slice whose reference is a whole frame; ASW
already proves the industry trusts depth and motion vectors to synthesise a
frame, and we use the same signal one layer earlier as a corrected predictor
rather than as displayed extrapolation. **Against Valve** we agree completely and
go one step further: they showed that owning the stack and foveating in the
transport wins, but they foveate a standard codec by varying encode quality,
while we put per-tile resolution and quantization in the bitstream and refresh one
region in one frame, which a standard decoder cannot express; their 6 GHz link is
the deployment we should assume rather than the exception. **Against Apple and
CloudXR** we are the open, vendor-neutral counterpart to a closed first-party
framework backed by one GPU vendor's runtime, and we should be candid that
CloudXR 6.0's fovea-density-preserving 1K transmission at 90 FPS over 5 GHz Wi-Fi
is a serious result we have not matched; we differ concretely on alpha, 4:4:4,
partial-frame presentation and a published bitstream anyone can implement.
**Against ALVR and WiVRn** we are not a competitor but a proposed encoder backend
and a research answer to the one thing neither can fix, that loss costs an IDR.
**Against Virtual Desktop** we should be humble: 10-bit AV1 on Quest 3 is a
genuinely excellent picture and our quality claims must be measured against it,
not against a straw man. **Against the standards bodies** we are not competing
with AV2, VVC or JPEG XS on compression efficiency; we are a special-purpose
codec for synthetic frames whose renderer hands over pose, depth, stereo and
layer structure, and our only bitrate claim is that we spend the fewest bits
during head motion. **Against Qualcomm's roadmap** we should retire "the hardware
decoder is too slow", because Reality Elite claims 8K60 decode with low-latency
slice-based decoding, and replace it with the argument that survives every
generation of silicon: the hardware decoder has the wrong interface, and no
increase in its pixel rate gives it alpha, per-tile references, or the ability to
present a frame that is not finished.

---

*Every vendor number here is that vendor's claim, not a measurement by this
project. Items marked unverified must not be repeated as fact.*
