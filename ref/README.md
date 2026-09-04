# nxvc_ref — the NX Warp reference codec

`nxvc_ref` is the bit-exact CPU reference implementation of the NX Warp v1
bitstream. It is not a fast codec and is not meant to become one. It exists so
that the Vulkan decoder has something to be *equal to*.

The normative syntax is **[`docs/SYNTAX.md`](../docs/SYNTAX.md)**. This library
is that document in executable form; `docs/PAPER.md` is the rationale and is not
normative.

**Scope: intra (Phase 1) and inter (Phase 2).** All five tile modes are
implemented — `WARP_SKIP`, `STATIC_MV`, `WARP_MV`, `INTRA` and `STEREO` — with
the pose warp, the four-slot reference ring, one picture per eye, and
deterministic concealment. Layers are still out of scope. Inter is **opt-in**:
`nxvc_config_default()` leaves it off, so an existing caller and every syntax
v1.3 stream are byte-identical to what a v1.3 build produced.

**Syntax revision v1.6** adds the two intra detail tools -- the 4x4 transform
split (bit 19) and chroma from luma (bit 24) -- plus a named, per-band encoder
dead zone that changes no syntax at all. Together they are worth **-18.8
BD-rate points on 4:4:4 and -16.0 on 4:2:0**, so the encoder turns both **on
by default**; `nxv-enc --split4x4 off --cfl off` writes a v1.4 stream. Both
are additive: `v01`-`v56` of the conformance set are byte-identical to what a
v1.4 build produced, which `ctest -R ref.vectors` checks on every commit. See
[`docs/SYNTAX.md`](../docs/SYNTAX.md) 6.7 and 7.7 and
[`RESULTS-detail-a.md`](RESULTS-detail-a.md).

v1.6 also adds the entropy and context package: tool bit 26 `TAB_V2` (a
transmitted probability-table set gains a per-row "use the built-in default"
flag and becomes variable length) and tool bit 25 `CTX_V3` (a neighbour-
conditioned model, with `CBF` and `LAST` conditioned on whether the previous
coefficient unit *the same rANS lane* decoded in the same unit class was
coded). Both are additive and both ship **off**: `nxv-enc` bare reproduces a
v1.4 stream byte for byte, and the 56 committed conformance vectors are
unchanged. See [`docs/SYNTAX.md`](../docs/SYNTAX.md) 9.4.1 and 9.8 and
[`RESULTS-ctx-b.md`](RESULTS-ctx-b.md).

**Syntax revision v1.4** adds the inter path: the frame-header flag
`warp_present` (bit 3) and the 36-byte-per-eye `warp_ext()` that follows the
frame header, the four inter tile modes, `eyes == 2` with row-major/eye-minor
tile rows, and the 12-bit STEREO `disparity` in place of `mv_x`/`mv_y`. See
[`docs/SYNTAX.md`](../docs/SYNTAX.md) 13 and
[`RESULTS-inter.md`](RESULTS-inter.md).

The predictor itself is **not implemented here**: `ref/src/inter.h` links
`nxvc_warp_ref` from [`warp/`](../warp), which owns `warp_tile()`, the corner
derivation and the saturation rules. `ref/` builds the reference ring, the
prediction state and the mode decision around it.

**Syntax revision v1.3** adds the three v2 intra tools — directional intra
(bit 17), the 16-context entropy model (bit 21) and sign data hiding (bit 22).
All three are additive: every stream a v1.2 build wrote is still byte-identical
and still decodes, and `v01`-`v35` of the conformance set are unchanged. The
encoder turns all three **on by default**, so a decoder that does not implement
them refuses the handshake; `nxv-enc --intra-dir off --ctx v1 --no-sign-hide`
writes a v1.2 stream.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build -R 'ref\.'
```

C++20, no dependencies beyond the standard library. The sources build once as
`nxvc_ref_obj` and link into both `nxvc_ref` (static, what the project uses) and
`nxvc_ref_shared` (`libnxvc_ref.so`, what the Python bindings load over ctypes);
the CLIs are `nxv-enc`, `nxv-dec`, `nxv-info` and build alongside them.
`nxvc_version_string()` reports the build's syntax revision.

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
`--matrix 0..3`, `--wm 0..3`, `--chroma-qp-off N`, `--no-custom-tables`,
`--tile-420`, `--rgb`, `--color-space unspecified|yuv709l|yuv709f`, `--stats`,
`--quiet`, the rate-distortion controls `--no-rdo` / `--rdo-lambda F`, and the
v1.3 tool switches:

| flag | default | effect |
|---|---|---|
| `--intra-dir on\|off\|layer` | `on` | directional intra; `layer` predicts the DC-plane residual instead of the samples (measured worse, see below) |
| `--intra-dir-cand N` | 2 | modes RD-checked per block after the SATD sort; 8 is exhaustive and worth 0.1 % for 2.2x the encode time |
| `--ctx v1\|v2` | `v2` | 12 or 16 entropy contexts |
| `--no-sign-hide` | off | code every sign |

and the v1.5 detail switches:

| flag | default | effect |
|---|---|---|
| `--split4x4 on\|off` | `on` | per-block 4x4 transform split; sets tool bit 19 |
| `--cfl on\|off` | `on` | the chroma-from-luma intra mode; sets tool bit 24, and needs `--intra-dir on --ctx v2` |

and the v1.4 inter switches:

| flag | default | effect |
|---|---|---|
| `--inter on\|off` | `off` | inter prediction; sets tool bits 10 and 11 |
| `--eyes 1\|2` | 1 | 2 means `--w` is the FULL side-by-side width and each half is a picture |
| `--poses FILE` | none | the `.poses.json` sidecar the warp matrix is derived from |
| `--fov H,V` | `95,95` | field of view in degrees, for that derivation |
| `--intra-period N` | 180 | rolling intra refresh period; 1 forces every tile every frame |
| `--ref-sel 0..2` | 0 | which reference inter tiles ask for, `N-1-ref_sel` |
| `--stereo on\|off` | `off` | `STEREO` inter-view mode on the right eye (needs `--eyes 2`) |
| `--mv-range N` | 16 | coarse integer search radius in samples |
| `--skip-thresh F` | 1.0 | `WARP_SKIP` early-out gate, multiples of `qstep^2/12` per sample |
| `--skip-map FILE` | none | per-tile `force_warp_skip` flags from the rate controller |
| `--mode-lambda F` | 0.25 | lambda of the per-tile mode decision relative to the trellis |

and the v1.5 entropy switches:

| flag | default | effect |
|---|---|---|
| `--ctx v1\|v2\|v3` | `v3` | 12, 16 or 22 entropy contexts (tool bits 21, 25) |
| `--tab v1\|v2` | `v2` | transmitted-table coding: flat 5-bit deltas, or the compact per-row form (tool bit 24) |
| `--table-iters N` | 3 | Lloyd iterations refining the eight per-frame table sets; 0 is the v1.4 encoder. **Encoder only** |

```sh
# a stereo inter stream from a corpus sequence and its pose log
nxv-enc --in vr-mixed-1024.yuv420p.yuv --w 2048 --h 1024 --pix yuv420p \
        --qp 8 --eyes 2 --inter on --poses vr-mixed-1024.poses.json \
        --out out.nxv
nxv-info --in out.nxv --tiles      # per-tile mode, vector, ref_sel, warp_ext
```

Rate-distortion quantization is **on by default**. It is encoder-only work -- a
trellis over the level syntax, `ref/src/codec.cpp` `rdoq_unit()` -- so a stream
encoded with it decodes through exactly the same path as one encoded without.
It costs about 2.7x encode time and is worth -8.8 % BD-rate; `--no-rdo` gets
the old dead-zone quantizer back. `--wm` sets the per-tile weighting-matrix id
that the rate controller's degradation ladder uses.

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
(`nxvc_tile_layout_get_ex()` when `eyes == 2`) and `nxvc_encoder_tiles()` /
`nxvc_decoder_tiles()` for the per-tile records (mode, resolved QP,
`res_level`, `tskip`, `table_set`, payload length, and for v1.4 the vector,
`ref_sel`, `ref_delta`, `disparity`, `skipped`, `concealed` and
`age_since_coded`).

### The inter path

```c
cfg.eyes = 2; cfg.inter = 1;                 /* width/height are PER EYE */
nxvc_encoder *e = nxvc_encoder_create(&cfg, &st);

nxvc_view v[2] = { ... };                    /* this frame's per-eye view */
nxvc_encoder_set_views(e, v, 2);             /* before every encode_frame */
nxvc_encoder_encode_frame(e, &img, NULL, NULL, buf, sizeof buf, &len);
```

`nxvc_encoder_set_views()` is the only floating-point input the codec takes; its
result reaches the decoder already quantised to the nine `int32` of
`warp_ext()`. The image is `eyes * width` samples wide, eye 0 first.

**The loss/concealment contract.** The decoder is told which tiles the client
did not get; the encoder is told the same thing and replays the identical
concealment on its shadow, so it predicts the next frame from what the client
actually shows:

```c
nxvc_decoder_set_lost_tiles(d, lost, ntiles);      /* consumed by one frame */
nxvc_decoder_decode_frame(d, buf, len, &img, &consumed);

nxvc_encoder_set_received_tiles(e, received, ntiles);   /* after encode */
nxvc_encoder_shadow_image(e, &shadow);                  /* == the decoder */
```

`tests/ref/test_inter.cpp` asserts that equality byte for byte over 200 frames
with random tile loss. `nxvc_encoder_set_skip_map()` is the rate controller's
`force_warp_skip` request (docs/RATECONTROL.md 8.7): applied after the mode
search, overridden wherever a coded tile is required.

## Byte layout at a glance

```
file  := stream_header ext_area frame*
frame := frame_header [warp_ext] [custom_matrices] [table_sets] tile_row*
tile_row := row_header tile*                        // eyes * rows of them
tile  := tile_header [mv | disparity] [alpha] payload
```

| structure | size |
|---|---|
| stream header | 64 bytes + `ext_len` bytes of TLVs |
| frame header | 40 bytes (+`36 * eyes` `warp_ext`, +128 custom matrices, +120 per table set, 160 with `CTX_V2`) |
| tile-row header | 12 bytes (`frame_number`, `row_index`, `tile_count`, 64-bit skip bitmap) |
| probability table set | 120 bytes, 160 with `CTX_V2`, 220 with `CTX_V3`; variable and usually far smaller with `TAB_V2` |
| tile header | 8 bytes (+2 MV, +1 constant alpha) |
| tile payload | interleaved rANS, `4 * lanes` bytes of initial state first |

Fixed parameters: 64x64 tiles, 8x8 blocks, 8x8 integer DCT with 9-bit
Loeffler-derived constants, QP 0..63 with `step = 2^(QP/6)` as a Q4 table,
interleaved rANS with a 32-bit state, 10-bit probabilities, 12, 16 or 22
contexts of 16 symbols, and 1 to 8 lanes per tile (`nsub_log2`; 8 is one subgroup cluster and
the value a GPU decoder should assume as the maximum).

## Source map

| file | contents |
|---|---|
| `src/common.h` | constants, contexts, tile geometry |
| `src/tables.cpp` | QP steps, weighting matrices, scans, LAST classes, table normalization |
| `src/default_tables.inc` | the 8 built-in probability table sets, v1 (12 contexts) and v2 (16) |
| `src/transform.h/.cpp` | 8x8 integer DCT/IDCT and the bilinear resampler |
| `src/entropy.h/.cpp` | rANS and the per-lane syntax state machine |
| `src/codec.cpp` + `src/codec_impl.inc` | headers, encoder, decoder |
| `tools/` | `nxv-enc`, `nxv-dec`, `nxv-info` |
| `../tests/ref/gentables.cpp` | regenerates `default_tables.inc` (dev tool); `nxv-gentables v1\|v2\|v3\|both` |
| `../tests/ref/vectors.cpp` | generates and checks the conformance vectors |
| `../tests/ref/test_saturate.cpp` | range safety of the normative decode path |
| `RESULTS-intra.md` | the Phase 1 intra measurements, in full |
| `RESULTS-detail-a.md` | the v1.6 detail tools, measured before and after |
| `RESULTS-ctx-b.md` | the v1.6 entropy and context package, in full |

The per-lane syntax state machine in `entropy.cpp` is the piece the Vulkan Pass A
shader mirrors: one `LaneMachine` per rANS lane, driven identically by the
encoder and the decoder, so the two can never disagree about the symbol order.

The 4x4 split lives in `transform.cpp` (`fdct4x4`/`idct4x4` and the `kD0/D1/D2`
constants), `kScan4Split` in `tables.cpp`, `residual_block()`/`forward_block()`
in `codec.cpp`, and the `kSplit` phase of `LaneMachine`. Chroma from luma is
`cfl_luma()`, `cfl_fit()` and the `kIntraCfl` case of `predict_block()`, with
`TileCoder::cfl_for()` deciding when the mode exists.

Directional intra lives in three places: `predict_block()` / `build_refs()` in
`codec.cpp` are the normative predictor and reference derivation,
`reconstruct_plane()` is the decoder's raster loop, and `analyze_plane_dir()`
is the encoder's fused mode-decision-plus-quantization pass. The mode symbols
are a `UNIT_MODE` coding unit in `entropy.cpp`; `hide_sign_unit()` is the
encoder half of sign data hiding, whose decoder half is the parity fixup in
`LaneMachine::advance_pos()`.

## Tests and conformance vectors

`ctest -R 'ref\.'` runs: `ref.transform` (DCT round trip, range, resampling),
`ref.rans` (round trip on random and pathological streams, truncation, junk),
`ref.codec` (rate/quality monotonicity, lossless bit-exactness, every tool
combination, odd picture sizes, multi-frame), `ref.headers` (TLV forward
compatibility and header validation), `ref.saturate` (the inverse transform and
the dequantizer at their documented bounds), `ref.fuzz_smoke` (random and
mutated streams never crash), `ref.cli` (the three tools end to end) and
`ref.vectors`.

`ref.saturate` exists to be run under the sanitizers, where a signed overflow
in the normative path aborts instead of being ignored:

```sh
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan
ctest --preset asan-ubsan -R 'ref\.'
```

`tests/vectors/` holds 61 committed `.nxv` vectors and `vectors.md5`, which pins
both the MD5 of each bitstream and the MD5 of its decoded planes, plus 32
**rejection vectors** and `rejects.md5`, which pin the exact status each
malformed stream must be refused with. `VERSION`, `UNSUPPORTED`, `BITSTREAM`
and `TRUNCATED` are not interchangeable: a transport falls back on one and
re-requests on another, so a decoder that swaps them recovers wrongly. See
`docs/SYNTAX.md` 12.

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

The encoder is written for clarity. On one desktop core, a 2048x2048 4:2:0
intra frame at QP 28 encodes in about 0.57 s and decodes in 0.07 s; with
`--no-rdo` the encode is 0.21 s, and with `--no-custom-tables` as well it is
0.29 s. The decode time is the same either way -- every encoder-side tool here
is invisible to the decoder by construction. 4:4:4 costs roughly twice as much
on both sides. Full table in [`RESULTS-intra.md`](RESULTS-intra.md).

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

## The Phase 1 gate

**The gate is not met.** Measured by `tools/quality/compare.py` on
`vr-mixed-1024` against `x264 --keyint 1`, over the 100-400 Mbit band the
criterion is stated in:

| | v1.2 BD-rate | v1.2 mean deficit | v1.3 BD-rate | v1.3 mean deficit | verdict |
|---|---|---|---|---|---|
| yuv444p | +65.79 % | -5.937 dB | **+40.35 %** | **-4.047 dB** | FAIL (needs -1.0 dB) |
| yuv420p | +43.69 % | -4.678 dB | **+25.86 %** | **-2.988 dB** | FAIL |

The full record — every before/after number, the tools that were measured and
rejected and why, the GPU cost of the wavefront, and encode times — is
**[`RESULTS-intra.md`](RESULTS-intra.md)**. The short version:

* **Directional intra** (tool bit 17) is built and on by default, and it is
  the largest single tool in the codec: **-22.5 BD-rate points on 4:4:4**
  (+65.8 % to +43.3 %) and -16.6 on 4:2:0, about 1.5 dB. Nine modes per 8x8
  block from reconstructed neighbours *inside the tile*, with the DC plane
  still coded as the tile-border neighbour source, so tiles stay independent.
  Mode 0 is the DC-plane prediction, which makes the tool a strict superset of
  v1 per block.
* It costs the decoder **nothing arithmetically** — decode time is unchanged —
  and costs it a 22-step in-tile wavefront at 4.5 % occupancy and 69 barriers
  per 4:4:4 tile. `RESULTS-intra.md` section 7 and `SYNTAX.md` 7.6 price three
  restrictions that cut that to 7 steps and 14.3 % occupancy for 1.8 % of
  rate, so Pass B has a menu rather than an estimate.
* The **16-context model** (bit 21) gives the DC plane its own CBF/LAST/LEVEL
  contexts and the intra mode its own context: a further -2.3 points on 4:4:4,
  most of it the mode context. Streams without the bit keep the v1 12-context
  tables byte for byte.
* **Sign data hiding** (bit 22) is worth -0.6 points.
* Rate-distortion quantization (v1.2) is on by default and worth -8.8 % /
  +0.92 dB, encoder only, at 2.7x encode time and no decoder cost.
* An in-tile intra **pyramid** was measured before being built and rejected;
  so was a 4-byte tile header. The largest untried item is now a **4x4
  transform split**, which is exactly the regime directional prediction
  creates.
* Encode is **2.9-3.4x** slower with the v2 tools; decode is unchanged.

One caveat that belongs next to the verdict: all of this material is synthetic
and 75 % of its pixels are horizontally constant, which is close to the best
case for x264 and the worst case for a block-mean predictor plus an 8x8 DCT.
It is also close to the best case for directional intra, which is part of why
the tool beat its own prediction by half again. The next measurement that
matters is a real capture.

### The v1.3 default, and how to get v1.2 back

```sh
nxv-enc --in in.yuv --w 2048 --h 1024 --pix yuv444p --qp 16 --out out.nxv
#   == --intra-dir on --ctx v2 --sign-hide --split4x4 on --cfl on
#      (tools 17, 19, 21, 22, 24 set)

nxv-enc ... --split4x4 off --cfl off                    # a v1.4 stream
nxv-enc ... --split4x4 off --cfl off \
            --intra-dir off --ctx v1 --no-sign-hide     # a v1.2 stream
```

`--lossless` forces `--no-sign-hide`: hiding a sign spends one level step, so
it cannot coexist with bit-exact coding, and a stream declaring both
`LOSSLESS` and `SIGN_HIDE` is refused (`r17`).

### Gap analysis (as first written, and how it held up)

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

**How that analysis held up when it was measured** (`RESULTS-intra.md`): item 4
was right, and is now done (+0.92 dB). Item 1 was right about the *cause* but
its size was overstated at the Phase 1 operating point, where directional intra
is worth about 1 dB rather than the whole gap. Item 2 was wrong for this band:
the 8-byte header is a low-rate fact, and at 100-400 Mbit it is 3 % of the
frame. Item 3 stands unmeasured. What none of the four saw is that the deficit
is a roughly constant ~1.8x bit-efficiency factor spread across the quantizer,
the context model and the predictor, with no single dominant term -- which is
why no single tool on the list closes it.

**And how *that* held up.** It was right that no single tool closes the gate
and wrong about the ordering: item 1 turned out to be worth 1.5 dB rather than
the ~1 dB the oracle measurement predicted, and item 3's context model, which
stood unmeasured, is worth 0.2. Directional intra beat its own proxy because
the proxy could not see the two things that matter most about it -- that mode 0
is the DC plane, so a block never pays for a bad mode, and that a
well-predicted block improves the references of every block after it in the
tile. Three tools later the factor is down from ~1.8x to ~1.4x on 4:4:4 and
~1.26x on 4:2:0, and the remaining 3-4 dB still has no single dominant term.

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
