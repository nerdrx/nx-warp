"""Raw planar YUV sequence I/O.

The harness's interchange format is headerless planar YUV, 8 bit, either
``yuv444p`` or ``yuv420p``, exactly what ``ffmpeg -pix_fmt`` names and exactly
what ``nxv-enc --pix`` accepts.  A sequence is one file; frame ``i`` starts at
``i * frame_bytes``.

Geometry is described by :class:`Format`.  Everything else in the harness takes
and returns :class:`Frame` objects, which are three ``uint8`` planes.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import Iterator

import numpy as np

PIX_FMTS = ("yuv444p", "yuv420p")

# Rec.709 / BT.601-style plane weights used for the "YCbCr weighted" PSNR the
# video-coding literature reports (JVET/HM convention: (6*Y + Cb + Cr) / 8).
YCBCR_WEIGHTS = (6.0 / 8.0, 1.0 / 8.0, 1.0 / 8.0)


@dataclass(frozen=True)
class Format:
    """Geometry of a raw planar YUV sequence."""

    width: int
    height: int
    pix_fmt: str = "yuv444p"

    def __post_init__(self) -> None:
        if self.pix_fmt not in PIX_FMTS:
            raise ValueError(f"unsupported pix_fmt {self.pix_fmt!r}, want one of {PIX_FMTS}")
        if self.width <= 0 or self.height <= 0:
            raise ValueError("width and height must be positive")
        if self.pix_fmt == "yuv420p" and (self.width % 2 or self.height % 2):
            raise ValueError("yuv420p needs even width and height")

    @property
    def subsampling(self) -> tuple[int, int]:
        """(horizontal, vertical) chroma decimation factors."""
        return (1, 1) if self.pix_fmt == "yuv444p" else (2, 2)

    @property
    def chroma_size(self) -> tuple[int, int]:
        sx, sy = self.subsampling
        return (self.width // sx, self.height // sy)

    @property
    def plane_shapes(self) -> tuple[tuple[int, int], ...]:
        cw, ch = self.chroma_size
        return ((self.height, self.width), (ch, cw), (ch, cw))

    @property
    def frame_bytes(self) -> int:
        return sum(h * w for h, w in self.plane_shapes)

    def frame_count(self, path: str | os.PathLike) -> int:
        size = os.path.getsize(path)
        if size % self.frame_bytes:
            raise ValueError(
                f"{path}: size {size} is not a whole number of {self.frame_bytes}-byte frames "
                f"({self.width}x{self.height} {self.pix_fmt})"
            )
        return size // self.frame_bytes


@dataclass
class Frame:
    """One decoded frame: three uint8 planes, Y first."""

    y: np.ndarray
    u: np.ndarray
    v: np.ndarray

    @property
    def planes(self) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        return (self.y, self.u, self.v)

    @property
    def shape(self) -> tuple[int, int]:
        return self.y.shape  # type: ignore[return-value]

    def copy(self) -> "Frame":
        return Frame(self.y.copy(), self.u.copy(), self.v.copy())

    def tobytes(self) -> bytes:
        return b"".join(np.ascontiguousarray(p, dtype=np.uint8).tobytes() for p in self.planes)

    @classmethod
    def gray(cls, fmt: Format, y_value: int = 128) -> "Frame":
        cw, ch = fmt.chroma_size
        return cls(
            np.full((fmt.height, fmt.width), y_value, np.uint8),
            np.full((ch, cw), 128, np.uint8),
            np.full((ch, cw), 128, np.uint8),
        )


def read_frame(path: str | os.PathLike, fmt: Format, index: int = 0) -> Frame:
    """Read a single frame by index."""
    with open(path, "rb") as fh:
        fh.seek(index * fmt.frame_bytes)
        buf = fh.read(fmt.frame_bytes)
    if len(buf) != fmt.frame_bytes:
        raise EOFError(f"{path}: short read for frame {index}")
    return _frame_from_bytes(buf, fmt)


def _frame_from_bytes(buf: bytes, fmt: Format) -> Frame:
    planes = []
    off = 0
    for h, w in fmt.plane_shapes:
        n = h * w
        planes.append(np.frombuffer(buf, np.uint8, count=n, offset=off).reshape(h, w))
        off += n
    return Frame(*planes)


def read_sequence(path: str | os.PathLike, fmt: Format, limit: int | None = None) -> Iterator[Frame]:
    """Yield frames from a raw YUV file, streaming (one frame in memory)."""
    n = fmt.frame_bytes
    with open(path, "rb") as fh:
        i = 0
        while limit is None or i < limit:
            buf = fh.read(n)
            if not buf:
                return
            if len(buf) != n:
                raise EOFError(f"{path}: truncated frame {i}")
            yield _frame_from_bytes(buf, fmt)
            i += 1


class SequenceWriter:
    """Append frames to a raw YUV file."""

    def __init__(self, path: str | os.PathLike, fmt: Format):
        self.path = str(path)
        self.fmt = fmt
        os.makedirs(os.path.dirname(os.path.abspath(self.path)) or ".", exist_ok=True)
        self._fh = open(self.path, "wb")
        self.count = 0

    def write(self, frame: Frame) -> None:
        for (h, w), plane in zip(self.fmt.plane_shapes, frame.planes):
            if plane.shape != (h, w):
                raise ValueError(f"plane shape {plane.shape} != expected {(h, w)}")
            self._fh.write(np.ascontiguousarray(plane, dtype=np.uint8).tobytes())
        self.count += 1

    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> "SequenceWriter":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def write_sequence(path: str | os.PathLike, fmt: Format, frames) -> int:
    with SequenceWriter(path, fmt) as w:
        for f in frames:
            w.write(f)
        return w.count


# --- colour helpers ------------------------------------------------------


def rgb_to_yuv444(rgb: np.ndarray) -> Frame:
    """BT.709 limited-range RGB(uint8) -> YUV 4:4:4 planes (uint8)."""
    r, g, b = (rgb[..., i].astype(np.float32) for i in range(3))
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    u = (b - y) / 1.8556
    v = (r - y) / 1.5748
    yq = np.clip(np.rint(16.0 + y * (219.0 / 255.0)), 0, 255).astype(np.uint8)
    uq = np.clip(np.rint(128.0 + u * (224.0 / 255.0)), 0, 255).astype(np.uint8)
    vq = np.clip(np.rint(128.0 + v * (224.0 / 255.0)), 0, 255).astype(np.uint8)
    return Frame(yq, uq, vq)


def yuv_to_rgb(frame: Frame) -> np.ndarray:
    """BT.709 limited-range YUV -> RGB (uint8, HxWx3), the exact inverse of
    :func:`rgb_to_yuv444`.

    4:2:0 chroma is upsampled by pixel replication, which is what a 2x nearest
    expansion of the codec's own reconstruction grid gives; the metrics that
    consume this are luminance-driven, so the chroma interpolant is not a
    meaningful degree of freedom here.
    """
    y = (frame.y.astype(np.float32) - 16.0) * (255.0 / 219.0)
    u = (frame.u.astype(np.float32) - 128.0) * (255.0 / 224.0)
    v = (frame.v.astype(np.float32) - 128.0) * (255.0 / 224.0)
    if u.shape != y.shape:
        sy = y.shape[0] // u.shape[0]
        sx = y.shape[1] // u.shape[1]
        u = np.repeat(np.repeat(u, sy, axis=0), sx, axis=1)[: y.shape[0], : y.shape[1]]
        v = np.repeat(np.repeat(v, sy, axis=0), sx, axis=1)[: y.shape[0], : y.shape[1]]
    b = u * 1.8556 + y
    r = v * 1.5748 + y
    g = (y - 0.2126 * r - 0.0722 * b) / 0.7152
    return np.clip(np.rint(np.stack((r, g, b), axis=-1)), 0, 255).astype(np.uint8)


def downsample_chroma(frame: Frame) -> Frame:
    """4:4:4 -> 4:2:0 by 2x2 box average of the chroma planes."""
    def box(p: np.ndarray) -> np.ndarray:
        h, w = p.shape
        q = p[: h - h % 2, : w - w % 2].astype(np.uint16)
        return np.rint(
            (q[0::2, 0::2] + q[0::2, 1::2] + q[1::2, 0::2] + q[1::2, 1::2]) / 4.0
        ).astype(np.uint8)

    return Frame(frame.y.copy(), box(frame.u), box(frame.v))


def to_format(frame: Frame, fmt: Format) -> Frame:
    """Convert a 4:4:4 frame to the requested output format."""
    if fmt.pix_fmt == "yuv420p" and frame.u.shape == frame.y.shape:
        return downsample_chroma(frame)
    return frame


# --- pose logs -----------------------------------------------------------


def write_pose_log(path: str | os.PathLike, poses: list[dict]) -> None:
    """Write the per-frame pose log (JSON, one object per frame)."""
    os.makedirs(os.path.dirname(os.path.abspath(str(path))) or ".", exist_ok=True)
    with open(path, "w") as fh:
        json.dump({"version": 1, "frames": poses}, fh, indent=1)
        fh.write("\n")


def read_pose_log(path: str | os.PathLike) -> list[dict]:
    with open(path) as fh:
        doc = json.load(fh)
    return doc["frames"] if isinstance(doc, dict) else doc
