# Live-check and build provenance

`paired/` contains a fresh, short stationary wake capture: four wakeups at three-second intervals, five 2-second client render windows, and bounded server encoder excerpts. Server U_LOG summaries have no wall-clock timestamps, so packet means are reported separately and are not aligned with individual app windows. Full logs and raw photos are not copied.

`production-build-metadata.json` records the root-reported pushed PC source, running server binary and SPIR-V hashes, pinned Vulkan header hashes, shader source hashes, process IDs, and Vulkan 1.3 SPIR-V validation. The binary hash matches the running server process.

The headset was stationary and asleep off-head between wakeups. These checks do not establish motion performance, complex-scene 90 Hz, photon latency, or perceived image quality.
