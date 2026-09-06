#!/usr/bin/env python3
"""Prepare PNG inputs for the texture-native ASTC feasibility study.

Source: inter_pan8.yuv, 12 frames, 1088x1088, 4:2:0 planar 8-bit.
Emits, per frame:
  luma_c64/fNN.png     1088x1088 grey  (tile cell 64; used for 4x4 and 8x8)
  luma_c66/fNN.png     1122x1122 grey  (tile cell 66; used for 6x6, edge-replicated pad)
  chroma_c32/fNN.png    544x544  RGB   (R=U, G=V, B=128)  cell 32
  chroma_c36/fNN.png    612x612  RGB   (cell 36, for 6x6 on chroma)
  rgb_c64/fNN.png      1088x1088 RGB   (BT.709 limited-range YUV420 -> RGB, option A)
"""
import os, sys
import numpy as np
from PIL import Image

SRC = "/run/media/nerdrx/Lex/claude/nx-scratch/inter_pan8.yuv"
OUT = "/run/media/nerdrx/Lex/claude/nx-scratch/texnative-host/img"
W = H = 1088
CW = CH = 544
NF = 12
TILE = 64
NT = 17  # tiles per row/col
FRAME_BYTES = W * H + 2 * CW * CH
assert FRAME_BYTES == 1775616, FRAME_BYTES


def read_frames():
    raw = np.fromfile(SRC, dtype=np.uint8)
    assert raw.size == NF * FRAME_BYTES, (raw.size, NF * FRAME_BYTES)
    raw = raw.reshape(NF, FRAME_BYTES)
    ys = raw[:, : W * H].reshape(NF, H, W)
    us = raw[:, W * H : W * H + CW * CH].reshape(NF, CH, CW)
    vs = raw[:, W * H + CW * CH :].reshape(NF, CH, CW)
    return ys, us, vs


def tilesheet(plane, tile, cell):
    """Lay NTxNT tiles of size `tile` on a grid of pitch `cell` (cell>=tile),
    padding each cell by replicating the tile's edge pixels."""
    n = NT
    out = np.zeros((n * cell, n * cell), dtype=plane.dtype)
    for r in range(n):
        for c in range(n):
            t = plane[r * tile : (r + 1) * tile, c * tile : (c + 1) * tile]
            pad = cell - tile
            if pad:
                t = np.pad(t, ((0, pad), (0, pad)), mode="edge")
            out[r * cell : (r + 1) * cell, c * cell : (c + 1) * cell] = t
    return out


def tilesheet3(planes, tile, cell):
    chans = [tilesheet(p, tile, cell) for p in planes]
    return np.stack(chans, axis=-1)


def yuv2rgb(y, u, v):
    """BT.709 FULL range (the clip measures Y in 0..255) -> RGB."""
    u4 = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1).astype(np.float32) - 128.0
    v4 = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1).astype(np.float32) - 128.0
    yf = y.astype(np.float32)
    r = yf + 1.5748 * v4
    g = yf - 0.1873 * u4 - 0.4681 * v4
    b = yf + 1.8556 * u4
    return np.clip(np.stack([r, g, b], -1) + 0.5, 0, 255).astype(np.uint8)


def main():
    for d in ("luma_c64", "luma_c66", "chroma_c32", "chroma_c36", "rgb_c64"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)
    ys, us, vs = read_frames()
    for f in range(NF):
        y, u, v = ys[f], us[f], vs[f]
        Image.fromarray(tilesheet(y, 64, 64), "L").save(f"{OUT}/luma_c64/f{f:02d}.png")
        Image.fromarray(tilesheet(y, 64, 66), "L").save(f"{OUT}/luma_c66/f{f:02d}.png")
        b = np.full_like(u, 128)
        Image.fromarray(tilesheet3([u, v, b], 32, 32), "RGB").save(f"{OUT}/chroma_c32/f{f:02d}.png")
        Image.fromarray(tilesheet3([u, v, b], 32, 36), "RGB").save(f"{OUT}/chroma_c36/f{f:02d}.png")
        rgb = yuv2rgb(y, u, v)
        sheet = np.stack([tilesheet(rgb[:, :, i], 64, 64) for i in range(3)], -1)
        Image.fromarray(sheet, "RGB").save(f"{OUT}/rgb_c64/f{f:02d}.png")
        print("frame", f, "done", flush=True)
    # a single 64x64 luma tile, for per-invocation timing
    Image.fromarray(ys[0][:64, :64], "L").save(f"{OUT}/tile64.png")
    Image.fromarray(np.pad(ys[0][:64, :64], ((0, 2), (0, 2)), mode="edge"), "L").save(
        f"{OUT}/tile66.png"
    )
    # plain full-frame luma (no tiling), for timing the 1088x1088 case
    Image.fromarray(ys[0], "L").save(f"{OUT}/full_luma.png")


if __name__ == "__main__":
    main()
