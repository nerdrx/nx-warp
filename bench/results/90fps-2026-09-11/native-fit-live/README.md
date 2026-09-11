# Native fit live evidence (2026-09-11)

Four 60-second `NXWARP_BENCH_FULL_FIELD` runs from `nx-scratch/motion-live`.
The tested change is commit `4497e61` (native jobs bypass unused palette fitting).
No server/client binaries are stored here.

Client values below are the post-warm summaries from
`raw/analyze_warm.py`; its selected windows begin
after the first 10 seconds of client telemetry. They are fresh-frame rates, not
display rates, and do not support a 90 FPS claim.

| run | fresh/s | decoder GPU ms | Pass A / B ms | own GPU ms | source offset ms |
|---|---:|---:|---:|---:|---:|
| fit-base-a | 48.08 | 12.912 | 4.708 / 8.216 | 2.838 | 76.07 |
| fit-skip-a | 53.23 | 13.124 | 5.032 / 8.068 | 3.213 | 77.59 |
| fit-skip-b | 52.58 | 13.150 | 5.046 / 8.108 | 2.908 | 77.52 |
| fit-base-b | 53.08 | 12.992 | 4.776 / 8.196 | 3.354 | 78.23 |

`fit-base-a` is the 48.08 fresh/s outlier; the other three are 52.58--53.23
fresh/s. This small sample does not establish a robust throughput gain.

The server logs report sampled two-second encode windows. Representative
post-start samples were 2.5--2.7 ms/frame at roughly 52--57 paced FPS, with
about 270 KB/frame and client decode around 15.0--17.5 ms. These are raw
server-log observations, not an aggregate and are retained only for context.

Raw client/server logs, status snapshots, analyzer output and orchestration script are in `raw/`. Source-offset values are software timing proxies, not physical motion-to-photon latency.
