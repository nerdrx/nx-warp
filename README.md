<div align="center">

<img src="brand/nx-wordmark-light.svg" width="150" alt="NX"><br>
<strong>Warp</strong>

### VR-specific compression. GPU-native reconstruction. Latency first.

[![CI](https://img.shields.io/github/actions/workflow/status/nerdrx/nx-warp/ci.yml?branch=main&label=CI&labelColor=0c0818&color=7700FF)](https://github.com/nerdrx/nx-warp/actions/workflows/ci.yml)
[![Format](https://img.shields.io/github/actions/workflow/status/nerdrx/nx-warp/format.yml?branch=main&label=format&labelColor=0c0818&color=7700FF)](https://github.com/nerdrx/nx-warp/actions/workflows/format.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-00e5ff?labelColor=0c0818)](LICENSE)
![Status](https://img.shields.io/badge/status-research%20prototype-ffb300?labelColor=0c0818)

[Design paper](docs/PAPER.md) · [Measured results](#measured-results) · [Build](#building) · [WiVRn NX integration](https://github.com/nerdrx/wivrn-nx/tree/atlas-live)

</div>

> [!IMPORTANT]
> **Research prototype:** there is no end-user release or download yet. Developers can build the experimental code using the [instructions below](#building).

## Abstract

**NX Warp is an experimental video codec for rendered VR, developed with the custom WiVRn NX streaming stack.** It explores a simple premise: head pose, reusable tiles and renderer information should let a headset reconstruct useful pixels with less work than a conventional whole-frame pipeline.

The current work combines a Vulkan encoder, a Vulkan decoder and an atlas renderer tested on Pico 4. The priority is **low latency, full-resolution output and inexpensive reconstruction**, especially at low bitrate. Visual approximations are valid experiments when they preserve useful structure and measurably reduce cost. Quality, bitrate and power remain measured tradeoffs.

The immediate target follows the Pico display: **90 Hz / 11.11 ms per update**, with centre-first correction scheduling under investigation. The stretch target remains **240 Hz / 4.17 ms per update**. Individual stages and repeated-render throughput have crossed parts of that budget; **consistent 240 Hz delivery has not been demonstrated**. This is a research prototype, with visible artifacts and incomplete quality gates, rather than a production-ready streaming release.

> **Removing work beats optimizing work.**

![Actual Pico stereo capture with native-resolution vertex warp enabled](bench/results/240fps-2026-09-08/atlas-vertex-warp/v3-final/vertex-v3-on-screen-06.png)

*Figure 1. Actual Pico capture from the native atlas renderer, configured for 2160 × 2160 output per eye. Tile seams and cube trails remain visible. A screenshot establishes the captured appearance, not moving-head quality or display FPS. [Capture settings, control image and binary identity](bench/results/240fps-2026-09-08/atlas-vertex-warp/v3-final/README.md).*

## At a glance

| | Current scope |
|---|---|
| Application | Rendered stereo VR through **custom WiVRn NX** |
| Implementation | C++20 and Vulkan compute/graphics; library identifier `nxvc` |
| Primary measured hardware | Radeon RX 7900 XTX host; Pico 4 / Adreno 650 headset |
| Native output in recent captures | **2160 × 2160 per eye**; sequence fixture uses a padded 4352 × 2176 stereo atlas |
| Live integration | WiVRn NX [`atlas-live`](https://github.com/nerdrx/wivrn-nx/tree/atlas-live); experimental renderer switches remain opt-in |
| Presentation target | **90 → 120 → 144 → 180 → 240 Hz**; recent live Pico captures use 90 Hz |
| Evidence | Controlled comparisons, raw timings, fixture/build identities and actual captures |
| Open problems | Completion-time tails, correction scheduling, motion artifacts, compression quality and sustained thermal behavior |

**Read next:** [Architecture](#architecture) · [Results](#measured-results) · [Visual results](#visual-results) · [Measurement rules](#measurement-rules) · [Status](#status) · [Roadmap](#roadmap) · [Documentation](#documentation)

## Architecture

The design divides images into **64 × 64 tiles** and makes reusable content cheap. The reference codec defines reconstruction behavior; Vulkan implementations and live rendering experiments are checked against their relevant reference or control paths.

The current atlas work separates stored tile content from the mapping used to render it. Reusing content can avoid full-picture reconstruction, while a tile-aware mesh moves warp calculations out of repeated fragment work. Changes to codec references still need explicit synchronization and correct feedback handling.

```mermaid
flowchart TB
    subgraph PC["PC"]
        direction LR
        A["Rendered stereo frame<br/>and prediction inputs"] --> B["Vulkan encoder"]
    end
    B --> C["Tile stream · WiVRn NX transport"]
    subgraph HS["Headset"]
        direction LR
        D["Vulkan decoder"] --> E["Atlas content<br/>and tile mapping"]
        E --> F["Native renderer<br/>pose-aware warp"]
        F --> G["OpenXR compositor"]
    end
    C --> D
    D -. "receipt / reference feedback" .-> B
```

*Figure 2. Simplified experimental integration. Encoding, transport, decoding, rendering and compositor scheduling have distinct costs; a gain in one box is not automatically an end-to-end gain.*

The next architectural direction is to separate work by what each region needs:

| Region or tile | Intended compute policy | Research question |
|---|---|---|
| Predictable / unchanged | Warp or skip; reuse stored content | Can reconstruction and memory writes disappear? |
| Sparse correction | Update only the residual that matters | Can lightweight work avoid the dense path? |
| Dense / disoccluded | Spend more work on a fallback | Can difficult content stay within a deadline? |
| Fovea | Preserve detail and useful edges | Where does additional compute improve perception most? |
| Periphery / invisible | Cheaper correction, mostly warp, or skip | Can foveation control work as well as quantization? |

These are development priorities, not a claim that every path is integrated. Depth, motion vectors, visibility and object information could improve prediction further. The codec should ultimately describe what the renderer's prediction cannot explain.

### 240 Hz without 240 heavy corrections

A presentation update can reuse a stable correction state and apply a newer pose. This makes **60–120 Hz correction with faster presentation** worth investigating. The sequence probe already tests repeated rendering, but currently repeats a static pose; it does not establish this complete pose-aware scheduling architecture.

```text
Correction state   A ───────────── B ───────────── C
Presentation       warp → warp → warp → warp → warp → …
                   target: one useful update every 4.17 ms
```

Stable references, disocclusion handling and bounded image age are essential. Queueing more work can increase throughput while making the displayed image older.

## Measured results

[Five-minute sustained run](bench/results/90fps-2026-09-10/res150-soak/README.md): **~71 fresh selections/s**, **~72 ms source-offset proxy**, zero session stops; vendor GPU temperature peaked at **81°C**. Sustained load is a remaining limitation.

[Longer 64-thread validation](bench/results/90fps-2026-09-10/compact-flat64-long/README.md): decode GPU **6.56 → 5.93 ms**, source-offset proxy **2.88 ms lower**, but **~2.4% fewer fresh selections**. Retained for latency, not a fresh-FPS gain; vendor GPU temperature peaked at **84.5°C**.

[Selection timing audit](bench/results/90fps-2026-09-10/selection-delay/README.md): at 2688² per eye, selecting a frame to entering the presentation pass takes **0.047 ms mean** across 8,514 calls. Moving selection later offers little room; this is diagnostic evidence, not an end-to-end latency win.

[Static sparse decoder — longer validation](bench/results/90fps-2026-09-10/sparse-layout-long/README.md): at 2688² per eye, four 120-second runs improve decode GPU **5.88 → 5.39 ms** and fresh selections **68.9 → 76.9 per covered wall-second**. Source-offset proxy is **61.00 → 61.09 ms**: the earlier short-run latency gain did not reproduce. Pixel checks pass; the optimized decoder remains selected.

[Source-offset budget](bench/results/90fps-2026-09-10/source-budget/README.md): a diagnostic run splits the **57.88 ms proxy** into **31.69 ms presentation lead**, **9.51 ms decode wall**, **6.75 ms packet span**, **3.13 ms ready-frame wait**, **0.81 ms decoder queue** and **6.00 ms source-to-first arrival**. Presentation lead is not automatically removable delay; these are software timings, not photon latency.

**Selected decode improvement:** [64-thread compact flat PLANAR](bench/results/90fps-2026-09-10/compact-flat64/README.md) reduced **Pass B GPU time by 5.3%**, total decode GPU by **4.1%**, and source-offset proxy by **2.34 ms**, with essentially unchanged fresh delivery. A Pico fixture decoded byte-for-byte identically; startup failures and repeat methodology are disclosed.

**New optimization baseline: 2688×2688 per eye.** [Pico measurements and both-eye captures](bench/results/90fps-2026-09-10/resolution150/README.md): **152.6% of previous encoded pixels**, roughly **71 fresh selections/s** versus **88/s** before. Decode GPU time grows **4.41 → 6.54 ms**; source-offset proxy grows **51.40 → 63.97 ms**. This is working higher-resolution support, not a 90/240 FPS success.

**Current low-latency profile:** with the faster shader and continuous wake, [four new wait-budget trials](bench/results/90fps-2026-09-10/awake-wait/README.md) favor a **1 ms ready wait**: source-time offset **51.72 → 46.71 ms**, at about **1% fewer fresh updates** (89.08 → 88.17 per covered wall-second). All four runs had zero session stops. Source offset is a software proxy, not measured photon latency. The earlier shorter-wait rejection below concerns an older profile and remains historical evidence.

The [zero-wait follow-up](bench/results/90fps-2026-09-10/zero-wait/README.md) keeps **1 ms selected**: disabling the wait saved only **0.31 ms** of source-offset proxy while reducing fresh source selections by about **1.6%**. All four runs stayed awake, but per-run drift limits attribution.

[High-resolution follow-up](bench/results/90fps-2026-09-10/res150-jit/README.md) retains the **5 ms cap**: 7.5 ms increased source-offset proxy by 0.69 ms despite more fresh updates.

**Selected scheduler cap: 5 ms.** [Four longer 120-second trials](bench/results/90fps-2026-09-10/jit-cap-long/README.md) confirmed the earlier experiment: source-offset proxy **50.64 → 47.69 ms**, fresh selections **88.46 → 89.16 per covered wall-second**, zero session stops. Both pairs improved; these are software measurements, not photon latency. Ready-frame wait remains 1 ms.

**Capture failure recovery:** screenshot readback is now optional when creating the presentation swapchain. A [forced-rejection Pico test](bench/results/90fps-2026-09-10/capture-fallback/README.md) kept streaming for 60 seconds through three unsupported capture requests, with no repeated allocation loop. This improves failure handling, not frame rate.

**Continuous-delivery correction:** off-head Pico tests were sleeping and waking repeatedly despite Android stay-on. One earlier run’s **89.3 fresh updates/s in active windows was only 67.0/s across its covered wall time**. [Fix and validation](bench/results/90fps-2026-09-10/awake-stability/README.md): the awake override and corrected logging produced a **120-second run with zero session stops**, retaining 60 render/decode windows and measuring **89.1 fresh updates/s across 108.3 seconds after warmup**. This is logged source selection, not photon timing or 240 FPS. Historical active-window rates must not be read as uninterrupted delivery.

**Peripheral quality experiments (10 September):** [colour-aware palettes and rounded quality regions](bench/results/90fps-2026-09-10/round-colour/RESULTS.md) preserve chromatic edges that the brightness-only palette missed. On the synthetic colour fixture, U/V error falls about 30%; retained native-centre pixels are unchanged and Pico decoder readbacks match the CPU reference. This changes palette selection, not the 4:2:0 format.

![Rounded per-eye quality policy](bench/results/90fps-2026-09-10/round-colour/policy-geometry.png)

The selected block softening reuses the presentation sampler with **no extra texture reads**. A live motion pair measured 6.28 ms presentation GPU versus 5.71 ms without it; four-tap blur was rejected at 10.23 ms. [Measurements and limitations](bench/results/90fps-2026-09-10/round-colour/RESULTS.md#cheapest-presentation-softening).

**Follow-up:** [simpler sampling maths](bench/results/90fps-2026-09-10/remap-math/README.md) reduced mean presentation GPU time **6.61 → 6.35 ms (3.9%)** in four alternating motion trials, excluding startup telemetry. All tile classifications matched; coordinate differences stayed below one millionth of a source pixel. This refinement is included in the same smoothing setting.

**Cheaper outer shading:** [four moving-content Pico trials](bench/results/90fps-2026-09-10/fdm-ring/README.md) measured **6.47 → 6.09 ms presentation GPU time (6.0% less)** with the same full-density centre. Only the surrounding shading ring gets cheaper; fresh-update telemetry stayed similar. Source-time offset fell 63.92 → 60.06 ms, which is not a motion-to-photon measurement. A separate [coordinate-fusion attempt](bench/results/90fps-2026-09-10/remap-fused/README.md) regressed 5.3% and was reverted.

**Inactive-effect specialization:** [four Pico motion-scene trials](bench/results/90fps-2026-09-10/static-post/README.md) reduced presentation GPU time **6.24 → 4.86 ms (22.1%)** and raised fresh-update telemetry **82.3 → 88.8/s**, with the same centre, peripheral softening and colour conversion. The opt-in shader removes inactive effect branches; any active affected effect restores the general shader. Source-time offset fell **60.41 → 53.68 ms**, a software proxy rather than photon latency.

**Inactive edge-extension follow-up:** [four more Pico trials](bench/results/90fps-2026-09-10/static-bleed/README.md) lowered presentation GPU time **4.76 → 4.22 ms (11.4%)**, using the mean of per-run frame-weighted timings. This removes unused edge-extension code without changing the centre or softening. A visibility transition lowered the candidate’s raw update-rate average; the report retains that window and shows the sensitivity analysis. This is presentation cost, not proof of 240 FPS end to end.

**Compact mapping simplification:** [four Pico trials](bench/results/90fps-2026-09-10/compact-clamp/README.md) measured **4.33 → 4.18 ms presentation GPU time (3.3% less)** after removing a redundant coordinate clamp, with essentially unchanged fresh-update telemetry. Float32 checks at 200,648 points matched exactly; this does not establish bit-exact GPU output or 240 FPS end to end.

The [shorter-wait follow-up](bench/results/90fps-2026-09-10/fdm-wait/README.md) was rejected: reducing the ready-frame wait from 4 ms to 1 ms lost **8.1% of fresh updates** while reducing source-time offset by only **0.29 ms**. That older profile retained 4 ms; the newer continuously awake profile is evaluated above.

Optional Kuwahara approximations reduce extra samples from 16 to [8](bench/results/90fps-2026-09-10/kuwahara-eight-tap/RESULTS.md), [4](bench/results/90fps-2026-09-10/kuwahara-four-tap/RESULTS.md), and [2](bench/results/90fps-2026-09-10/kuwahara-two-tap/RESULTS.md). Isolated Pico RGBA probes measured about 38% lower draw time for eight taps versus sixteen, then a further 22% for four versus eight; the two-tap gain was smaller and varied between runs. These are approximate filters and GPU microbenchmarks, not live latency improvements.

**Latest live tradeoff:** [decode queue priority](bench/results/90fps-2026-09-09/queue-priority/README.md) reduced the mean of two run medians from **27.44 to 24.48 ms encode-to-selection**, while fresh source selections fell from **76.52 to 72.67/s**. Four animated Pico runs; opt-in, with equal priority still the default. This is not photon latency.

![Live Pico latency and freshness tradeoff](bench/results/90fps-2026-09-09/queue-priority/comparison.png)

**Latest work-omission experiment:** [exact PLANAR reuse](bench/results/90fps-2026-09-09/exact-reuse/README.md) passes CPU-reference and dropped-frame checks. It saves about **0.30 ms** in a local-motion standalone decoder fixture, but shows **no full-motion decode-time gain**. The prototype remains archived; it is not a live latency improvement.

![Exact-reuse Pico experiment: reconstruction and standalone decode times](bench/results/90fps-2026-09-09/exact-reuse/timings.png)

| Profile | Current status | Evidence and scope |
|---|---|---|
| **Live wider-ring profile** | Enabled opt-in on the custom WiVRn NX / Pico path | **77–82 fresh updates/s**, about **4.94 ms decode GPU time** and **2.04 ms server encode time**; native 512 × 512 centre with a 1024 × 1024 fine-detail boundary. [Live and fixture results](bench/results/90fps-2026-09-09/wide-ring/README.md) |
| **Synchronized GPU probe** | Standalone Pico test; not live integrated | 14 exact-output checks; guide/history reconstruction costs 1.67–1.77 ms. [Scope and timings](bench/results/90fps-2026-09-09/synchronized-gpu/README.md) |
| **CPU quality prototype** | **NOT integrated; not Pico measured**; separate quality experiment | Alternating-eye prototype results are recorded in the [experiment note](bench/results/90fps-2026-09-09/alternating-eye/README.md). [Trace image](bench/results/90fps-2026-09-09/alternating-eye/trace.png) |
| **Stretch target** | 240 Hz remains unproven | The target is **4.17 ms per update**; no live 240 Hz delivery or physical-motion proof is established. [Motion-proof archive](bench/results/240fps-2026-09-08/motion-proof/README.md) |

### Measured live software latency

A fresh [90-second Pico scene-motion capture](bench/results/90fps-2026-09-09/live-latency/README.md)
measures **27.65 ms median / 34.84 ms p95 / 37.61 ms p99** from encode start to
first render selection across **6,456 selected stereo frames**. Canonical frame
IDs and WiVRn clock conversion are checked. This excludes application rendering
before encode, later presentation work and scanout: **it is not motion-to-photon
latency**. The synchronized-history prototype is not enabled in this baseline.

### Synchronized refresh now has a Pico GPU probe

A [standalone Vulkan reconstruction probe](bench/results/90fps-2026-09-09/synchronized-gpu/README.md)
keeps both centres fresh and switches both eyes between full detail and
current-guide/warped-history reconstruction. **14 independent pixel checks pass
on Pico**, including signed translation across the packed sampling boundaries,
eye seams and invalid-history fallback.

The simple guide/history pass costs **1.67–1.77 ms** in isolated Pico runs.
Sharing guide decisions across pixels was exact but slower (**1.89 ms**) and was
rejected. These are reconstruction costs, **not decoder savings or live latency**.
The timed inputs are static; GPU motion-quality and complete pipeline integration
remain open. The wider-ring streamer stays on its tested profile.

![Pico reconstruction overhead, including the rejected variant](bench/results/90fps-2026-09-09/synchronized-gpu/gpu-cost.png)

### Alternating detail: useful history, uneven stereo error

The [alternating-eye CPU experiment](bench/results/90fps-2026-09-09/alternating-eye/README.md)
keeps both centres fresh, refreshes one eye’s peripheral detail per frame, and
uses a current half-resolution guide to validate warped history for the other.
Synchronized controls refresh both eyes every second frame at the same average
detail budget. Both cadence phases are tested because motion can align with the
refresh rhythm.

![Alternating versus synchronized detail schedule](bench/results/90fps-2026-09-09/alternating-eye/schedule.png)

In the synthetic moving fixture, alternating history reduced peripheral MAE
from **5.199** for fresh guides alone to **0.239**. Synchronized controls measured
**0.212–0.267** at the same detail budget. Alternating had higher stereo residual
error (**0.479 versus 0.306–0.389**), so better retained detail is not sufficient
to choose it for VR. These are intensity errors on a 0–255 scale, not measured
comfort or GPU savings.

Centres, refresh budgets, lost-detail recovery, history reset and ambiguous
checker motion have CPU checks. **GPU/codec integration and Pico validation
remain open.** The live wider-ring profile is unchanged.

![Cheap-eye reconstruction and error map](bench/results/90fps-2026-09-09/alternating-eye/comparison.png)

![Alternating-eye CPU experiment trace](bench/results/90fps-2026-09-09/alternating-eye/trace.png)

**Wider detail falloff:** the opt-in [wide PLANAR ring](bench/results/90fps-2026-09-09/wide-ring/README.md)
expands the fine-detail boundary from 768×768 to 1024×1024 per eye and
moves the coarser bands outward. The native 512×512 centre stays unchanged.
These are wider 4-pixel PLANAR cells, **not a true half-resolution ring**.
The 32-frame fixture costs 5.3% more bytes with identical decoded native centres.
Two 90-second live Pico motion trials delivered 77–82 fresh updates/s, with
about 4.94 ms decode GPU time and 2.04 ms server encode time. The wider profile
is enabled; these results show no demonstrated latency reduction.

![Wider ring geometry and decoded comparison](bench/results/90fps-2026-09-09/wide-ring/wide-ring.png)

<details>
<summary>Historical measured results and experiments</summary>

**Follow-up:** [compact decoder workgroups and shorter ready waits](bench/results/90fps-2026-09-09/compact-workgroups/README.md)
includes a corrected, inconclusive workgroup test: its harness did not explicitly
enable the specialized shader. The shorter ready-wait trial showed no gain.
The next proposed architecture uses a
[low-resolution guide with retained full-resolution detail](docs/TEMPORAL-TILES.md#cpu-quality-model-proposed-low-resolution-guide-with-retained-detail),
with explicit history poses and stereo-aware repairs. Its CPU quality model is
implemented in the [guide-history fixture](bench/results/90fps-2026-09-09/guide-history/README.md);
GPU and codec integration remain proposed.

**Adaptive peripheral updates (historical, September 9):** the [experiment](bench/results/90fps-2026-09-09/adaptive-planar/README.md)
keeps native centres fresh and selectively reuse peripheral fits. Removing unused
PLANAR transforms reduced full-resolution offline encode GPU time from about
5.9–6.1 ms to 4.7–4.8 ms. Cadence caching itself adds little measured saving and
remains opt-in; decoder skipping and motion-correct tile history are still open.
Live Pico motion checks with fractional sender pacing deliver **81–82 fresh
updates/s versus 75** in the control, with roughly 25% lower server encode time.
Reported source offset is 57–59 ms versus 56 ms; **halved latency is not achieved**.
These timings are not end-to-end latency.

![Measured peripheral refresh/reuse counts](bench/results/90fps-2026-09-09/adaptive-planar/cadence.png)


**Peripheral shading probe:** a coarser outer density map saved only **0.23 ms**
of presentation GPU time and did not reduce source offset, so it was reverted.
[Measured comparison](bench/results/90fps-2026-09-09/peripheral-density/README.md).
The next proposed approach is [staggered temporal tile updates](docs/TEMPORAL-TILES.md):
full-rate centre, half-rate middle, quarter-rate periphery, followed by a separately
tested small peripheral blend. **This tile scheduler is not implemented yet.**

**Pacing audit:** experimental estimator changes were reverted after incomplete
live trials. The benchmark now rejects stalled clients even when their processes
survive. A restored 90-second run passed with **71.92 fresh updates/s** and
**58.75 ms source offset**; this is recovery evidence, not a new latency gain.
[Failure records and validity checks](bench/results/90fps-2026-09-09/pacer-audit/README.md).

**Latency follow-up (September 9):** disabling glow and dithering preserved the
native centre and reduced presentation GPU work to **4.77 / 5.04 ms**, versus
**5.37 ms** with them enabled. Source display-time offset measured **61.68 / 58.33 ms**
versus **64.41 ms** in the intervening control, with **68.27 / 70.37** versus
**63.92 fresh updates/s**. These sequential trials vary; **halving physical latency
has not been demonstrated**. Removing packet spreading alone did not show the
large benefit suggested by comparison with an earlier session.
[Raw trials, controls, graph and capture](bench/results/90fps-2026-09-09/latency-probes/README.md).

**Broad scene motion (September 9):** a moving 195-cube wall on the Pico exposes
limits hidden by the earlier small scene. With the headset stationary, the
FDM + 4 ms readiness-wait profile delivered **77.22 / 77.02 fresh updates/s**,
compared with **68.23/s** for the original profile. Source display-time offset
remained **~70 ms**; the earlier 13 ms improvement does not carry over to this workload.

A server rate-control correction now accounts for frame-admission tolerance:
the old byte budget assumed fewer frames than the sender actually admitted.
Two corrected runs delivered **77.90 / 77.80 fresh updates/s** at **90.59 / 90.28
Mbit/s estimated payload**, versus **98.57 Mbit/s** before. QP reached **40**,
and payload still exceeds the **76.3 Mbit/s** allowance. This is an accounting
fix with a quality cost, not a demonstrated latency improvement or strict bitrate cap.
[Methods, raw motion logs, captures and reproduction](bench/results/90fps-2026-09-09/motion-live/README.md).

![Broad motion timing comparison](bench/results/90fps-2026-09-09/motion-live/live-comparison.png)

![Actual Pico captures of the animated scene](bench/results/90fps-2026-09-09/motion-live/motion-captures.png)

**Earlier small-scene readiness wait (September 9):** waiting briefly for a newer completed
frame recovers much of the fresh-frame loss from reduced-density shading.
Two 4 ms-cap trials reached **86.37 / 85.95 fresh updates/s**, versus
**83.82/s** for the original shading baseline. Source display-time offset fell
**70.52 → 57.08 / 57.19 ms**; this is not photon latency. The test profile is
active on Pico, while both experimental switches default off. No sustained
90-fresh-FPS or physical-motion proof yet. One baseline startup crashed during
runtime swapchain creation; its retry passed, and the cause remains unresolved.
[Methods, controls, raw logs and eye captures](bench/results/90fps-2026-09-09/ready-wait/README.md).

![Readiness wait results](bench/results/90fps-2026-09-09/ready-wait/live-comparison.png)


**Fragment-density map experiment (September 9):** an opt-in static FDM
implementation cut presentation GPU time from **8.37 ms** in the baseline to **4.76 /
5.01 ms** in two settled 90-second runs, while rendered cadence reached about
**89.6/s**. Fresh source updates regressed from **83.97/s** to **58.08 /
61.12/s**; baseline behavior was restored and the FDM switch remains opt-in
with default off. Source display-time offset was **69.74 → 55.25 / 55.41 ms**.
These are last-30 approximately two-second window means, not frame percentiles,
and source offset is not photon latency. Six
90-second runs included a full-density attachment control, a JIT-off control,
and a final baseline restoration check. There was no physical motion or photon
measurement. [Methods, metrics, density map and live chart](bench/results/90fps-2026-09-09/fragment-density/README.md).

![Fragment-density live comparison](bench/results/90fps-2026-09-09/fragment-density/live-comparison.png)

**Direct compact reconstruction (September 9):** flat PLANAR tiles now evaluate
only the samples that reach the compact output. Decoder p50 is **8.23 → 7.27 ms**
with byte-exact retained pixels in UINT and UNORM checks. Two live repeats
measured **83.15 / 82.88 fresh updates/s**, versus **70.87/s** for the previous
compact implementation, with the same centre and peripheral sampling settings.
Source display-time offset was **70.76 → 68.95 / 69.33 ms**, not photon latency.
The final APK repeated **83.60 updates/s**, with source offset **70.39 ms**;
latency improvement is small and variable. These stationary tests still fall
short of sustained 90 Hz.
[Methods, tails, raw logs and diagnostic caveats](bench/results/90fps-2026-09-09/compact-direct/README.md).

![Direct compact live comparison](bench/results/90fps-2026-09-09/compact-direct/live-comparison.png)

Actual application-eye GPU captures now work before Pico's tracking overlay;
they verify visible stereo output, not physical head-motion performance.
The centre is sharp while the low-bitrate periphery remains deliberately coarse.
[Left eye](bench/results/90fps-2026-09-09/compact-direct/direct-eye0.png) ·
[Right eye](bench/results/90fps-2026-09-09/compact-direct/direct-eye1.png).


**Compact-output experiment (September 9):** keep the native 512 × 512 centre
while storing quarter-density outer axes in a 1856 × 928 stereo NV12 image.
Pico decoder-only p50 fell **11.10 → 8.23 ms**, with byte-exact retained samples.
The first live mapper regressed; replacing branches recovered that loss.
With the extra peripheral filter removed (hardware bilinear sampling remains),
two settled live captures measured **71.22 / 72.25 fresh updates/s**, versus **53.88/s**
for the same-APK full-size control, and source display-time offset
**73.27 → 69.98 / 69.38 ms**. This changes peripheral quality; it is not an equal-quality
comparison or photon-latency measurement. The stationary Pico had a tracking
dialog covering its view, so visual and physical-motion validation are still
outstanding. Compact output remains opt-in.
[Methods, correctness, rejected variants and raw logs](bench/results/90fps-2026-09-09/compact-centre/README.md).

![Compact output live comparison, including rejected mapper](bench/results/90fps-2026-09-09/compact-centre/live-comparison.png)

![Native centre and compact stereo sampling layout](bench/results/90fps-2026-09-09/compact-centre/compact-centre-map.png)


**Peripheral smoothing capture (September 9):** the archived Pico captures show
the filter softening coarse PLANAR edges outside the protected centre. In a
matched hello workload at pacing `.1`, the last 15 completed windows with
smoothing on (11:24:50–11:25:18) averaged **5.65 ms GPU pass / 46.2 fresh
updates/s**; smoothing off (11:25:40–11:26:08) averaged **4.96 ms / 53.7
fresh updates/s**. Source offset was **73.84 / 75.24 ms** and submit lead
**38.49 / 38.41 ms**. These are window means rather than frame percentiles, and
workload variance is visible, so no clear latency penalty or win is established.
[Capture, script, and raw logs](bench/results/90fps-2026-09-09/peripheral-smoothing/).

**Graduated centre falloff:** the selected experiment keeps the native 512 × 512 centre, then uses **4 → 8 → 16 → 32-pixel PLANAR cells** outward. Paired Pico decode means were **10.206 ms graduated / 10.213 ms control**, with identical centre pixels. It remains a stepped two-colour approximation. [Before/after images, sampling diagram and rejected experiment](bench/results/90fps-2026-09-09/graduated-centre/README.md).

**Native-detail centre experiment (September 9):** mixed frames retain native pixel sampling in a 512 × 512 centre per eye (optionally 1024 × 1024), with cheap two-colour PLANAR tiles outside. This corrects a limitation of the earlier all-flat mode: a native-sized output did not preserve native detail. A one-pixel stripe test retains 191.96 levels of alternating-column contrast, versus zero in the flat mode. The Pico quarter-centre decoder averages **10.83 ms**, with **13.92 ms p95**, in an isolated 60-frame test; this does **not** establish sustained 90 Hz streaming. [Settings, pixel proof and timings](bench/results/90fps-2026-09-09/centre-detail/README.md).

The matching live Pico smoke test delivered **54.27 fresh updates/s** in short active windows; the full capture includes an XR interruption and averages 37.65/s. [Actual headset screenshot, logs and exclusions](bench/results/90fps-2026-09-09/centre-detail/live/README.md).

![One-pixel stripe detail: source, flat approximation and native-detail centre](bench/results/90fps-2026-09-09/centre-detail/centre-pixel-detail.png)

*Controlled decoded-pixel comparison, not a headset screenshot. The centre uses ordinary INTRA coding; the periphery remains coarse. The stripe comparison uses the 1024-centre mode, while the timing above uses the smaller 512-centre mode.*

**Live integration boundary:** the fast PLANAR renderer is now connected to custom WiVRn NX as an explicit opt-in. The earlier Pico benchmark numbers do not describe the installed streaming client. [Interface and supported frames](docs/integration/planar-direct.md). A 22-second live smoke test reported 87 fresh updates/s; visual and physical head-motion checks remain outstanding. [Live evidence and limitations](bench/results/90fps-2026-09-08/planar-direct-integration/live/README.md).

Borrowed decoder output is separately opt-in with Android property
`debug.wivrn.nx.borrowed_output=1`; changing it requires a reconnect. The
three-frame Pico correctness probe and live copy/direct measurements
are archived in the [borrowed-output evidence note](bench/results/90fps-2026-09-09/borrowed-output/README.md).

**90 Hz centre-first optimization:** single-pass admission halves the earlier multi-pass median (5.32–5.45 → 2.73–2.76 ms), with zero deadline misses across two 720-frame native Pico motion runs. One run retains outer pixels for one frame; the other refreshes every tile. This remains an offscreen experiment, not live streaming proof. [Paired results and actual capture](bench/results/90fps-2026-09-08/single-pass/README.md).

**Decoder materialized-reference improvement (historical, September 8):** consecutive full-picture frames now reuse an
already materialized reference. In the native Pico motion stress control,
eligible-frame median decode time falls **121.82 → 66.11 ms** with matching output
hashes; motion-phase median falls **98.56 → 64.71 ms**. This is a real but incomplete
improvement: live motion performance and 240 Hz delivery remain unproven.
[Implementation, validation and timing figure](bench/results/240fps-2026-09-08/materialized-copy/README.md).

The live sender also now treats decoder-worker backlog as an overload signal;
previously only deliberate decode-stride drops triggered immediate pacing backoff.
[WiVRn NX change and limits](https://github.com/nerdrx/wivrn-nx/blob/atlas-live/docs/bench/backlog-pacing-20260908/README.md).
Its live cadence benefit has not yet been measured.

The copy shortcut also covers a complete full-resolution INTRA refresh, with
matching Pico output hashes. A paired-colour-plane shader experiment showed no
speedup and remains disabled. [Follow-up tests and limits](bench/results/240fps-2026-09-08/intra-refresh-copy/README.md).
The client now rebuilds decoders after seamless reconnect even when settings
are unchanged; decoder recreation was observed, but resumed video remains unverified.
[Reconnect evidence](https://github.com/nerdrx/wivrn-nx/blob/atlas-live/docs/bench/reconnect-decoder-reset-20260908/README.md).

**Motion regression baseline:** isolated native-resolution Pico motion stress now
reproduces the reported lag: the original 128-pixel rebuild threshold takes
**94.24 ms median per moving-frame decode**, versus **1.39 ms** during static
recovery. A matched decode-plus-render run completes only **17.18 fresh pairs/s**
after two startup frames. This adversarial synthetic input changes pose while
keeping pixels fixed; it is a regression stress case, not a rendered head-turn
quality test. Earlier sparse/static results do not characterize this workload.
[Motion timings and actual Pico offscreen captures](bench/results/240fps-2026-09-08/native-motion-stress/README.md).
The [native pose-transition staging failure](bench/results/240fps-2026-09-08/native-pose-transition/README.md)
is fixed; large-motion performance and reconnect reliability remain open.

**Evidence snapshot: 2026-09-08.** The rows below use different fixtures and measurement scopes. They must not be added together or interpreted as one unified benchmark.

| Experiment | Observed result | What it establishes |
|---|---|---|
| Remove unused encoder readback | **8.72 → 2.38 ms median**, **3.49 ms p99**; restored control 8.71 ms | An encoder-stage improvement; four fixture comparisons were bit-identical |
| Opt-in native vertex warp | **6.30 → 3.10 ms**, median of GPU window means | Less renderer GPU work at native output; not frame-level p99 |
| Sequential Pico decode + render | **177–178 completed pairs/s**, approximately **7.75 ms p99** | Improved offscreen sparse-motion performance; still over the 240 Hz deadline |
| Four renders per correction, long run | **298 renders/s**, **74.5 fresh corrections/s**; **67.6%** within 4.17 ms | Repeated-render capacity over an 81.5-second capture; not paced 240 Hz delivery |
| Renderer GPU timestamp diagnostic | **3.99 ms GPU interval p99**, **7.05 ms completion p99** | GPU command intervals and completion latency differ materially |
| CPU completion polling | **324 repeated renders/s**, **7.52 ms p99**, about **7× CPU time** | A costly throughput tradeoff; disabled by default |

### Motion proof attempt

A new native **4352 × 2176** proof attempt still fails strict 240 Hz deadlines. The 30-second changing-pixel pan missed **41 / 7,200** deadlines. A separate rendered-camera trajectory (yaw ±60°, pitch ±25°, translation; 720 distinct input frames over three seconds) missed **3 / 720** deadlines, then **17 / 720** on repeat. Worst camera completion was **4.83 ms**, above the 4.17 ms budget. These offscreen PLANAR measurements exclude encoding, network and compositor latency.

[Raw timings, verified input hashes, camera trajectory and actual Pico captures](bench/results/240fps-2026-09-08/motion-proof/README.md). Average throughput remains approximately 240 FPS; consistent delivery and live head-motion performance are not established.

### PLANAR refresh work

An opt-in GPU fit now emits coarse two-region tiles without CPU fitting or an unused warp-prediction pass. Encoder references use a separate padded PLANAR buffer; host-fit and GPU-fit reference checks pass. On the native synthetic fixture, removing unused clears and selecting the exact zero-slope decoder kernel reduced median reconstruction wall time from roughly **31 ms to 10–11 ms**. This remains above the 240 Hz budget.

Host conformance passes **260 streams**, with three skips. Pico conformance remains incomplete: directional and related cases fail, and synthetic failures reproduce with both new decoder optimizations disabled. These results do **not** qualify the build for live use. [Logs, scope and encoder validation](bench/results/240fps-2026-09-08/planar-refresh/README.md).

### PLANAR direct-render evidence

The standalone PLANAR probe now has a paired 7,200-frame Android capture on a
**4352 × 2176** full-resolution synthetic changing-pixel pan fixture at
approximately **157 Mb/s**. The fixture is low complexity and all-PLANAR; the
probe has no PC encoder, network, XR compositor, head-motion pose warp or live
presentation path. With the queue-priority HIGH configuration, the paired runs
average approximately **240 scheduled updates/s**, while about **9.3% of
deadlines are missed**. This is a renderer and queueing diagnostic, not evidence
of sustained 240 Hz delivery.

![Actual Pico PLANAR async capture](bench/results/240fps-2026-09-08/planar-direct/async-capture.png)

![PLANAR paced latency summary](bench/results/240fps-2026-09-08/planar-direct/paced7200-summary.png)

![PLANAR paced latency trace](bench/results/240fps-2026-09-08/planar-direct/paced7200-trace.png)

*Figure 3. Actual Pico output and scientific latency diagnostics from the
standalone probe. `total_ms` is scheduled arrival to observed completion and
drives the deadline-miss count; `interval_ms` measures cadence and includes
inter-frame CPU cleanup. The plots exclude cold startup/device and pipeline
creation from the timed window. [Archive, raw captures, summary and exact
reproduction](bench/results/240fps-2026-09-08/planar-direct/README.md).*

The experiment supports an approximate GPU PLANAR reconstruction direction and
motivates reducing redundant decoder memory work. Those are research directions;
no additional numerical decoder claim is made here pending matched validation.

A same-binary pacing follow-up completed four HIGH-priority 7,200-frame runs on
the same synthetic fixture. After excluding 24 warmup frames, the three busy-spin
configurations missed **0.47–0.82%** of 240 Hz deadlines, compared with **6.19%**
for the sleep control. The runs report approximately 240 scheduled updates/s,
but this still does not establish consistent 240 Hz delivery. Busy-wait pacing
trades CPU time and thermal headroom; power was not measured. [Raw captures,
summary script, chart and binary/shader identities](bench/results/240fps-2026-09-08/planar-pacing/README.md).

### Removing an entire copy

The encoder was reading back **55 MiB of unused coefficients per frame**. Removing that transfer cut measured live Lite encoding time without changing the compared bitstreams. The control was restored to check that the gain followed the change.

![Encoder time and selection latency before, after and after restoring the unused copy](bench/results/240fps-2026-09-08/unused-coefficient-readback/coefficient-copy-comparison.png)

*Figure 4. Controlled removal of unused GPU→CPU work. The encoder result falls inside 4.17 ms; the full pipeline still has other costs. [Methods, profiler data, screenshots and bitstream checks](bench/results/240fps-2026-09-08/unused-coefficient-readback/README.md).*

### Moving warp work into the mesh

The native renderer splits its mesh at tile and foveation boundaries and performs tile homographies in the vertex path. The final same-APK comparison retained full-resolution output and reproduced the reduction from **6.30 to 3.10 ms** in GPU window means. It did not establish a consistent encode-to-selection tail-latency improvement.

![Native-resolution renderer GPU measurements](bench/results/240fps-2026-09-08/atlas-vertex-warp/render-comparison.png)

*Figure 5. Prototype renderer comparisons; the [final cleaned pair](bench/results/240fps-2026-09-08/atlas-vertex-warp/v3-final/README.md) is recorded separately. These GPU aggregates are not per-frame percentiles. [Implementation and experiments](bench/results/240fps-2026-09-08/atlas-vertex-warp/README.md).*

A subsequent live experiment removes a cancelling color-conversion pair using mutable UNORM attachment views over SRGB swapchain images. GPU window medians were **2.7 / 3.2 / 2.6 ms** for enabled / disabled / repeat, with no consistent selection-latency win. It remains opt-in. [WiVRn NX source, fallback behavior and captures](https://github.com/nerdrx/wivrn-nx/tree/atlas-live/docs/bench/atlas-unorm-20260908).

### Throughput is only one part of 240 Hz

The long sequence test completed **24,000 offscreen renders in 81.5 seconds**, using four static-pose renders per correction. Its steady render p99 was **7.29 ms**. The renderer has useful capacity, but late iterations still prevent a consistent 4.17 ms cadence.

![Offscreen render rates and fresh correction rates shown separately](bench/results/240fps-2026-09-08/sequence-throughput/cadence-comparison.png)

*Figure 6. Fresh corrections and repeated renders count different work. The sparse monochrome fixture is not a broad scene-quality test. [Raw CSVs, reproducible probe, timestamps and negative controls](bench/results/240fps-2026-09-08/sequence-throughput/README.md).*

### Comparison with hardware HEVC

One native-resolution, reversed-order comparison measured encode-start-to-render-selection latency as follows:

| Pipeline | p50 | p95 | p99 |
|---|---:|---:|---:|
| Custom WiVRn NX hardware HEVC | 21.822 ms | 31.159 ms | 33.322 ms |
| NX with experimental target cache | **17.633 ms** | **22.321 ms** | **23.256 ms** |

This is a measured pipeline advantage under those conditions. **Bitrate, quality, stereo organization and rendering paths differed.** HEVC reached decoded pixels sooner; NX spent less time from decode completion to selection. An [earlier native comparison](bench/results/240fps-2026-09-07/live-atlas/native-csv-hevc-nx/README.md) favored HEVC. Neither establishes general codec superiority or photon latency. [Controlled pairs and canonical frame mapping](bench/results/240fps-2026-09-07/live-atlas/borrowed-cache-corrected/v2-live/README.md).

</details>

## Visual results

The following figures come from archived measurements and actual Pico GPU readbacks. The 90 Hz charts use native padded stereo **4352 × 2176**, synthetic camera motion and 720 source frames per run. They measure offscreen parsing, upload and rendering; they do not measure live WiVRn NX or motion-to-photon latency. [Full visual gallery and regeneration script](docs/visual-results/README.md).

### Distribution, rather than average FPS

![Latency distributions for full, multi-pass and single-pass rendering](docs/visual-results/01-latency-ecdf.png)

*Single-pass scheduling moves the latency distribution toward the full-draw control. The dashed line is the 11.111 ms display-period budget. Each curve represents a separate run.*

![Per-frame latency during synthetic motion](docs/visual-results/02-single-vs-full-timeseries.png)

*Individual frames expose stalls that an average FPS number hides. Source frame order is preserved; no warmup samples are removed.*

### Where the time goes

![CPU parsing, upload and GPU timing distributions](docs/visual-results/03-stage-components.png)

*Independent component medians and p99s. These bars must not be summed into an end-to-end percentile; fence waiting overlaps GPU execution.*

![Exact deadline miss counts](docs/visual-results/04-deadline-misses.png)

*Both final single-pass runs complete all 720 frames within 11.111 ms. This finite observation does not guarantee future deadlines or sustained thermal behavior.*

### Speed and pixel freshness together

![Skipped tile fractions and age during the final comparisons](docs/visual-results/05-skips-and-age.png)

*Skipped work is useful only if retained content remains useful. Single A retains some outer pixels for one frame; Single B updates every tile.*

![Historical scheduler starvation comparison](docs/visual-results/06-historical-v1-v2-starvation.png)

*Historical controls: the first scheduler's lifetime-maximum estimate caused prolonged starvation. Expiring measurements helped, but did not establish a hard pixel-age bound. These are different runs from the final single-pass comparison.*

![Artificial admission pressure exposes stale peripheral pixels](docs/visual-results/07-budget-pressure-age.png)

*New failure case: imposing a 2 or 3 ms admission threshold at 90 Hz leaves the outermost pixels stale for almost eight seconds. The artificial threshold is separate from the 11.111 ms cadence. [Raw pressure-test results](bench/results/90fps-2026-09-08/budget-pressure/README.md).*

### Real Pico output: complete versus retained pixels

| Fresh complete frame | Forced centre-only update |
|---|---|
| ![Fresh frame 19 rendered on Pico](bench/results/90fps-2026-09-08/single-pass/single-all.png) | ![Frame 19 centre with initial outer pixels retained](bench/results/90fps-2026-09-08/centre-first/centre19.png) |

*The forced-retention control is intentionally discontinuous: each centre contains frame 19 while its outside retains frame 0. Exact RGBA checks verify both regions. Captures are outside timing, and single-pass output matches the full-draw control.*

### Camera-motion capture sequence

| Forward view | Positive yaw | Negative yaw |
|---|---|---|
| ![Pico camera fixture, frame 0](bench/results/240fps-2026-09-08/motion-proof/camera-000.png) | ![Pico camera fixture, frame 75](bench/results/240fps-2026-09-08/motion-proof/camera-075.png) | ![Pico camera fixture, frame 225](bench/results/240fps-2026-09-08/motion-proof/camera-225.png) |

*Actual readbacks from the earlier 240-FPS source trajectory, including approximately ±60° yaw. These historical images establish changing rendered views, not physical headset tracking or successful 240 Hz presentation. [Trajectory, identities and failed deadline tests](bench/results/240fps-2026-09-08/motion-proof/README.md).*

## Quality and visual research

Speed is the immediate Pico priority. The desired low-bitrate appearance preserves useful edges and structure even when texture or exact reconstruction is sacrificed. That is an objective to test, not a property every current image achieves.

| Rendered test material | Deliberate approximation |
|---|---|
| ![Stereo room fixture with text, thin edges and moving figures](docs/assets/vrroom-mid.png) | ![Source, transform and planar tile comparison](docs/assets/lowpoly-panels.png) |
| The rendered room corpus exercises stereo, head motion and independently moving content. | Planar tiles trade fidelity for hard-edged facets. The recorded experiment costs 2–4 dB; it is a visual choice, not a free quality gain. |

*Figure 7. Existing research figures. [Settings, measurements and regeneration commands](docs/GALLERY.md).*

Historical [reference-codec quality gates](tools/quality/reports/gates-v2-2026-09-04.md) failed on the tested band-limited material. Later timing wins do not close those gates. Live captures still show seams and motion trails, and sustained thermal behavior needs further qualification.

## Measurement rules

A result belongs with its fixture, build and definition of completion.

- **Stage time:** encoder or decoder work alone. Its inverse is not complete-pipeline FPS.
- **GPU interval:** timestamped GPU work, potentially including dependency stalls. Periodic GPU window means cannot supply per-frame p95 or p99.
- **Completed offscreen work:** a GPU fence confirms completion. Repeated rendering does not create new source frames.
- **Render selection:** the client selected a decoded frame for rendering. The logged `blit` event is not a photon measurement.
- **Presentation:** paced updates delivered by the runtime and display. Recent Pico captures use 90 Hz; no physical 240 Hz presentation is demonstrated here.

Compare controls in both orders where practical, preserve warmup and session gaps, and report deadline misses alongside p50/p95/p99. Historical “displayed pose age” labels represented a difference between display timestamps, not measured motion-to-photon latency. [Full protocol and corrections](docs/240FPS.md).

Rejected and inconclusive experiments remain useful evidence: [R16 live handoff](bench/results/240fps-2026-09-07/live-atlas/r16-handoff/README.md), [shared-load pacing](bench/results/240fps-2026-09-07/live-atlas/shared-load-pacing/README.md), [all-skip uploads](bench/results/240fps-2026-09-07/skip-uploads/README.md), and [CPU affinity, color approximation and polling](bench/results/240fps-2026-09-08/sequence-throughput/README.md). Less GPU work alone is insufficient justification for more complexity.

## Status

| Area | Current state | Entry point |
|---|---|---|
| Reference codec and syntax | CPU reference, conformance material and recorded quality results | [Reference](ref/README.md), [syntax](docs/SYNTAX.md) |
| Vulkan encoder | Executable GPU encoder and measured live Lite path | [Encoder](vk/encoder/README.md) |
| Vulkan decoder | Compute decode, reconstruction and atlas experiments | [Decoder](vk/decoder/README.md), [atlas design](docs/ATLAS-DECODER.md) |
| Live streaming | Experimental encoder/decoder/renderer integration in the custom fork | [WiVRn NX `atlas-live`](https://github.com/nerdrx/wivrn-nx/tree/atlas-live) |
| Performance probes | Host and Pico evidence; sequential native-render probe | [Sequence probe](probe/sequence/README.md), [240 Hz work](docs/240FPS.md) |
| Quality evaluation | Synthetic and rendered material, anchors and published gate results | [Quality harness](tools/quality/README.md), [gallery](docs/GALLERY.md) |
| Further design work | Renderer-assisted prediction, compute foveation and hybrid approaches have varying implementation depth | [Architecture](docs/ARCHITECTURE.md), [hybrid](docs/HYBRID.md), [roadmap](ROADMAP.md) |

The design paper records a broader intended system than the measured live path. Consult implementation evidence before treating a design capability as available.

## Building

For CPU development, use **CMake 3.25+**, a **C++20 compiler** and **Ninja** with the checked-in presets:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev --output-on-failure
```

For Vulkan development, install the Vulkan headers, loader, a usable Vulkan driver and `glslc` (shaderc). Some supporting components also use `glslangValidator`:

```sh
cmake --preset dev-vk
cmake --build --preset dev-vk --parallel
ctest --preset dev-vk --output-on-failure
```

Use `cmake --list-presets` to inspect other configurations. Vulkan is off in the default CPU preset. Building this repository does not install the custom headset client.

| Task | Instructions |
|---|---|
| Encode/decode from the reference tools | [Reference codec](ref/README.md) |
| Run Vulkan codec tools | [Encoder](vk/encoder/README.md), [decoder](vk/decoder/README.md) |
| Reproduce offscreen native rendering | [Sequence probe](probe/sequence/README.md); Android runs require an NDK and a matching decoder build |
| Reproduce quality comparisons | [Quality harness](tools/quality/README.md); Python dependencies and external anchors are documented there |
| Work on live VR streaming | [Custom WiVRn NX branch](https://github.com/nerdrx/wivrn-nx/tree/atlas-live) and its build instructions |

## Roadmap

Progress is measured against **90 → 120 → 144 → 180 → 240 Hz**, with full-resolution output and low latency carried through every checkpoint.

1. **Close the completion-time gap.** Separate GPU work, CPU scheduling, synchronization and correction cost; reduce late iterations rather than only average time.
2. **Decouple corrections from presentation.** Keep reference state stable while newer poses drive faster updates. Measure image age and disocclusion failures.
3. **Remove work by region.** Strengthen warp/skip and sparse paths; use foveation, visibility and renderer information to decide what needs computing.
4. **Prove the whole path.** Run matched scene and bitrate comparisons, moving-head captures, long thermal tests and actual presentation measurements on suitable hardware.

Stage fusion, specialized queues and decode-cost-aware rate control are candidates when measurements justify them. Every feature must earn its complexity through quality, bitrate, latency, GPU cost and power behavior. [Experiment direction](docs/240FPS.md) · [Broader roadmap and historical gates](ROADMAP.md).

## Documentation

| Document | Purpose |
|---|---|
| [Documentation index](docs/README.md) | Map of the project |
| [Design paper](docs/PAPER.md) | Rationale, alternatives and intended architecture |
| [Bitstream syntax](docs/SYNTAX.md) | Normative syntax; takes precedence over the design paper |
| [Architecture](docs/ARCHITECTURE.md) | Modules and interfaces |
| [Transport](docs/TRANSPORT.md) | Wire format, feedback and packet handling |
| [240 Hz experiments](docs/240FPS.md) | Performance direction, methodology and limitations |
| [Research gallery](docs/GALLERY.md) | Figures with settings and regeneration commands |
| [Architecture decisions](docs/adr/) | Recorded implementation choices and tradeoffs |
| [Integration design](docs/INTEGRATION.md) | Design context; use the live fork for current implementation |

## Contributing

Start with [CONTRIBUTING.md](CONTRIBUTING.md). Useful work includes reproducible performance experiments, difficult motion and disocclusion fixtures, conformance coverage, and independent hardware measurements.

Codec syntax changes need the reference behavior, documentation and vectors to agree. Lossy visual experiments should state the approximation and compare it against a named control; they do not excuse undefined reconstruction, reference corruption or unsafe synchronization. Keep measurements scoped, retain negative results and run the checks relevant to the change.

## License and acknowledgements

[Apache-2.0](LICENSE). Licensing does not constitute patent clearance; the repository's [review brief](docs/FTO-BRIEF.md) records that separate work.

NX Warp builds on the open VR ecosystem: [WiVRn](https://github.com/WiVRn/WiVRn), [WiVRn NX](https://github.com/nerdrx/wivrn-nx), [Monado](https://monado.freedesktop.org/), [ALVR](https://github.com/alvr-org/ALVR), and Mesa. The [design paper](docs/PAPER.md) discusses the compression, reprojection and perceptual research behind the project. Branding belongs to the [NX family](brand/README.md).

<div align="center">
<img src="brand/nx-warp-mark-128.png" width="48" alt="NX mark">
<br>
<sub>Useful pixels. Less work. Measured progress.</sub>
</div>
