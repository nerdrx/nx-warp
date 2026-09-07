# Initial live atlas comparison

This is an initial headset comparison of ordinary `atlas:off` and explicit
`atlas:auto`, using the same candidate APK and server. The measured medians were:

| mode | new-source/s | decoder GPU | display GPU |
| --- | ---: | ---: | ---: |
| off | 65.0 | 10.5 ms | 1.8 ms |
| auto | 52.5 | 12.8 ms | 1.5 ms |

The atlas run therefore regressed source cadence and decoder GPU time; keep atlas
off by default. These are client counters and GPU timings, not physical display
FPS. The comparison is limited to the captured scene with the headset mostly at
rest; the copied screenshots are scene evidence, not image-quality proof:
[off](../../../../docs/figures/240fps/live-atlas-off-screen.png),
[auto](../../../../docs/figures/240fps/live-atlas-auto-screen.png).

Filtered client and server logs, startup R8 proof, manifest, hashes, and the
reproducible summarizer are retained here. The separate copy-elision prototype
is not included in this comparison.

Recompute window statistics: `python3 summarize_live_atlas.py --out recomputed.json`.
The table reports medians of logged window means; these are not individual-frame
percentiles. New-source rates use printed, rounded window durations.
