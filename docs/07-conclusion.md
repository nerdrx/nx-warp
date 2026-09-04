# 7. Conclusion and roadmap

## 7.1 What the paper claims

- A codec whose only structure is the independent 64x64 tile can run every stage as one GPU
  workgroup per tile, on both the PC and the headset, with no CPU in the hot path except datagram
  decryption.
- Pose-warped prediction turns most of a VR frame into skip tiles during head motion, which is
  exactly when hardware codecs spend the most bits and add the most latency. At rest the paper expects
  parity with HEVC, not victory, and says so.
- Per-tile reference tracking with a deterministic concealment warp removes the IDR entirely.
  Loss costs bits in the affected tiles for one round trip and nothing else.
- Row-band pipelining and deadline presentation cut the render-to-photon floor to 12 to 23 ms
  against about 100 ms measured today, with the compositor phase wait, not the network, as the
  remaining long pole.
- One bitstream serves a hybrid hardware-plus-compute decoder on today's Pico 4 and a pure compute
  decoder on stronger hardware, selected by capability bits and by a decode-time governor.

## 7.2 What it does not claim

It does not claim a compression win over HEVC on static scenes, fast object motion or mirrors. It does
not claim the Pico 4 can run the pure compute path at 90 Hz. It does not claim a gigabit over WiFi in
the first release. Each of those is a measured number in the roadmap, not an assumption.

## 7.3 Roadmap

| Phase | Weeks | Deliverable | Exit criteria |
|---|---|---|---|
| 0 | 3 | Adreno benchmark app, capability probe | K1 to K6 measured on Pico 4, pure or hybrid decision recorded |
| 1 | 8 | Intra-only codec, reference decoder, conformance, fuzzing, quality harness | Bit-exact on lavapipe, RADV and Adreno; within 1 dB of x264 intra; Pass B under 5 ms p50; 24 h fuzz clean |
| 2 | 10 | Pose-warped inter, skip, per-tile reference tracking | Cross-vendor determinism green; within 10 percent of x265 zerolatency at rest, 30 percent better on motion frames; no drift under 5 percent loss |
| 3 | 6 | WiVRn NX integration, hybrid mode, telemetry, governor | Glass-to-glass under 40 ms at 150 Mbit on WiFi 6; one hour on Pico 4 without a crash; FTO review done |
| 4 | open | Stereo, foveated tiles, 4:4:4 fovea, depth stream | 25 percent saving at equal FovVideoVDP on the fovea; decode time unchanged |

## 7.4 The first thing to build

The Phase 0 benchmark in Section 3.4. It is about a week of work, it reuses real kernels, and every
later decision in this paper hangs on its table. Nothing else should be written until that table
exists.
