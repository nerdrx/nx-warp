# Reproduce the row-predictor measurements

These standalone programs need the production `common/` headers and caller-supplied NXDF files. The photo-derived fixtures are private and are not included here. Use files with the same layout: 2176×2176 per eye, stereo, RGB888 native center of side 256.

`row_bench` takes a destination CSV followed by named fixtures. Frame input is `name:path`; residual input is `name:old-path|current-path`. For example:

```sh
./row_bench samples.csv forest_s0:/data/forest-s0.nxdf forest_0to8:/data/forest-s0.nxdf\|/data/forest-s8.nxdf > summary.csv
```

The shell quoting/escaping around `|` is required. The harness derives motion residuals from the pair and checks reconstruction before benchmarking. It checks every decoded result byte-for-byte outside the timed section. It prints the fixture summary to stdout and writes individual warm/measured samples to the first argument.

`selector_probe` takes the same named fixture arguments and prints the LZ4, dense-Zstd and stride-4 predicted-Zstd sizes plus the production selector winner. This confirms which existing representation is the baseline for a fixture; it does not time encoding.

## Build

Set `NXWARP_COMMON` to the checkout's `common/` directory. On a host with Zstd and LZ4 development libraries:

```sh
c++ -O3 -DNDEBUG -std=c++20 -I "$NXWARP_COMMON" row_bench.cpp -lzstd -o row_bench
c++ -O3 -DNDEBUG -std=c++20 -I "$NXWARP_COMMON" selector_probe.cpp -lzstd -llz4 -o selector_probe
```

For Android arm64, set `NDK`, `NXWARP_COMMON`, `ZSTD_INCLUDE` and `ZSTD_STATIC_LIB` to local SDK/header/library locations:

```sh
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++" \
  -O3 -DNDEBUG -std=c++20 -fPIE -pie -static-libstdc++ \
  -I "$NXWARP_COMMON" -I "$ZSTD_INCLUDE" row_bench.cpp \
  "$ZSTD_STATIC_LIB" -lm -o row_bench
```

The recorded Pico run used Android NDK r29, Android API 29 and Zstandard 1.5.7. It performed 12 warm and 12 measured ABBA/BAAB blocks per fixture (24 samples per path and phase); percentile estimates from 24 measured samples are coarse. The recorded run tested standalone decode only; it did not launch the app or server. Reproduction requires the original fixture bytes and equivalent compiler/library versions.
