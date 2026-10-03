# NX ASTC GPU encoder harness

Small Vulkan 1.1 compute benchmark for the fixed ASTC 8x8 encoder shader in
`shaders/encode.comp`. It measures the kernel and the full host call that
retrieves encoded blocks and LZ4-compresses them. This is a benchmark harness,
not a production codec.

## Build

Requires a Vulkan SDK/loader and headers, CMake, C/C++ compiler, `glslangValidator`,
`spirv-val`, and LZ4 source (`lz4.c`/`lz4.h`). Set `LZ4_DIR` if LZ4 is elsewhere;
`GLSLANG_VALIDATOR` and `SPIRV_VAL` can override shader tool paths.

```sh
LZ4_DIR=/path/to/lz4 ./build.sh
```

The build creates `build/astc-gpu` and validates SPIR-V for Vulkan 1.1.

## Run

Input is tightly packed RGBA8 (one pixel per four bytes). Width and height must
match the exact input length. `fit=0` uses bounding-box endpoints; `fit=1` uses
PCA endpoint fitting; `fit=2` adds least-squares weights; `fit=3` adds a flat-area
shortcut to least-squares fitting. `quantBits` selects endpoint quantization (4–6): six uses all 64 values, five rounds in steps of two and four in steps of four, with a saturated 63 endpoint retained in every mode. Every wire endpoint field stays six bits.

```sh
build/astc-gpu input.rgba output.astc WIDTH HEIGHT FIT QUANTBITS [resident|upload]
```

`resident` uploads source once before timing; `upload` stages the source each
sample and includes source transfer in GPU transfer+encode timing. Both modes
run 12 warmups and 30 measured iterations. The `.astc` output has a standard
16-byte ASTC header, followed by raw blocks. `.blocks.lz4` contains a raw LZ4
block stream of the encoded block payload (dimensions are in the ASTC header),
`.csv` contains per-sample timings and compressed size, and `.json` contains
aggregate median/p95 timings. CPU full-call timing includes per-frame staging
copy/flush for upload mode, submission/fence wait, readback/invalidate, output
copy, and `LZ4_compress_default`.

GPU transfer+encode+readback includes the source copy when in upload mode, the
compute pass, and encoder-output copy to host-cached readback through the bottom
of the pipeline. GPU encode isolates compute.
It does not include image decode or application integration. GPU clocks, driver,
shader compiler, and workload affect results; retain device/build metadata with
any published comparison.

LZ4 source used during initial local builds is upstream LZ4 v1.10.0; see its
`LICENSE` and `PROVENANCE.md` in the source distribution. No LZ4 source is copied
into this harness.

The `runs/cached-*` set was measured with host-cached readback and GPU transfer+encode+readback timestamps. Earlier files without `cached-` are preliminary results from uncached readback memory.
