# LZ4 payload benchmark

Standalone host and Android arm64/API 29 helper. It loads the raw LZ4 block payload and ASTC reference before timing, allocates the exact decoded size once, then calls `nx_lz4_decompress_payload()` 12 warmups and 30 measured times into that persistent buffer. The small wrapper is in `lz4_payload.h` for reuse by the combined render/decode helper. It checks each result against ASTC bytes after the 16-byte header. No file I/O or allocation occurs in the timed region.

Build using the bundled `nx_lz4` source:

```sh
cmake -S . -B build-host -DLZ4_DIR=/path/to/nx_lz4-src/lib -DCMAKE_BUILD_TYPE=Release
cmake --build build-host -j
cmake -S . -B build-android \
  -DLZ4_DIR=/path/to/nx_lz4-src/lib \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

Run `lz4_payload_bench input.lz4 reference.astc`. It reports median and p95 decode time and fails if any sample differs from the ASTC payload.

## Dark fixture

Input is 870,184 bytes; decoded ASTC blocks are 1,057,056 bytes. Host benchmark output is preserved in `host-benchmark.log`: median 0.168 ms, p95 0.199 ms, with all 30 samples byte-exact. Hashes are in `hash-manifest.txt`.

Android arm64/API 29 binary compiled successfully with NDK r29. It was not run because Pico access is reserved for the active render benchmark. No Pico LZ4 timing claim is made.

Previous XUASTC benchmark source at parent directory `../main.cpp` contains 12 warmups and 30 measured runs. Its observed Android transcode time was 29.692 ms on the dark fixture; same-run logs reported 30 samples / 12 warmups. Host and Android ASTC outputs matched byte-for-byte and match the pre-existing CLI ASTC output. See parent `../README.md` for those single-fixture timings and scope.
