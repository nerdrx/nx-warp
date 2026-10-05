# Native NXVC: overnight evidence and remaining gates

Finalized 2026-10-05 04:00 UTC /06:00 Berlin. The authorized work window is complete. All owned benchmark jobs and the bounded Luna audits have finished; user applications and live sessions were left untouched.

## Current position

The last recorded live profile is native **2176×2176 per eye, ASTC8×8**, without foveation, JPEG, blur or object motion warp. The last recorded headset client is c514841f; this run performed no installation. Source improvements are built and pushed separately; the server was not restarted and experimental options were not enabled. Native90 fresh frames/s, parity with hardware HEVC and photon latency are still **unproven**.

The strongest new result is reduced PC host work: an ordinary-Zstd3 native stereo harness improves by 2.620 ms paired mean through overlapping eye packing, with identical texture and packet bytes. GPU work itself stays essentially unchanged. The largest earlier fence wait no longer reproduces under the later observed GPU context; that is not credited as a code optimization.

## What has evidence

| Change or check | Actual result | Scope / rollout |
|---|---|---|
| Ordinary-Zstd3 parallel eyes | Full-call p50/p95 7.696/7.843 → 4.886/5.002 ms; all 20 matched pairs improve | Offscreen PC; default-off source option |
| Recheck of earlier GPU fence wait | First wait p50/p95 .934/.943 ms; earlier median 9.461 ms not reproduced | Changed readonly load context; no cause or code win inferred |
| Cached terminal-shard assist | Two-round fixture recovers 64/64 lost tail shards, baseline 2/64 | Actual classes, parity withheld; source 7b7ae360, default off |
| Archived-client compatibility | Old c514841f receiver accepts current packets and completes after the selected terminal reply; normal and strict sanitizer checks pass | CPU protocol path; valid nonempty metadata fixture, no installed-client or session proof |
| Repair history 1 → 2 MiB/encoder | Holds all 521 shards two frames back at synthetic 1 Gbit/s, 90 Hz stereo budget; old ring 0 | Actual history/serialization; built, not installed |
| Readiness-count cache | Asleep Pico helper due-query mean 5.287 → 2.932 µs, matching decisions | Tiny metadata CPU component; not stream/FPS latency |
| Quiet-stream recovery polling | First host-adapter request opportunity 20.06 → 3.08 ms in controlled signal test | Default off; excludes actual Wi-Fi/repair/Pico delivery |
| UDP receive reuse | Approx. 40 KiB allocations −95.7%, timing effectively unchanged | Loopback allocation win; source built, not installed |
| Compact independent records | Payload −5.01–5.77%; extra Pico CPU .055–.126 ms/eye | Lossless blocks, requires v4 client; default off |

The rows use different fixtures and timing boundaries. They must not be summed into a claimed end-to-end latency saving. Source7b7ae360 is pushed to `pyrowave-probe`; live installation is a separate gate.

![Matched native host overlap](../overnight-recovery/fence-overlap-recheck/overlap.png)

![Lost-tail recovery within two rounds](../overnight-recovery/end-shard-assist/recovery.png)

## What was cut or held

Repeated per-pixel conversion loses 156 µs GPU time. Caching converted tile pixels saves about 84 µs in its generated-image GPU test but doubles driver scratch; packed caching reduces scratch yet costs another 30 µs. None is integrated. Their source-format, masking, layers, bounds and compositor input-lifetime gates remain.

Deferred sender-wait placement duplicates an earlier rejected prototype: mixed timings under high unknown GPU load, with no robust complete-cycle win. Its original fixtures are unavailable for a faithful bounded recheck. No new source proposal was retained. Whole-eye receiver completion also blocks a small compatible partial-band transport change; no idealized region replay is presented as a production result.

## What must happen next

1. Validate the default-off eye overlap and recovery options in a short real compositor/Pico run, including head movement, fresh stereo frame IDs, decode/presentation cost and network recovery. Keep the ordinary native profile as the comparison.
2. Measure the actual sender wait, GPU readback and CPU pack spans. Current source holds its two-image pool through packet preparation and earlier-send completion; removing those holds without a per-frame ownership contract is unsafe.
3. Confirm the terminal assist's duplicate traffic and adaptive-loss feedback under real link loss. It does not rescue a newest unknown tail without an ordinary NACK; 128 lost tail shards remain incomplete in two rounds.
4. Obtain physical display timing before making photon-latency claims. Viewer loops and kernel timers cannot substitute for that measurement.

The existing Perfetto CPU lanes also need an actual concurrent capture before they are used to infer eye overlap. The current build has Perfetto disabled and no SDK/trace processor; the shared-lane nesting concern is a source audit, not a proven runtime fault.

Detailed runnable checks, raw rows and limitations: [active queue](OVERNIGHT.md), [host overlap](../overnight-recovery/fence-overlap-recheck/README.md), [end assist](../overnight-recovery/end-shard-assist/README.md), [earlier component results](../overnight-recovery/README.md). The [actual-history concurrency check](../overnight-recovery/endpoint-concurrency/README.md) now passes normal, strict ASan/UBSan and TSan, including full frame identity and concurrent enable/disable cycles. It is finite validation, not a new performance result.

The [archived-client compatibility gate](../overnight-recovery/end-assist-compat/README.md) retains exact revisions, a runnable mixed-header driver and captured normal/sanitizer logs. It keeps the old empty-vector parser limitation explicit.

A newest-frame tail probe could reuse the ordinary NACK format, but is held pending arrival/deadline validation: an uncached next shard can spend one of two repair rounds without a response. Before the sender records `end_frame`, ordinary cached data may be available but the terminal selector has no completed count. Delayed parity and confirmed-hole priority also need coverage. No speculative probe policy was added.
