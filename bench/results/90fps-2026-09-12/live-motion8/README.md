# Live 8px motion-grid candidate

**Built and ready to install; not yet verified live on the Pico.** ADB had no connected headset during preparation. Source: WiVRn [`163d0b84`](https://github.com/nerdrx/wivrn-nx/commit/163d0b8457331fbe0e4d0a1690641e1c224cb1ff).

## What changed

- The live server can use 8, 16, 32 or 64px motion cells through `WIVRN_NX_MOTION_BLOCK_PX`. The prepared profile selects **8px**. The compiled fallback remains 64px.
- At the user's 100% resolution, **2176×2176 per eye → 272×272 motion vectors per eye**, instead of 34×34. The 8px research comparisons were previously offline, not deployed live.
- Sender and receiver accept bounded grids up to 512×512. Entire stereo fields remain bounded to 1 MiB; each decoded chunk remains bounded to 1120 bytes. Oversized grids are explicitly rejected.
- Two 272-cell rows now share one packet: **272 packets per stereo field**, down from 544 with the old payload budget.
- Repeated XY vectors use lossless per-chunk run-length encoding; noisy chunks retain raw bytes. Bounds, exact decoded length and encoding are validated before changing assembly state. Raw chunks require no extra decode copy.
- The presentation path retains an immutable field snapshot and releases the network lock before runtime waits and rendering. Field reception no longer needs to wait for that entire pass.
- A perfect stationary patch skips expensive motion search. The shortcut abandons its check at the first unequal sample, limiting overhead when content changes.
- The headset Transport page displays **Received motion grid**, so received density can be checked rather than inferred from an offline demonstration.

## Full-size transport exercise

A 512² Blender scene was upscaled to 2176² for an allocation/transport exercise; this does not supply native 2176² detail. The actual Vulkan estimator produced a 272² stereo field, which was quantized and processed by the production splitter and assembler.

| This single full-size field | Result |
|---|---:|
| Raw vector payload | 295,936 bytes |
| Raw serialized/encrypted-counter budget | 311,440 bytes |
| Compressed serialized/encrypted-counter budget | **126,758 bytes** |
| Reduction | **59.3%** |
| Chunks using RLE | 256 / 272 |
| Exact host round trips | 130 / 130 |
| Host encode p50 / p95 | 125.92 / 133.38 µs |
| Host assemble p50 / p95 | 69.68 / 74.00 µs |

The byte budget includes packet type, serialization and 8-byte UDP encryption counter, but excludes IP/UDP headers. Repeating this one field at 60 Hz would imply roughly **60.8 Mbit/s** of motion traffic rather than **149.5 Mbit/s** raw. This is a calculation, not measured Wi-Fi throughput. Both the ratio and CPU times are content/device dependent; the CPU results are from the host, not the Pico. RLE does not reduce packet count in this version.

## Exact stationary shortcut

On the RX 7900 XTX, 2176² per eye, experimental 8px grid, 30 warmups + 100 measured dispatches:

| Input | Original median | Shortcut median |
|---|---:|---:|
| Static synthetic | 3.098 ms | 0.489 ms |
| Mostly flat translated synthetic | 3.102 ms | 0.555 ms |
| Real-image pair | 3.176 ms | 3.163 ms |
| Unrelated random images | 3.189 ms | 3.194 ms |

These are GPU timestamp measurements of the estimator dispatch and its barrier only. They exclude downsampling, encoding, networking, headset decoding and presentation. Single short paired runs are preliminary: the real-image difference is too small to claim a gain. The synthetic pattern occupies only part of the large image, explaining its many stationary cells. This optimization is valuable when exact stationary patches exist; it does not make all motion workloads cheap.

Both compared shaders used `glslc -O`. Timestamp period is 10 ns with 64 valid timestamp bits. The shortcut produced **57/57 byte-identical fields and 57/57 byte-identical reconstructions** on the separate 512² stop/reversal fixture with Vulkan validation enabled. Native search tests passed 3,161 checks.

## Candidate settings and remaining check

HEVC 10-bit image stream; 100% resolution; 60 Hz source cap; 90 Hz headset mode; 8px field; blur off; four retained sources; conservative pose checks and guarded EMA retained. The new direction-reset research variant is not enabled.

Android release and server builds passed. Packet tests passed **2,284 checks** under AddressSanitizer and UndefinedBehaviorSanitizer, including dense serialized round trips, malformed encodings, truncated/oversized runs, raw fallback, packet loss and bad dimensions. Binary hashes are in `status.json`.

The required next check is on the Pico: installation of the matched client/server, a received 272×272 grid, complete-field arrival rate under head motion, render/fresh-frame pacing and visual jitter. The denser field increases work and traffic; no 90 FPS or latency improvement is claimed before that test.

Local prepared installer: `/run/media/nerdrx/Lex/claude/nx-scratch/motion-regions/pico-motion8/install_and_start.py`. It checks that one Pico is connected, installs the matching APK, then launches the 8px profile. `--grid 64` is the protocol-compatible conservative rollback (also disables EMA); `--check` changes nothing. This archive is a machine-specific script snapshot, not a downloadable release bundle. Do not combine an older APK with the new wire format.

[Stop/reversal animations and quality comparison](../motion-reversal/README.md)
