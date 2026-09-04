# A Vulkan Compute Video Codec for Low-Latency VR Streaming

**Design paper, draft 1. 2026-09-04.**
**Target integration: WiVRn NX. First target hardware: Pico 4 (Snapdragon XR2 Gen 1) streamed from a PC.**

## Abstract

General-purpose video codecs are the wrong tool for VR streaming. H.264, HEVC and AV1 are built for
file storage and broadcast: whole-frame slices, reference lists, B-frames, serial entropy coding and
fixed-function hardware whose latency and session limits we cannot change. VR streaming has structure
those codecs cannot see. The encoder knows the head pose that produced every frame and the pose the
next frame will be rendered at. The two eyes share nearly all content. The lens throws away most of
the periphery. The display would rather show a slightly stale tile than wait for a whole frame.

This paper designs a codec that is built around those facts and nothing else. The frame is a set of
independent tiles, each its own bitstream, each decoded by one GPU workgroup. Prediction is
pose-warped reprojection of the previous decoded frame, with small per-tile corrections, so motion
search largely disappears and a lost tile conceals itself. The bitstream is layered so a weak headset
can decode a hardware HEVC base layer and add a compute enhancement layer, while a strong headset
decodes everything in compute. Rate control, entropy coding, packetization and telemetry all run in
Vulkan compute with no CPU on the hot path. The codec is vendor-neutral on the PC, which removes the
NVENC/AMF/VAAPI dependency and its session ceilings.

The paper states the compute budget honestly. The first target GPU, Adreno 650, is weak, and the
project begins with a go/no-go benchmark before any codec code is written.

## Design principles

1. **Latency before bits.** Every tool is judged first by what it does to the time between render
   finish and photons, and only then by compression ratio.
2. **The GPU is the codec.** No tool may require serial state across tiles. If it cannot run as one
   workgroup per tile, it does not exist.
3. **Use what the renderer knows.** Pose, depth, motion, stereo and layer structure are inputs, not
   things to rediscover.
4. **Degrade, never stall.** Every loss, every deadline miss, every budget overrun has a defined
   graceful output.
5. **One format, many decoders.** Hybrid hardware-plus-compute and pure compute read the same
   bitstream. Capability bits, not forks.
6. **Bit-exact and simulatable.** Integer normative path, a CPU reference decoder, conformance
   vectors, fuzzing. The encoder runs the real decoder.
7. **Patent hygiene.** Public-domain and expired tools only, with a formal review before release.
