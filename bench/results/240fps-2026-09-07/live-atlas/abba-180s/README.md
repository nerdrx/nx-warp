# 180 s ABBA live comparison

**Dated caveat (2026-09-07): later review found the output-scale viewport bug. These timing results remain genuine, but both reduced-output and AUTO 0.50 captures used cropped/zoomed fields rather than equivalent full-field rendering. Corrected measurements are pending.**

This matched ABBA sequence alternated the existing control APK
`0582f10bd6a4c45da161981381122ad539b4deab212a1fdb1595ce8db23e8e99` with the
combined handoff APK `c55a2fa1e49069a541bdd44f42c9a46f7576455cb800a12c986955321ecebc91`.
All arms used server `a7b47b3e164eaa1578774526598911546b00ae83b55ba7a33f874e2504040d82`.

| arm | renderer GPU ms | copy ms | decoder wall ms | decoder GPU ms | active sources/s | reported wall-span/s |
|---|---:|---:|---:|---:|---:|---:|
| control 1 | 2.5 | .29 | 2.3 | 1.2 | 88.5 | 62.0593 |
| combined 1 | 1.6 | .01 | 1.9 | 1.3 | 88.5 | 67.1447 |
| combined 2 | 1.6 | .01 | 2.0 | 1.3 | 87.5 | 69.1805 |
| control 2 | 2.3 | .29 | 2.3 | 1.3 | 84.75 | 64.0819 |

Values are canonical active-window summaries; cadence varies and report gaps
remain, so this establishes no physical FPS, causal speedup, or thermal
benefit. Repeated stage savings motivate a forthcoming PICO4-only speed
preset, while the default configuration remains unchanged. Raw capture files,
canonical parser outputs, screenshots, and checksums are retained here.
