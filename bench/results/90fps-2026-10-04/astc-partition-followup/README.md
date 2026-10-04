# Selective partition vs guarded selected dual-plane q6

**Decision: reject.** I reused the prior exact-photo one-seed queue candidate (two-partition CEM8, mode 0x053): it replaces qualifying projected-line baseline tiles only where its gate passes. I compared that completed output with the current selected q6 dual-plane policy output reconstructed from the guarded policy’s actual `.blocks.lz4` payloads (SPIR-V `b8447734…`, q4–q6 relative SSE gate 0.80). The candidate ASTC block payloads were independently decoded by the same Basis decoder; full frames use the same exact 1920×1080 source RGBA hashes.

| Scene | Region | Guarded selected dual-plane PSNR | Partition PSNR | Partition delta | Zstd-3 delta |
|---|---|---:|---:|---:|---:|
| Dark | Full | 30.8213 dB | 30.2961 dB | −0.5253 dB | +43 B |
| Dark | High-chroma pixels | 24.1620 dB | 23.4315 dB | −0.7305 dB | +43 B |
| Dark | Chroma edges | 19.1114 dB | 18.5526 dB | −0.5588 dB | +43 B |
| Forest | Full | 36.7674 dB | 36.6914 dB | −0.0761 dB | +51 B |
| Forest | High-chroma pixels | 34.3067 dB | 34.1438 dB | −0.1629 dB | +51 B |
| Forest | Chroma edges | 21.3148 dB | 21.2448 dB | −0.0700 dB | +51 B |

The partition queue emitted 82 dark and 17 forest mode-0x053 blocks. The guarded selected policy emitted 543 and 35 mode-0x442 blocks. The partition result loses across the full images and both diagnostic colour masks, and compressed size grows. It is not a bitrate-neutral improvement over the guarded selected policy.

Metric reconciliation: the earlier 30.9077/36.7838 dB figures used `dark-final-q6.astc` / `forest-final-q6.astc` from experimental SPIR-V `4b6f6be4…`. They are superseded, more permissive `.95` scratch outputs (944/58 dual-plane blocks), not the guarded `.80` selected-policy outputs compared above (543/35 blocks). I independently reconstructed the guarded output from its `.blocks.lz4`, externally decoded it, and reproduced the prior report’s 30.821332/36.767441 dB exactly. Both measurements use the same source hashes and the same full-frame RGB-only metric; alpha and ROI averaging are excluded. `reconciliation.csv` records the source, ASTC, raster, and SPIR-V hashes for both policy artifacts.

High-chroma pixels have RGB range ≥40; chroma-edge pixels have RGB range ≥24 plus a right/down adjacent max-channel step ≥20. Masks are diagnostic only. The prior partition q6 GPU measurement was 0.134 ms dark / 0.133 ms forest for baseline + finalize + candidate dispatch, versus 0.027 ms baseline-only in that harness. No timing comparison against the selected encoder is claimed because those measurements came from different harnesses.

Both outputs occupy 518,416 raw ASTC bytes. Zstd-3 sizes are for the complete ASTC files including the 16-byte header. `manifest.json` records source, block-payload, ASTC, and decoded-raster hashes. No full source images or payloads are included.
