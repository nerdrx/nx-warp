# nxvc -- Python bindings for the NX Warp codec

Pure Python over `ctypes`. There is no compiled extension: the package binds
the C ABI in [`include/nxvc/nxvc.h`](../include/nxvc/nxvc.h) at runtime, so
there is no wheel to match against a compiler, an ABI tag or a Python version.
`numpy` is the only dependency.

The package has two halves, and **the useful one works without the codec**:

| module | needs the C library | what it is |
|---|---|---|
| `nxvc.bitstream` | no | a from-scratch parser for the stream, frame, tile-row and tile headers, straight out of `docs/SYNTAX.md` |
| `nxvc.metrics` | no | PSNR / SSIM / MS-SSIM, delegating to `tools/quality`'s `nxq.metrics` when it is importable |
| `nxvc.codec` | **yes** | `Encoder` / `Decoder`, numpy planes in and out |
| `nxvc.cli` | `info` no, `encode`/`decode` yes | `python -m nxvc`, mirroring `nxv-enc` / `nxv-dec` / `nxv-info` |

---

## Install

```sh
python3 -m venv --system-site-packages /run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/venv-py
V=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/venv-py
$V/bin/pip install pytest
$V/bin/pip install -e python/          # from the repository root
$V/bin/python -m nxvc probe
```

`probe` is the first thing to run when something does not work: it prints the
package version, whether the shared library was found and where, which metrics
backend is live, and -- when the library is missing -- every directory that was
searched and how to build one.

---

## Finding the shared library

The loader tries, in order:

1. `$NXVC_LIBRARY` -- a path to the library file. Set this and nothing else is
   consulted.
2. `$NXVC_LIBRARY_PATH` (a `:`-separated list of directories) and
   `$NXVC_BUILD_DIR` (a CMake build tree; `<dir>/ref` and `<dir>` are searched).
3. Every `build*/` directory of the nx-warp checkout the package was installed
   from, most recently modified first, looking in `<build>/ref`, `<build>` and
   `<build>/lib`.
4. The system loader (`libnxvc_ref.so`, then `libnxvc.so`).

On each of those it accepts `libnxvc_ref.so` or `libnxvc.so` (`.dylib` on
macOS, `.dll` on Windows).

### Building it

`ref/CMakeLists.txt` has an **`nxvc_ref_shared`** target producing
`libnxvc_ref.so` beside the static `libnxvc_ref.a` that the default target
builds. `ctypes` cannot load a `.a`, so a build tree has to be asked for it:

```sh
cmake --build build-ref --target nxvc_ref_shared
export NXVC_LIBRARY=$PWD/build-ref/ref/libnxvc_ref.so
```

`tests/python/CMakeLists.txt` hands that target's path to the pytest suite
automatically, so through ctest nothing needs setting.

Two loader behaviours are worth knowing:

* A library that loads but is **missing a symbol** this binding declares is
  refused, not used. It was built from an older `include/nxvc/nxvc.h`, and its
  structure layouts would disagree with these ones silently -- `nxvc_tile_info`
  grew four fields between syntax v1.2 and v1.4, so a stale library would hand
  back per-tile records read at the wrong stride. Rebuild it.
* A library carrying a **sanitizer runtime** is never picked up by the
  automatic search. Loading one through `ctypes` aborts the interpreter before
  `main` -- "ASan runtime does not come first in initial library list" -- with
  no output and no traceback, which is a miserable thing to debug in a
  checkout that has a `build-asan-ubsan/` in it. Candidates are checked for
  `__asan_init` and friends before being loaded, and instrumented-looking
  build trees are searched last anyway. `NXVC_LIBRARY` still reaches one for
  anyone who means it.

---

## Inspecting a stream (no library needed)

```python
from nxvc import bitstream as bs

stream = bs.parse_stream(open("out.nxv", "rb").read())
print(stream.header.width, stream.header.height, stream.header.tool_names())
print(stream.header.color_space_name)          # descriptive, SYNTAX.md 2.2

frame = stream.frames[0]
print(frame.header.base_qp, len(frame.tiles), frame.payload_bytes)
for tile in frame.tiles[:4]:
    t = tile.header
    print(t.tile_index, t.mode_name, t.res_level,
          t.resolved_qp(frame.header.base_qp), t.payload_len)

print(bs.phase1_reject_reason(stream.header, frame))   # None if a Phase 1
                                                       # decoder would accept it
```

Every header is a dataclass with a `pack()` that round-trips, so a bitstream
can be built by hand in a test, or a field flipped and re-serialized:

```python
hdr = bs.StreamHeader(width=2048, height=2048, chroma_format=1)
raw = hdr.pack()                       # 64 bytes
assert bs.StreamHeader.parse(raw) == hdr
```

The parser stops at the entropy-coded payload -- tile payloads are sliced out
as bytes, never decoded. The reference codec is the specification; nothing here
is a second opinion about pixels.

**Adding a field the syntax grows** is one line in the `FIELDS` table of the
structure concerned (`("color_space", 42, "B")` is exactly how `color_space`
was added), plus its rule in `validate()`. Byte ranges no field claims are
carried through a parse/pack cycle verbatim in `reserved`.

Four more tables carry the rest of the shape, and between them they are where a
syntax revision lands:

| table | what it drives |
|---|---|
| `FRAME_SECTIONS` | every block between the frame header and the first tile row, in wire order. `warp_ext()` is one row of it |
| `_TILE_EXTRAS` | the optional bytes after a tile header -- a `u16` disparity for a `STEREO` tile, an `i8` MV pair otherwise, a constant-alpha byte |
| `_FRAME_RULES` / `_TILE_RULES` | the constraints that need two structures to check: a frame flag against the stream's tool mask, a tile mode against the frame's `warp_present` |

The order of the rules is part of the specification, not an implementation
detail. A `STEREO` tile on the left eye must be reported as exactly that and
not as a malformed disparity, because until the eye is right those two bytes
are not a disparity at all -- `test_vectors.py` pins the reason each rejection
vector is refused for, not merely that it is refused.

### Stereo geometry

A picture is one eye. `width` and `height` in the stream header are **per eye**,
and a stereo frame carries two pictures rather than one double-wide one, so:

```python
hdr.cols_per_eye     # tile columns of one eye -- what tile_index and
                     # skip_bitmap are indexed by, and what the 64-bit
                     # bitmap bounds
hdr.cols             # eyes * cols_per_eye, the transport's grid width
hdr.rows             # tile rows
hdr.tile_row_count   # eyes * rows structures per frame, row-major eye-minor
hdr.tile_count       # cols * rows, across the eye pair
hdr.tile_first(row, eye, tile_index)   # the transport's linear index
```

The eye of a tile row is **positional** -- it is not a field of the row header
-- and `TileRow.eye` carries what the position said, which `parse_frame` checks
each tile's own `eye` field against.

---

## Encoding and decoding

```python
import numpy as np, nxvc

W = H = 2048
planes = [np.zeros((H, W), np.uint8),               # Y
          np.zeros((H, W), np.uint8),               # Co / U
          np.zeros((H, W), np.uint8)]               # Cg / V

with nxvc.Encoder(W, H, pix="yuv444p", base_qp=24) as enc:
    data = bytearray(enc.stream_header())
    data += enc.encode(planes)
    for t in enc.tiles()[:4]:
        print(t.tile_index, t.mode_name, t.qp, t.payload_len)

with nxvc.Decoder() as dec:
    info, consumed = dec.parse_stream_header(bytes(data))
    out, used = dec.decode(bytes(data)[consumed:])   # list of numpy planes
```

or walk a whole file:

```python
with nxvc.Decoder() as dec:
    for planes in dec.frames(open("out.nxv", "rb").read()):
        ...
```

### Per-tile foveation maps

`qp_map` and `res_map` are `uint8` arrays with one entry per tile in raster
order, shaped `(tiles_y, tiles_x)` or flat:

```python
with nxvc.Encoder(2048, 2048, pix="yuv444p", base_qp=30) as enc:
    enc.stream_header()
    qp = np.full(enc.layout.shape, 30, np.uint8)
    qp[12:20, 12:20] = 18                   # a sharper fovea
    res = np.zeros(enc.layout.shape, np.uint8)
    res[:4, :] = 1                          # code the top rows at 32x32
    frame = enc.encode(planes, qp_map=qp, res_map=res)
    print(enc.tile_map("qp"))               # what the codec actually used
```

`enc.tile_map(attr)` returns any `TileInfo` attribute as a `(tiles_y,
tiles_x)` array -- handy for plotting where the bits went.

### Bit accounting

```python
with nxvc.Encoder(W, H, collect_stats=1) as enc:
    enc.stream_header(); enc.encode(planes)
    st = enc.stats()
    print(st.bytes_payload, st.bytes_tile_headers, st.overhead_bytes,
          st.tiles_res)
```

### Pose and TLVs

```python
enc.add_tlv(0x8001, b"anything")        # before stream_header()
enc.set_pose(pose_26_bytes)             # before the next encode()
```

Pose is 26 opaque bytes (SYNTAX.md 3.2). `FrameHeader.decode_pose()` will
interpret them as 7 halves plus 3 floats for a human, which the codec itself
never does.

---

## Metrics

```python
from nxvc import metrics
print(metrics.BACKEND)                  # "nxq" or "numpy"
metrics.psnr(ref_y, dis_y)
metrics.psnr_planes(ref_planes, dis_planes)["psnr_ycbcr"]
metrics.ssim(ref_y, dis_y)
metrics.compare_frames(ref_planes, dis_planes, do_ms_ssim=True)
```

When `tools/quality` is importable these delegate to `nxq.metrics`, so a number
printed here and a number in a `compare.py` report cannot drift apart. The
weighted PSNR is formed in the MSE domain, the JVET convention -- not an
average of dB values.

---

## Command line

```sh
python -m nxvc probe
python -m nxvc info out.nxv [--tiles] [--json] [--frames N] [--library]

python -m nxvc encode --in in.yuv --w 2048 --h 2048 --pix yuv444p --qp 24 \
                      --qp-map qp.map --res-map res.map --out out.nxv
python -m nxvc decode --in out.nxv --out out.yuv [--pix yuv444p]
```

`encode` and `decode` take the same flags as `nxv-enc` and `nxv-dec`
(`--frames`, `--lossless`, `--tskip`, `--nsub`, `--matrix`, `--chroma-qp-off`,
`--custom-tables`, `--tile-420`, `--rgb`, `--quiet`), plus `--color-space`
for the descriptive stream field; `--rgb` implies `--color-space rgb`, as
SYNTAX.md 2.2 requires. A `nxvc` console script is installed as well, so
`nxvc info out.nxv` works once the package is on the PATH.

The encoder tuning and v2 intra tool flags mirror `nxv-enc` too:

| flag | what it sets |
|---|---|
| `--rdo` / `--no-rdo`, `--rdo-lambda F` | RD trellis quantizer and its lambda |
| `--qp-search N` | per-tile `qp_delta` searched in `[-N, +N]` |
| `--wm 0..3\|auto` | per-tile weighting matrix, tool bit 20 `WM_ID` |
| `--intra-dir off\|on\|layer` | directional intra, tool bit 17; `layer` is the layered form (frame `flags` bit 2, SYNTAX.md 7.5) |
| `--intra-dir-cand N` | modes RD-checked per block |
| `--ctx v1\|v2\|v3` | 12, 16 or 27 entropy contexts, tool bits 21 `CTX_V2` and 24 `CTX_V3` |
| `--sign-hide` / `--no-sign-hide` | sign data hiding, tool bit 22 |

Each of them defaults to *unset*, meaning "whatever `nxvc_config_default()`
chose" -- the reference encoder turns the RD trellis and all three v2 intra
tools on, and passing an explicit `0` for a flag nobody mentioned would quietly
turn them off. `EncoderConfig` spells the same thing with `None`.

`info` runs on the pure-Python parser and needs no library. `--library` adds a
full decode through the reference codec, which is the only thing that can
validate the entropy-coded payload.

---

## Tests

```sh
$V/bin/python -m pytest python/tests -q                 # from the repo root
NXVC_LIBRARY=/path/to/libnxvc_ref.so $V/bin/python -m pytest python/tests -q
```

or through ctest, which is how CI runs them:

```sh
cmake -S . -B build -DNXWARP_PYTHON=$V/bin/python
ctest --test-dir build -R python.pytest -V
```

The ctest entry returns **77 (skip)**, not a failure, when the interpreter has
no pytest or numpy. The end-to-end encode/decode tests skip themselves, with
the build command in the skip reason, when no shared library is found -- so the
suite is green both with and without the codec.

Coverage: the header parser against hand-assembled byte strings with the
offsets from SYNTAX.md spelled out; dataclass and ctypes-struct round trips;
metric identities; end-to-end encode/decode with lossless bit-exactness,
rate/quality monotonicity, per-tile maps, bit accounting, and a cross-check
that the pure-Python parser and the C decoder agree tile for tile on the same
bytes.

`test_vectors.py` runs the parser over **every committed bitstream** in
`tests/vectors/`: each conformance vector must parse structurally end to end
(a frame that does not consume exactly its `frame_bytes` is an error, so a
wrong table-set size or tile-header layout cannot hide), re-pack to the bytes
it came from, and match the geometry `vectors.md5` records; each rejection
vector must be refused, and most of them for the *stated* reason. Whether a
vector is one a Phase 1 decoder must accept is read from its own tool mask
rather than from its name, so a vector added on either side of that line is
covered the day it lands. When an `nxv-info` binary exists in any build tree
its output is parsed and compared field for field -- header, every frame
header, and every `warp_ext()` matrix as raw `int32`, two independent readings
of the same 56 files.

`test_version.py` additionally parses `include/nxvc/nxvc.h` and compares every
struct's **field order, names and integer widths** against `nxvc/_ffi.py`, and
asserts that every declared function is bound. That is the check that matters:
a field inserted in the middle of a C struct is invisible to every other test
-- the calls still succeed and the values are silently wrong -- and this suite
caught exactly that when `color_space` and `nxvc_encode_stats` landed.

---

## Versioning

`src/nxvc/_version.py` carries the package version statically. `python/tests/test_version.py` asserts
it against `project(nxwarp VERSION ...)` in the root `CMakeLists.txt`, and
asserts `NXVC_VERSION`, the tool-bit numbers and `NXVC_TOOLS_SUPPORTED`
against `include/nxvc/nxvc.h`, so the two cannot drift apart silently while the
repository is checked out. Those tests skip when only the wheel is installed.

Two version numbers are in play and they are not the same thing:

* `nxvc.NXVC_VERSION` is the **bitstream version** (1) carried in every stream.
* `nxvc.NXVC_BITSTREAM_MINOR` is the revision of `docs/SYNTAX.md` that
  `nxvc.bitstream` parses (4: the Phase 2 inter path). `nxvc.library_minor()`
  reports the loaded C library's, read out of `nxvc_version_string()`. The
  library may be **ahead** while a syntax revision is landing;
  `test_version.py` asserts only that the parser is never ahead of the header,
  since claiming a revision it does not implement is the failure that matters.
