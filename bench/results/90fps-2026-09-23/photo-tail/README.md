# Photo-tail paired workload

Six paired runs compare `tail0` with `tail64`. Both use the same static photo with the same ±8 px shifted paired view; `tail64` additionally sends 64 discarded 16-byte packets after each main frame. This is a controlled workload for pipeline behavior; it is not a user-scene or perceptual quality result.

All values are means of independent 2-second window means. Encoder and client windows are not index-aligned. `p95` uses nearest-rank `ceil(0.95*n)-1`. Server data discards its first five complete windows and only a final window shorter than 1.9 s. Client data starts at upload-marker +10 s and retains complete 2-second render windows. Network counters are an independent timestamp-filtered series.

| case | encoder FPS | new-source FPS | app render-loop FPS | payload Mbps | wire ms | own GPU ms | predicted runtime ms | holes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| crowd / tail0 | 90.03 | 87.68 | 89.44 | 103.48 | 10.70 | 4.20 | 49.32 | 0 |
| crowd / tail64 | 89.98 | 82.50 | 89.45 | 103.43 | 3.31 | 4.20 | 40.94 | 0 |
| dark / tail0 | 89.98 | 88.75 | 89.45 | 82.47 | 10.74 | 4.20 | 49.86 | 0 |
| dark / tail64 | 90.03 | 88.45 | 89.45 | 82.52 | 2.75 | 4.20 | 41.23 | 0 |
| forest / tail0 | 90.02 | 88.53 | 89.44 | 52.85 | 10.73 | 4.20 | 47.02 | 0 |
| forest / tail64 | 90.02 | 88.80 | 89.46 | 52.85 | 1.74 | 4.20 | 37.55 | 0 |

The crowd pair shows a tradeoff: new-source FPS falls from 87.68 to 82.50 with tail64 while the app render loop stays about 89.45 FPS. The tail result is therefore not an unqualified quality or latency win.

`predicted runtime` is the approximate same-client-clock sum `wire + queue + decode + decode-to-selection + selection-to-predicted`. It excludes the negative `source->first` field, which uses future predicted-pose timing, and is not physical end-to-end or photon latency. The own-GPU figure is the app's pass only; it is separate from the runtime estimate.

Payload arithmetic was independently checked as `frames × bytes/frame × 8 ÷ (seconds × 1e6)`. The raw retained window values and sanitized source line citations are in `windows.json`; aggregate values and definitions are in `summary.json`. `comparison.png` labels the main stage values and runtime estimate. `rebuild_report.py` regenerates `summary.json` and `comparison.png` from `windows.json`. No photos, private previews, absolute source paths, or production changes are included.
