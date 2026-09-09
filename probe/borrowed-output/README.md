# Borrowed output probe

This probe exercises the decoder's borrowed Y/CbCr output path against a
three-frame independent 8-bit 4:2:0 stream. It installs two borrowed target
pairs, checks rejection of an invalid width, submits the third frame
asynchronously, restores decoder-owned images with `NULL`, and checks that the
restore is real. The output is packed NV12 per frame (`Y` followed by `UV`).

The source in `main.cpp` is the parent-fixed harness used for the valid run.
The exact host build command was:

```sh
cd /run/media/nerdrx/Lex/claude/nx-warp
mkdir -p probe/borrowed-output/build
export HOST_VULKAN_INCLUDE=$PWD/../tools/Vulkan-Headers-1.4.309/include
export PLANAR_HOST_LIB=$PWD/build-vkdec/vk/decoder/libnxvc_vk_decoder.a
export PLANAR_BUILD=$PWD/probe/borrowed-output/build
c++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers \
  -I"$HOST_VULKAN_INCLUDE" -I"$PWD/include" \
  probe/borrowed-output/main.cpp "$PLANAR_HOST_LIB" \
  -lvulkan -lpthread -ldl -o "$PLANAR_BUILD/nx-borrowed-output"
```

Run it with an independent stream containing at least three complete frames:

```sh
probe/borrowed-output/build/nx-borrowed-output input.nxv output.nv12
```

The parent-fixed Pico validation passed for 3 frames at 4352x2176, with target
selection 0/1/0, asynchronous submission, `NULL` restoration, and invalid
geometry rejection. The corresponding APK SHA-256 is
`db24bcdc94b4159aff391c6885c5dd60cf8fa0405b2c9d8c947b422ec246a3ac`; the
native library SHA-256 is
`bf3c0c9d21bf9e85a890ea2f86c02d618693a8dabdd6ef8c635bb4d3bbbce60d`.

The implementation removes the compute-image copy, while the submit barrier
remains. This probe records correctness only; it does not claim a latency or
throughput result.
