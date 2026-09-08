# Encoder validation, 2026-09-08

Host: Radeon RX 7900 XTX. Build: `cmake --build build-vk`.

- GPU R2/coarse PLANAR fit: two-frame pixel regression against `nxv-dec` passes. Every output sample is checked on neutral and two-region tiles.
- Internal luma references match reference-decoded output for both frames, with ordinary INTER and with ATLAS.
- Host-fit PLANAR through the Vulkan encoder, GPU-fit environment unset: all six R2/R3/R4 fine/coarse configurations pass two-frame reference-ring comparisons.
- The encoder retains generic Pass B reconstruction. Serialized PLANAR bodies and padded reconstruction bodies have separate buffers.
- The ring dump tool now reads slot 0 for ATLAS; its previous rotating-slot read produced a false mismatch.

These are correctness checks, not encoder speed or general image-quality measurements. GPU fitting remains an explicit approximation experiment through `NXVC_ENC_PLANAR_GPU_FLAT=1`, requiring INTER and aligned 4:2:0 input. Unsupported GPU-image input shapes fail explicitly.

Reproduce the pixel regression:

```sh
python3 tests/vk-encoder/planar_gpu_fit.py --encoder build-vk/bin/nxvc-vkenc --decoder build-vk/bin/nxv-dec
```
