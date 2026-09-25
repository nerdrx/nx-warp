#!/usr/bin/env python3
"""Crop a shifted 256x256 centre from a 2160x2160 RGBA source for both eyes."""
from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("source", type=Path, help="raw 2160x2160 RGBA8 source")
    p.add_argument("output", type=Path, help="2*256*256 little-endian RGBA words")
    p.add_argument("--shift", type=int, default=0, help="horizontal source shift in pixels before foveation")
    args = p.parse_args()
    src = args.source.read_bytes()
    width = height = 2160
    if len(src) != width * height * 4:
        p.error(f"expected {width}x{height} RGBA8 ({width * height * 4} bytes), got {len(src)}")
    x0 = (width - 256) // 2
    y0 = (height - 256) // 2
    eye = bytearray(256 * 256 * 4)
    for y in range(256):
        sy = y0 + y
        for x in range(256):
            sx = min(width - 1, max(0, x0 + x + args.shift))
            si = (sy * width + sx) * 4
            di = (y * 256 + x) * 4
            eye[di:di + 4] = src[si:si + 4]
    out = eye + eye
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(f"shift={args.shift},input_sha256={hashlib.sha256(src).hexdigest()},output_sha256={hashlib.sha256(out).hexdigest()},bytes={len(out)}")


if __name__ == "__main__":
    main()
