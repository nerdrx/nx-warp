# Performance targets

Every number in this document is a **design estimate** from [docs/PAPER.md](PAPER.md). The "measured"
column of every table is empty, and it stays empty until the Phase 0 gate runs on real hardware.

> **Nothing in this project has been measured.** The estimates below are back-of-envelope figures from
> the design paper, computed from vendor peak numbers and field experience with other workloads. The
> paper itself says they could be wrong by 2x in either direction (paper 3.1), which is why Phase 0
> exists before any codec code (paper 7.4).

Measurements land in `bench/` (the Phase 0 gate app, kernels K1 to K6) and in `tools/quality/` (the
bitrate and quality harness). When they do, they are filled in here with the device, driver version
and date, and the estimate column is kept alongside so the error is visible.

## 1. Working numbers

The frame the whole design is sized against (paper 3.1).

| Quantity | Value | Kind |
|---|---|---|
| Frame | 2 views x 2048 x 2048 = 8.39 Mpixel | given |
| Frame period at 90 Hz | 11.1 ms (13.9 ms at 72 Hz) | given |
| GPU time already spent per vsync by WiVRn (reprojection plus compositor) | about 2 to 3 ms | field estimate |
| Decoder budget we can defend | 5 ms p50, 7 ms p99 | target |
| Adreno 650 peak | 1.2 TFLOPS FP32; roughly 600 G int32 simple ops/s, assume 300 G/s sustained | vendor peak, derated |
| Adreno 650 memory | about 44 GB/s peak, 25 GB/s usable by the GPU | vendor peak, derated |
| Bitstream per frame | 150 Mbit: 208 KB; 400 Mbit: 555 KB; 1 Gbit: 1.39 MB | arithmetic |
| Bitstream per 64x64 tile (2048 tiles) | 102 B at 150 Mbit, 270 B at 400 Mbit, 680 B at 1 Gbit | arithmetic |

Frame-level sanity check: 8.39 Mpixel x 150 ops = 1.26 G ops = 4.2 ms at 300 G/s; memory about 105 MB
per frame = 4.2 ms at 25 GB/s. ALU and memory overlap only partially. That is the whole risk in two
lines: **the decoder lands at 4 to 6 ms on this GPU if everything goes right.**

## 2. Decoder time on Adreno 650

Paper 3.2.5. Two eyes at 2048 squared, 90 Hz.

| Pass | Estimate | Measured (device, driver, date) |
|---|---|---|
| A: entropy decode (all tiles, if not hidden under packet arrival) | 0.5 to 1.0 ms | |
| B: reconstruct | 3.5 to 5.0 ms | |
| **Total** | **4 to 6 ms p50, 7 ms p99** | |
| C: hybrid enhancement (hybrid profile only) | under 2.0 ms target | |

Context for reading these: the CAS sharpening pass that hurt in the field was 9 texel taps plus one
store per pixel per vsync at 2 x 2160 squared, an estimated 4 to 5 ms on this GPU. Pass B is 4
cache-friendly fetches, one store and about 5x the ALU, once per frame. The paper expects it to land
in the same 4 to 5 ms band, and says two things plainly: at a 90 fps stream "once per frame" and "once
per vsync" are the same rate, so the argument only helps at 72 Hz or below the panel rate; and this is
GPU time the hardware HEVC path does not spend at all (paper 3.2.5).

## 3. Memory traffic per frame

Paper 3.2.5, pure compute, 2 x 2048 squared.

| Traffic | MB, estimate | Measured |
|---|---|---|
| Bitstream read | 0.2 to 1.4 | |
| Pass A coefficient write (dense int16) | 16.8 | |
| Pass B coefficient read | 16.8 | |
| Reference read (gather-4 through texture cache, nominal) | 33.5, effective 40 to 50 | |
| Output write (RGBA8 or RGB10A2, doubles as the next reference) | 33.5 | |
| **Total** | **about 105** | |

At 25 GB/s that is 4.2 ms if nothing overlaps. The first optimisation if Pass B turns out
bandwidth-bound is a sparse coefficient layout, roughly 4x smaller at typical QP; it is not in v1
because the dense layout keeps Pass A trivial (paper 3.2.5, 3.12).

## 4. Per-pixel op budget, inter tile

Paper 3.2.5. The field data allows 150 to 300 ops per pixel on this GPU.

| Stage | Ops | Fetches | Stores |
|---|---|---|---|
| Entropy decode, amortised (0.3 symbols/pixel x 25) | 8 | | |
| Coefficient load, dequantise | 6 | 0.06 (coalesced) | |
| Row plus column 8x8 integer DCT (2 x 44 / 8) | 22 | | |
| LDS transpose traffic | 6 | | |
| Warp coordinate | 6 | | |
| Bit-exact bilinear (4 loads, weights, blend) | 14 | 4 (same cache line) | |
| Add, clamp, YCoCg-R to RGB | 10 | | |
| Store | 1 | | 1 |
| **Total** | **about 75** | **4** | **1** |

The worked example in paper 1.11 arrives at about 150 ops per pixel for one INTER fovea tile including
the entropy stage amortised over the tile, which is the upper end of the same estimate. Neither has
been measured.

## 5. Phase 0 gate thresholds

Paper 3.4. A standalone Android app, NDK, Vulkan 1.1, no OpenXR, run on the Pico 4 at 90 Hz with the
display active and a fullscreen dummy reprojection pass submitted every vsync so the decoder competes
with realistic co-tenant work. Timed with timestamp query pairs, 600 frames after 120 warm-up,
reporting p50, p95, p99 plus a throttling check over 10 minutes (last-minute p50 against the first).

| Kernel | What it does | Pass threshold | Measured p50 | p95 | p99 |
|---|---|---|---|---|---|
| K1 copy | 8.39 Mpixel RGBA8 image to image via compute | reports achievable GB/s, expect over 20 | | | |
| K2 gather-4 | warp coordinate plus bit-exact 4-load bilinear plus store, from a full-frame reference | under 3.0 ms p50 | | | |
| K2b sampler | the same with one sampler tap | informational: the cost of bit-exactness | | | |
| K3 idct | Pass B without prediction: coefficient load, dequant, 8x8 int DCT through LDS, store | under 2.5 ms p50 | | | |
| K4 rans | Pass A on random symbol streams at 0.5 symbols/pixel, all 2048 tiles | under 1.5 ms p50 | | | |
| K5 full | Pass A plus Pass B as designed | under 5.0 ms p50, 7.0 ms p99 with the dummy reprojection running | | | |
| K6 hybrid | MediaCodec HEVC 2 x 2048 squared at 90 fps into AHardwareBuffer, imported, plus Pass C | decoder latency p50 under 15 ms, Pass C under 2.0 ms | | | |

Additional measurements the gate must produce, which are not kernel timings:

| Measurement | Why | Value |
|---|---|---|
| Decode watts | Compute decode heats the XR2 more than the hardware decoder; thermal is a first-class risk (paper 4.11) | |
| Throttled p50 (last minute vs first) | A 5 ms decoder that becomes a 7 ms decoder after 10 minutes has failed | |
| Subgroup ballot availability and cost | Load-bearing assumption of the rANS layout (paper 1.12) | |
| Register spill in Pass B | Adreno's compiler is opaque; report via `VK_KHR_pipeline_executable_properties` where available (paper 3.2.3) | |
| Vulkan submit overhead and timeline semaphore latency | Assumed 50 to 100 us and under 200 us; if worse, bands drop from 6 to 3 per frame and the latency floor grows by about 1.5 ms (paper 4.11) | |

**Decision rule**, fixed in advance (paper 3.4,
[ADR-0015](adr/0015-compute-budget-verdict-pending-phase-0.md)):

- K5 passes: pure compute is the default on Pico 4.
- K5 between 5 and 8 ms: pure compute at 72 Hz or with 1.5x foveated tile reduction; hybrid is the
  default.
- K5 over 8 ms: the Pico 4 is hybrid-only; pure compute continues on PC and next-generation Adreno.
- K6 failing on latency means MediaCodec low-latency mode is not working on this firmware and the
  vendor key `vendor.qti-ext-dec-low-latency.enable` needs verifying.

## 6. Encoder time on the PC

Paper 3.6, 4.2.

| GPU | Estimate, both views | Measured |
|---|---|---|
| RX 580 (6 TFLOPS, 256 GB/s) | 2.5 to 4 ms per frame | |
| 7900 XTX | under 1 ms per frame | |

On the RX 580 the E0 and E3 fullscreen passes dominate by bandwidth, about 250 MB per frame. That is
against an AMF hardware-encoder ceiling of roughly 35 to 50 fps for the same content, which is the
comparison that matters.

Per-frame CPU cost target on the server: **under 300 us at 90 fps**, consisting of one
`vkQueueSubmit` of pre-recorded command buffers, 8 semaphore waits and 8 `sendmmsg` calls (paper 3.6).

| Item | Target | Measured |
|---|---|---|
| Server CPU per frame | under 300 us | |
| rANS encode: integer division `x / freq` | 20 to 40 instructions on GPU; a reciprocal table is the planned optimisation | |

## 7. Latency budget

Paper 4.2. Row-band pipelining with 6 bands per frame. Assumptions: 7900 XTX encodes the frame in
3.0 ms (0.5 ms per band), RX 580 in 8 ms, XR2 compute decode budget 4.0 ms per frame (0.67 ms per
band), one-way network delay 3 ms on WiFi 6 with a quiet channel and 1 ms on USB.

| Milestone | Estimate | Measured |
|---|---|---|
| Band 0 encode done, first send | 0.5 ms after render finish | |
| Band 0 last datagram arrives (WiFi) | 3.6 ms | |
| Band 0 decoded into the frame ring | 4.3 ms | |
| Band 5 encode done, send | 3.0 ms | |
| Band 5 arrives | 6.1 ms | |
| **Frame complete (WiFi)** | **6.8 ms after render finish** | |
| Frame complete (USB) | 4.8 ms | |
| Frame complete (RX 580, encoder is the long pole) | about 12 ms | |

Compare the serial path: encode 3 ms, transfer 208 KB at 300 Mbit/s effective 5.5 ms, hardware HEVC
decode 8 to 15 ms through MediaCodec plus its 1 to 2 frame queue, totalling 17 to 25 ms.

End to end:

| Stage | Estimate | Measured |
|---|---|---|
| Render finish to frame complete | 6.8 ms (WiFi) | |
| Compositor phase wait | 0 to 11.1 ms, average 5.5 | |
| Runtime reprojection and scanout (Pico 4 LCD) | about 5 ms | |
| **Render finish to photons** | **12 to 23 ms** | |
| Plus pose uplink (1 to 2 ms) and render (up to 11 ms) | | |
| **Motion to photons** | **25 to 35 ms**, against about 100 ms measured today | |

The remaining long pole is the compositor phase wait, not the network. The air term of 3 ms plus
jitter is not what costs; only frameless presentation and render-side pacing attack the phase wait
(paper 4.2).

Phase 3's exit criterion is glass-to-glass under 40 ms at 150 Mbit on WiFi 6, measured by the existing
HUD path (paper 3.11).

## 8. Bitrate operating points

Paper 1.10. Stereo 2160 x 2160 at 72 Hz is 672 Mpixel/s, so 1 bpp is 672 Mbit. Foveation assumed as
20 percent of tiles at level 0, 30 percent at level 1, 50 percent at level 2.

| Operating point | Fovea bpp | Mid bpp | Periphery bpp | Mean bpp | Bitrate | Measured |
|---|---|---|---|---|---|---|
| Lite wireless (QP 30/36/42) | 0.20 | 0.05 | 0.012 | 0.061 | about 41 Mbit | |
| Standard wireless (QP 24/30/36) | 0.55 | 0.15 | 0.03 | 0.17 | about 115 Mbit | |
| USB / WiFi 7 near-lossless (QP 4, no foveation levels) | 1.0 | 1.0 | 1.0 | 1.0 | about 670 Mbit | |

Worked content estimate at HEVC-150 quality, VRChat with moderate head motion and no mirror
(paper 2.4): 0.28 bpp, about 2.3 Mbit per frame, 210 Mbit/s at 90 Hz.

## 9. Compression, honestly

Paper 1.10, 2.4, 2.5. All estimates, all Phase 1 and Phase 2 measurements.

| Case | Expectation against HEVC / x265 | Measured BD-rate |
|---|---|---|
| Intra-only tiles | 30 to 40 percent more bits at equal PSNR | |
| At rest, static camera | roughly parity | |
| Head rotation, roll, sub-pel drift | 2x to 4x fewer bits on the affected frames | |
| Fast object motion within one tile | 15 to 25 percent worse | |
| Reference at N-2 instead of N-1 (WiFi lower bands) | 5 to 10 percent more bits | |
| Stereo inter-view (`STEREO`, Phase 4) | 5 to 10 percent overall, 30 to 40 percent on intra-heavy frames | |
| rANS against a CABAC-class coder | about 8 percent more bits on the same coefficient statistics | |
| Rolling intra refresh overhead | about 0.014 bpp, under 5 percent of the budget | |

Phase exit criteria that bound these (paper 3.11, 7.3): within 1.0 dB PSNR of x264 intra in Phase 1;
BD-rate within 15 percent of x265 zerolatency single-reference on head-rotation sequences in Phase 2,
and within 10 percent at rest with at least 30 percent better on motion frames; at least 25 percent
saving at equal FovVideoVDP on the fovea in Phase 4.

## 10. Transport overhead

Paper 4.1, 4.4.

| Quantity | Estimate | Measured |
|---|---|---|
| Tile-run header overhead (20 average tiles per run) | about 5.5 percent | |
| Tile-as-packet overhead, rejected alternative | 30 to 50 percent | |
| Datagrams per second at 150 Mbit | about 13 k | |
| Datagrams per second at 1 Gbit | about 90 k | |
| XR2 Gen 1 practical UDP receive ceiling | 50 to 100 k pps | |
| FEC blended overhead (3/1/0 parity per 10 by class) | about 14.5 percent of bits | |
| Uplink feedback | 6 per frame x 90 Hz x about 100 bytes = 0.4 Mbit/s | |
| Frame header replication (26 B pose per band, 6 bands) | about 0.14 Mbit/s | |
| Per-tile MV cost at 8192 tiles, 3 bits per coded vector | about 2 Mbit/s at 90 Hz | |

## 11. How to fill this in

1. Build and run the Phase 0 app: see `bench/README.md`.
2. Record the device, its OS and driver build, the ambient conditions and the date alongside every
   number. A thermal-throttled number without that context is not a measurement.
3. Fill the measured column here and keep the estimate next to it. **Do not delete an estimate that
   turned out wrong**: the size of the error is the most useful thing this table can record.
4. If a Phase 0 threshold fails, the outcome is a superseding ADR carrying the measured table, not an
   edit to [ADR-0015](adr/0015-compute-budget-verdict-pending-phase-0.md).

## References

- Paper 3.1 (working numbers), 3.2.5 (traffic, ops and time), 3.4 (the gate), 3.6 (encoder),
  3.11 (milestones), 4.1 (transport overhead), 4.2 (latency), 1.10 (bitrate), 2.4 (content cost)
- `bench/README.md`, `tools/quality/README.md`
- [ROADMAP.md](../ROADMAP.md), [ADR-0015](adr/0015-compute-budget-verdict-pending-phase-0.md)
