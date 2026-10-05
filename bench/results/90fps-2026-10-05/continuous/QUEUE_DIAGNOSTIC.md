# Measure queue age before changing delivery policy

Source [831aafed](https://github.com/nerdrx/wivrn-nx/commit/831aafed) adds an **off-by-default** Android ASTC queue diagnostic. It reports mean/maximum completed-packet enqueue-to-worker-dequeue dwell, dequeue count, oldest-pending drop count and stream index in the existing 180-successful-handoff summary.

`debug.wivrn.nx.astc_queue_timing` must equal exactly `1` before decoder construction. Nothing was activated or installed. The disabled path preserves the previous log and performs no new clock reads or counter collection. Enabled counters and timestamps use the existing mutex; there is no queue, packet, reference or render-policy change.

This closes an instrumentation gap found by the [freshness audit](FRESHNESS_AUDIT.md): existing sender/decoder caps prevent unbounded queues, but source inspection cannot show actual dwell or drop frequency. The next live gate can distinguish waiting before decode from decode/copy, upload fence and host handoff costs. The mean includes all dequeues, even packets later rejected. Queue windows are not necessarily 180 dequeues, and a failure without 180 successful handoffs may emit no summary. Maximum dwell is not p95/p99 and none of these fields measures photons.

The complete Android arm64 **RelWithDebInfo native `wivrn` target built successfully**, including the ASTC object and header consumers. [Final max-dwell rebuild](android-relwithdebinfo-wivrn-final.log), [stream-tag build check](android-relwithdebinfo-wivrn-stream-tag.log), [artifact ordering](queue-build-artifacts.txt). An earlier Debug target failed because its cache pointed at a stale NXWarp checkout and could not find `nxvc/transport/common.h`; the failure is [retained](android-native-wivrn.log). The existing correctly configured release-like build succeeded without cache/source workarounds.

No standalone decoder instance test was possible without application/device/Vulkan ownership. **Runtime metric correctness and enabled-path overhead remain unmeasured**. No fresh-FPS, Wi-Fi or latency improvement is claimed for this diagnostic.

[Source configuration and metric boundaries](https://github.com/nerdrx/wivrn-nx/blob/pyrowave-probe/docs/ASTC_QUEUE_TIMING.md)
