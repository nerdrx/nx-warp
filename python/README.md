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

### There is no shared library yet

`ref/CMakeLists.txt` builds `nxvc_ref` as a **STATIC** library. `ctypes` cannot
load a `.a`, so out of a stock build tree `nxvc.NXVC_AVAILABLE` is `False` and
`nxvc.NXVC_LOAD_ERROR` explains exactly this.

> **Request to `ref/`: please add an `nxvc_ref_shared` target producing
> `libnxvc_ref.so`** (the same sources with `POSITION_INDEPENDENT_CODE ON`, or
> an `OBJECT` library shared by both). `tests/python/CMakeLists.txt` already
> looks for a target of that name and hands its path to the test suite, so the
> end-to-end tests start running the moment it exists. Nothing in `ref/` was
> modified to add this note.

Until then, build one yourself -- this does not touch the repository:

```sh
c++ -std=c++20 -O2 -fPIC -shared -Iinclude -Iref/src ref/src/*.cpp \
    -o /run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/libnxvc_ref.so
export NXVC_LIBRARY=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/libnxvc_ref.so
```

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
offsets from SYNTAX.md spelled out; dataclass and ctypes-struct round trips
(including struct sizes, so an ABI drift is caught); metric identities;
end-to-end encode/decode with lossless bit-exactness, rate/quality
monotonicity, per-tile maps, and a cross-check that the pure-Python parser and
the C decoder agree tile for tile on the same bytes.

---

## Versioning

`src/nxvc/_version.py` carries the version statically; there is no version
symbol in the C ABI to derive it from. `python/tests/test_version.py` asserts
it against `project(nxwarp VERSION ...)` in the root `CMakeLists.txt`, and
asserts `NXVC_VERSION`, the tool-bit numbers and `NXVC_TOOLS_SUPPORTED`
against `include/nxvc/nxvc.h`, so the two cannot drift apart silently while the
repository is checked out. Those tests skip when only the wheel is installed.
