# NX Warp examples

Five small programs against the public APIs. They are documentation you can run:
each one is short enough to read in a sitting, commented for someone who has
never seen the codec, and does something you will actually want done.

Nothing here is part of the library and nothing here is installed. If an example
stops compiling, an API changed and this directory is the first place that
should have noticed.

| Program | Target | Needs | What it does |
|---|---|---|---|
| `encode_frame.c` | `nxvc-example-encode` | `nxvc_ref` | raw planar YUV in, `.nxv` out |
| `decode_frame.c` | `nxvc-example-decode` | `nxvc_ref` | `.nxv` in, raw planar YUV out |
| `roundtrip_psnr.cpp` | `nxvc-example-roundtrip` | `nxvc_ref` | encode + decode in one process, per-plane PSNR |
| `tile_walk.cpp` | `nxvc-example-tilewalk` | `nxvc_ref` | dump a stream's tile layout, modes, QPs and byte map |
| `transport_loopback.cpp` | `nxvc-example-loopback` | `nxvc_transport` (+ `nxvc_ref`, optional) | packetize a frame, drop datagrams, depacketize, print the concealment map |

The first two are plain **C**, on purpose: `include/nxvc/nxvc.h` is a C ABI, and
if these ever need a C++ compiler then the ABI has grown a dependency it should
not have.

## Building

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/bin/nxvc-example-tilewalk --in stream.nxv
```

`NXWARP_BUILD_EXAMPLES` is `ON` by default; `-DNXWARP_BUILD_EXAMPLES=OFF` skips
the directory entirely.

Every target is guarded on the *targets* it needs existing, not on a
`find_package`. Components land at different times, so configuring this tree
with only some of them present is normal and must not error. When a component is
missing you get a status line, not a failure:

```
-- examples: building nxvc-example-encode nxvc-example-decode ...
-- examples: skipping loopback (needs target nxvc_transport)
```

`nxvc-example-loopback` links `nxvc_ref` too **when it exists**, which enables
its `--in file.nxv` mode; without the codec it still builds and runs on
synthetic tile sizes.

## Getting something to feed them

The examples take headerless planar 8-bit YUV, the same format the quality
harness and the reference CLIs use. Generate some:

```sh
export NXQ_SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp
python3 tools/quality/capture/gen_synthetic.py --out $NXQ_SCRATCH/corpus \
    --name vr-mixed-256 --frames 6 --eye-width 256 --eye-height 256 \
    --motion mixed --pix yuv444p
```

or `python3 corpus/fetch.py --sync`, which does the same thing for every
synthetic entry in `corpus/MANIFEST.json` and verifies the hashes. The coded
picture is side-by-side stereo, so a 256-per-eye sequence is `--w 512 --h 256`.

---

## `encode_frame.c`

```sh
nxvc-example-encode --in in.yuv --w 512 --h 256 --pix yuv444p \
                    --qp 26 --frames 3 --out out.nxv
```
```
encoded 3 frames, 37678 bytes of frame data + 64 byte header
  0.77 bits/pixel, 12.6 kB/frame
```

The five calls that make a `.nxv`: `nxvc_config_default`, `nxvc_encoder_create`,
`nxvc_encoder_stream_header` (**once**), `nxvc_encoder_encode_frame` (per frame,
appended), `nxvc_encoder_destroy`. A file is exactly
`stream_header ext_area frame*`.

The one thing to copy out of this file: always start from
`nxvc_config_default()`. A zeroed `nxvc_config` is not a legal one, and the
struct grows between versions.

## `decode_frame.c`

```sh
nxvc-example-decode --in out.nxv --out out.yuv
```
```
out.nxv: 512x256 4:4:4, 1 eye(s), 8-bit, tools 0xcd
decoded 3 frames to out.yuv
  plane sizes: 512x256 512x256 512x256
  play it back with:
    ffplay -f rawvideo -pixel_format yuv444p -video_size 512x256 out.yuv
```

Note what this program does **not** have: a `--pix` flag. Geometry comes from
`nxvc_decoder_stream_info()`, never from the command line. `nxv-dec`'s `--pix`
is a check against the stream, not an input, and a decoder that trusts the
caller instead of the bitstream is a decoder with a security bug.

The frame loop is driven by `*consumed`: frames are variable length and only the
decoder knows where each one ends.

## `roundtrip_psnr.cpp`

```sh
nxvc-example-roundtrip --in in.yuv --w 512 --h 256 --pix yuv444p --qp 26 --frames 3
```
```
frame       bytes   psnr_y   psnr_u   psnr_v  weighted
0           12622   36.510   41.087   42.179    37.333
1           12560   36.488   41.207   42.264    37.322
2           12496   36.553   41.081   42.228    37.374
---
3 frames, qp 26, 512x256 yuv444p
  mean PSNR   Y 36.517  U 41.125  V 42.224  weighted 37.343 dB
  rate        0.767 bits/pixel, 12.6 kB/frame
  at 90 Hz    9.0 Mbit/s
```

Encoder and decoder in one process, no files in between. Two invariants it
enforces rather than prints:

* `--lossless` must give `inf` on every plane. The program exits non-zero if any
  plane's PSNR is finite, so `examples.smoke` uses it as an assertion.
* the decoder must consume exactly the bytes the encoder wrote. If it does not,
  the two are desynchronised and every number after that point is meaningless,
  so it stops instead of printing more.

Averages are accumulated in the SSE domain, never as a mean of per-frame dB, and
the weighted figure is `(6·MSE_Y + MSE_Cb + MSE_Cr)/8` converted once — the JVET
convention, matching `tools/quality/nxq/metrics.py`.

**This is not the quality harness.** `tools/quality/compare.py` is: it drives the
real CLIs, runs the x264/x265 anchors, computes BD-rate and evaluates the
paper's gates. This is the ten-second version for after you touched a quantiser.

## `tile_walk.cpp`

```sh
nxvc-example-tilewalk --in out.nxv --frame 0 [--no-table] [--csv tiles.csv]
```
```
stream   out.nxv
  size          512x256, 4:4:4, 8-bit, 1 eye(s), 1 layer(s)
  profile/level 1/1, version 1, ext 0 byte(s) in 0 TLV(s) (0 unknown)
  tile grid     8 x 4 = 32 tiles of 64x64
  tools         0x00000000000000cd
                intra_dc_plane res_level chroma444 custom_tables nsub_var

frame 0  base_qp 26  chroma_qp_off +0  12622 bytes  32 tiles  flags 0x1
  modes        INTRA=32
  res_level    0=32
  table_set    0=9 3=8 4=4 5=11
  qp           26..26   tskip 0/32 tiles
  payload      11798 bytes total, 368.7 mean, 954 max (tile 28 at 4,3)
  overhead     824 bytes of frame not in tile payloads (6.5%)
  bytes/tile   p50 466  p95 886  (map scaled to p95)
   |........|
   |+*=*+*-*|
   |++-*++-*|
   |@@..@%..|
```

This is the tool you open first when a stream looks wrong. "Soft in the corners"
is a `res_level` question, "the bitrate spiked" is a per-tile bytes question,
"why is this frame three times the last one" is a mode-histogram question. The
ASCII map is bytes per tile scaled to the frame's p95, so a hot spot is visible
without reading 2312 numbers. `--csv` writes every field for a spreadsheet or a
plot.

**How the layout query works, and why it needs a decode.** Three different calls,
three different costs:

* `nxvc_tile_layout_get(w, h, &tl)` is pure arithmetic. No stream needed. This is
  the grid that `qp_map` and `res_map` are indexed in, raster order.
* `nxvc_decoder_scan_frame()` parses only the frame header: `frame_number`,
  `base_qp`, `frame_bytes`. Cheap, and enough to walk a file. **It does not give
  tile records.**
* `nxvc_decoder_tiles()` returns the records of the most recently *decoded*
  frame. The per-tile fields live in tile headers interleaved with the payloads,
  so getting them means walking the frame. The API is honest about that instead
  of pretending there is a free index.

`tile_walk` does both passes and cross-checks that `scan_frame` and
`decode_frame` agree about each frame's length — a disagreement there is a
bitstream bug, and it says so on stderr.

The overhead line (frame bytes not in any tile payload) is the number that
decides whether the transport's run packing pays for itself; compare it against
the loopback's header/directory/tag totals below.

## `transport_loopback.cpp`

```sh
nxvc-example-loopback --cols 20 --rows 12 --loss 0.08 --burst 3
nxvc-example-loopback --in out.nxv --loss 0.05 --paths 2      # real tile sizes
```
```
synthesised 240 tiles on a 20x12 grid

link        1 path(s), 8.0% datagram loss, burst 3, mtu 1400, FEC on
sender      53 datagrams (45 data, 8 parity), 42314 bytes on the wire
            240 tiles in 45 runs = 5.3 tiles/datagram (PAPER 4.1)
            overhead: 1080 header + 960 directory + 720 tag + 9914 parity bytes on 29588 tile bytes
channel     53 sent, 10 dropped (18.9%)
receiver    207 tiles delivered, 3 recovered by FEC (4 group(s), 1 unrecoverable)
presentation fresh 207  stale 0  concealed 33  undecodable 0  empty 0  of 240  -> frame is PARTIAL

receiver tile map  ('#' decoded  'c' concealed  'x' undecodable  '.' empty)
  >|####################|
  ...
   |cccccccc####cccccccc|
  ...

sender shadow map  ('#' believed received  'c' believed concealed  '?' no feedback yet)
  ...

shadow divergence: 0 tile(s)  (encoder and client agree)
```

There are no sockets. `nxvc_transport` is deliberately socket-free — the caller
supplies datagram buffers and a clock — so "sending" here is pushing a byte
vector from the `Sender` into the `Receiver`, and "loss" is not pushing it. That
is the real integration seam, which is why this is a genuine exercise of the
packetizer, the Reed-Solomon groups and the band deadlines rather than a mock of
them.

Three things to read in the output:

1. **`fresh` versus `concealed`.** 8 % datagram loss should not produce 8 %
   concealed tiles: class A carries parity (PAPER 4.4) and, with `--paths 2`,
   may be duplicated. If the two numbers track each other, FEC is not working.
   (The `channel` line reports more than `--loss` because `--burst` extends each
   drop — real WiFi loss is correlated, and an independent model flatters FEC.)
2. **The two maps.** The receiver's tile states and the sender's shadow of them
   must agree tile for tile once feedback has been applied. PAPER.md 2.11 item 4
   calls a divergence here "a permanent artefact until the next refresh"; this is
   the smallest program that can show one. **The exit code is that assertion**,
   which is what `examples.loopback_clean` and `examples.loopback_lossy` check.
3. **Tiles per datagram.** PAPER 4.1 rejected tile-as-packet on packet-rate
   arithmetic. This line is that decision measured on your geometry; `--mtu` and
   `--cols/--rows` move it.

The loopback uses `nxt::make_null_aead()` — keyed, corruption-detecting, and
**not cryptography**. The library never generates or exchanges keys
(`docs/TRANSPORT.md` 4); the integration supplies them from the WiVRn NX
handshake and picks a real backend.

Oversize tiles (a tile larger than the MTU can carry without fragmentation) are
dropped and counted rather than silently truncated. If that counter is non-zero,
raise `--mtu` or lower the QP.

---

## Tests

The examples register three ctest cases, named on the repo's
`<component>.<name>` convention:

```sh
ctest --test-dir build -R '^examples\.' --output-on-failure
```

| Test | What it proves |
|---|---|
| `examples.smoke` | encode → decode → tilewalk → roundtrip all run; the decoded file is the right size; the tile CSV contains INTRA tiles; `--lossless` is bit exact |
| `examples.loopback_clean` | a lossless link delivers every tile and the shadow agrees |
| `examples.loopback_lossy` | 8 % bursty loss conceals tiles but never diverges the shadow |

`examples.smoke` is driven by `smoke.cmake` — no Python, no ffmpeg, no test
framework, so it runs anywhere the codec builds. Its input is a repeating
printable-ASCII pattern reinterpreted as 8-bit YUV: not a picture, but legal
input with high-frequency content, which is all this test needs. Representative
material lives in `corpus/`.

See [`TESTING.md`](../TESTING.md) for the whole pyramid.
