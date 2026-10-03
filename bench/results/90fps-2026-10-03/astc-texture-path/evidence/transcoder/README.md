# XUASTC to ASTC CPU benchmark

Standalone host and Android arm64 executable. It reads one KTX2 packet into memory, runs 12 warmups and 30 measured samples, and writes mip 0 as a 16-byte ASTC header plus raw blocks after timing. Every sample constructs a fresh KTX2 transcoder. `init+start`, transcode, and full-call timings are reported separately. File I/O and process-wide `basisu_transcoder_init()` are outside the samples; full-call includes transcoder construction, metadata, and output allocation.

Build against the Basis checkout:

```sh
cmake -S . -B build-host -DBASIS_DIR=/path/to/basis_universal
cmake --build build-host -j
cmake -S . -B build-android \
  -DBASIS_DIR=/path/to/basis_universal \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

Run either executable with `input.ktx2 output.astc`. Input and output paths are the only arguments. The helper requires XUASTC LDR mip 0 in a single-face 2D image. ASTC output is raw block data; transfer-function metadata is not represented in the `.astc` header.

## Dark fixture result

Input: `dark-xuastc-12x12-q50.ktx2` (4352×2176, ASTC 12×12).

- Host: init+start median 0.005 ms; transcode median 15.908 ms; full-call median 15.915 ms, p95 16.322 ms.
- Pico 4 / Android 29: init+start median 0.006 ms; transcode median 29.692 ms; full-call median 29.701 ms, p95 30.866 ms.
- All 30 same-process outputs were byte-identical on each platform. Host and Android ASTC files were also byte-identical (`24452fe3f1ee4dec1f79b47f034806a2b649d38733ccccd238e0f7cc3865c60d`). Host result also matches the pre-existing host CLI ASTC output byte-for-byte.
- Android executable was NDK r29, arm64-v8a, API 29. This is one fixture and one run; it does not measure rendering or graphics performance.
