# R2 compact kernel comparison

Four 60-second full-field motion trials on Pico, 2688² per eye, 1024px
native centre, ungrouped R2, smoothing 3, SHA2 off. Client 87f2e9bb;
core decoder 2f58ed8. All four completed with current telemetry.

| Kernel | Fresh selections/s | Decode GPU |
|---|---:|---:|
| Compact flat64 | 52.53 | 13.016 ms |
| Previous flat kernel | 51.95 | 13.472 ms |

Post-warm-up window means, with two runs per kernel. The existing flat64
selection remains preferable, but this is not enough for 90 fresh FPS.
The native-centre workload is the next target; lowering peripheral colour
capacity alone does not solve it. These are neither per-frame latency
percentiles nor physical motion-to-photon measurements.

See [summaries](summary.json), [completion status](statuses.json), and
[raw logs](logs.tar.gz).
