# Same-device stereo ASTC GPU/CPU overlap probe

This scratch harness extends the one-eye Vulkan probe to two independent native RGBA images on the **same VkDevice and compute queue**. It uses the current production `server/shaders/astc_encode.comp` verbatim (the copied `shaders/encode_primary.comp` compares byte-for-byte), fit 3, quality 6, ASTC 8x8, direct RGB. Inputs are the local 2176x2176 dark/forest RGBA fixtures (18,939,904 bytes each); they remain at their original private paths and were not copied into this scratch directory.

Each eye owns an image/view, staging buffer, ASTC output and readback buffers, descriptor set, command buffer, fence, and query pool. The shader pipeline, device, queue, and command pool are shared. Uploads happen once before warmup and are excluded. Each measured call records both eyes and queues both command buffers before it waits on either readback fence. CPU modes then process the readbacks and q6 packet selection either serially, or with `std::async` waiting/reading/compressing the right eye while the caller does the left; the future is joined before the next call.

There are 20 warmup pairs and 100 measured pairs. Serial/async execution order follows repeating ABBA groups; CSV `order` is the actual within-pair execution index. `wall_ms` includes command recording, both queue-submit calls, fence waits, query retrieval, host readback copy, per-eye compression, and NX packet assembly. GPU timestamp pairs report dispatch duration and dispatch-through-readback-command duration per eye. CPU fence waits are shown individually; in async mode those waits overlap, so their sum is not a wall-time measure. Zstd level 3 is preferred; LZ4 is attempted only if Zstd fails or exceeds half the raw size; Zstd is selected only when it is at least 10% smaller than the raw/LZ4 fallback.

## Results

Device: AMD Radeon RX 7900 XTX (RADV NAVI31), one device and one queue. Upload was excluded. Median / p95 milliseconds over 100 measured pairs:

| Measurement | Serial | Async right + caller left |
|---|---:|---:|
| Complete two-eye call | 13.865 / 15.432 | 11.364 / 13.007 |
| Command recording | 0.0069 / 0.0178 | 0.0069 / 0.0173 |
| Two queue-submit API calls | 0.0203 / 0.0473 | 0.0197 / 0.0468 |
| Eye 0 fence wait | 9.457 / 11.067 | 8.681 / 10.338 |
| Eye 1 fence wait | 0.0022 / 0.0062 | 9.077 / 10.737 |
| Async thread launch | — | 0.0201 / 0.0454 |

The interleaved async calls were 2.501 ms faster at p50 (18.0%) and 2.425 ms faster at p95 (15.7%) in this harness. GPU time itself stayed the same: eye 0 dispatch 0.506 / 0.519 ms and dispatch-through-readback command 0.576 / 0.590 ms; eye 1 dispatch 0.329 / 0.341 ms and dispatch-through-readback 0.399 / 0.413 ms. This indicates CPU-side overlap, not a faster GPU dispatch. Fence wait p50/p95 for eye 0/1 were 9.46/11.07 and 0.002/0.006 ms serial, versus 8.68/10.34 and 9.08/10.74 ms async; async waits occur concurrently.

Each ASTC stream produced 1,183,744 raw block bytes. Packets were byte-identical between serial and async and passed the harness's decompression-to-ASTC check. Both selected NX packet encodings were Zstd (codec id 2), with 414,921 and 257,982 bytes including the 24-byte header. The emitted standard ASTC files were independently decoded by `astcenc` as 2176x2176 ASTC 8x8; decoded RGB MAE/PSNR against source was 2.293 / 33.861 dB for dark and 1.405 / 41.320 dB for forest.

This is a controlled same-device/queue A/B timing of the headless harness path, under whatever host/GPU load existed during this single run. It is not a same-GPU-loaded control study and does not establish live compositor, network, headset, or Pico timing. No server process or GUI was involved.

Published per-sample timings are in `build/results/stereo.csv`. Raw ASTC, packets, RGBA inputs and decoded photo files remain private and are not bundled. To reproduce:

```sh
glslc -O -I shaders shaders/encode_primary.comp -o build/encode_primary.spv
cmake -S . -B build
cmake --build build -j4
mkdir -p build/results
./build/stereo-gpu /path/to/dark-left.rgba /path/to/forest-left.rgba build/results/
```

![Same-device full-call and unchanged GPU durations](stereo-gpu.png)
