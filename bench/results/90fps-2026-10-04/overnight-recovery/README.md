# Overnight recovery work — 4 October 2026

**Source experiments; live profile unchanged.** The live profile stays native ASTC 8×8. Three questions are being tested: can independent packets carry identical detail
in fewer bytes, can the PC encode both eyes concurrently, and can smaller
independent delivery units cover losses before bitrate adaptation catches up? Neither question is a measured photon-latency
result or proof of sustained 90 fresh frames/s.

## Completed source experiments

- **Parallel eye packing, opt-in:** the CPU-only two-eye batch falls from
  **3.516/4.096 to 2.130/2.474 ms p50/p95** using a right-eye async task and the
  existing left-eye worker. Packet bytes are identical. Host/server build and
  ownership review pass. Real GPU fence waits, sender pacing and live motion
  remain unverified. [Method, samples and figure](stereo-packing/README.md).
- **Compact independent records, opt-in:** removing fixed ASTC mode bits saves
  **5.01–5.77%** on two native fixtures with identical decoded texture bytes.
  Pico strict CPU decode adds about **0.055–0.126 ms per eye**. It uses one Zstd
  attempt and restores blocks in existing scratch, without a GPU pass. PC and
  Android builds, compatibility tests and sanitizers pass. It remains off in
  the live profile. [Exact format, timings and figure](compact/README.md).
- **Independent regions, modeled only:** 256px regions recover about
  **98.7–99.1% of area per send** at modeled 2% random packet loss, versus
  **61.5–73.9%** complete whole-frame sends. Estimated payload/FEC overhead
  rises **1.6–5.7%**. The native inputs are repeated static snapshots; this
  proves neither motion quality nor temporal freshness. Square regions remain slower on Pico. Full-width bands with a reused
  context decode near whole-frame cost and directly update contiguous ASTC
  rows. These are CPU probes; partial transport and pose-safe presentation
  are still unimplemented. [Pico costs and rejected allocator churn](band-cpu/README.md). [Model and procedural
  animation](regions/README.md).

![PC compression batch wall time](stereo-packing/stereo-packing.png)

![Same texture bytes versus isolated Pico decode cost](compact/compact-results.png)

## Same-device Vulkan follow-up

The offscreen two-eye test now includes both native image dispatches, readback,
fence waits and packet compression on one RX 7900 XTX device/queue. Async eye
processing reduces complete-call p50/p95 from **13.865/15.432 to
11.364/13.007 ms**, with identical ASTC and packet bytes. GPU dispatch durations
are unchanged; this is CPU overlap. Dominant CPU fence waits remain about 9ms
and are being investigated. Upload is excluded; actual live compositor, Wi-Fi,
viewer and photon latency are not measured. [Source, 100 interleaved pairs,
external decode checks and graph](stereo-gpu/README.md).

![Same-device full call versus shader durations](stereo-gpu/stereo-gpu.png)

## Small independent safety image

![Fixture payload and raw backup bandwidth bounds](thumbnail-budget.png)

Five static inputs were encoded at 128², 256² and 512² per eye with the older
standalone ASTC fit3/q6 encoder, then compressed with Zstd level 3. The bytes
include the 16-byte ASTC file header, exclude transport/FEC overhead, and are
normalized to two **duplicated** eyes at 90 updates/s. The photographic input is
private; only size measurements are published. These are neither distinct stereo
images nor live Wi-Fi measurements. The initial thumbnail preprocessing used
Lanczos resizing; the separate image-input shader probe below uses linear
texture sampling.

At 256², measured payload spans **11.34–18.56 Mbit/s**. That leaves little
headroom for the highest-cost fixture. At 512² it spans **26.55–64.61 Mbit/s**.
Compression cannot guarantee those rates for other scenes.

The raw ASTC bound makes **192² per eye** a more conservative starting point:
576 blocks × 16 bytes × 2 eyes × 90 × 8 = **13.27104 Mbit/s**. The graph's 35%
extra allowance is an illustrative budget, **not measured packet/FEC overhead**.
The corresponding raw 256² rate is already **23.59296 Mbit/s** before overhead.
Main-image resolution would remain native; the smaller image would be used only
when detail delivery stalls.

An independent sampled-image Vulkan harness now creates a standard coarse ASTC
texture directly from a native RGB source in one encode dispatch. It needs no
intermediate downscale image/pass. Native 2176² dark/forest inputs and 192²/256²
outputs decode correctly with an external ASTC decoder; repeated outputs match.
The PC GPU dispatch is sub-millisecond in this probe, but **8.7–9.0 ms median
fence waits dominate the synchronous host call**. These measurements do not prove
a live encoder or headset latency improvement.

## Integration requirements

The current ASTC profile has separate left/right streams and no auxiliary
safety-stream role. The existing NX Warp safety implementation is for a
different paired-eye representation. A thumbnail alone cannot be plugged into
that path safely.

An integration must reserve transmission opportunity for safety packets, keep
their decoding outside the primary stereo join, select a coherent backup pair
after two missed refresh periods, and return only to newer coherent primary
content. A backup still cannot bypass an already congested kernel/AP queue or
recover a complete link outage. No live takeover has been implemented or timed
in this report yet.

A standalone selection-policy draft tests mismatched eyes, duplicate frames,
late primary arrivals, stale per-eye decoder completions, future predicted
display targets, clock rollback and focus reset. `view_info.display_time` is a
**server-predicted display target**, not source capture time. Its ordering can
prevent backwards selection; its age cannot prove content or photon latency.

## Rejected delivery/entropy shortcuts

- `sendmmsg` improved one loopback datagram microprobe by about 2.3%; it excludes
  production serialization, pacing and FEC costs. No sender change is justified
  by that measurement.
- Row-reset byte residuals increased compressed bytes about 10.6% over the
  36-frame camera/object scene and 18.9% over the native photo fixtures.
- Separating mode/endpoint/weight bit fields increased the moving-scene output
  about 16.1%. Compact per-block records are documented above with their
  measured Pico CPU cost.
- The old session's UI/font activity was not reproduced as a current bug.
  Current readiness transitions already have fixes; it is not evidence that
  font loading caused the present delivery problem.

## Reproduction and limits

`thumbnail-payload.json` contains the measured fixture byte counts.
`python3 plot_budget.py` regenerates the figure. Raw user photos are excluded.
This report separates measured payload, calculated bandwidth, standalone GPU
work, standalone policy checks and unimplemented live behavior. The next tests
target independent-region delivery under packet loss rather than assuming an
incomplete whole frame must freeze every pixel.
