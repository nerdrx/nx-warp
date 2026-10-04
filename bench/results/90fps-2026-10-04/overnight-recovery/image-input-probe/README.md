# ASTC thumbnail safety encoder probe

Scratch-only Vulkan compute harness and output container check. No production source was edited. `gpu/shaders/encode_primary.comp` is an exact copy of the server encoder shader; `gpu/shaders/encode.comp` changes only the direct-RGB pixel read to normalized output-grid UVs with linear clamp sampling. The 192²/256² thumbnail is sampled from the native-size 2176² image inside the encode dispatch. No intermediate downscale image or pass is used.

Inputs are the native 2176×2176 `dark-left.rgba` and `forest-left.rgba` from `/tmp/nx-astc-quality-test`. Each raw file byte-matches the corresponding PNG converted to RGBA (18,939,904 bytes; SHA-256 dark `9d1aa653c578751e6491d8c7786183b283de190d1928dc172959387e0da1355e`, forest `8dbe4a025512bd0cff3dfd38f632ee7c487b6430eb5090ddba8c6ca2705e5f5f`).

The run uses fit 3, quality 6, 12 warmups and two 20-sample legs per condition. Per scene the order is primary2176, safety192, safety256, safety256, safety192, primary2176 (ABCCBA), yielding 40 measured samples per condition. Each call uploads the native input once before warmup; upload, pipeline creation, and process setup are excluded. `gpu_dispatch_ms` and `gpu_dispatch_readback_ms` are Vulkan timestamps. CPU stages separately report command recording, queue-submit API, post-submit fence wait, query retrieval, mapped-buffer invalidation/copy, LZ4, Zstd level 3, and total through both compression attempts. The output includes valid 16-byte ASTC headers plus ASTC blocks, `.lz4`, `.zst`, and the production-policy `.selected` payload. Quality is restricted to 0–6.

The external `astcenc` CLI was built from the already-present astc-encoder source into `tools/astcenc-build`. `gpu/decode_outputs.sh` decompresses each native-primary and thumbnail output. Decodes have the expected dimensions and visibly retain each scene. Metrics compare primary output with its PNG and thumbnail outputs with a Pillow bilinear resize of that PNG:

| Scene / output | GPU dispatch med / p95 (ms) | Fence wait med / p95 (ms) | CPU total med / p95 (ms) | Selected bytes / eye | MAE | PSNR |
|---|---:|---:|---:|---:|---:|---:|
| Dark primary 2176² | 0.497 / 0.510 | 8.970 / 14.268 | 12.386 / 17.606 | 414,897 | 2.293 | 33.86 dB |
| Dark safety 192² | 0.278 / 0.285 | 8.677 / 9.757 | 8.838 / 9.940 | 7,091 | 5.440 | 27.79 dB |
| Dark safety 256² | 0.285 / 0.288 | 8.894 / 9.693 | 9.093 / 9.881 | 11,740 | 4.766 | 28.69 dB |
| Forest primary 2176² | 0.340 / 0.349 | 8.847 / 13.695 | 11.577 / 16.308 | 257,958 | 1.405 | 41.32 dB |
| Forest safety 192² | 0.264 / 0.269 | 8.936 / 9.506 | 9.113 / 9.675 | 5,644 | 2.923 | 31.86 dB |
| Forest safety 256² | 0.273 / 0.278 | 9.022 / 10.216 | 9.217 / 10.418 | 8,362 | 2.543 | 33.02 dB |

Selected bytes follow the encoder policy: try LZ4/Zstd-3 results and retain Zstd only when it improves by at least 10%. Actual compressed sizes are content-dependent. A two-eye, 90 fps extrapolation from these single-eye left inputs is 10.21/16.91 Mbit/s for dark safety192/256 and 8.13/12.04 Mbit/s for forest safety192/256; it is not a measured stereo stream. The native primary extrapolation exceeds 20 Mbit/s for both inputs. No Pico result is inferred.

The post-submit CPU fence wait dominates these PC samples, at roughly 8–10 ms median compared with 0.26–0.51 ms GPU dispatch. This is reported as measured here; it is not a latency-win claim. Repeated ASTC output files are byte-identical within each condition. `spirv-val` passed on both shaders.

To rebuild the published harness from `gpu/`, using your own tightly packed RGBA8 input:

```sh
mkdir -p build
glslc -O -I shaders shaders/encode.comp -o build/encode.spv
glslc -O -I shaders shaders/encode_primary.comp -o build/encode_primary.spv
cmake -S . -B build
cmake --build build -j4
./build/astc-safety own2176.rgba build/safety192 2176 2176 192 192 3 6 build/encode.spv
./build/astc-safety own2176.rgba build/primary 2176 2176 2176 2176 3 6 build/encode_primary.spv
```

Run primary/192/256/256/192/primary in that order to reproduce the interleaved shape. Each invocation writes CSV and ASTC output. Original measurement used vendored LZ4; the public CMake resolves installed LZ4/Zstd development libraries. Library versions and host load can change timing. Raw measured CSVs are retained under `gpu/build/`; no private RGBA, texture or decoded photo is published.
