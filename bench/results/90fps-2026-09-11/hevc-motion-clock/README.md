# Application-clock extrapolation: short Pico screen

2026-09-11. Custom WiVRn NX, full HEVC 10-bit, 2688×2688 per eye, source cap 60, Pico presentation 90 Hz. Animated full-field hello_xr under headless gamescope; stationary physical headset. Two sequential 30-second trials, first 10 seconds excluded from client telemetry. Not a paired statistical latency study.

| Warm telemetry | Existing clock | Application clock (opt-in) |
|---|---:|---:|
| Fresh first-eye selections/s | 59.61 | 59.50 |
| Render iterations/s | 90.00 | 90.00 |
| Mean own GPU time (ms) | 5.73 | 5.79 |
| Active pose-compensated shifts | 14 | 34 |
| Application-clock shifts | 0 | 34 |
| Missing / unsafe pose rejections | 1 / 0 | 2 / 0 |
| Median compositor minus app timestamp (ms) | 0.0345 | 0.095 |

**No latency reduction demonstrated.** Source selections and render iterations are different metrics; neither establishes 90 distinct images or photon timing. GPU numbers are averages of telemetry windows, not per-frame percentiles. Source timestamp denotes the application's requested display time, not a universal simulation timestamp. This scene animates from its predicted display time.

An earlier diagnostic (`hevc-clock`) had a 30.995 ms median timestamp difference, but only 30.25 fresh selections/s under changed pacing. The stable-cap runs above do not reproduce that large offset. It must not be presented as recoverable latency. Clock statistics retain new application commits after 10 seconds; client statistics use their own telemetry warmup origin.

Implementation [b52e947d](https://github.com/nerdrx/wivrn-nx/commit/b52e947d) transmits the application timestamp and interval separately from compositor pose timing. Set `debug.wivrn.nx.motion_source_clock=1` to opt in. Missing, invalid or multi-layer source timing falls back to the existing clock. Original compositor span still identifies the predecessor pose. Both endpoints require matching rebuilt protocol versions. Default remains off.

Packet tests: 273 checks passed. Server and Android release builds passed. Raw logs, summaries and original local run/analyzer scripts are included; scripts contain machine-specific paths. `final-build-sha256.json` identifies the final binaries, whose server additionally includes the later exact-match shader fix, not the precise server binary used for these A/B trials.

The selected NX large-centre configuration and default motion properties were restored. Next gate: robust GPU image prediction, then actual moving-head and synchronized optical latency measurement. See the [GPU truth failures and fix](../motion-gpu-truth/README.md).
