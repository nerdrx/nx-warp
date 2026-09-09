# Pacing audit and benchmark validity

**No latency improvement established. Experimental pacing code was reverted.**
The restored server completed a 90-second moving-scene run with 71.92 fresh
updates/s and 58.75 ms source display-time offset (not photon latency).
Presentation GPU averaged 5.07 ms and decoder GPU 4.95 ms over the last
30 complete reporting windows. Native centre and effects-off profile remain.

## What was tried

The server's pacing estimator takes a 99.5th percentile of present-to-decoded
samples. The archived, **rejected** patch adds telemetry, a predicate for its
worker notification, and batches by accepted feedback count instead of specific
frame IDs. It is preserved for investigation, not recommended for deployment.
No budget trace was observed even after enabling its informational output;
this does not prove feedback is absent. The reason remains unresolved.

Two runs (`pacer-feedback`, `pacer-trace-valid`) lost useful client telemetry
partway through despite the app process surviving. The latter includes a Pico
watchdog stack-dump event; a later ActivityManager query reported no recorded
ANR. We do not attribute the failure to a specific thread, driver, or change.
A server restart also stalled. Debugger attachment was denied by the host, so
no native stack diagnosis was obtained. The owned streamer was recovered.

Those two statuses are explicitly marked invalid; their original process-only
check is retained for audit. Their short early-window timings are excluded from
`valid-summary.json`. An additional aborted restart trial is not benchmark data.

## Harness correction

`capture_live.py` now requires continuing scene animation, at least 30 complete
render **and** decode report windows for a 90-second test, and a render report
within 10 seconds of the headset's current clock. It rejects a live but stalled
process. `pacer-restored-status.json` records both 30-window checks and a final
render-log age of 0.575 seconds. The scene advanced through 86.3 seconds.

The restored source is WiVRn NX `64afe113`; the server pacing files match that
commit. The APK and workload are unchanged from the adjacent
[latency probes](../latency-probes/README.md). The preceding telemetry-only
`pacer-budget` run is retained as a nonrandomized control, not a matched proof
of a causal improvement. The half-latency goal remains unmet.
