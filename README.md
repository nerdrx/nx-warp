<div align="center">

<img src="brand/nx-warp-logo.png" width="460" alt="NX Warp">

### VR-specific compression. GPU-native reconstruction. Latency first.

[![CI](https://img.shields.io/github/actions/workflow/status/nerdrx/nx-warp/ci.yml?branch=main&label=CI&labelColor=0c0818&color=7700FF)](https://github.com/nerdrx/nx-warp/actions/workflows/ci.yml)
[![Format](https://img.shields.io/github/actions/workflow/status/nerdrx/nx-warp/format.yml?branch=main&label=format&labelColor=0c0818&color=7700FF)](https://github.com/nerdrx/nx-warp/actions/workflows/format.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-00e5ff?labelColor=0c0818)](LICENSE)
![Status](https://img.shields.io/badge/status-research%20prototype-ffb300?labelColor=0c0818)

[Design paper](docs/PAPER.md) · [Measured results](#measured-results) · [Build](#building) · [WiVRn NX integration](https://github.com/nerdrx/wivrn-nx/tree/atlas-live)

</div>

## Abstract

**NX Warp is an experimental video codec for rendered VR, developed with the custom WiVRn NX streaming stack.** It explores a simple premise: head pose, reusable tiles and renderer information should let a headset reconstruct useful pixels with less work than a conventional whole-frame pipeline.

The current work combines a Vulkan encoder, a Vulkan decoder and an atlas renderer tested on Pico 4. The priority is **low latency, full-resolution output and inexpensive reconstruction**, especially at low bitrate. Visual approximations are valid experiments when they preserve useful structure and measurably reduce cost. Quality, bitrate and power remain measured tradeoffs.

The stretch target is **240 Hz / 4.17 ms per update**. Individual stages and repeated-render throughput have crossed parts of that budget; **consistent 240 Hz delivery has not been demonstrated**. This is a research prototype, with visible artifacts and incomplete quality gates, rather than a production-ready streaming release.

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

**Read next:** [Architecture](#architecture) · [Results](#measured-results) · [Measurement rules](#measurement-rules) · [Status](#status) · [Roadmap](#roadmap) · [Documentation](#documentation)

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

**Latest decoder improvement:** consecutive full-picture frames now reuse an
already materialized reference. In the native Pico motion stress control,
eligible-frame median decode time falls **121.82 → 66.11 ms** with matching output
hashes; motion-phase median falls **98.56 → 64.71 ms**. This is a real but incomplete
improvement: live motion performance and 240 Hz delivery remain unproven.
[Implementation, validation and timing figure](bench/results/240fps-2026-09-08/materialized-copy/README.md).

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

### Removing an entire copy

The encoder was reading back **55 MiB of unused coefficients per frame**. Removing that transfer cut measured live Lite encoding time without changing the compared bitstreams. The control was restored to check that the gain followed the change.

![Encoder time and selection latency before, after and after restoring the unused copy](bench/results/240fps-2026-09-08/unused-coefficient-readback/coefficient-copy-comparison.png)

*Figure 3. Controlled removal of unused GPU→CPU work. The encoder result falls inside 4.17 ms; the full pipeline still has other costs. [Methods, profiler data, screenshots and bitstream checks](bench/results/240fps-2026-09-08/unused-coefficient-readback/README.md).*

### Moving warp work into the mesh

The native renderer splits its mesh at tile and foveation boundaries and performs tile homographies in the vertex path. The final same-APK comparison retained full-resolution output and reproduced the reduction from **6.30 to 3.10 ms** in GPU window means. It did not establish a consistent encode-to-selection tail-latency improvement.

![Native-resolution renderer GPU measurements](bench/results/240fps-2026-09-08/atlas-vertex-warp/render-comparison.png)

*Figure 4. Prototype renderer comparisons; the [final cleaned pair](bench/results/240fps-2026-09-08/atlas-vertex-warp/v3-final/README.md) is recorded separately. These GPU aggregates are not per-frame percentiles. [Implementation and experiments](bench/results/240fps-2026-09-08/atlas-vertex-warp/README.md).*

A subsequent live experiment removes a cancelling color-conversion pair using mutable UNORM attachment views over SRGB swapchain images. GPU window medians were **2.7 / 3.2 / 2.6 ms** for enabled / disabled / repeat, with no consistent selection-latency win. It remains opt-in. [WiVRn NX source, fallback behavior and captures](https://github.com/nerdrx/wivrn-nx/tree/atlas-live/docs/bench/atlas-unorm-20260908).

### Throughput is only one part of 240 Hz

The long sequence test completed **24,000 offscreen renders in 81.5 seconds**, using four static-pose renders per correction. Its steady render p99 was **7.29 ms**. The renderer has useful capacity, but late iterations still prevent a consistent 4.17 ms cadence.

![Offscreen render rates and fresh correction rates shown separately](bench/results/240fps-2026-09-08/sequence-throughput/cadence-comparison.png)

*Figure 5. Fresh corrections and repeated renders count different work. The sparse monochrome fixture is not a broad scene-quality test. [Raw CSVs, reproducible probe, timestamps and negative controls](bench/results/240fps-2026-09-08/sequence-throughput/README.md).*

### Comparison with hardware HEVC

One native-resolution, reversed-order comparison measured encode-start-to-render-selection latency as follows:

| Pipeline | p50 | p95 | p99 |
|---|---:|---:|---:|
| Custom WiVRn NX hardware HEVC | 21.822 ms | 31.159 ms | 33.322 ms |
| NX with experimental target cache | **17.633 ms** | **22.321 ms** | **23.256 ms** |

This is a measured pipeline advantage under those conditions. **Bitrate, quality, stereo organization and rendering paths differed.** HEVC reached decoded pixels sooner; NX spent less time from decode completion to selection. An [earlier native comparison](bench/results/240fps-2026-09-07/live-atlas/native-csv-hevc-nx/README.md) favored HEVC. Neither establishes general codec superiority or photon latency. [Controlled pairs and canonical frame mapping](bench/results/240fps-2026-09-07/live-atlas/borrowed-cache-corrected/v2-live/README.md).

## Quality and visual research

Speed is the immediate Pico priority. The desired low-bitrate appearance preserves useful edges and structure even when texture or exact reconstruction is sacrificed. That is an objective to test, not a property every current image achieves.

| Rendered test material | Deliberate approximation |
|---|---|
| ![Stereo room fixture with text, thin edges and moving figures](docs/assets/vrroom-mid.png) | ![Source, transform and planar tile comparison](docs/assets/lowpoly-panels.png) |
| The rendered room corpus exercises stereo, head motion and independently moving content. | Planar tiles trade fidelity for hard-edged facets. The recorded experiment costs 2–4 dB; it is a visual choice, not a free quality gain. |

*Figure 6. Existing research figures. [Settings, measurements and regeneration commands](docs/GALLERY.md).*

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
