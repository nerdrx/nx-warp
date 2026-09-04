# Academic research scout, 2022 to 2026

A survey of the published literature for tools worth stealing, prototyping or ignoring in NX
Warp, across pose compensated prediction, foveated coding and rate control, VR perceptual
metrics, learned compression on phone class hardware, GPU entropy coding, neural texture
compression, splatting streaming, error resilient wireless VR coding, and compute decoding.

**Provenance rule.** "Verified" entries were opened through the arXiv API or an abstract page
here; title, authors and date are as returned. "Listing only" entries came from an API result set
whose individual abstract was not opened, so author ordering and venue may be wrong. "Not found"
means it could not be retrieved, usually because the paper is ACM or IEEE only. Nothing is
invented. Cost figures attributed to NX Warp are my estimates, not the papers'.

**Cost anchor.** The Adreno 650 in the Pico 4 is roughly 1.2 TFLOP FP32, call it 2 TFLOP FP16
peak with 40 to 60 percent achievable in a bandwidth bound kernel. At 2160x2160 per eye, stereo,
90 Hz, the decoder sees about 840 Mpixel/s, a whole decoder budget near 1200 FLOP per pixel, so
about **600 MAC per pixel for everything**, transform and entropy and warp included. Any learned
tool costing 1k MAC/px can touch only a fraction of the frame, which is why most verdicts below
are "prototype" rather than "implement".

---

## Ranked shortlist

1. **Transmit entropy scale parameters, do not demand bit exact arithmetic** (MLVC, ECCV 2026).
   Removes the determinism wall 5.4 calls its highest risk item.
2. **Recoil style resumable rANS** (ICPP 2023). Intermediate state metadata turns our fixed
   eight lanes into a decoder adaptive split.
3. **Foveated frame rate reduction** (Flöter et al. 2025). Peripheral tiles can refresh below
   the fovea rate unnoticed, a saving our ladder does not take.
4. **Peripheral temporal visibility model** (Tursun and Didyk, TOG 2022). The calibrated
   threshold that tells the refresh scheduler when a tile update will be seen.
5. **Learned in loop filtering as a lookup table** (Li et al. 2024, 2025). A learned deblocker
   as a texture fetch: bit exact by construction, nearly free on Adreno.
6. **Checkerboard context** (Fu et al. 2023). The only learned entropy shape that fits one
   workgroup per tile with no serial state.
7. **Gaze prediction from head motion** (GazeProphetV2, 2025). A fovea box following predicted
   gaze beats a static box on the Pico 4, which has no eye tracking.
8. **Sandwiched compression** (Isik et al. 2023). Our out of loop rule formalised, with the
   networks trained through the codec instead of against generic degradation.
9. **Neural texture compression** (Farhadzadeh et al. 2024; Matai et al. 2026). Proof a learned
   decoder can be per pixel independent, and a warning that divergence costs up to 8.48x.
10. **GRACE and ReVo** (2023, 2026). Shaping a codec for loss beats adding FEC, and this is the
    baseline our concealment claim must be measured against.

---

## Detailed entries

### 1. MLVC: Multi-platform Learned Video Codec for Real-World Deployment

Pärnamaa, Lumiste, Loot, Indenbom, Znobishchev, Saabas. arXiv 2606.28027, ECCV 2026. Verified.

Neural codecs die in the field because float inference differs across vendors and the entropy
decoder then desynchronises catastrophically. MLVC stops chasing bit exact arithmetic and
instead transmits the entropy model's scale parameters through the hyperprior, so both sides use
identical distributions even when features differ; the bitrate cost is recovered with gated
memory, ReGLU and long term reference recovery. Reported: over 70 percent BD-rate (MOS) gain
over hardware HEVC, encoder and decoder near 100 FPS on commodity Apple, Intel and Qualcomm
NPUs. Cost: NPU class, far over our budget, but the idea costs nothing. Fit: perfect, and per
tile parameter transmission is already what our quant table capability does. Patent risk: the
architecture is likely encumbered, but "send the distribution" is hard to claim narrowly. **Verdict: implement now, as a design principle.** Add a
`TOOL_TRANSMIT_ENTROPY_PARAMS` capability and stop deferring in loop learned tools purely on
determinism grounds.

### 2. Recoil: Parallel rANS Decoding with Decoder-Adaptive Scalability

Lin, Arunruangsirilert, Sun, Katto. arXiv 2306.12141, ICPP 2023. Verified.

A single rANS stream can be decoded from an arbitrary position given the intermediate state
there. Recoil stores those states as metadata beside symbol indices, splits the bitstream with a
load balancing heuristic, and lets a decoder merge splits it cannot run concurrently, so
parallelism overhead scales to the decoder rather than being fixed by the encoder. Throughput is
reported comparable to conventional rANS while scaling on CPUs and GPUs; no GB/s figure is
given. Cost: near zero compute, a few metadata bytes per split. Fit: excellent and tile local,
since all splits live inside one tile. It fixes the compromise in 6.3, where fat tiles want more
than eight lanes and thin tiles waste them, and serves principle 5 directly: a strong headset
could use 32 lanes on the same bitstream a weak one decodes with 8. Patent risk: low, though the
Microsoft rANS variant patent in 5.7 still needs steering around. **Verdict: prototype behind a tool bit.** The gain is
scheduling, not bits, so measure against Phase 0's K3 first.

### 3. Evaluating Foveated Frame Rate Reduction in Virtual Reality for Head-Mounted Displays

Flöter, Geringer, Reina, Weiskopf, Ropinski. arXiv 2505.03682, 2025. Verified.

Instead of cutting peripheral resolution, cut peripheral *frame rate*: fovea at full rate,
periphery lower. With 15 participants they find average rendered pixel count can be cut
substantially before observers consistently report temporal artifacts; the abstract gives no
single percentage, so the magnitude is unquantified. Cost: negative, a skipped tile costs
nothing to decode. Fit: the cleanest in this survey, since refresh cadence is a per tile
scheduling decision with no serial state. 2.8 already gives temporal decoupling and 5.1.1
already has a foveation map with three consumers; this adds a fourth, refresh rate. The catch is
that per tile refresh flicker is exactly the artifact 5.3 says our pop-in metric must track.
**Verdict: implement now** as an extension of the degradation ladder, gated on entry 4.

### 4. Perceptual Visibility Model for Temporal Contrast Changes in Periphery

Tursun and Didyk. arXiv 2205.00108, ACM TOG 2022, doi 10.1145/3564241. Verified.

Psychophysics of sensitivity to spatio-temporal stimuli across wide eccentricity, fitted into a
model predicting how visible a temporal change is at a given distance from gaze. Their own
applications are inserting peripheral content unnoticed and evaluating temporal aliasing in
foveated rendering. It is the missing half of entry 3: not just that peripheral frame rate can
drop, but whether a specific tile's specific update will be seen.

Cost: zero, it runs encoder side. Fit: a per tile scalar function of eccentricity and local
contrast, the same shape as `dQ_ecc` and `dQ_act`, evaluable alongside the existing per tile
variance pass. Gain: no bitrate claim, the contribution is the threshold. Patent risk: none
meaningful. **Verdict: implement now** in the rate controller as the refresh scheduler's cost
function, and cite it in 5.2 beside Watson and Mullen.

### 5. In-Loop Filtering Using Learned Look-Up Tables for Video Coding

Li, Li, Li, Li, Li, Liu, Wu, arXiv 2509 (2025), and "In-Loop Filtering via Trained Look-Up
Tables", arXiv 2407 (2024), overlapping authors. Listing only, so author ordering is uncertain.

A DNN filter is trained over a small fixed input range, then every possible input is enumerated
and cached into lookup tables, so at decode time the filter is table lookups plus interpolation
with no inference. The 2024 version reports 0.13 / 0.34 / 0.51 percent BD-rate with 164 KB to
1148 KB of tables; the 2025 version reports 0.82 / 2.97 / 1.63 percent. Cost: a handful of
texture fetches, 10 to 30 equivalent MAC/px, inside budget even frame wide. Fit: perfect. A LUT
is bit exact on every vendor, killing the determinism problem, and has no cross tile state, so
this could run *in loop* where 5.4 forbids learned tools. BD-rate is small, but our artifact is
ringing and blocking in the periphery, where a filter earns more than BD-rate suggests. Patent
risk: recent, with possible pending claims; filter-as-table has old prior art. **Verdict: prototype behind a tool
bit**, and note it would be the first in loop learned tool we can justify.

### 6. Fast and High-Performance Learned Image Compression With Improved Checkerboard Context Model, Deformable Residual Module, and Knowledge Distillation

Fu, Liang, Liang, Wang, Zhang, Han. arXiv 2023-09. Listing only.

Checkerboard context splits latents into two interleaved sets, the first decoded with no context
and the second conditioned on it, so decoding is two parallel passes rather than a raster scan;
separate distribution networks recover the RD loss. Reported about 70x to 90x faster decode than
sequential context adaptive learned codecs, 2.3 percent better RD, beating VVC intra on Kodak.
The decoder is still out of reach frame wide on Adreno 650, but the context *shape* is the point:
two pass checkerboard is the only learned entropy structure that maps to one workgroup per tile
with no serial state, since both passes are within tile. Patent risk: widely published, this
combination may be claimed. **Verdict: watch**, the template a future learned coefficient model
must follow.

### 7. GazeProphetV2: Head-Movement-Based Gaze Prediction Enabling Efficient Foveated Rendering on Mobile VR

Ebadulla, Mudlpaur, Chaurasia, Gaurav BV. arXiv 2511.19988, 2025. Verified. No peer reviewed
venue stated, so treat the numbers with caution.

A gated fusion model with cross modal attention combines gaze history, head orientation and
scene content to predict gaze 1 to 3 frames ahead. Trained on 22 scenes and 5.3M gaze samples,
reporting 93.1 percent cross scene validation accuracy. The motivation is foveation on headsets
with no eye tracker. Cost: zero on the headset, it runs server side on the pose stream we
already send twice (6.7). Fit: it changes only the foveation map, a per tile input with no cross
tile state. Gain: 5.1.3 falls back to a fixed box sized by the lens on the Pico 4, and a gaze
prior should let `R_box` shrink, which is where nearly all our bits go. Risk: a wrong prediction
puts the fovea in the wrong place, far worse than a slightly large box, so it must widen the box
as a soft prior rather than move it. Patent risk: Meta and Tobii hold commercial patents here.
**Verdict: prototype behind a tool bit**, encoder side, confidence gated against the fixed box.

### 8. Practical Saccade Prediction for Head-Mounted Displays: Towards a Comprehensive Model

Arabadzhiyska, Tursun, Seidel, Didyk. arXiv 2205.01624, 2022. Verified.

Follows the 2017 saccade landing work by asking which extra factors matter, saccade orientation
in 3D, smooth pursuit and inter user variability, and offers a cheap correction adapting existing
predictors to these without new data collection; no accuracy number appears in the abstract.
Cost: zero, encoder side. Fit: 5.1.4 budgets padding for eye tracking latency, and landing
prediction shrinks it, while saccadic suppression means tiles in transit need not be sharp at
all. Patent risk: Sony and Meta hold rendering side patents, academic prior art from 2017.
**Verdict: watch** until we run on an eye tracked headset. Dead weight on the Pico 4.

### 9. Sandwiched Video Compression: Efficiently Extending the Reach of Standard Codecs with Neural Wrappers

Isik, Guleryuz, Tang, Taylor, Chou. arXiv 2303, 2023. Listing only.

A neural preprocessor maps the source into a proxy image, a standard codec transports it, and a
neural postprocessor maps back, both trained end to end through differentiable approximations of
the codec including its in loop filtering. Reported 6.5 dB over HEVC on the tasks studied and
about 30 percent rate improvement measured with LPIPS. Cost: the postprocessor is a full
network, affordable only on a fraction of pixels, matching our 5.4 upsampler budget of about
1.1k MAC/px on quarter scale tiles. Fit: this is the academic formalisation of our out of loop
rule, and more useful than that rule is today, because the sandwich is trained *through* the
codec. Our 5.4 tools are trained against generic degradation; training them against our actual
quantiser and warp should be worth more than the architecture choice. Patent risk: Google
authored, likely patented, with extensive prior art on pre/post wrappers. **Verdict: implement now**, not as a new tool but as the training
methodology for the peripheral upsampler already planned.

### 10. Neural Graphics Texture Compression Supporting Random Access

Farhadzadeh, Hou, Le, Said, Rauwendaal, Bourd, Porikli. arXiv 2405, 2024, Qualcomm. Listing only.

A heavy convolutional encoder offline paired with a small fully connected decoder evaluable for
one texel without its neighbours, with mip support. Random access is the property that matters,
since a per texel MLP has no decode order dependency; the sibling cooperative vector work
(Belcour and Benyoub, arXiv 2506) puts a comparable decoder at 0.55 ms for a 4K texture set at
1080p on a desktop GPU. Per texel independence is stronger than our per tile rule, so principle 2
is satisfied trivially, and this is the best evidence that a learned decoder can be made output
independent. Patent risk: Qualcomm, high. **Verdict: watch.**

### 11. Thread-Efficient Decoding for Neural Texture Compression

Matai, Ikeda, Lipski, Harada. arXiv 2608.27888, 2026, AMD. Verified.

Per material MLP decoders cause GPU thread divergence when neighbouring pixels use different
decoders. The fix is a shared decoder MLP trained with gradual freezing, plus semantic texture
clustering via CLIP embeddings so a workgroup mostly runs one decoder. Reported 25 to 52 percent
divergence reduction and up to 8.48x speedup on a Radeon RX 9070 XT over 500+ textures. Fit: the
lesson transfers hard. NX Warp deliberately lets adjacent tiles differ in scale, chroma format,
mode and QP, exactly the divergence pattern measured here at up to 8.48x. If Phase 0's K3
disappoints, clustering tiles by mode within a dispatch so a workgroup's neighbours share a code
path may be worth more than any coding tool. Patent risk: AMD, but the transferable part is a
scheduling observation, not a mechanism.
**Verdict: implement now** as a dispatch ordering experiment in Pass B, costing an indirect
draw list and nothing else.

### 12. GRACE: Loss-Resilient Real-Time Video through Neural Codecs

Cheng, Zhang, Li, Arapin, Zhang, Zhang, Liu, Zhang, Yan, Mazumdar, Feamster, Jiang. arXiv
2305.12333, 2023. Verified.

Encoder and decoder are jointly trained under a spectrum of simulated packet losses, so the
decoder degrades gracefully instead of producing an undecodable frame. Reported: quality
comparable to H.265 when lossless, 95 percent fewer undecodable frames and 90 percent less stall
time than FEC, 38 percent higher MOS in a 240 participant study. Cost: a neural codec, out of
budget on Adreno 650. Fit: the architectural claim partly *competes* with us. Our answer to loss
is structural, which is cheaper and bit exact, but their result that trained resilience beats
FEC on stall time is a caution that our 4.4 FEC overhead may buy less than it looks. **Verdict: reject as a tool, adopt as a
benchmark.** Our loss experiments should report undecodable frame rate and stall duration, not
just PSNR under loss.

### 13. ReVo: A Cross-Layer Reliable Volumetric Videoconferencing System

Aditya, Maji, Wang, Ramakrishna, Sitaraman, Shenoy. arXiv 2026-04. Listing only.

Splits volumetric video into RGB and depth streams, applies FEC selectively based on which
stream and region matters, and adds post decode neural recovery. Reported median SSIM up
32 percent (RGB) and 13 percent (depth), freezes down 95.7 percent.

Cost: the neural recovery stage is the expensive part; selective FEC is free. Fit: the selective
FEC half extends 4.4 directly. Our FEC priority is driven by foveation and tile class; theirs is
driven by which stream a datagram belongs to. Once we carry a depth stream in Phase 4 that
distinction becomes ours, and depth tolerates loss very differently from colour. Patent risk:
low. **Verdict: prototype behind a tool bit** in transport, when the depth stream lands.

### 14 and 15. DCVC-RT and MobileNVC, the two mobile neural codec reference points

**Towards Practical Real-Time Neural Video Compression (DCVC-RT)**, Jia, Li, Li, Xie, Qi, Li,
Lu, arXiv 2502.20762, CVPR 2025, Microsoft. Verified. The headline is the diagnosis, not the
architecture: the bottleneck in neural codecs is *operational* cost, memory I/O and kernel
launches, not arithmetic. It drops explicit motion for implicit temporal modelling on one low
resolution latent, integerizes the model for cross device consistency, and reports
125.2 / 112.8 fps encode / decode at 1080p with 21 percent bitrate saving over H.266 VTM. That
is roughly two orders of magnitude beyond Adreno 650 at our pixel rate, so no fit as a codec,
but the memory I/O diagnosis is the same conclusion 3.2.5 reaches independently and should
govern how Phase 0 is judged. Patent risk: Microsoft, high. **Verdict: reject as a tool, cite as
corroboration** that traffic sets our budget.

**MobileNVC: Real-time 1080p Neural Video Compression on a Mobile Device**, van Rozendaal,
Singhal, Le, Sautiere, Said, Buska, Raha, Kalatzis, Mehta, Mayer, Zhang, Nagel, Wiggers, arXiv
2310.01258, WACV 2024, Qualcomm. Verified. The first real time 1080p neural inter frame decoder
on a phone: block based motion instead of dense flow, integer quantization throughout, and a
split across three engines with inference on the NPU, entropy coding on the GPU and warping on
dedicated hardware. Up to 48 percent BD-rate over prior mobile neural codecs. That is 1080p30
class against our 840 Mpixel/s, about 13x short. Two things transfer: entropy coding on the GPU
concurrent with inference elsewhere is a pattern the hybrid path can copy, and they chose block
based motion over dense flow for the reason we chose one vector per tile. Patent risk: Qualcomm,
high, and close to our territory. **Verdict: watch**, add to the Phase 3 FTO scope.

### 16. Nebula: Enable City-Scale 3D Gaussian Splatting in Virtual Reality via Collaborative Rendering and Accelerated Stereo Rasterization

Zhu, Liu, Li, Wu, Zhao, Liu, Gan, Leng, Feng. arXiv 2512.20495, 2025. Verified.

Instead of streaming video, the cloud streams intermediate results after a level of detail
search and the client rasterises. Two contributions: temporal aware LoD search improving memory
locality, and a stereo rasteriser where the two eyes share most computation with bit accurate
quality. Reported 2.7x motion-to-photon speedup and 19 to 25 percent bandwidth reduction versus
lossy video streaming. Fit: the strongest competing architecture found, and the numbers are
uncomfortable, being the same shape of claim NX Warp makes. It requires the scene to be a
Gaussian field, so engine integration and a largely static scene, which we refuse to require,
the same objection 5.6 raises against Shading Atlas Streaming and QuadStream. Their stereo
sharing result bears directly on our 2.5 inter view prediction. **Verdict: reject as an architecture, steal the stereo observation**, and
update 5.6 with their number.

### 17. Mon3tr: Monocular 3D Telepresence with Pre-built Gaussian Avatars

Lin, Hu, Liu, Zhuang, Lin, Zhang. arXiv 2601, 2026. Listing only.

Offline multi view reconstruction builds an avatar; monocular inference then drives it through a
corrective deformation network. Reported about 60 FPS on a Meta Quest 3, under 0.2 Mbps, about
80 ms end to end, a real Adreno 740 result. Fit: none for the codec, but 5.5 names social VR
with dozens of avatars as the hard case that sets our bitrate floor. If avatars travel as driven
models rather than pixels, that case has a non codec answer. **Verdict: watch**, note in 5.5.

### 18. EyeNexus: Adaptive Gaze-Driven Quality and Bitrate Streaming for Seamless VR Cloud Gaming

Wu, Alhilal, Tsui, Siekkinen, Hui. arXiv 2509.11807, 2025. Verified. No peer reviewed venue
stated.

Combines gaze driven spatial compression, a warp shrinking the periphery before encoding, with
gaze driven encoding, QP modulation inside the encoder, aligning the gaze point precisely
between the two, and adapts the foveation region to both bandwidth and gaze. Reported up to
70.9 percent latency reduction, 24.6 percent quality improvement, and up to 48 percent
playability improvement with reduced motion sickness in an IRB approved study. Cost: the client
side inverse warp is a sampler pass. Fit: this is what ALVR and WiVRn do plus per block QP, the
closest published system to our 5.1 model. Two findings transfer: the spatial warp and the QP
map must be exactly aligned or the two foveations fight, which makes 5.1.1's one map three
consumers rule load bearing rather than tidy; and adapting the foveation *region* to bandwidth
is a rate control input we do not have. Patent risk: Meta and NVIDIA hold foveated encoding
patents. **Verdict: implement now**, specifically making
the foveation radius a function of rate controller pressure and not only of gaze and lens.

### 19 and 20. Also noted, briefly

**Fast Entropy Decoding for Sparse MVM on GPUs (dtANS)**, Schätzle, Pegolotti, Püschel, arXiv
2603.01915, IPDPS 2026, verified. A GPU oriented ANS variant giving up to 11.77x size reduction
and up to 3.48x end to end speedup, meaning the entropy decode is cheaper than the traffic it
saves, the same bet 3.2.5 makes. Open source CUDA. **Verdict: watch**, read the lane layout
before finalising Pass A.

**Foveated Video Streaming for Cloud Gaming**, Illahi, Siekkinen, Masala, arXiv 2017-06, listing
only, older than our window but the origin of the "over 50 percent bandwidth" figure our
foveation targets should be compared against. The 2020 follow up cited in 5.6 was **not found**
on arXiv here and should be checked before publication. **Verdict: cite, do not implement**,
superseded by EyeNexus.

---

## Surprises: things that contradict our paper

**Bit exactness may not be the right requirement.** 5.4 makes integer exact inference on both
ends the gate for any in loop learned tool and calls it the highest risk item. MLVC solved this
differently: transmit the entropy model's parameters and let the features diverge. Our gate is
stricter than the state of the art requires, and it blocks the tool with the largest claimed
gain.

**Our loss story may be measured against the wrong baseline.** 4.4 spends bits on prioritized
FEC. GRACE reports 95 percent fewer undecodable frames and 90 percent less stall than FEC by
shaping the codec for loss instead of protecting it. We shape structurally for loss too, so we
may be paying for FEC we do not need. The experiment 4.11 should run is FEC off, concealment
only, under 5 percent loss.

**Gaussian splatting streaming already beats video on our own axes.** Nebula reports 19 to
25 percent less bandwidth *and* 2.7x better motion to photon than lossy video streaming on real
VR content. 5.6's objection that the object space family needs engine integration still holds,
but "nothing to reuse" understates how strong the competing numbers now are.

**Divergence may cost more than any coding tool gains.** Matai et al. measure up to 8.48x from
reducing thread divergence alone. NX Warp deliberately allows adjacent tiles to differ in scale,
chroma sampling, mode and QP, and nothing in 3.2 accounts for the divergence that creates. If
Phase 0's K3 misses, this is the first place to look, ahead of the transform or the entropy
coder.

**The periphery can lose frames, not just pixels.** Our degradation ladder is entirely spatial:
blur, scale, chroma. Entries 3 and 4 say the temporal axis is available and calibrated, and we
are not using it.

---

## What nobody has done: gaps NX Warp could own

**Pose warped prediction inside the bitstream, at tile granularity.** Every system found here
either warps *outside* the codec (ATW and ASW on the client, EyeNexus's warp before the encoder)
or streams a scene representation instead of pixels (Nebula, Mon3tr). Nobody here puts head pose
into the predictor of a conventionally shaped residual codec and codes the correction. That is
the core NX Warp claim, and no direct academic competitor turned up in 2022 to 2026, which also
means there is no published baseline to compare against.

**Per tile refresh rate as a coded, signalled property.** Foveated frame rate reduction exists
as a rendering technique and the visibility model exists, but no codec carries per tile refresh
cadence in its bitstream with a defined concealment for skipped frames. Our per tile reference
tracking is already the machinery this needs.

**A decoder adaptive entropy split in a video bitstream.** Recoil shows the technique in general
compression. No video codec found here lets the *decoder* choose its parallelism width on an
unmodified bitstream, because every other codec has cross block entropy state. Combined with
principle 5, this is a differentiator nobody else has an architecture for.

**Learned tools trained through the real quantiser and the real warp.** The sandwich paper trains
through a standard codec. Nobody trains a peripheral reconstruction network through a *pose
warped* predictor, where the job is to fix a reprojection hole rather than generic blur. Our 5.4
upsampler could be the first, and the training data is free because the simulator already
replays real sessions.

**Foveation radius driven by network pressure.** EyeNexus adapts the region to bandwidth, but as
a system heuristic outside the codec. A codec whose foveation map is a first class rate control
variable, degrading box radius under pressure the way a bitrate ladder degrades resolution, does
not appear in this survey.

**An honest VR codec benchmark.** Every paper here reports a different metric on different
content: BD-rate against VTM, MOS, SSIM, motion to photon. Nothing found measures a VR streaming
codec end to end in display space with FovVideoVDP *and* motion to photon *and* loss behaviour
on one axis. 5.3 already specifies that harness; publishing it with the replay corpus may be a
larger contribution than the codec.

---

## Sources not opened

Unverified, named only: ColorVideoVDP (Mantiuk et al. 2024, Cambridge PDF seen in search results
only); QuadStream and Shading Atlas Streaming (not on arXiv); Illahi et al. 2020 (not found);
Krajancich et al. 2021 flicker fusion (carried from 5.6, not searched). Searches for error
resilient wireless VR, compute shader video decoding, and view synthesis based inter prediction
returned zero arXiv results, which is itself a finding: the compute shader decoder literature
appears to be industrial (GDeflate, Oodle) rather than academic. Anything in 5.6 this document
does not repeat remains unchecked.
