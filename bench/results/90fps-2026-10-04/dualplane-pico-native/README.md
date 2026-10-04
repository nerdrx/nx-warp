# Native Pico ASTC mode 0x442 proof

**Scope:** format/sampler correctness and an offscreen GPU timing at 2176×2176, single eye. The input is a synthetic repeated-block image: 272 distinct q6 dark-photo ASTC blocks repeated across 272 rows. Baseline contains 73,984 ordinary 5×5 one-plane blocks; stress fixture contains 73,984 legal ASTC mode 0x442 dual-plane blocks. This is not natural-image quality, native-pixel quality, compositor, motion, or live-VR proof. No full photos are included.

The harness ran A/B/A with 12 warmups and 30 timed samples per run on Pico Adreno 650 Vulkan 1.1. It checked device readback against the external ASTC decoder: all runs exited 0; max channel error 1; zero pixels exceeded tolerance 2. Thermal HAL status was 0 before/after; GPU temperature reported 35.6°C before and 36.8°C after (HAL current values).

GPU total median (upload + sample): baseline A 2065.26 µs, all-dual 1844.74 µs, baseline B 1855.10 µs. Sample-only medians: 1899.32, 1718.75, 1719.53 µs. Baseline A/B drift is substantial relative to the apparent difference, so do not claim a speedup; the evidence establishes legal decoding and comparable sampler cost at this synthetic size.

A first harness attempt was stopped before Vulkan initialization because the cloned harness still referenced the prior test directory; the clone-only path was corrected and A/B/A rerun. No APK, device configuration, or production files were changed.

Artifacts: `summary.csv` (per-run errors and medians), `samples-astc.csv` (all 90 timed rows), `gpu-medians.svg`, `fixture-manifest.json`, `run-manifest.json`, and thermal snapshots. Hashes bind fixture ASTC and decoded references.
