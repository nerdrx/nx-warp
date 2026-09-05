# Compatibility

Which GPUs, operating systems and headsets NX Warp is expected to support, and at what level.

> **Every entry in this document says "expected".** Nothing has been measured on any device. Support
> levels are derived from the vendor capability table in paper 3.7 and from the compute budget
> estimates in paper 3.1 and 3.2.5. A device only moves from "expected" to "verified" when a Phase 0
> style benchmark has run on it and its numbers are in [PERFORMANCE.md](PERFORMANCE.md).

## Support levels

| Level | Meaning |
|---|---|
| **Full** | Pure compute decode, Catmull-Rom prediction filter, per-tile 4:4:4, the full latency path |
| **Lite** | Pure compute decode with the reduced tool set: bilinear filter, 4:2:0, no stereo inter-view, optionally the bit-plane entropy coder |
| **Hybrid only** | Hardware HEVC or H.264 base through the platform decoder, compute enhancement layers. Compatibility mode, not the latency path ([ADR-0014](adr/0014-layered-bitstream-hybrid-mode.md)) |
| **Encoder only** | Runs the PC encoder; not a decode target |
| **Unsupported** | Cannot run the pure compute decoder; listed so the reason is on record |

## Hard requirements for the pure compute path

From paper 3.2.6 and 3.7:

1. Vulkan 1.1 compute.
2. **Subgroup size of at least 8**, with subgroup ballot. The rANS layout uses 8-lane clusters and
   derives each renormalising lane's read offset from `subgroupBallot` plus an exclusive bit count.
   8 divides 8, 32, 64 and 128, so clusters never straddle a subgroup on any supported width.
3. 16-bit integer storage (`GL_EXT_shader_explicit_arithmetic_types_int16`).
4. At least 16 KB of shared memory usable per workgroup (Pass A needs 8 KB of symbol tables, Pass B
   8 KB of transpose buffer).
5. Enough GPU time. This is the real gate and it is the one nobody can predict from a spec sheet.

Not required, deliberately: `shaderInt64` (64-bit products come from `OpUMulExtended`, core SPIR-V),
floating point of any precision on the normative path, integer division, and `robustBufferAccess`
semantics. See [ADR-0010](adr/0010-integer-only-normative-path-cpu-reference-is-the-spec.md).

## GPU matrix

From paper 3.7. Everything here is expected, from vendor capability documentation, not measured.

| Target | Subgroup size | Ballot | int64 | int16 storage | Expected level | Notes |
|---|---|---|---|---|---|---|
| AMD GCN4 (RX 580), RADV and Windows | 64 | yes | yes | yes | Encoder only (reference PC encoder platform) | The encoder's long pole; 2.5 to 4 ms per frame estimated |
| AMD RDNA (7900 XTX) | 32 or 64, driver chooses | yes | yes | yes | Encoder only; Full as a PC-class client | Never assume which subgroup width |
| NVIDIA | 32 | yes | yes | yes | Encoder only; Full as a PC-class client | |
| Intel ANV | 8, 16 or 32 per shader | yes | yes | yes | Full (PC-class client) | Force 32 with subgroup size control; clusters of 8 work at any width |
| Adreno 6xx (Pico 4, XR2 Gen 1) | 64 | yes | unreliable | yes | **Hybrid only expected**, pending Phase 0 | Proprietary compiler; avoid clustered ops. See below, and [ADRENO-RULES.md](ADRENO-RULES.md) for the four shader rules its driver forces on every kernel |
| Adreno 7xx (XR2 Gen 2 class) | 128 | yes | unreliable | yes | Full expected | About 2.5x the GPU of Gen 1; the paper expects pure compute to become the default here |
| Mali Valhall | 16 | yes | no | yes | Hybrid only, unless a Phase 0 style bench passes on the device | |
| Mali Bifrost | 4 to 8 | partial | no | yes | Unsupported for pure compute; hybrid | Subgroup below 8 breaks the 8-lane cluster rule |
| Apple through MoltenVK | 32 | yes | no | yes | Unknown | Metal has no 64-bit integer, which the normative path does not need; sampler behaviour differs, which the normative path does not use. No client exists |
| lavapipe (CPU) | 8 | yes | yes | yes | Full, for conformance only | Subgroup width 8 is exactly why the cluster size is 8. A CI target, not a real-time decoder |
| SwiftShader (CPU) | varies | yes | yes | yes | Conformance only | CI target alongside lavapipe |

## Headset matrix

| Device | SoC / GPU | Expected level | Basis |
|---|---|---|---|
| **Pico 4** | Snapdragon XR2 Gen 1, Adreno 650 | **Hybrid only expected**; pure compute pending Phase 0 | The first target. Estimated 4 to 6 ms p50 decode against a 5 ms budget on a GPU already spending 2 to 3 ms per vsync. The paper's own verdict (6.10) expects hybrid; Phase 0 (K5) decides with numbers ([ADR-0015](adr/0015-compute-budget-verdict-pending-phase-0.md)) |
| Quest 2 | XR2 Gen 1, Adreno 650 | Same expectation as Pico 4 | Same GPU family. Not independently analysed in the paper |
| Quest 3 / Quest Pro class | XR2 Gen 2, Adreno 7xx class | Full expected | Paper 5.8 puts XR2 Gen 2 at about 2.5x the GPU of Gen 1, which is the generation the paper expects pure compute to be the default on |
| Pico 4 Enterprise / eye-tracked devices | XR2 Gen 1 plus eye tracking | As Pico 4, plus eye-tracked foveation | Eye tracking changes the bit budget (estimated 0.18 of full-resolution samples against 0.50 fixed), not the decode level (paper 5.1.2, 5.1.3) |
| PSVR2 | not a target | Unsupported | Wired DisplayPort, no compression, closed platform. Present in the paper only as the latency bar (paper 5.6) |
| Apple Vision Pro | not a target | Unsupported | Closed platform, no Vulkan client path |
| Mali-based standalone headsets | Valhall or Bifrost | Hybrid only, or unsupported | See the GPU matrix |

Anything not listed is unknown, and this project will not guess. The way a device gets a row here is
that somebody runs the Phase 0 app on it.

## Operating systems

| Platform | Role | Expected support | Notes |
|---|---|---|---|
| Linux (Monado / WiVRn NX server) | Encoder | Primary target | The compositor's render target is a `VkImage` on the same device, so there is no external memory: one image barrier, and a queue family ownership transfer if the encoder uses a dedicated compute queue (paper 3.6). Note that the encoder input is already YCbCr 4:2:0 and already foveated by the compositor, which is what `YCBCR_PASSTHROUGH` exists for ([ADR-0021](adr/0021-stream-level-color-space-ycbcr-passthrough.md)) |
| Windows (SteamVR) | Encoder | Supported by design; `platform/win/` | Per-eye `ID3D11Texture2D` is copied into a shared texture and imported through `VK_KHR_external_memory_win32`; a D3D11.4 shared fence imported as a timeline semaphore puts the copy and the encode on one timeline, with `VK_KHR_win32_keyed_mutex` as the fallback. Intel needs the fence path (paper 3.8) |
| Android (NDK, OpenXR client) | Decoder | Primary target | Vulkan 1.1 compute; hybrid path uses MediaCodec plus `AHardwareBuffer` import with no copy (paper 3.5) |
| macOS | neither | Unsupported | No target, no client |

## Codec feature support by profile

| Feature | Lite | Full | Pro | Hybrid |
|---|---|---|---|---|
| Tile size | 64x64 | 64x64 | 64x64 | 64x64 |
| Prediction filter | bilinear | Catmull-Rom | Catmull-Rom | as the profile it runs on |
| Entropy coder | rANS, or `ENT_BITPLANE` | rANS, 8 lanes | rANS, 8 lanes | same |
| Chroma | 4:2:0 | 4:2:0 or 4:4:4 per tile | 4:4:4 fovea | as the source allows |
| Bit depth | 8 wire, 10 internal | 8 or 10 | 10 | as the base allows |
| Alpha plane | yes | yes | yes | enhancement layers only |
| Lossless tiles | no | no | yes, up to 4 fragments | no |
| Stereo inter-view (`STEREO`) | off | Phase 4 | Phase 4 | off |
| `res_level` foveation | yes | yes | yes | yes |
| Per-tile references, no IDR | yes | yes | yes | enhancement layers only; the base uses HEVC's own reference invalidation |
| Row-band pipelining | yes | yes | yes | enhancement layers only; MediaCodec needs whole access units |

## Colour space support

| `color_space` | Source | Reference storage | Lossless | Where it is expected to be used |
|---|---|---|---|---|
| `YCOCG_R` | RGB render targets | display format (RGBA8, RGB10A2) | yes | Windows/SteamVR capture, any RGB source, conformance vectors |
| `YCBCR_PASSTHROUGH` | already YCbCr 4:2:0 | the source's own 4:2:0 layout | no, near-lossless only | The WiVRn NX Linux path, where the encoder input is already 4:2:0 and the Android decoder outputs 2-plane 4:2:0 |

See [ADR-0021](adr/0021-stream-level-color-space-ycbcr-passthrough.md).

## Capability negotiation

Support is negotiated, not assumed, and the mechanism is an intersection rather than a version number
(paper 1.2):

1. The client sends its own `tools` mask, `profile` and `level` in the connect handshake.
2. The server may only set bits the client offered.
3. A decoder that encounters an unknown **mandatory** tool bit refuses the stream instead of guessing.
4. Tool bits 0 to 31 are defined in v1; bits 32 to 63 are reserved and must be zero.

Unknown TLVs in the stream header's extension area are skipped, so additive extensions do not break
older decoders.

This is the Vulkan feature-bit model. It means a new device does not need a code change to be
supported at a reduced level, and it means a device that cannot do something says so rather than
producing wrong output.

## What would change these levels

- **Phase 0 results on the Pico 4.** The single biggest unknown. K5 under 5.0 ms p50 moves the Pico 4
  from "hybrid only expected" to Full.
- **Subgroup ballot cost on Adreno.** If ballot is unavailable or slow, the `ENT_OFFSET_TABLE`
  fallback costs about 8 extra bytes per tile but keeps the device supported (paper 1.12, 3.12).
- **Thermal behaviour over a session.** A device that passes cold and fails after ten minutes is a
  hybrid device in practice. Phase 0 runs for ten minutes for exactly this reason.
- **MediaCodec low-latency behaviour on Pico firmware**, which determines whether the hybrid path is
  usable at all on the first target device (paper 3.4, K6).

## References

- Paper 3.7 (vendor differences and bit-exactness rules), 3.2.6 (subgroup portability rules),
  3.4 (Phase 0 gate), 3.5 (hybrid implementation), 3.8 (Windows), 1.2 (capability negotiation),
  5.8 (hardware trends), 6.10 (compute budget verdict)
- [PERFORMANCE.md](PERFORMANCE.md), [ARCHITECTURE.md](ARCHITECTURE.md#9-profiles-and-capability-negotiation)
