# Pico Vulkan upload and bilinear sampling benchmark

Build SPIR-V on the host, cross-compile the standalone Android binary, then
push the three files to `/data/local/tmp/jpeg-periphery/` and run the binary
from a shell. It creates no window and does not start WiVRn.

```sh
glslangValidator -V --target-env vulkan1.1 bench/results/90fps-2026-09-29/jpeg-periphery/pico-check/harness/sample.vert -o /tmp/jpeg-sample.vert.spv
glslangValidator -V --target-env vulkan1.1 bench/results/90fps-2026-09-29/jpeg-periphery/pico-check/harness/sample.frag -o /tmp/jpeg-sample.frag.spv
NDK=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++" \
  -std=c++20 -O2 -Wall -Wextra bench/results/90fps-2026-09-29/jpeg-periphery/pico-check/harness/gpu_upload_bench.cpp \
  -lvulkan -o /tmp/jpeg-periphery-gpu-upload-bench
adb shell mkdir -p /data/local/tmp/jpeg-periphery
adb push /tmp/jpeg-periphery-gpu-upload-bench /data/local/tmp/jpeg-periphery/bench
adb push /tmp/jpeg-sample.vert.spv /data/local/tmp/jpeg-periphery/sample.vert.spv
adb push /tmp/jpeg-sample.frag.spv /data/local/tmp/jpeg-periphery/sample.frag.spv
adb shell 'cd /data/local/tmp/jpeg-periphery && chmod +x bench && ./bench sample.vert.spv sample.frag.spv'
```

It uploads two deterministic 544×544 RGBA images and bilinearly samples each
into a 2176×2176 offscreen target for 12 warmup and 100 timed frame pairs.
Vulkan timestamps separate image upload/copy from rendering when available;
fence wall time is reported too. One final readback checks every output pixel
against a CPU bilinear reference (2-level channel tolerance for GPU interpolation)
and prints a checksum. This excludes JPEG
decode and transport; run it only after the CPU JPEG benchmark, and treat it as
Pico Vulkan-path evidence rather than end-to-end latency.
