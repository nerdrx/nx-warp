# Current live software latency baseline

**27.646 ms median / 34.835 ms p95 / 37.609 ms p99**, from server encode start
to the first recorded frame selection in the Pico render loop. This is measured
software pipeline latency, **not motion-to-photon latency** and not the latency
of the unintegrated synchronized-history prototype.

[Frame-matched stage breakdown](../live-stage-audit/README.md) now separates packet arrival, worker delay, completion and selection for one common cohort. This is additional analysis of this capture, not a new latency result.

## Measurement

The running custom WiVRn NX wider-ring profile completed a 90-second headless
animated full-field scene test. The scene advanced through 87.99 seconds, client
survived, and current decode/render telemetry was present at the end. A complete
CSV session records canonical NX wire-to-outer frame mappings. The first ten
seconds after initial packet reception are excluded; duplicates use the earliest
event per frame. No decoded/selected frames lack encode-start mappings, and the
parser rejects negative durations or ambiguous mapping. Stream 0 is the paired
stereo stream; streams are never combined into one latency distribution.

| Encode start to… | Matched frames | p50, ms | p95, ms | p99, ms |
|---|---:|---:|---:|---:|
| First packet received on headset | 6,712 | 5.581 | 7.110 | 8.044 |
| Decoder completion | 6,712 | 21.596 | 28.750 | 31.122 |
| First render selection | 6,456 | 27.646 | 34.835 | 37.609 |

Percentiles are per-frame, not percentiles of two-second FPS averages. Stages
have different cohorts, so subtracting the displayed percentiles is not a valid
stage-by-stage decomposition. Selection latency describes selected frames only;
it excludes received frames never selected and is not a lost-frame penalty.

For context only, the final 30 two-second client windows average 81.13 fresh
source selections/s, 4.97 ms decoder GPU time and 5.82 ms presentation GPU time.
Those averages are **not** end-to-end latency and cannot be added to the above
percentiles. The source display-time offset averages 58.01 ms; that is a
different scheduling diagnostic, not the measured encode-to-selection interval.

## Clock and endpoint audit

Server encode events use monotonic nanoseconds. Headset feedback is converted
into the server domain using WiVRn's clock-offset estimator. The CSV carries
explicit `nx_frame_map` entries; NX feedback IDs cannot be joined directly to
outer encoder IDs. The parser handles wire-ID epochs and checks forward order.

The client log confirms **decoupled display: frames published complete** and
borrowed NV12 output. `decode_end` in this profile is recorded after the decoder
completion fence. `blit` is frame selection in the render loop, before later
presentation work. `display` is predicted OpenXR display time, not observed
scanout; it is deliberately excluded.

External clock-offset uncertainty has not been independently calibrated. This
is a software measurement under WiVRn's clock synchronization, not a certified
absolute timing instrument. CSV logging itself may perturb the run. Application
render time before encode, presentation GPU completion, compositor/scanout and
photons are outside this interval. No camera, photodiode or physical head-motion
rig was used. Synthetic scene animation does not establish head-motion latency.

## Decision

Use this as a current instrumented baseline, not evidence of a new improvement.
The synchronized standalone shader adds reconstruction work and is not selected
in this live profile. The next bounded experiment should omit detail work for
one peripheral band while sampling valid pose-correct history in the existing
presentation pass, with both eyes sharing tile phases and both centres fresh.
Measure the complete integrated path against this baseline, including invalid
history, disocclusions and frame drops. Do not infer latency savings from shader
execution time or fresh-frame throughput.

## Reproduce the analysis

```sh
python3 summarize_pipeline_latency.py timings.csv.gz --codec nx --warmup 10
```

[Raw CSV](timings.csv.gz) · [Summary](summary.json) · [Trial validity](status.json)
· [Build/profile identity](metadata.json). Enable `WIVRN_DUMP_TIMINGS` before a
fresh server session, collect it from the first frame through session shutdown,
and do not concatenate sessions. Timing dumps were disabled after this run and
the normal wider-ring streamer was restored.
