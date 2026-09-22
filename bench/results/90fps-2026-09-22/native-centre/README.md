# Native-colour centre

The experimental direct stream now carries a 128×128 RGB888 patch per eye, sampled before 4:2:0 chroma averaging and bypassing the four-colour block palette. The outer image keeps its existing cheaper representation. This first version uses a tile-aligned square patch; a separate smooth colour-resolution transition is not implemented yet.

## What was proved

1. **Actual foveation shader dispatch:** identity-mapped one-pixel red/green checkerboard, opposite phase per eye; all 32,768 captured RGB words matched exactly.
2. **GPU encoder and decoder format:** a native checkerboard deliberately different from the NV12 base survived the complete native safety/LZ4 envelope with all 32,768 RGB words exact.
3. **Live Pico:** client negotiated the extended 3,127,568-byte native frame capacity, decoded the stream, and reported 89–90/90 presentation FPS in inspected runtime samples. Several sender windows encoded 180 frames per two seconds. These short moving-scene checks are not a thermal soak or a human visual verdict.
4. **Actual live remap:** `x 1..1, y 1..1 source texels per encoded pixel` across the central patch, confirming source-native spatial sampling in this tested configuration.
5. Legacy wire/LZ4/safety tests and the updated Android/server builds passed. Native malformed-offset/truncation tests passed ASan/UBSan.

No optical latency claim: source-clock timing fields in the run included negative intervals and must not be presented as photon latency.

## Cost and limits

The patch holds 32,768 RGB888 pixels in GPU-friendly 32-bit words, plus 32 padding words: 131,200 bytes before compression. That is 94.46 Mbit/s at 90 updates/s, or a conservative 118.08 Mbit/s reservation including 25% transport allowance. Replaced palette blocks reduce the actual increment; LZ4 may reduce it further. The patch is included in rate admission and spatial-budget decisions. At low bandwidth, the outer image may lose detail or fresh updates may slow; the low-resolution safety image remains independent and does not contain the native patch.

Native here means spatial colour sampling: this is 8-bit sRGB per channel, not lossless floating-point/HDR or a new 10-bit path. Existing renderer resolution and foveation still apply; other configurations must check their source footprint rather than assuming 1:1.

No additional Pico rendering pass: the existing presentation shader reads the RGB word for marked tiles. Additional upload bytes and shader branching are real costs; no isolated Pico cost reduction is claimed.

## Integration

Enable `NX_DIRECT_NATIVE_CENTER=1` in the server environment for paired direct LZ4+safety streams at least 256×256 per eye. The paired updated client is required (stream versions 7/8; mixed frame version 2). Unset it to return to the prior path. The user server was restarted with the option enabled and saved bitrate settings restored; no test scene remains.

The host reserves a coherent buffer per compositor image slot, writes it before YCbCr conversion, applies a GPU-write/host-read barrier, and reads only after the existing producer fence. Native pixels replace the centre tiles in a bounded independent frame. Safety frames remain unchanged.

Reproducible integration tests: `tests/direct_native_source_test.cpp` takes the compiled production foveation SPIR-V; `tests/direct_blocks_gpu_test.cpp` enables the native roundtrip with `NX_DIRECT_TEST_WIDTH=2176` and `NX_DIRECT_TEST_NATIVE`. The source fixture uses an identity remap and checkerboard. Filtered live logs are adjacent.
