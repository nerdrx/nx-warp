"""Planar 8-bit YUV 4:2:0 frames, memory-mapped sequences, and BT.709 colour.

A :class:`Frame` is three float32 planes (Y at WxH, Cb/Cr at W/2 x H/2) held in
the 0..255 full-range 8-bit domain.  Float rather than uint8 because every
stage of the simulator (warp, upsample, prediction blend, residual) is
naturally fractional; the only places that clamp and round to 8 bits are the
final reconstruction and anything handed to ffmpeg.
"""

from __future__ import annotations

import os
from dataclasses import dataclass

import numpy as np

# Weighted-PSNR weights used throughout the paper's quality work: (6Y+Cb+Cr)/8.
YCBCR_WEIGHTS = (6.0 / 8.0, 1.0 / 8.0, 1.0 / 8.0)


@dataclass
class Frame:
    y: np.ndarray
    cb: np.ndarray
    cr: np.ndarray

    @property
    def shape(self) -> tuple[int, int]:
        return self.y.shape  # type: ignore[return-value]

    def planes(self) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        return self.y, self.cb, self.cr

    def copy(self) -> "Frame":
        return Frame(self.y.copy(), self.cb.copy(), self.cr.copy())

    def to_bytes(self) -> bytes:
        out = bytearray()
        for p in self.planes():
            out += np.clip(np.rint(p), 0, 255).astype(np.uint8).tobytes()
        return bytes(out)


def frame_nbytes(w: int, h: int) -> int:
    return w * h + 2 * (w // 2) * (h // 2)


def frame_from_bytes(buf: memoryview | bytes, w: int, h: int) -> Frame:
    a = np.frombuffer(buf, dtype=np.uint8)
    n = w * h
    c = (w // 2) * (h // 2)
    y = a[:n].reshape(h, w).astype(np.float32)
    cb = a[n : n + c].reshape(h // 2, w // 2).astype(np.float32)
    cr = a[n + c : n + 2 * c].reshape(h // 2, w // 2).astype(np.float32)
    return Frame(y, cb, cr)


class YuvSequence:
    """Read-only memory-mapped yuv420p sequence."""

    def __init__(self, path: str, w: int, h: int):
        self.path = path
        self.w = w
        self.h = h
        self.fsize = frame_nbytes(w, h)
        total = os.path.getsize(path)
        if total % self.fsize:
            raise ValueError(f"{path}: {total} bytes is not a whole number of {w}x{h} frames")
        self.count = total // self.fsize
        self._mm = np.memmap(path, dtype=np.uint8, mode="r")

    def __len__(self) -> int:
        return self.count

    def __getitem__(self, i: int) -> Frame:
        if not 0 <= i < self.count:
            raise IndexError(i)
        off = i * self.fsize
        return frame_from_bytes(self._mm[off : off + self.fsize].tobytes(), self.w, self.h)


def write_sequence(path: str, frames) -> None:
    with open(path, "wb") as fh:
        for f in frames:
            fh.write(f.to_bytes())


# --- colour --------------------------------------------------------------
#
# The synthetic material is authored in RGB and converted with the full-range
# BT.709 matrix.  Full range (not the limited-range 16..235 a hardware decoder
# emits) because the simulator never leaves the 8-bit domain and matching
# ffmpeg's default -color_range setting keeps the base round trip neutral; the
# real hybrid decoder's limited-range -> YCoCg-R step is a separate concern
# discussed in docs/HYBRID.md.


def rgb_to_ycbcr(rgb: np.ndarray) -> np.ndarray:
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    cb = (b - y) / 1.8556 + 128.0
    cr = (r - y) / 1.5748 + 128.0
    return np.stack([y, cb, cr], axis=-1)
