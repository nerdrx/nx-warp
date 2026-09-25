# Regional-motion validation bundle

This bundle contains the generated Vulkan codec harness, a build script, the raw CSV from the rejected first selector, and source/binary hashes. The rejected CSV is retained only to show why global-v1 is tried first: uniform pan grew when regional-v2 won without comparison. Final matched measurements and Pico helper timings are in the project result report.

Build against an existing configured host build, a checkout at baseline commit `070b671b1e2cbd18cd1c676bc88c0d12391cf553`, and the candidate checkout:

```sh
python3 build.py --baseline /path/to/baseline --candidate /path/to/candidate \
  --build /path/to/configured/build --out ./build-out
```

The script replaces only the direct-codec object and a test wrapper at link time. Supply two local 2160×2160 RGBA8 fixture files when running; the harness shifts their native RGB pixels before calling the production native-center blend. The offscreen GPU periphery remains synthetic.

```sh
NX_BENCH_FOREST=/path/fixture-a.rgba NX_BENCH_DARK=/path/fixture-b.rgba ./build-out/baseline/bench
NX_BENCH_FOREST=/path/fixture-a.rgba NX_BENCH_DARK=/path/fixture-b.rgba ./build-out/regions/bench
```

Each run covers opposing regional shifts, a continuous pan, a photo-scene cut, and no-ACK, with six warmup and 24 measured frames per case. It checks exact decoded NXDF equality and prints measured rows to stdout. No fixture images or executables are included.

`restore_microbench.cpp` isolates Pico-side motion-header parsing and reconstruction on a 2176×2176 stereo NXDF. The residual body is prepared and copied outside the timer; exact output checks run after timing. To build for Android, use the NDK clang++ with `--target=aarch64-linux-android29 -std=c++20 -O3 -static-libstdc++`, include the repository root and `tests` directory, then push and run the executable with `adb`. It does not start the app or server.


The exact candidate source is WiVRn NX commit `08f55c16af6e96980d5b5f522b70efc6ca8492dd`. A configured build must be compatible with that checkout. The helper source includes `direct_motion_test.cpp`, so the candidate `tests` include path is required.

The published [final measurements](../encoder-samples.csv) use run order A1, B1, B2, A2. The [summary](../summary.json) uses the sample median (average of the middle two values for an even count); the microbench's diagnostic stderr prints the lower nearest-rank p50. Both derive from the same raw samples. The CSV's `sample` label can repeat within a mode/block; each row is a separate timed call.
