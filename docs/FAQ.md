# FAQ

Honest answers. Nothing in this project has been measured yet: every number below is a design estimate
from [docs/PAPER.md](PAPER.md) and is labelled with the section it comes from. Where the honest answer
is "we do not know until Phase 0", that is the answer given.

## Why not just use AV1, HEVC or H.264?

Because they are built for a different problem, and because their implementations are fixed function.

General-purpose codecs are designed for file storage and broadcast: whole-frame slices, reference
lists, B-frames, serial entropy coding. VR streaming has structure they cannot see. The encoder knows
the head pose that produced every frame. The two eyes share nearly all content. The lens throws away
most of the periphery. The display would rather show a slightly stale tile than wait for a whole
frame. (paper abstract)

Concretely, what no hardware codec path can do:

- Put the head pose into the predictor. A 300 deg/s head turn is about 70 px per frame at 90 Hz, past
  the effective search range of hardware encoders at low-latency presets, so they fall back to intra.
  That is the bitrate spike on head turn that WiVRn users know. The warp makes that motion cost zero
  bits. (paper 2.2)
- Refresh one lost tile without an IDR or a slice trick.
- Code alpha, or 4:4:4, on a mobile decoder.
- Present a partial frame.
- Exceed the decoder's fixed pixel rate. The XR2 Gen 1 decoder cannot decode two 2160x2160 streams at
  90 Hz, which is why every hardware-codec streamer encodes below panel resolution. (paper 5.8)

And on the PC side, using hardware encoders means an NVENC/AMF/VAAPI dependency and its session
ceilings. This codec is vendor neutral there. (paper abstract,
[ADR-0011](adr/0011-vulkan-compute-over-fixed-function.md))

The counter-argument is real and the paper states it: on the Pico 4 the H.264 decoder is fixed
function and costs no GPU time at all, so this codec cannot beat it on headset GPU cycles. It has to
win on bits during head motion, on latency, on loss behaviour, and on what a low bitrate looks like.
Those four are the contest. (paper 4.6.1)

## Does it beat H.264 or HEVC on compression?

At rest, no, and the paper says so plainly in its "what it does not claim" section.

The expectation (paper 1.10, 2.4, all design estimates):

| Case | Expectation against HEVC |
|---|---|
| Intra-only tiles | 30 to 40 percent more bits at equal PSNR (no directional intra, no adaptive contexts) |
| Static scene, camera at rest | roughly parity; HEVC's per-block translation approximates a small rotation well |
| Head rotation, roll, sub-pel drift over textured floors | 2x to 4x fewer bits on those frames |
| Fast object motion within one tile | 15 to 25 percent worse, because there is one MV per 64x64 tile |
| Mirrors, particles, light shows | no better; nothing here fixes that case |

A worked VRChat frame at HEVC-150 quality comes out at 0.28 bpp, about 210 Mbit/s at 90 Hz
(paper 2.4). That is honest rather than flattering.

The average-bitrate case is not the case for this codec. Latency and loss behaviour are.

## Why not JPEG XS or VC-2? They already run in compute.

They are the right shape and the wrong tool. Intra-only wavelet codecs have line-based latency (32
lines for JPEG XS), are visually lossless at 6:1 to 10:1, and are simple enough to run in compute.
They prove the design point that Phase 1 of this project resembles.

What they cannot do is use temporal redundancy, so 4K90 stereo sits at 500 Mbit to 1 Gbit, which is
USB only. Pose-warped prediction is the entire reason this codec exists at wireless bitrates.
(paper 5.6)

On licensing: JPEG XS is patent-licensed (intoPIX, Fraunhofer and others). VC-2 is royalty-free under
the BBC's declaration and is the safer reference for wavelet tools, which is why the 5/3 wavelet is
reserved as a v2 tool bit rather than dismissed. (paper 1.4, 5.7)

## Will it run on the Pico 4?

Unknown, and that is the honest answer. Phase 0 exists to decide it.

The estimate is 4 to 6 ms p50 for the full compute decoder on an Adreno 650 at two eyes by 2048
squared, against a defensible budget of 5 ms p50 and 7 ms p99 in an 11.1 ms frame, on a GPU already
spending 2 to 3 ms per vsync on reprojection and compositing (paper 3.1, 3.2.5). The paper's own
expectation (6.10) is that the Pico 4 lands in **hybrid mode**: hardware HEVC carries the base layer
and compute carries the pose-warped enhancement.

The decision rule is fixed in advance, before any measurement, so it cannot be argued with afterwards:

- K5 under 5.0 ms p50: pure compute is the default on Pico 4.
- K5 between 5 and 8 ms: pure compute at 72 Hz or with 1.5x foveated tile reduction; hybrid is the
  default.
- K5 over 8 ms: the Pico 4 is hybrid-only, and pure compute waits for Adreno 740 class hardware.

That last outcome is acceptable, not a failure of the design.
([ADR-0015](adr/0015-compute-budget-verdict-pending-phase-0.md))

## Will it work on Quest? On Mali?

Expected support levels are in [COMPATIBILITY.md](COMPATIBILITY.md), and every row there says
"expected" because nothing is measured.

The hard requirement is a subgroup size of at least 8 with subgroup ballot, because the rANS layout
uses 8-lane clusters and derives read offsets from a ballot. So:

- Adreno 6xx and 7xx: subgroup 64 (128 on 7xx), ballot available. The pure compute target.
- Mali Valhall: subgroup 16, ballot available, no int64. Hybrid unless a Phase 0 style benchmark
  passes on the device.
- Mali Bifrost: subgroup 4 to 8, partial ballot. Unsupported for pure compute; it gets hybrid.
- Apple through MoltenVK: subgroup 32, no 64-bit integer in Metal, and different sampler behaviour.
  The integer-only rule already forbids int64 in normative shaders, so this is mostly a question of
  whether anyone builds the client.

Quest devices are Adreno, so they are in the same family as the first target. The paper does not
give per-device Quest numbers, and this project will not invent them.
(paper 3.7, [ADR-0010](adr/0010-integer-only-normative-path-cpu-reference-is-the-spec.md))

## What about patents?

The tool policy is: only public-domain, expired, or explicitly royalty-free coding tools, with a
written record of the origin of each one kept from day one. The licence is Apache-2.0, chosen for its
explicit patent grant and defensive termination.
([ADR-0020](adr/0020-apache-2-0-and-patent-hygiene.md))

That policy costs efficiency and the project pays it knowingly: no CABAC (an estimated 8 percent), no
directional intra in v1 (an estimated 25 to 40 percent on intra tiles), no HEVC-class transform or
in-loop filters.

Assembled-from-safe-parts is not the same as safe, so four constructions get a scoped
freedom-to-operate review before Phase 3 ships: pose delta as global motion parameters, `STEREO`
against 3D-HEVC view synthesis prediction, the enhancement-over-hardware-base structure against
LCEVC, and the fixed-precision rANS construction against the 2022 Microsoft claims. Each has a defined
fallback, so a negative finding is a downgrade rather than a dead end.
([ADR-0017](adr/0017-fto-review-scope.md))

None of this is legal advice. It is an engineering map of where the mines are, and the paper says so
in both places it appears (1.9, 5.7).

## What is the licence, and can I use this commercially?

Apache-2.0. See [LICENSE](../LICENSE). The licence grants what it grants; it does not and cannot grant
patent rights held by third parties, which is what the FTO review above is about. Read
[ADR-0020](adr/0020-apache-2-0-and-patent-hygiene.md) before shipping anything built on this.

## Is there a decoder I can run today?

There is a CPU reference decoder in `ref/`, which is the normative specification of the bitstream, and
conformance vectors in `tests/vectors/`. There is no usable streaming pipeline. See
[ROADMAP.md](../ROADMAP.md) for what actually exists at the moment.

## Why 64x64 tiles? A smaller tile would conceal better.

Per-tile fixed cost. At 100 Mbit and 72 Hz the average 64x64 tile carries about 75 bytes, so header
plus rANS flush has to stay under about 8 bytes. At 32x32 the average tile is 37 bytes and the same
fixed cost doubles to about 20 percent of the payload, while a 64-lane workgroup would be underfed at
24 pixels per lane instead of 96. 32x32 stays reserved by a stream header bit for high-bitrate USB
profiles. ([ADR-0002](adr/0002-64x64-tiles.md))

## Why is one lost datagram allowed to lose a dozen tiles?

Because the alternative does not fit. One datagram per tile at 150 Mbit and 90 Hz would be 208,000
packets per second against an XR2 receive ceiling estimated at 50,000 to 100,000, and the 52 bytes of
UDP/IP plus codec header per 90-byte tile would nearly double the bitrate.

The loss unit is coarse; the concealment unit stays fine. A lost run conceals per tile, the encoder
knows exactly which tiles were lost from the run header, and FEC is strongest on the tiles the eye is
pointed at. ([ADR-0001](adr/0001-datagram-is-a-tile-run.md))

## There is no keyframe at all. What happens when I connect, or when everything is lost?

A full intra frame happens on stream start, on a profile change, and when the feedback bitmap history
has a gap. Those are the only three cases.

Everything else is handled by per-tile reference selection: a tile references the newest of N-1, N-2
or N-3 whose 3x3 neighbourhood is fully acknowledged, and codes intra if none qualifies. With no
feedback for four frames, every tile goes intra at the per-tile capped size, which is a QP jump rather
than a stall or a 4x to 8x bitrate spike.
([ADR-0006](adr/0006-acknowledged-neighbourhood-references-no-idr.md))

## What does it look like when the bitrate is not enough?

Blurry, not blocky. That is a requirement, not a hope: when the budget is short the picture must lose
texture before it loses structure. Tiles are classified as text, edge, texture or flat, and the classes
descend different ladders. Texture and flat tiles lose high-frequency coefficients, then drop to half
and quarter resolution, then to a DC plane with planar interpolation, which is a smooth gradient field
rather than a mosaic. Edge and text tiles hold their step until the others are exhausted.

No new syntax is needed for any of this; every rung uses a v1 tool. The only new thing is the order in
which rate control spends. ([ADR-0013](adr/0013-degradation-ladder-blur-never-block.md), paper 4.6.1)

## Why do you convert to YCoCg-R if my source is already YCbCr 4:2:0?

We do not. The stream header carries a `color_space` field: `YCOCG_R` for RGB sources, and
`YCBCR_PASSTHROUGH` for sources that are already YCbCr 4:2:0, where no colour conversion happens on
either end.

This matters because of the first integration target. WiVRn's encoder input on Linux is already YCbCr
4:2:0 and already foveated by the compositor, and the client already consumes 4:2:0 from its decoder,
so the Android decoder outputs 2-plane 4:2:0 on that path. Round-tripping through RGB would add two
conversions to the normative path and inflate reference traffic to preserve chroma resolution the
source has already discarded.

The trade is that lossless belongs to the `YCOCG_R` path: a 4:2:0 passthrough stream cannot be
mathematically lossless with respect to an RGB original, so lossless UI text tiles degrade to
near-lossless there.
([ADR-0021](adr/0021-stream-level-color-space-ycbcr-passthrough.md))

## Why GPU compute for the encoder too? NVENC is free.

Free, and vendor-locked, and session-limited, and unable to reconstruct references the way the decoder
will. The rule that makes the whole loss model work is that the encoder's reconstruction pass (E3) is
byte-identical SPIR-V to the decoder's Pass B, so the encoder can never hold a reference the decoder
cannot reproduce. A hardware encoder cannot participate in that. (paper 3.6,
[ADR-0010](adr/0010-integer-only-normative-path-cpu-reference-is-the-spec.md))

## Why is there no CPU in the hot path, and what is the one exception?

Because a CPU stage means a scheduling dependency, a cache miss and a wakeup on a device that has none
of those to spare. The encoder runs E0 to E5 with no CPU between passes; the decoder runs Pass A
incrementally on packet arrival and Pass B once at the deadline.

The one deliberate exception is datagram decryption. AEAD verification and Reed-Solomon repair run on
the CPU network thread, writing plaintext straight into a host-visible ring the decoder reads. The CPU
moves bytes and checks tags; it never parses the bitstream. ARMv8 crypto extensions are estimated at
2 to 4 GB/s per core against 125 MB/s at 1 Gbit/s. (paper 4.1)

## Why C++ and GLSL rather than Rust and Slang?

The codec's substance is fifteen shaders and a set of integer tables, and it has to link into a C++
server and an NDK C++ client. Rust would add cargo-ndk and an FFI seam for no gain in the hot path;
Slang would add a toolchain that neither WiVRn nor the Android build has. The safety argument for Rust
is real, and it is answered with fuzzing, bounds-clamped loads and GPU-assisted validation rather than
dismissed. ([ADR-0019](adr/0019-cpp20-with-a-c-abi.md),
[ADR-0018](adr/0018-glsl-via-glslang.md))

## Can it do multi-user? Why not multicast the base layer?

It can do multi-user by sharing the encode, not the air. 802.11 multicast goes out at a basic rate,
typically 6 Mbit/s, unacknowledged and unretried, so a 100 Mbit/s base layer cannot be multicast on any
consumer access point; access points that convert multicast to unicast just send N copies with less
information than the server has. The shared base tiles are therefore encoded once and sent N times,
with per-user fovea tiles. ([ADR-0009](adr/0009-no-multicast.md))

## How will you know whether any of this worked?

Every phase has measurable exit criteria and they are in [ROADMAP.md](../ROADMAP.md). The headline
ones: bit-exactness across lavapipe, RADV and Adreno; within 1 dB of x264 intra in Phase 1; within
10 percent of x265 zerolatency at rest and at least 30 percent better on head-motion frames in
Phase 2; glass-to-glass under 40 ms at 150 Mbit on WiFi 6 in Phase 3.

Quality is measured with FovVideoVDP in display space, after the real reprojection shader, so that
warped-reference concealment is charged for what it actually shows. PSNR is kept only as a sanity
number: it weighs every pixel equally when 80 percent of them are in the periphery at quarter sampling
by design. And latency is treated as a quality metric, because a codec that gains 1 JOD by adding 8 ms
has lost. (paper 5.3)

## Is this a WiVRn fork or a general-purpose codec?

Neither, quite. It is a standalone codec library (`nxvc`, Apache-2.0) whose first and only integration
target is WiVRn NX. The design deliberately refuses to require engine integration, which is what
separates it from object-space streaming approaches; the optional OpenXR extension that carries
velocity, depth and stencil changes no bitstream syntax and the codec works without it. (paper 2.3,
5.6)
