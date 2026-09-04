# NX Warp

A video codec built only for VR streaming. Vulkan compute on both ends, independent 64x64 tiles,
pose-warped prediction, no IDR, no CPU on the hot path. Designed for [WiVRn NX](https://github.com/nerdrx/wivrn-nx).

Library and codec identifier: `nxvc`.

## Status

Design paper draft 1 is in [docs/PAPER.md](docs/PAPER.md). Phase 0 (the Adreno 650 compute gate,
paper section 3.4) is being built in `bench/`. The CPU reference codec in `ref/` is the normative
specification of the bitstream; the Vulkan encoder and decoder in `vk/` must match it bit for bit.

Nothing here is usable yet.

## Layout

| Dir | What |
|---|---|
| `docs/` | The design paper and the normative syntax document |
| `ref/` | Bit-exact CPU reference encoder/decoder (the spec) |
| `vk/` | Vulkan compute encoder and decoder, GLSL kernels |
| `bench/` | Phase 0 Android benchmark app (kernels K1 to K6) |
| `tools/` | Quality harness, conformance vector generator, fuzz drivers |
| `tests/` | Unit and conformance tests |

## Roadmap

| Phase | Deliverable | Gate |
|---|---|---|
| 0 | Adreno benchmark | K5 under 5 ms p50 at 2x2048^2, 90 Hz, with a dummy reprojection pass co-running |
| 1 | Intra-only codec | Bit-exact on lavapipe, RADV, Adreno; within 1 dB of x264 intra |
| 2 | Pose-warped inter | Within 10 percent of x265 zerolatency at rest, 30 percent better on head-motion frames |
| 3 | WiVRn NX integration | Glass-to-glass under 40 ms at 150 Mbit on WiFi 6 |
| 4 | Stereo, foveation, depth | 25 percent saving at equal FovVideoVDP on the fovea |

## License

Apache-2.0. The codec uses only public-domain and expired coding tools; see paper sections 1.9 and 6.12.
