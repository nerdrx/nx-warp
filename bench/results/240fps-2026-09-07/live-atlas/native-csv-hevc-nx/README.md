# Native CSV latency comparison

This native full-resolution checkerboard capture compares the regular
hardware HEVC path with the specialized NX path. The APK was identical in both
arms (`1a484ee3a88c57ac74402ba8d7a3a127b2d825c8e03e9b46bb473d560b6aced3`); the
server build was `e220c9c86e465a2363439d0ec4070a524286d87856326234d8f068f73a99d8b6`.
The scene was the same, but bitrate and render post-processing were not matched,
so this is an observed pipeline comparison rather than a quality or causal
benchmark. Both screenshots show the checkerboard/cube scene; NX has visible trails around the moving cubes. Screenshot file origins are recorded in `provenance.json`.

After the first 10 seconds, arrival-to-ready p50/p95/p99 was 5.821/12.011/13.057
ms for HEVC (4,677 received; 3,971 paired) and 12.649/21.581/23.043 ms for NX
(4,711 received). Arrival-to-render-selection was 16.754/20.501/27.540 ms for
HEVC (3,931 selected) versus 19.555/28.366/30.703 ms for NX (4,306 selected).
Thus NX did not beat hardware HEVC in this test. These are reported-frame
selection timings, not compositor submission, photon, or end-to-end latency.

The archive contains relative timestamps only. Reproduce the values with:
`python3 reproduce-native-csv.py`. The script uses stream 0, removes the first
10 seconds, takes the earliest duplicate event per frame, and computes the
reported percentiles. Raw logs remain outside the repository; executable,
configuration, and screenshot provenance must be retained with any rerun.
