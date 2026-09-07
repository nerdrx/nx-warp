# Empty ATLAS pipeline demand

These six runs use the same binary and fixture. Each process decodes four frames; frame 0 is the common cold start. Frame 1 is the first all-skipped frame (`0` lane groups, one compose dispatch). `force` sets `NXVC_VKD_ATLAS_FORCE_PIPELINES=1`; `demand` leaves the pipeline-demand optimization enabled.

| Pair | Frame 1 control submit / total (ms) | Frame 1 demand submit / total (ms) |
|---|---:|---:|
| 1 | 67.176 / 68.234 ([force1](243-force1.log)) | 0.123 / 2.937 ([demand1](243-demand1.log)) |
| 2 | 80.053 / 80.371 ([force2](243-force2.log)) | 0.156 / 5.282 ([demand2](243-demand2.log)) |
| 3 | 78.197 / 78.626 ([force3](243-force3.log)) | 0.162 / 4.315 ([demand3](243-demand3.log)) |

Frames 2 and 3 are steady state: demand submit/total are 0.052/0.296 and 0.037/0.272 ms (pair 1), 0.101/0.503 and 0.122/0.446 ms (pair 2), and 0.161/0.417 and 0.038/1.730 ms (pair 3). The large frame 1 gap is consistent across all three pairs and is therefore strong evidence that unused pipeline creation dominates the first empty frame. These are startup-latency measurements, not sustained-FPS results or independent fixed-GPU-clock estimates; no clocks were sampled in this round.

Run provenance is recorded in [demand-provenance.log](demand-provenance.log).
