# Native ASTC handoff — 4 October 2026

The detailed current handoff is [dense-colour/NEXT.md](dense-colour/NEXT.md).

- Latest WiVRn source: `86e0d678`, pushed on `pyrowave-probe`. Host server/runtime and Android builds passed together.
- Final signed APK is installed without changing app data. SHA-256: `4073507c97bdf73ca06942d0cf751c08ac7825af592397d3e7b8f00929acc120`.
- Active research profile: native 2176 × 2176 per eye, 90 Hz, ASTC 8×8, selective dual-plane colour fit, same-queue asynchronous upload. No foveation, JPEG, blur or object-motion field.
- Native pacing now starts at a 2 ms cap, probes upward by at most 1 ms, and stays below half the predicted refresh period. Short stationary cold/warm checks and final-build producer-pause check logged zero scheduler-attributed misses. Startup, off-head runtime state, fresh content rate and physical display rate remain separate.
- Optional direct RGB input passed correctness and native smoke checks; default remains off. Direct mapped-memory decompression and unproven partition shortcuts were rejected. Zstd level 3 retained.
- No sustained motion, complex-scene 90 Hz, perceived Pico quality, physical photon latency or 240 Hz proof is claimed.

Deadline: 09:00 Europe/Berlin / 07:00 UTC. Stop owned test processes at that time, preserve the final package and pause the heartbeat.
