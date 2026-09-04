# nxvc_ref — the NX Warp reference codec

`nxvc_ref` is the bit-exact CPU reference implementation of the NX Warp v1
bitstream. It is not a fast codec and is not meant to become one. It exists so
that the Vulkan decoder has something to be *equal to*.

The normative syntax is **[`docs/SYNTAX.md`](../docs/SYNTAX.md)**. This library
is that document in executable form; `docs/PAPER.md` is the rationale and is not
normative.

**Phase 1 scope: intra only.** No inter prediction, no pose warp, no stereo, no
layers. The full v1 syntax is parsed and validated — a Phase 2 stream is refused
cleanly rather than misparsed — but only `INTRA` tiles are reconstructed.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build -R 'ref\.'
```

C++20, no dependencies beyond the standard library. The library target is
`nxvc_ref`; the CLIs are `nxv-enc`, `nxv-dec`, `nxv-info` and build alongside it.

## Tools

```sh
# encode raw planar 8-bit YUV (multi-frame files are concatenated frames)
nxv-enc --in in.yuv --w 2048 --h 2048 --pix yuv420p --qp 28 --out out.nxv

# per-tile foveation: one byte per tile per frame, raster order, value 0..2
nxv-enc --in in.yuv --w 2048 --h 2048 --pix yuv444p --qp 24 \
        --res-map res.map --qp-map qp.map --out out.nxv

# lossless (bit exact through the codec)
nxv-enc --in in.yuv --w 512 --h 512 --pix yuv444p --lossless --out ll.nxv

nxv-dec  --in out.nxv --out out.yuv [--pix yuv420p|yuv444p]
nxv-info --in out.nxv [--tiles]
```

`nxv-enc` extras: `--frames N`, `--tskip off|on|auto`, `--nsub 0..5|auto`,
`--matrix 0..3`, `--chroma-qp-off N`, `--no-custom-tables`, `--tile-420`,
`--rgb`, `--color-space unspecified|yuv709l|yuv709f`, `--stats`, `--quiet`.

`--rgb` means planes 0..2 are R, G, B and the codec applies YCoCg-R. Without it
the planes are coded exactly as given, which is the path a WiVRn capture takes
(its Linux encoder input is already YCbCr 4:2:0) and what the quality harness
uses; `--color-space` only tags what those planes are.

`--stats` prints where the bits went:

```
bit breakdown, frame 0 (1024 tiles, 1.22 lanes/tile, 0 transform-skip)
  tile headers            8192 B    7.56%  0.01562 bpp
  rANS init/flush         4996 B    4.61%  0.00953 bpp
    DC planes            33180 B   30.62%  0.06329 bpp
    luma blocks          37553 B   34.65%  0.07163 bpp
    chroma blocks         3453 B    3.19%  0.00659 bpp
```

`nxv-dec` writes Y, then Co/Cg (or U, V), then alpha if the stream has it;
`--nv12` writes Y followed by interleaved UV instead. `--pix` is checked against
the stream header rather than trusted.

## Library

The C ABI is [`include/nxvc/nxvc.h`](../include/nxvc/nxvc.h).

```c
nxvc_config cfg; nxvc_config_default(&cfg);
cfg.width = 2048; cfg.height = 2048; cfg.base_qp = 28;

nxvc_status st;
nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);
nxvc_encoder_stream_header(e, hdr, sizeof hdr, &hdr_len);
nxvc_encoder_encode_frame(e, &img, qp_map, res_map, buf, sizeof buf, &len);

nxvc_decoder *d = nxvc_decoder_create(&st);
nxvc_decoder_parse_stream_header(d, hdr, hdr_len, &consumed);
nxvc_decoder_decode_frame(d, buf, len, &out_img, &consumed);
```

Both sides expose the frame's tile layout: `nxvc_tile_layout_get()` for the grid
and `nxvc_encoder_tiles()` / `nxvc_decoder_tiles()` for the per-tile records
(mode, resolved QP, `res_level`, `tskip`, `table_set`, payload length).

## Byte layout at a glance

```
file  := stream_header ext_area frame*
frame := frame_header [custom_matrices] [table_sets] tile_row*
tile_row := row_header tile*
tile  := tile_header [mv] [alpha] payload
```

| structure | size |
|---|---|
| stream header | 64 bytes + `ext_len` bytes of TLVs |
| frame header | 40 bytes (+128 custom matrices, +120 per table set) |
| tile-row header | 12 bytes (`frame_number`, `row_index`, `tile_count`, 64-bit skip bitmap) |
| tile header | 8 bytes (+2 MV, +1 constant alpha) |
| tile payload | interleaved rANS, `4 * lanes` bytes of initial state first |

Fixed parameters: 64x64 tiles, 8x8 blocks, 8x8 integer DCT with 9-bit
Loeffler-derived constants, QP 0..63 with `step = 2^(QP/6)` as a Q4 table,
interleaved rANS with a 32-bit state, 10-bit probabilities, 12 contexts of 16
symbols, and 1 to 8 lanes per tile (`nsub_log2`; 8 is one subgroup cluster and
the value a GPU decoder should assume as the maximum).

## Source map

| file | contents |
|---|---|
| `src/common.h` | constants, contexts, tile geometry |
| `src/tables.cpp` | QP steps, weighting matrices, scans, LAST classes, table normalization |
| `src/default_tables.inc` | the 8 built-in probability table sets |
| `src/transform.h/.cpp` | 8x8 integer DCT/IDCT and the bilinear resampler |
| `src/entropy.h/.cpp` | rANS and the per-lane syntax state machine |
| `src/codec.cpp` + `src/codec_impl.inc` | headers, encoder, decoder |
| `tools/` | `nxv-enc`, `nxv-dec`, `nxv-info` |
| `../tests/ref/gentables.cpp` | regenerates `default_tables.inc` (dev tool) |
| `../tests/ref/vectors.cpp` | generates and checks the conformance vectors |

The per-lane syntax state machine in `entropy.cpp` is the piece the Vulkan Pass A
shader mirrors: one `LaneMachine` per rANS lane, driven identically by the
encoder and the decoder, so the two can never disagree about the symbol order.

## Tests and conformance vectors

`ctest -R 'ref\.'` runs: `ref.transform` (DCT round trip, range, resampling),
`ref.rans` (round trip on random and pathological streams, truncation, junk),
`ref.codec` (rate/quality monotonicity, lossless bit-exactness, every tool
combination, odd picture sizes, multi-frame), `ref.headers` (TLV forward
compatibility and header validation), `ref.fuzz_smoke` (random and mutated
streams never crash), `ref.cli` (the three tools end to end) and `ref.vectors`.

`tests/vectors/` holds 32 committed `.nxv` vectors and `vectors.md5`, which pins
both the MD5 of each bitstream and the MD5 of its decoded planes.

```sh
build/tests/ref/nxv-vectors --check    tests/vectors   # ctest runs this
build/tests/ref/nxv-vectors --generate tests/vectors   # only when the format changes
```

**A Vulkan decoder is conformant when it reproduces every `decoded_md5` in
`tests/vectors/vectors.md5`.** Regenerating the vectors means the bitstream
changed; that is a deliberate act and should come with a `docs/SYNTAX.md` diff.

A libFuzzer target is available with `-DNXVC_FUZZ=ON` (clang):

```sh
cmake -S . -B build-fuzz -DNXVC_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
build-fuzz/tests/ref/nxvc_fuzz_decode -max_len=4096 corpus/
```

## Performance

The encoder is written for clarity, but a 2048x2048 4:2:0 intra frame encodes in
about 0.12 s and decodes in about 0.04 s single-threaded on a desktop core —
fast enough to drive the quality harness over long sequences. (`--stats` and
`--custom-tables` each add a pass; `--no-custom-tables` encodes in about 0.07 s.)

## Where it stands against x264 intra

Single 1024x1024 frames, 4:2:0, `x264 --crf N --keyint 1` versus `nxv-enc --qp
N` at default settings, luma PSNR against the source:

**Mandelbrot** (dense natural-looking detail — the representative case):

| bpp | x264 | nxvc | delta |
|---|---|---|---|
| 0.82 | 50.24 | 48.66 | -1.6 dB |
| 0.55 | 45.56 | 44.04 | -1.5 dB |
| 0.36 | 40.37 | 39.03 | -1.3 dB |
| 0.21 | 34.91 | 34.60 | -0.3 dB |
| 0.12 | 31.19 | 31.53 | **+0.3 dB** |

**Synthetic render** (large smooth gradients, a few hard edges — the worst case
for us):

| bpp | x264 | nxvc | delta |
|---|---|---|---|
| 0.19 | 52.81 | 49.32 | -3.5 dB |
| 0.12 | 51.09 | 45.38 | -5.7 dB |
| 0.09 | 47.82 | 42.03 | -5.8 dB |
| 0.06 | 43.82 | 38.94 | -4.9 dB |

### Gap analysis

The paper's estimate was "30-40% more bits than x265 intra". On detailed
content we are inside that and better; on smooth synthetic content we are well
outside it. The difference is entirely explained, and each part is measurable:

1. **No directional intra.** A smooth gradient is exactly what H.264's 16x16
   plane mode and its directional 8x8/4x4 modes were built for; they predict it
   almost exactly and spend nothing. Our DC-plane predictor interpolates
   *quantized block means*, so it can only be as smooth as the DC plane is
   fine. This is the whole 3-6 dB on the render image and essentially none of
   the gap on the mandelbrot image. `INTRA_DIR` is the reserved tool bit for
   it; SYNTAX.md 12 keeps the door open. **Not** implemented here — Phase 1's
   job was an honest baseline, and the paper gates directional intra on this
   measurement.
2. **Per-tile fixed cost.** 8 bytes of tile header plus the rANS flush is
   13.7% + 6.9% of a QP 36 frame. On the render image at 0.06 bpp that is over
   a third of the file. x264 has no per-64x64 cost at all. This is a syntax
   limit, not an encoder limit (SYNTAX.md Appendix B).
3. **Static probability tables and no adaptivity.** Per-frame tables recover
   most of it (they are on by default and worth 10-45%), but CABAC still wins
   perhaps 5-8% on the same coefficients, exactly as PAPER 1.6 predicted.
4. **No RDO beyond a dead-zone quantizer.** x264 at `--crf` runs trellis
   quantization and mode decision. Worth perhaps 0.5-1 dB, and it is pure
   encoder work that changes no bitstream.

What was fixed to get here, in order of effect: quantizing the DC plane at
`qp >> 1` instead of `qp - 6` (+3 dB at QP 38 *and* -10% bits — coarse block
means made the planar prediction blocky and every AC block paid for it),
training the eight built-in probability tables on real tile statistics and
letting the encoder pick the cheapest per tile (-30% at QP 24), and choosing
the rANS lane count per tile so the flush stays under a tenth of the payload
(the flush was 33% of a QP 38 frame). On the 2048x2048 textured frame that is
0.114 bpp at 30.2 dB where the first working version needed 0.220 bpp for the
same picture.

Sanity checks that the baseline is honest, all covered by `ctest -R 'ref\.'`:
the DC-plane predictor is verified to be the thing being coded (lossless mode
is bit exact through it), transform skip round trips and is exercised by
vectors v10-v13, the probability tables are trained rather than uniform
(`tests/ref/gentables.cpp` regenerates them), and rate falls monotonically with
QP while PSNR falls with it.
