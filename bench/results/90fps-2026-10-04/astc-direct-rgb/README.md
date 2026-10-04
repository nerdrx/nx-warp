# ASTC direct RGB equivalence proof

`WIVRN_ASTC_DIRECT_RGB=1` is opt-in and only selects RGBA8 output when both eye streams are enabled ASTC and every other compositor stream (including alpha/quad) is disabled. The device must support RGBA8 optimal-tiling Storage + Sampled + TransferSrc at the per-eye extents and three image layers; otherwise initialization retains NV12. Other profiles stay planar. The output still runs through the compositor’s existing source crop/FOV remap, Y flip, motion warp, linear-to-sRGB conversion, lens mask, and optional native-center write. The ASTC server reads RGBA8_UNORM encoded-sRGB bytes directly; packet format and client are unchanged. No speedup claim: output images use more bandwidth than NV12.

The focused host check dispatches the production optimized foveation SPIR-V on a local Vulkan PC with synthetic 320×320 sRGB eyes and a 128×128×2 RGBA8_UNORM target. Coordinate arrays include nonuniform scaling and reversed Y; mask tiles differ between eyes. GPU image readback was compared with CPU `encoded_rgb` immediately before the production YCbCr matrix: max channel error 1 byte, zero channels >1, MAE 0.023735. The reference crop below is CPU-generated from that synthetic fixture; the actual GPU readback was not saved as an image.

![CPU reference crop of synthetic eye 0; grey square is the masked tile](/run/media/nerdrx/Lex/claude/nx-scratch/astc-direct-rgb/expected-cpu-reference-crop.png)

The first harness launch failed before Vulkan initialization because the server’s `.spv` file is a generated text array; using the optimized binary `.spv-opt` fixed it. That failed attempt is diagnostic only, not a pass.

The server target `wivrn-server` builds from worktree HEAD `6d03c6334fa40f63d957194f400744999e1ce7db`. This is source/build/shader equivalence only, not native-photo quality, performance, live compositor/stream, Pico, or headset proof. No photo content is included. `provenance.json`, `foveation.spv`, `harness/`, CSV samples, and `source-diff.patch` make the check reproducible/reviewable.
