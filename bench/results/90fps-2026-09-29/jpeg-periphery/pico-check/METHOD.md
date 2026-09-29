# Pico Q20 JPEG decode measurements

Standalone hardware measurements, captured 2026-09-29 UTC. Image assets remain
private.

## Device and build

- Pico A8110, Android 10 / API 29; build fingerprint:
  `Pico/Phoenix_ovs/PICOA8110:10/5.13.8/smartcm.1788350898:user/dev-keys`.
- SoC properties: `qcom`, `kona`; Linux 4.19.81-perf, AArch64. `/proc/cpuinfo`
  reports cores 0–3 as implementer `0x51`, part `0x805`, and cores 4–7 as
  implementer `0x41`, part `0xd0d`.
- NDK r29 `29.0.14206865`, target `arm64-v8a`, Android API 29.
- Upstream libjpeg-turbo tag 3.2.0, commit
  `c85e6b905bf237038faa936dab160ebfc5da0344`; CMake Release, static,
  TurboJPEG API enabled, ARM64 SIMD enabled. The benchmark links
  `libturbojpeg.a` from this build, not Android's private `libjpeg.so`.
- The benchmark is a standalone shell executable. It calls
  `tj3Decompress8()` on CPU and writes RGB888 or RGBA8888 output. GPU and
  hardware decoder are unused. No headset app, server, or stream was running.

Build used these relevant CMake settings:

```sh
NDK=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
SRC=/run/media/nerdrx/Lex/claude/nx-warp/scratch/jpeg-periphery/pico/libjpeg-turbo-3.2.0
BUILD=/run/media/nerdrx/Lex/claude/nx-warp/scratch/jpeg-periphery/pico/build-arm64
BENCH=/run/media/nerdrx/Lex/claude/nx-warp/scratch/mjpeg-decode-agent/jpeg_decode_bench.cpp
OUT=/run/media/nerdrx/Lex/claude/nx-warp/scratch/jpeg-periphery/pico/jpeg_decode_bench-arm64
cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29 -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DWITH_SIMD=ON
cmake --build "$BUILD" --target turbojpeg-static jpeg-static -j6
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++" \
  -O3 -DNDEBUG -static-libstdc++ -I"$SRC" -I"$BUILD" -I"$SRC/src" \
  "$BENCH" "$BUILD/libturbojpeg.a" -lm -ldl -o "$OUT"
```

The RGBA benchmark is a scratch copy of the same harness, changing its output
buffer to four bytes per pixel and selecting `TJPF_RGBA`; all timing and hash
checks are otherwise unchanged.

## Procedure

Each input ran in a separate serial process on the Pico, with 12 warmups and
24 measured repetitions. Each JPEG is 1088×544 pixels (544 pixels high).
Timing covers the decode call(s); hashing and CSV writes are outside the timed
interval. RGB and RGBA were tested separately. Per-run before/after temperature
and CPU0 scaling frequency are in the raw logs. Thermal zone 0 is reported as
`aoss0-usr`. CSV `exact=1` means the decoded byte hash matched that run's first
warmup hash; it does not compare against an independent image decoder.

## Results

| Input | Output | JPEG bytes | p50 | p95 | JPEG FNV-1a | Decoded FNV-1a |
|---|---:|---:|---:|---:|---|---|
| Forest Q20 | RGB888 | 12,070 | 1.261 ms | 1.361 ms | `cfb35ef7d9f1e5cf` | `9cc5c91f14e68583` |
| Dark Q20 | RGB888 | 25,247 | 1.459 ms | 1.487 ms | `11b4d8d700d12081` | `ed367f8b65576c53` |
| Forest Q20 | RGBA8888 | 12,070 | 1.310 ms | 1.369 ms | `cfb35ef7d9f1e5cf` | `5183f2b7196a7c05` |
| Dark Q20 | RGBA8888 | 25,247 | 1.512 ms | 1.568 ms | `11b4d8d700d12081` | `70c0637a50349019` |

The four CSVs contain all samples; the four `*-run.log` files contain tool
summary, hashes, device build, and per-run before/after readings. The inputs
remain in the parent scratch directory and are not copied into public results.
