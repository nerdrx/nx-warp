"""Round trips: dataclass -> bytes -> dataclass, and ctypes struct -> dataclass.

The ctypes half runs without the shared library: the structure definitions in
``nxvc._ffi`` are Python objects and can be filled in by hand, which is exactly
what makes them worth testing separately from an end-to-end encode.
"""

from __future__ import annotations

import ctypes
import struct

import pytest

import nxvc
from nxvc import _ffi, bitstream as bs
from nxvc.codec import FrameInfo, StreamInfo, TileInfo, TileLayout, plane_shapes

from conftest import make_stream_header


# --------------------------------------------------------- byte round trips


def test_stream_header_round_trip():
    hdr = make_stream_header(
        width=1024,
        height=768,
        profile=2,
        level=7,
        chroma_format=1,
        color_transform=1,
        color_space=nxvc.ColorSpace.RGB,
        alpha_present=1,
        eyes=2,
        num_layers=2,
        layer_desc=(0x11, 0x22, 0, 0),
        tools=nxvc.TOOLS_SUPPORTED,
    )
    raw = hdr.pack()
    assert len(raw) == 64
    back = bs.StreamHeader.parse(raw)
    for name, _, _ in bs.StreamHeader.FIELDS:
        assert getattr(back, name) == getattr(hdr, name), name
    assert back.pack() == raw


def test_stream_header_round_trip_with_tlvs():
    hdr = make_stream_header(tlvs=[bs.Tlv(0x8001, b"hello"), bs.Tlv(0x0002, b"")])
    raw = hdr.pack()
    assert hdr.ext_len == 12 + 4  # 4+5+3 pad, then 4
    assert len(raw) == 64 + hdr.ext_len
    back = bs.StreamHeader.parse(raw)
    assert back.tlvs == hdr.tlvs
    assert back.pack() == raw


def test_reserved_bytes_survive_a_round_trip():
    """An unknown reserved byte must come back out unchanged, not zeroed.

    This is what lets the parser carry a field a later spec version adds
    through a parse/pack cycle before this module knows its name -- as
    ``color_space`` (byte 42) was carried before it was named.
    """
    raw = bytearray(make_stream_header().pack())
    raw[43] = 0x5A
    hdr = bs.StreamHeader.parse(bytes(raw), validate=False)
    assert hdr.reserved[0] == 0x5A
    assert hdr.pack() == bytes(raw)


def test_stream_header_field_table_covers_the_documented_offsets():
    offsets = {name: off for name, off, _ in bs.StreamHeader.FIELDS}
    assert offsets == {
        "magic": 0,
        "version": 4,
        "profile": 5,
        "level": 6,
        "tile_size_code": 7,
        "width": 8,
        "height": 10,
        "eyes": 12,
        "bit_depth": 13,
        "num_layers": 14,
        "chroma_format": 15,
        "layer_desc": 16,
        "tools": 32,
        "alpha_present": 40,
        "color_transform": 41,
        "color_space": 42,
        "ext_len": 62,
    }
    assert bs.StreamHeader._gaps() == [(43, 19)]


def test_frame_header_round_trip():
    hdr = bs.FrameHeader(
        frame_number=0xBEEF,
        pose=bytes(range(26)),
        base_qp=63,
        chroma_qp_off=-5,
        alpha_qp_off=7,
        quant_matrix=255,
        tables_present=0xA5,
        ref_slots=3,
        flags=0x03,
        frame_bytes=12345,
    )
    raw = hdr.pack()
    assert len(raw) == 40
    back = bs.FrameHeader.parse(raw)
    for name, _, _ in bs.FrameHeader.FIELDS:
        assert getattr(back, name) == getattr(hdr, name), name
    assert back.tile_map_reset and back.stereo_inter_view and back.custom_matrix
    assert back.table_sets == [0, 2, 5, 7]
    assert back.pack() == raw


def test_tile_row_header_round_trip():
    hdr = bs.TileRowHeader(
        frame_number=1234, row_index=9, tile_count=13, skip_bitmap=0xDEADBEEFCAFEBABE
    )
    raw = hdr.pack()
    assert len(raw) == 12
    back = bs.TileRowHeader.parse(raw)
    assert back == hdr
    assert back.skipped_tiles(4) == [1, 2, 3]  # low nibble of ...BABE is 0b1110


def test_tile_header_round_trip_exhaustive_fields():
    hdr = bs.TileHeader(
        layer=3,
        eye=1,
        tile_index=4095,
        payload_len=65535,
        mode=nxvc.TileMode.STEREO,
        res_level=2,
        chroma444=1,
        alpha_mode=2,
        qp_delta=-32,
        table_set=7,
        nsub_log2=5,
        mv_present=1,
        ref_sel=3,
        tskip=1,
        wgt=3,
        mv_x=-128,
        mv_y=127,
    )
    raw = hdr.pack()
    assert len(raw) == 10  # 8 + MV, no constant-alpha byte at alpha_mode 2
    back = bs.TileHeader.parse(raw)
    assert back == hdr
    assert back.pack() == raw


def test_tile_header_constant_alpha_byte():
    hdr = bs.TileHeader(alpha_mode=1, alpha_value=42)
    raw = hdr.pack()
    assert len(raw) == 9
    assert bs.TileHeader.parse(raw).alpha_value == 42


def test_tlv_round_trip_pads_to_four():
    for n in range(0, 9):
        tlv = bs.Tlv(0x8000, bytes(n))
        raw = tlv.pack()
        assert len(raw) % 4 == 0
        assert len(raw) == tlv.total_size
        typ, length = struct.unpack_from("<HH", raw)
        assert (typ, length) == (0x8000, n)


# ------------------------------------------------------- ctypes -> dataclass


def test_tile_info_from_c_struct():
    c = _ffi.nxvc_tile_info(
        tile_index=7,
        payload_len=1024,
        layer=1,
        eye=0,
        mode=nxvc.TileMode.INTRA,
        res_level=1,
        chroma444=1,
        alpha_mode=1,
        table_set=4,
        nsub_log2=3,
        tskip=1,
        wgt=2,
        ref_sel=1,
        mv_present=1,
        qp_delta=-9,
        mv_x=-3,
        mv_y=5,
        alpha_value=255,
        qp=19,
    )
    info = TileInfo._from_c(c)
    assert info.tile_index == 7 and info.payload_len == 1024
    assert info.qp_delta == -9 and info.mv_x == -3 and info.mv_y == 5
    assert info.qp == 19 and info.mode_name == "INTRA"
    assert info.coded_size == 32


def test_stream_info_from_c_struct():
    c = _ffi.nxvc_stream_info(
        magic=nxvc.NXVC_MAGIC,
        version=1,
        width=2048,
        height=2048,
        eyes=1,
        bit_depth=8,
        num_layers=1,
        chroma=nxvc.Chroma.C444,
        color_transform=1,
        color_space=nxvc.ColorSpace.RGB,
        alpha=0,
        tools=nxvc.TOOLS_SUPPORTED,
        ext_len=8,
        ext_tlv_count=1,
        ext_unknown_count=0,
    )
    c.layer_desc[0] = 0x1234
    info = StreamInfo._from_c(c)
    assert info.chroma444 and info.pix_fmt == "yuv444p"
    assert info.color_space_name == "RGB"
    assert info.layer_desc == (0x1234, 0, 0, 0)
    assert "YCOCGR" in info.tool_names()


def test_frame_info_from_c_struct():
    c = _ffi.nxvc_frame_info(
        frame_number=5,
        base_qp=30,
        chroma_qp_off=-2,
        alpha_qp_off=0,
        quant_matrix=1,
        tables_present=0b101,
        ref_slots=0,
        flags=1,
        frame_bytes=4096,
        tile_count=1024,
    )
    for i in range(26):
        c.pose[i] = i
    info = FrameInfo._from_c(c)
    assert info.pose == bytes(range(26))
    assert info.table_sets == [0, 2]
    assert info.tile_map_reset
    assert info.chroma_qp_off == -2


def test_signed_ctypes_fields_are_signed():
    """A sign error in the struct table is silent until it bites; check it here."""
    c = _ffi.nxvc_tile_info(qp_delta=-1, mv_x=-1, mv_y=-128)
    assert (c.qp_delta, c.mv_x, c.mv_y) == (-1, -1, -128)
    c2 = _ffi.nxvc_frame_info(chroma_qp_off=-31, alpha_qp_off=-1)
    assert (c2.chroma_qp_off, c2.alpha_qp_off) == (-31, -1)


def test_ctypes_struct_sizes_match_the_c_layout():
    # Hand-computed from nxvc.h with natural alignment; a mismatch means the
    # header and this binding have drifted apart.
    assert ctypes.sizeof(_ffi.nxvc_tile_layout) == 16
    assert ctypes.sizeof(_ffi.nxvc_tile_info) == 22
    assert ctypes.sizeof(_ffi.nxvc_image) == ctypes.sizeof(ctypes.c_void_p) * 4 + 16
    # 14 u32 (56) + u64 (64) + layer_desc 4 u32 (80) + 3 u32 (92), rounded up
    # to the struct's 8-byte alignment = 96.
    assert ctypes.sizeof(_ffi.nxvc_stream_info) == 96
    assert ctypes.alignment(_ffi.nxvc_stream_info) == 8


def test_tile_layout_dataclass():
    layout = TileLayout(tiles_x=32, tiles_y=32, tile_count=1024, tile_size=64)
    assert layout.shape == (32, 32)


# ------------------------------------------------------------ pure arithmetic


@pytest.mark.parametrize(
    "w, h, chroma, alpha, expect",
    [
        (64, 64, nxvc.Chroma.C420, False, [(64, 64), (32, 32), (32, 32)]),
        (64, 64, nxvc.Chroma.C444, False, [(64, 64), (64, 64), (64, 64)]),
        (64, 64, nxvc.Chroma.C444, True, [(64, 64), (64, 64), (64, 64), (64, 64)]),
        (2048, 1024, nxvc.Chroma.C420, False, [(1024, 2048), (512, 1024), (512, 1024)]),
    ],
)
def test_plane_shapes(w, h, chroma, alpha, expect):
    assert plane_shapes(w, h, chroma, alpha) == expect


def test_tool_names_round_trip():
    mask = nxvc.Tool.INTRA_DC_PLANE | nxvc.Tool.LOSSLESS | nxvc.Tool.YCOCGR
    assert nxvc.Tool.names(mask) == ["INTRA_DC_PLANE", "LOSSLESS", "YCOCGR"]
    assert nxvc.Tool.names(1 << 40) == ["bit40"]


def test_status_strings_exist_without_the_library():
    for value in (0, -1, -2, -3, -4, -5, -6):
        assert nxvc.status_string(value)
        assert nxvc.Status.name(value).startswith(("OK", "ERR_"))
