# Auto bitrate photo-tail paired run

This report compares the same `crowd-auto500` photo workload with `tail0` and `tail64`. `tail64` adds 64 discarded packets after each main frame. It is a controller and pipeline experiment, not a link-capacity or image-quality proof.

| case | encoder FPS | new-source FPS | app render-loop FPS | encode ms | own GPU ms | predicted runtime ms | holes |
|---|---:|---:|---:|---:|---:|---:|---:|
| crowd / tail0 | 74.63 | 70.53 | 89.43 | 1.50 | 1.76 | 46.26 | 0 |
| crowd / tail64 | 89.91 | 84.77 | 89.43 | 4.55 | 3.95 | 41.68 | 0 |

The controller quality-budget target trajectory is the useful comparison: tail0 backed off from 500 to 12.6 Mbit/s across five logged decisions. Tail64 produced 45 order-indexed decisions and stayed between 374.4 and 500 Mbit/s. These are encoder control values derived by the automatic controller; they are not measured link capacity. Server logs have no timestamps, so the graph's x-axis is event order, not seconds.

The higher tail64 new-source rate comes with a different controller trajectory and higher encode cost. It does not establish that the tail improves quality at a fixed budget. App render-loop FPS stayed about 89.43 in both runs, and network hole totals were zero in the retained client windows.

All aggregates are means of independent 2-second window means. The server series drops its first five complete windows; the client series starts at upload marker +10 seconds. `p95` uses nearest-rank `ceil(0.95*n)-1`. `predicted runtime` is the approximate same-client-clock sum of wire, queue, decode, decode-to-selection, and selection-to-predicted; it is not photon or physical end-to-end latency.

`windows.json` contains sanitized numeric windows, controller transitions, and log-line citations. `summary.json` contains aggregates. `comparison.png` shows the quality-budget trajectory and client new-source FPS. `rebuild_report.py` regenerates both from `windows.json`. No photos, previews, absolute source paths, or production changes are included.
