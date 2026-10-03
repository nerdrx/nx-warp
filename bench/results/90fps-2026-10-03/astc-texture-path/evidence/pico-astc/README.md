# Pico ASTC sampling and upload microbenchmark

Standalone offscreen Vulkan 1.1 executable. It uploads one ASTC LDR image, samples it
into a 4352x2176 RGBA8 storage image, and reports upload, sample, and total GPU
times plus CPU staging/flush/record/submit/fence time. It repeats the same passes
for a raw RGBA8 image. The extra full-frame sample pass is not free and this
benchmark does not represent compositor presentation or remove its work.

## Inputs

Place these files in `/data/local/tmp/nx-astc-pico/`:

* `input.astc`: 16-byte ASTC header followed by blocks. Supports 2D 8x8 or 12x12
  UNORM LDR blocks. Header magic is `13 AB A1 5C`.
* `raw.rgba`: tightly packed RGBA8 image with dimensions from ASTC header.
* `reference.rgba`: desktop-decoded ASTC RGBA8 reference, same dimensions.
* `input.astc.blocks.lz4`: LZ4 block-payload stream required by `--lz4`.
* `input2.astc`, `input2.lz4`, `reference2.rgba`: optional moving second
  frame. `--lz4` alternates input frames every iteration and checks output
  against the second desktop decode. The included two-frame run uses a
  synthetic 2-pixel horizontal shift; it does not represent headset motion.

The framebuffer/output extent is fixed at 4352x2176, matching inputs. The
desktop decoder reference comparison allows maximum channel error 2 and reports
MAE, max error, and out-of-tolerance pixels.

`--lz4` benchmarks raw ASTC payload beside LZ4 decompression plus upload.
Decompression writes into a persistent cached buffer before the mapped staging
copy; its time is included in CPU call time. The 500 Mbps value is a
payload-only transfer-time lower bound, not a network measurement.

Run as the `shell` user. Aggregate results go to stdout and
`/data/local/tmp/nx-astc-pico/result.txt`; each sample's CPU and GPU timings go
to `samples.csv`.

## Build

Run `./build.sh`. It compiles the compute shader for Vulkan 1.1, validates and
embeds SPIR-V, then builds the arm64 executable with NDK 29. The script records
SHA-256 hashes for source, shader, fixtures, LZ4 source, SPIR-V, and executable
in `build/SHA256SUMS`. Override local tools with `ASTC_PICO_NDK`,
`ASTC_PICO_CMAKE`, `ASTC_PICO_VULKAN_HEADERS`, `ASTC_PICO_GLSLANG`,
`ASTC_PICO_SPIRV_VAL`, or `ASTC_PICO_LZ4_DIR`.

## Run protocol

The executable checks API >= 1.1, `textureCompressionASTC_LDR`, sampled-image
support, and image format/usage limits before enabling that feature. It reports
all checks and exits without measurements when unsupported. Each measured path gets 12 warmups and 30
timed iterations. Device clocks/thermals are not controlled. This is a GPU
microbenchmark, not a headset compositor or live XR result.
