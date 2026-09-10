#!/usr/bin/env python3
from pathlib import Path
import hashlib, struct

root = Path(__file__).parent
w, h, eyes = 2688, 2688, 2
pair_w = w * eyes
src = root / "fixture.yuv"
if not src.exists():
    y = bytearray(pair_w * h)
    uv = bytearray(pair_w * h // 2)
    for yy in range(h):
        for xx in range(pair_w):
            eye = xx // w
            x = xx % w
            y[yy * pair_w + xx] = (x * 37 + yy * 19 + eye * 73) & 255
            uv[(yy // 2) * pair_w + xx] = ((x * 11 + yy * 7 + eye * 101) & 255)
    src.write_bytes(y + uv)
    print("generated", src, len(y) + len(uv))
else:
    print("fixture", src, len(src.read_bytes()))

for name in ("native", "compact-default", "compact-large"):
    p = root / f"{name}.yuv"
    if p.exists():
        b = p.read_bytes()
        print(name, len(b), hashlib.sha256(b).hexdigest())
        if name != "native":
            pw = 1440 * eyes if name == "compact-large" else 1152 * eyes
            ph = 1440 if name == "compact-large" else 1152
            print(name, "expected", pw * ph * 3 // 2)

def unpack_axis(p, tiles=42, centre_tiles=16):
    c0 = (tiles - centre_tiles) // 2
    left = c0 * 16
    middle = centre_tiles * 64
    if p < left:
        t, q = divmod(p, 16)
        return t * 64 + q * 4 + 1
    if p < left + middle:
        t, q = divmod(p - left, 64)
        return (c0 + t) * 64 + q
    t, q = divmod(p - left - middle, 16)
    return (c0 + centre_tiles + t) * 64 + q * 4 + 1

def unpack_chroma(p, tiles, centre_tiles):
    c0 = (tiles - centre_tiles) // 2
    left = c0 * 8
    middle = centre_tiles * 32
    if p < left:
        t, q = divmod(p, 8)
        return t * 32 + q * 4 + 1
    if p < left + middle:
        t, q = divmod(p - left, 32)
        return (c0 + t) * 32 + q
    t, q = divmod(p - left - middle, 8)
    return (c0 + centre_tiles + t) * 32 + q * 4 + 1

native = (root / "native.yuv").read_bytes()
large = (root / "compact-large.yuv").read_bytes()
nw, nh, cw, ch = 5376, 2688, 2880, 1440
ny = memoryview(native)[:nw * nh]
cy = memoryview(large)[:cw * ch]
checks = []
for eye in (0, 1):
    for x, y in ((0, 0), (15, 15), (207, 207), (208, 208), (1023, 1023),
                 (1231, 1231), (1232, 1232), (1439, 1439)):
        px, py = unpack_axis(x), unpack_axis(y)
        checks.append(cy[y * cw + eye * 1440 + x] == ny[py * nw + eye * 2688 + px])
print("large representative luma matches", sum(checks), "/", len(checks))

# Chroma is the same packing at half resolution, with 21 native tiles and an
# eight-tile native centre; compare both interleaved Cb/Cr channels.
nuv = memoryview(native)[nw * nh:]
cuv = memoryview(large)[cw * ch:]
checks = []
for eye in (0, 1):
    for x, y in ((0, 0), (7, 7), (103, 103), (104, 104), (511, 511),
                 (615, 615), (616, 616), (719, 719)):
        px, py = unpack_chroma(x, 42, 16), unpack_chroma(y, 42, 16)
        for k in (0, 1):
            checks.append(cuv[y * 2880 + eye * 1440 + x * 2 + k] ==
                          nuv[py * 5376 + eye * 2688 + px * 2 + k])
print("large representative chroma matches", sum(checks), "/", len(checks))

try:
    import numpy as np
    lx = np.concatenate((np.arange(1, 832, 4), np.arange(832, 1856),
                         np.arange(1857, 2688, 4)))
    ly = lx
    cx = np.concatenate((np.arange(1, 416, 4), np.arange(416, 928),
                         np.arange(929, 1344, 4)))
    cyy = cx
    a = np.frombuffer(native, dtype=np.uint8)
    b = np.frombuffer(large, dtype=np.uint8)
    nuv3 = a[nw*nh:].reshape(nh//2, nw//2, 2)
    cuv3 = b[cw*ch:].reshape(ch//2, cw//2, 2)
    for eye in (0, 1):
        assert np.array_equal(b[:cw * ch].reshape(ch, cw)[:, eye*1440:(eye+1)*1440],
                              a[:nw * nh].reshape(nh, nw)[np.ix_(ly, eye*2688 + lx)])
        assert np.array_equal(cuv3[:, eye*720:(eye+1)*720],
                              nuv3[np.ix_(cyy, eye*1344 + cx)])
    print("full axis exactness: PASS")
except ImportError:
    print("full axis exactness: SKIP (numpy unavailable)")
