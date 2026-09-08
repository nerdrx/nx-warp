# PLANAR direct backend: Pico offscreen validation

This directory is **OFFSCREEN backend validation only**. It does not measure,
exercise, or certify the live APK, client route, compositor, or server.

Device: Pico (Adreno 650), serial `PA8150MGGB110166G`.

The reusable `nxvc::PlanarDirect` API rendered frame 19 from
`camera90.nxv`. The pulled RGBA readback is byte-for-byte identical to the
existing `full19.rgba` reference (`cmp` exit 0; both SHA256
`0c5d1523d2cf09ec1b077c9c8c85dc39310caa6293d90bd7e12b58cfe0a1accf`).

The same process rendered 720 frames with 11.111 ms admission pacing (90 Hz):
average total render time 3.380233 ms, maximum 8.82115 ms, average Vulkan GPU
timestamp 1.478123 ms, and zero frames over the 11.111 ms deadline. Per-frame
measurements are in [pico-api.motion.csv](pico-api.motion.csv).

Artifacts:

- [pico-api.frame19.png](pico-api.frame19.png): lossless PNG of the device readback; raw pixel hash above.
- [pico-api.motion.csv](pico-api.motion.csv): 720 per-frame rows.
- [pico-api.run.log](pico-api.run.log): captured device result and hashes.
- [build-command.txt](build-command.txt): exact cross-build command.

The tested harness is `probe/planar-direct/direct-harness.cpp`. The test binary
SHA256 is recorded in `pico-api.run.log`; it was linked with the supplied
decoder archive and a freshly compiled `vk/decoder/planar_direct.cpp` object.

![Actual Pico backend readback, motion frame 19](pico-api.frame19.png)

For the subsequent mixed native-detail centre experiment, see the
[September 9 evidence](../../90fps-2026-09-09/centre-detail/README.md).
