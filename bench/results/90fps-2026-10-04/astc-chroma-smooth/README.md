# ASTC q6 chroma-smoothing preview

> **REJECTED LIVE TRIAL — optional chroma smoothing is OFF in the restored fixed ASTC 8×8 baseline.** The live build that included this option was reverted with commits `bf547738`, `af913d4e`, and `25e008cb` by `wivrn-nx` commit `d95fbe99` after the user reported “stuttery low quality ass mess.” This CPU preview remains an offline math/appearance study; it is not evidence that smoothing improved the live experience or caused the reported failure.

CPU-only preview of the rejected trial’s `peripheral_smooth == 6` math from `client/shaders/reprojection.glsl`, applied to the exact guarded `.80` q6 decoded RGBA fixtures. It uses two diagonal samples at ±3 texels and weights the result as centre 0.5, each neighbor 0.25. It subtracts the BT.709 encoded-RGB luma component from the chroma delta, then scales the delta to fit the channel gamut. Alpha stays unchanged.

The 512×512 crops come from the `astc-rgb-input-quality` manifest ROIs. Inputs hash-match the guarded q6 rasters in `astc-partition-followup/manifest.json`. Only the crops are saved here; source photos and APKs remain outside this report. `preview.py` regenerates both panels and `metrics.json` with Python, NumPy, and Pillow.

| Scene crop | Before / after |
|---|---|
| Dark, purple hair | ![q6 decoded purple-hair crop before and after CPU smoothing](dark-purple-hair-512-before-after.png) |
| Forest, pale hair and dark edges | ![q6 decoded pale-hair crop before and after CPU smoothing](forest-white-hair-512-before-after.png) |

Semantic checks in `metrics.json` show near-zero floating-point luma drift, no alpha changes, and in-range UNORM8 output. Float gamut scaling has only sub-1e-6 numerical residue. Quantization can move luma by at most 0.5 code value. These are math and appearance previews, not PSNR wins, GPU readback, device validation, or performance measurements.

ASTC 8×8 always carries 128 bits per block. The quality rung selects how those bits represent the tile; it does not pad output to consume its bitrate target. A later live server snapshot reported about 157 Mbit/s pair payload against about 343 Mbit/s pair encoder budget. Treat those as that snapshot's samples, not a sustained ceiling.

The rejected-live client capture later showed 179, 179, and 147 render iterations across the final three two-second windows, with only 15, 10, and 4 fresh source selections. The corresponding last server window was fixed ASTC 8×8 q6 on both eyes, with Zstd on all frames and no expansion drops. This records severe source-delivery degradation but does not isolate smoothing as its cause; rollback does not establish that the root delivery problem is fixed. No photon-latency claim is made. See the rejected-live evidence in the sibling `adaptive-astc-blocks/README.md`.
