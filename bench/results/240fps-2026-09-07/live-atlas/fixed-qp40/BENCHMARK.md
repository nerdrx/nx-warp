# Hello XR benchmark patch

Patch base: upstream OpenXR `2b99fec` (release 1.1.63); patch commits:
`61d35388`, `0b7c904a`, `3636e60d`.

Build from the patched checkout with the cached Vulkan toolchain:

```sh
cmake -S /run/media/nerdrx/Lex/claude/nx-scratch/hello-xr-bench \
  -B /run/media/nerdrx/Lex/claude/nx-scratch/hello-xr-bench-build \
  -DOPENXR_BUILD_LOADER=ON -DOPENXR_BUILD_TESTS=OFF
cmake --build /run/media/nerdrx/Lex/claude/nx-scratch/hello-xr-bench-build \
  --target hello_xr -j4
```

Executable: `hello-xr-bench-build/src/tests/hello_xr/hello_xr`.
For the fixed scene/input pattern, set
`NXWARP_BENCH_VIEW_SCENE=1 NXWARP_BENCH_PATTERN=1`. The first option
submits the view-relative 3x5 cube scene anchored to the first display time;
the second overlays a static high-contrast 8x8 checkerboard in each view
subimage. Both are opt-in.
