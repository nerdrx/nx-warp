"""Shared fixtures: hand-built bitstreams and the library-availability skip."""

from __future__ import annotations

import struct

import pytest

import nxvc
from nxvc import bitstream as bs

requires_library = pytest.mark.skipif(
    not nxvc.NXVC_AVAILABLE,
    reason=(
        "the nxvc shared library was not loaded. ref/ has an nxvc_ref_shared "
        "target; build it and point NXVC_LIBRARY at the result:\n"
        "  cmake --build build-ref --target nxvc_ref_shared\n"
        "  NXVC_LIBRARY=build-ref/ref/libnxvc_ref.so python -m pytest python/tests\n"
        "A library OLDER than include/nxvc/nxvc.h is refused rather than "
        "loaded: the struct layouts would disagree silently.  See "
        "python/README.md, and `python -m nxvc probe` for the full reason."
    ),
)


def make_stream_header(width=64, height=64, tlvs=(), **kwargs) -> bs.StreamHeader:
    """A valid 4:2:0 stream header with the mandatory tool bit set."""
    hdr = bs.StreamHeader(
        width=width,
        height=height,
        tools=kwargs.pop("tools", nxvc.Tool.INTRA_DC_PLANE),
        **kwargs,
    )
    hdr.tlvs = list(tlvs)
    return hdr


def make_tile(payload=b"", **kwargs) -> tuple[bs.TileHeader, bytes]:
    hdr = bs.TileHeader(payload_len=len(payload), **kwargs)
    return hdr, payload


def make_frame(
    stream: bs.StreamHeader,
    frame_number=0,
    base_qp=28,
    tiles_per_row=None,
    payload=b"",
    **kwargs,
) -> bytes:
    """Serialize one frame unit: header, then one row structure per tile row.

    Every tile carries the same *payload*, which is opaque here -- the parser
    never looks inside it.
    """
    tiles_x = stream.tiles_x if tiles_per_row is None else tiles_per_row
    kwargs.setdefault("mode", nxvc.TileMode.INTRA)
    body = bytearray()
    for row in range(stream.tiles_y):
        rh = bs.TileRowHeader(
            frame_number=frame_number, row_index=row, tile_count=tiles_x, skip_bitmap=0
        )
        body += rh.pack()
        for i in range(tiles_x):
            th = bs.TileHeader(
                tile_index=i,
                payload_len=len(payload),
                **kwargs,
            )
            body += th.pack() + payload
    hdr = bs.FrameHeader(
        frame_number=frame_number,
        base_qp=base_qp,
        frame_bytes=bs.FrameHeader.SIZE + len(body),
    )
    return hdr.pack() + bytes(body)


def make_bitstream(width=64, height=64, frames=1, **kwargs) -> bytes:
    """A whole hand-built ``.nxv`` buffer that the parser accepts."""
    stream = make_stream_header(width, height)
    out = bytearray(stream.pack())
    for n in range(frames):
        out += make_frame(stream, frame_number=n, **kwargs)
    return bytes(out)


@pytest.fixture
def stream_header() -> bs.StreamHeader:
    return make_stream_header()


@pytest.fixture
def one_frame_stream() -> bytes:
    return make_bitstream()
