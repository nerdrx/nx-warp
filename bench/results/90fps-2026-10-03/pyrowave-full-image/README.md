# Full-image PyroWave probe — 3 October 2026

![Measured rate and distortion](rate-distortion.png)

**Decision:** PyroWave is a convincing *local image-quality* winner against the current NXVC representation on these two frames. At approximately the same application payload, 4:4:4 gains **6.85 dB** on the forest and **5.53 dB** on the dark scene. The 500 Mbit/s setting preserves still more detail. This does **not** yet establish a live VR win: the Pico was disconnected, the eyes were identical, the scenes were static, and the NXVC comparison includes its current foveation. Do not replace the active streamer on this evidence alone.

The two supplied VRChat screenshots are valuable because one combines a detailed silhouette and dark foliage, while the other has fine bright patterns against dark objects. We used their existing private 2176 × 2176 per-eye source fixtures and duplicated each eye to form a 4352 × 2176 stereo frame. A second dark-scene check shifted the right eye by 8 pixels: its matched-rate 4:4:4 packet was 86,936 bytes with 28.74 dB mean stereo PSNR and 0.389/0.164 ms GPU encode/decode stage sums. This reduces one obvious duplicated-eye concern but is still a static synthetic stereo pair. Original screenshots and decoded crops are intentionally omitted from this public report.

| Scene | Representation | Bytes/stereo frame | 90 Hz payload | RGB PSNR | GPU encode | GPU decode |
|---|---|---:|---:|---:|---:|---:|
| Forest | Current NXVC | 66,851 | 48.1 Mbit/s | 28.09 dB | — | — |
| Forest | PyroWave 4:4:4, matched | 66,848 | 48.1 Mbit/s | **34.94 dB** | 0.382 ms | 0.157 ms |
| Forest | PyroWave 4:2:0, matched | 66,828 | 48.1 Mbit/s | 34.62 dB | 0.190 ms | 0.095 ms |
| Forest | PyroWave 4:4:4, 500 Mbit/s | 694,380 | 500.0 Mbit/s | 42.02 dB | 0.381 ms | 0.136 ms |
| Dark | Current NXVC | 86,944 | 62.6 Mbit/s | 23.18 dB | — | — |
| Dark | PyroWave 4:4:4, matched | 86,828 | 62.5 Mbit/s | **28.71 dB** | 0.371 ms | 0.179 ms |
| Dark | PyroWave 4:2:0, matched | 86,896 | 62.6 Mbit/s | 28.00 dB | 0.213 ms | 0.094 ms |
| Dark | PyroWave 4:4:4, 500 Mbit/s | 694,328 | 499.9 Mbit/s | 35.64 dB | 0.405 ms | 0.226 ms |

The [machine-readable data](results.csv) also contains 1 Gbit/s points. Bitrate is calculated as `frame_bytes × 8 × 90`; it is not a measured wireless throughput. NXVC bytes comprise the selected detail and safety payload from the [prior independent frame test](../../90fps-2026-09-29/mjpeg-vs-nxvc/nx-summary.json). PyroWave bytes exclude its 4-byte frame prefix and one-time 40-byte stream header. Neither side includes Wi-Fi packetization or FEC. NXVC timing in that earlier test is a CPU helper decode and is intentionally **not** compared with PyroWave's GPU timings.

The PyroWave figures are one-frame Vulkan timestamp sums from its CLI. Encode includes packing, resolve, analysis, quantization, and DWT; decode includes inverse DWT and dequantization. They omit transfers, CPU submission, shader compilation, transport, compositor work, and headset thermals. Thus 0.1–0.2 ms here cannot be translated into Pico motion-to-photon latency or sustained 90 Hz.

## What to borrow

PyroWave's [own design](https://github.com/Themaister/pyrowave) is intra-only Vulkan compute with a CDF 9/7 wavelet, simple parallel coefficient coding, exact per-frame byte control, 4:2:0 and 4:4:4 modes, and independent 64 × 64 coefficient blocks for local recovery. The notable fit for NXVC is **bounded GPU work for a full image**, with bitrate adjusted by quantization rather than by making the periphery permanently low-resolution. 4:2:0 roughly halves measured desktop shader time here, but 4:4:4 retains more color detail. Copying its *idea* of a cheap GPU-resident full-frame path is justified; importing it as the default decoder is not yet justified.

Next experiment: run a fixed full-image 4:2:0 and 4:4:4 path on the Pico at 90 Hz with real stereo motion, packet loss and bitrate steps, and record headset GPU p50/p95, received-frame age, thermals, and visible recovery. Preserve the present NXVC path as the control. If PyroWave makes the Pico miss the 11.11 ms display deadline or uses substantially more headset power, reject it despite the desktop PSNR win. Its MIT-licensed code may be used only with the license notice retained.

## Reproduction and scope

PyroWave was checked out at `89f7e47d4abbf650c91fae766728af866c5e32a0`, with Granite `1b2d1801d2910fb09ebcded2f0bb3a3a781103b5`, and built in an isolated `/tmp/pyrowave-bench` directory. `pyrowave-encode` and `pyrowave-decode` were built in Release mode with `PYROWAVE_DEVEL=ON`. Each source PNG was horizontally duplicated with FFmpeg, converted to full-range `yuv444p` or `yuv420p` Y4M, encoded with the target bytes/frame, decoded back to Y4M, and converted to RGB PNG for PSNR. The source fixtures are `scratch/jpeg-periphery/{forest,dark}-s0-source.png`; the current NXVC decoded references are matching `*-s0-nx.png`. Both are private local material and are not required to understand the published summary.

PSNR is RGB against the original screenshot after conversion back from YUV. Identical left/right duplication, still frames, the current foveated NXVC baseline, and slight packet accounting differences can all favor one candidate. This result tests today's representations, not whether wavelets universally outperform NXVC tools. No live server, WiVRn build, or Pico install was changed for this probe.
