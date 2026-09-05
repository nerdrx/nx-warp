"""The pure-Python bitstream parser, driven off hand-built byte strings.

Every expectation here is written against docs/SYNTAX.md sections 2, 3 and 4
directly -- byte offsets and bit positions spelled out -- so that this suite
fails if the parser drifts from the specification, not merely if it drifts
from itself.
"""

from __future__ import annotations

import struct

import pytest

import nxvc
from nxvc import bitstream as bs

from conftest import make_bitstream, make_frame, make_stream_header

MAGIC = b"NXV1"


def raw_stream_header(**over) -> bytes:
    """A 64-byte stream header assembled byte by byte, not via the packer."""
    f = {
        "magic": MAGIC,
        "version": 1,
        "profile": 0,
        "level": 0,
        "tile_size": 0,
        "width": 128,
        "height": 64,
        "eyes": 1,
        "bit_depth": 8,
        "num_layers": 1,
        "chroma_format": 0,
        "tools": nxvc.Tool.INTRA_DC_PLANE,
        "alpha_present": 0,
        "color_transform": 0,
        "color_space": 0,
        "ext_len": 0,
    }
    f.update(over)
    out = bytearray(64)
    out[0:4] = f["magic"]
    out[4] = f["version"]
    out[5] = f["profile"]
    out[6] = f["level"]
    out[7] = f["tile_size"]
    struct.pack_into("<HH", out, 8, f["width"], f["height"])
    out[12] = f["eyes"]
    out[13] = f["bit_depth"]
    out[14] = f["num_layers"]
    out[15] = f["chroma_format"]
    struct.pack_into("<Q", out, 32, f["tools"])
    out[40] = f["alpha_present"]
    out[41] = f["color_transform"]
    out[42] = f["color_space"]
    struct.pack_into("<H", out, 62, f["ext_len"])
    return bytes(out)


# ------------------------------------------------------------- stream header


def test_stream_header_offsets():
    hdr = bs.parse_stream_header(raw_stream_header())
    assert hdr.magic == nxvc.NXVC_MAGIC == struct.unpack("<I", MAGIC)[0]
    assert (hdr.width, hdr.height) == (128, 64)
    assert hdr.tile_size == 64
    assert (hdr.tiles_x, hdr.tiles_y, hdr.tile_count) == (2, 1, 2)
    assert hdr.total_size == 64
    assert hdr.tool_names() == ["INTRA_DC_PLANE"]


def test_stream_header_32x32_tiles():
    hdr = bs.parse_stream_header(raw_stream_header(tile_size=1))
    assert hdr.tile_size == 32
    assert (hdr.tiles_x, hdr.tiles_y) == (4, 2)


def test_stream_header_odd_sizes_round_up():
    # 130x66 is not a multiple of 64: SYNTAX.md 4.2 codes the edge as full
    # tiles and discards the samples outside the picture.
    hdr = bs.parse_stream_header(raw_stream_header(width=130, height=66))
    assert (hdr.tiles_x, hdr.tiles_y, hdr.tile_count) == (3, 2, 6)


@pytest.mark.parametrize(
    "over, fragment",
    [
        ({"magic": b"NXV0"}, "bad magic"),
        ({"version": 2}, "unsupported version"),
        ({"tile_size": 0x02}, "bits 1-7"),
        ({"width": 14}, "outside [16, 4096]"),
        ({"width": 4098}, "outside [16, 4096]"),
        ({"height": 65}, "not even"),
        # 4096 luma samples of 32x32 tiles is 128 tiles per row; the row skip
        # bitmap is only 64 bits wide, so the stream is illegal.
        ({"width": 4096, "tile_size": 1}, "exceeds the 64-bit skip bitmap"),
        ({"chroma_format": 2}, "chroma_format"),
        ({"color_transform": 2}, "color_transform"),
        ({"alpha_present": 2}, "alpha_present"),
        ({"color_transform": 1, "chroma_format": 0}, "YCoCg-R requires 4:4:4"),
        ({"color_space": 4}, "color_space 4 out of range"),
        # RGB without YCoCg-R, and YCoCg-R without RGB, are both illegal.
        ({"color_space": 3, "chroma_format": 1}, "disagree"),
        ({"color_transform": 1, "chroma_format": 1, "color_space": 1}, "disagree"),
        ({"eyes": 0}, "must be 1 or 2"),
        ({"bit_depth": 12}, "must be 8 or 10"),
        ({"num_layers": 5}, "outside [1, 4]"),
        ({"tools": 1 << 30}, "reserved tool bits"),
        (
            {"tools": nxvc.Tool.LOSSLESS | nxvc.Tool.SIGN_HIDE},
            "LOSSLESS and SIGN_HIDE are mutually exclusive",
        ),
        ({"tools": nxvc.Tool.TAB_V2}, "TAB_V2 without CUSTOM_TABLES"),
        ({"tools": nxvc.Tool.CTX_V3}, "CTX_V3 without CTX_V2"),
    ],
)
def test_stream_header_rejects(over, fragment):
    with pytest.raises(bs.BitstreamError) as exc:
        bs.parse_stream_header(raw_stream_header(**over))
    assert fragment in str(exc.value)


def test_stream_header_reserved_bytes_must_be_zero():
    raw = bytearray(raw_stream_header())
    raw[50] = 0x01
    with pytest.raises(bs.BitstreamError, match="reserved bytes 43-61"):
        bs.parse_stream_header(bytes(raw))


def test_color_space_is_descriptive_and_tied_to_the_transform():
    # SYNTAX.md 2.2: values 0-2 are YCbCr/unspecified with no transform,
    # 3 is RGB and requires YCoCg-R.
    hdr = bs.parse_stream_header(raw_stream_header(color_space=1))
    assert hdr.color_space == 1
    assert hdr.color_space_name == "YCbCr BT.709 limited"
    rgb = bs.parse_stream_header(
        raw_stream_header(color_space=3, color_transform=1, chroma_format=1)
    )
    assert rgb.color_space_name == "RGB"


def test_stream_header_truncated():
    with pytest.raises(bs.BitstreamError) as exc:
        bs.parse_stream_header(raw_stream_header()[:40])
    assert "truncated stream header" in str(exc.value)
    assert exc.value.offset == 0


def test_unsupported_tools_reported():
    """`unsupported_tools` is about the reference decoder, `non_phase1_tools`
    about an intra-only one.  Since syntax v1.4 they are not the same set."""
    hdr = bs.parse_stream_header(
        raw_stream_header(tools=nxvc.Tool.INTRA_DC_PLANE | nxvc.Tool.FILTER_CATMULLROM)
    )
    assert hdr.unsupported_tools() == nxvc.Tool.FILTER_CATMULLROM
    assert "FILTER_CATMULLROM" in nxvc.Tool.names(hdr.unsupported_tools())

    inter = bs.parse_stream_header(
        raw_stream_header(tools=nxvc.Tool.INTRA_DC_PLANE | nxvc.Tool.INTER)
    )
    assert inter.unsupported_tools() == 0
    assert inter.non_phase1_tools() == nxvc.Tool.INTER


# ------------------------------------------------------------------ TLV area


def test_tlv_padding_and_walk():
    # Two records: a 3-byte payload (1 pad byte) and a 4-byte payload (none).
    tlvs = [bs.Tlv(0x8001, b"abc"), bs.Tlv(0x8002, b"wxyz")]
    assert tlvs[0].total_size == 8
    assert tlvs[1].total_size == 8
    ext = b"".join(t.pack() for t in tlvs)
    raw = bytearray(raw_stream_header(ext_len=len(ext)))
    hdr = bs.parse_stream_header(bytes(raw) + ext)
    assert [(t.type, t.payload) for t in hdr.tlvs] == [(0x8001, b"abc"), (0x8002, b"wxyz")]
    assert all(t.private for t in hdr.tlvs)
    assert hdr.total_size == 64 + 16


def test_tlv_zero_length_record():
    ext = bs.Tlv(0x1234, b"").pack()
    assert len(ext) == 4
    hdr = bs.parse_stream_header(raw_stream_header(ext_len=4) + ext)
    assert hdr.tlvs == [bs.Tlv(0x1234, b"")]
    assert not hdr.tlvs[0].private


def test_tlv_overrunning_record_is_malformed():
    ext = struct.pack("<HH", 0x8001, 64)  # claims 64 bytes, 0 follow
    with pytest.raises(bs.BitstreamError, match="runs past the extension area"):
        bs.parse_stream_header(raw_stream_header(ext_len=4) + ext)


def test_tlv_nonzero_padding_is_malformed():
    ext = struct.pack("<HH", 0x8001, 3) + b"abc" + b"\xff"
    with pytest.raises(bs.BitstreamError, match="non-zero padding"):
        bs.parse_stream_header(raw_stream_header(ext_len=8) + ext)


def test_tlv_area_truncated():
    with pytest.raises(bs.BitstreamError, match="truncated TLV extension area"):
        bs.parse_stream_header(raw_stream_header(ext_len=16))


# ----------------------------------------------------------------- tile header


def test_tile_header_bitfields_are_at_the_documented_positions():
    # word0: layer=2 (bits 0-1), eye=1 (bit 2), tile_index=5 (bits 4-15),
    #        payload_len=300 (bits 16-31)
    word0 = 2 | (1 << 2) | (5 << 4) | (300 << 16)
    # word1: mode=2 WARP_MV, res_level=1, chroma444=1, alpha_mode=1,
    #        qp_delta=-4, table_set=6, nsub_log2=2, mv_present=1, ref_sel=2,
    #        tskip=1, wgt=3.  The mode has to be an inter one: SYNTAX.md 4.1
    #        requires ref_sel == 0 on INTRA and STEREO tiles.
    word1 = (
        2
        | (1 << 3)
        | (1 << 5)
        | (1 << 6)
        | ((-4 & 0x3F) << 8)
        | (6 << 14)
        | (2 << 17)
        | (1 << 20)
        | (2 << 21)
        | (1 << 23)
        | (3 << 24)
    )
    raw = struct.pack("<II", word0, word1) + struct.pack("<bb", -7, 9) + bytes([200])
    t = bs.TileHeader.parse(raw)
    assert (t.layer, t.eye, t.tile_index, t.payload_len) == (2, 1, 5, 300)
    assert t.mode == nxvc.TileMode.WARP_MV and t.mode_name == "WARP_MV"
    assert (t.res_level, t.chroma444, t.alpha_mode) == (1, 1, 1)
    assert t.qp_delta == -4  # signed 6-bit two's complement
    assert (t.table_set, t.nsub_log2) == (6, 2)
    assert (t.mv_present, t.ref_sel, t.tskip, t.wgt) == (1, 2, 1, 3)
    assert (t.mv_x, t.mv_y) == (-7, 9)
    assert t.alpha_value == 200
    assert t.header_size == 8 + 2 + 1
    assert t.total_size == 11 + 300
    assert t.coded_size == 32  # 64 >> res_level
    assert t.chroma_coded_size(True) == 32  # chroma444 tile, res_level 1
    assert t.chroma_coded_size(False) == 16  # 4:2:0 stream ignores the tile bit
    assert t.resolved_qp(28) == 24


@pytest.mark.parametrize("qp_delta", [-32, -1, 0, 1, 31])
def test_tile_qp_delta_round_trips_over_its_whole_range(qp_delta):
    t = bs.TileHeader(qp_delta=qp_delta)
    assert bs.TileHeader.parse(t.pack()).qp_delta == qp_delta


def test_tile_resolved_qp_clamps():
    assert bs.TileHeader(qp_delta=-32).resolved_qp(4) == 0
    assert bs.TileHeader(qp_delta=31).resolved_qp(60) == 63


@pytest.mark.parametrize(
    "kwargs, fragment",
    [
        ({"mode": 5}, "mode 5 is reserved"),
        ({"res_level": 3}, "res_level 3 is reserved"),
        ({"alpha_mode": 3}, "alpha_mode 3 is reserved"),
        ({"nsub_log2": 6}, "nsub_log2 6 exceeds 5"),
        ({"word0_reserved": 1}, "word0 bit 3 must be zero"),
        ({"word1_reserved": 1}, "word1 bits 30-31 must be zero"),
        ({"xform_size": 3}, "xform_size 3 is reserved"),
        ({"xform_size": 1, "tskip": 1}, "xform_size != 0 on a transform-skip"),
    ],
)
def test_tile_header_rejects(kwargs, fragment):
    raw = bs.TileHeader(**kwargs).pack()
    with pytest.raises(bs.BitstreamError, match=fragment.replace("[", r"\[")):
        bs.TileHeader.parse(raw)


def test_tile_header_truncated_mv():
    raw = bs.TileHeader(mv_present=1).pack()[:9]
    with pytest.raises(bs.BitstreamError, match="truncated tile motion vector"):
        bs.TileHeader.parse(raw)


# ----------------------------------------------------------------- frame walk


def test_frame_and_row_walk():
    stream = make_stream_header(width=128, height=128)  # 2x2 tiles
    data = stream.pack() + make_frame(stream, base_qp=24, payload=b"\x01\x02\x03\x04")
    parsed = bs.parse_stream(data)
    assert len(parsed.frames) == 1
    frame = parsed.frames[0]
    assert frame.header.base_qp == 24
    assert len(frame.rows) == 2
    assert [r.header.row_index for r in frame.rows] == [0, 1]
    assert len(frame.tiles) == 4
    assert all(t.payload == b"\x01\x02\x03\x04" for t in frame.tiles)
    assert frame.payload_bytes == 16
    assert parsed.size == len(data)


def test_multiple_frames_walk():
    data = make_bitstream(frames=3)
    parsed = bs.parse_stream(data)
    assert [f.header.frame_number for f in parsed.frames] == [0, 1, 2]
    assert sum(f.size for f in parsed.frames) + parsed.header.total_size == len(data)


def test_max_frames_stops_early():
    data = make_bitstream(frames=5)
    assert len(bs.parse_stream(data, max_frames=2).frames) == 2


def test_frame_bytes_mismatch_is_caught():
    stream = make_stream_header()
    frame = bytearray(make_frame(stream))
    struct.pack_into("<I", frame, 36, len(frame) + 8)  # frame_bytes too large
    with pytest.raises(bs.BitstreamError, match="truncated frame unit"):
        bs.parse_frame(stream.pack() + bytes(frame), stream.total_size, stream)


def test_frame_consumed_mismatch_is_caught():
    stream = make_stream_header()
    frame = bytearray(make_frame(stream))
    struct.pack_into("<I", frame, 36, len(frame) + 4)
    data = stream.pack() + bytes(frame) + b"\x00" * 4
    with pytest.raises(bs.BitstreamError, match="frame consumed"):
        bs.parse_frame(data, stream.total_size, stream)


def test_row_index_must_match():
    stream = make_stream_header(width=64, height=128)
    frame = bytearray(make_frame(stream))
    # second row header starts after: 40 header + 12 + 8 = 60
    frame[60 + 2] = 7
    with pytest.raises(bs.BitstreamError, match="row_index"):
        bs.parse_frame(stream.pack() + bytes(frame), stream.total_size, stream)


def test_row_frame_number_must_match():
    stream = make_stream_header()
    frame = bytearray(make_frame(stream, frame_number=3))
    struct.pack_into("<H", frame, 40, 4)  # row header frame_number
    with pytest.raises(bs.BitstreamError, match="frame_number"):
        bs.parse_frame(stream.pack() + bytes(frame), stream.total_size, stream)


def test_tile_count_must_match_the_skip_bitmap():
    stream = make_stream_header(width=128, height=64)  # 2 tiles per row
    frame = bytearray(make_frame(stream))
    struct.pack_into("<Q", frame, 40 + 4, 0b01)  # claim tile 0 skipped
    with pytest.raises(bs.BitstreamError, match="tile_count 2 != 2 tiles minus 1"):
        bs.parse_frame(stream.pack() + bytes(frame), stream.total_size, stream)


def test_skip_bitmap_above_the_row_width_is_rejected():
    stream = make_stream_header(width=64, height=64)
    frame = bytearray(make_frame(stream))
    struct.pack_into("<Q", frame, 40 + 4, 1 << 5)
    with pytest.raises(bs.BitstreamError, match="bits set above tile"):
        bs.parse_frame(stream.pack() + bytes(frame), stream.total_size, stream)


def test_frame_header_validation():
    with pytest.raises(bs.BitstreamError, match="base_qp 64"):
        bs.FrameHeader.parse(bs.FrameHeader(base_qp=64, frame_bytes=40).pack())
    with pytest.raises(bs.BitstreamError, match="quant_matrix 7"):
        bs.FrameHeader.parse(bs.FrameHeader(quant_matrix=7, frame_bytes=40).pack())
    with pytest.raises(bs.BitstreamError, match="smaller than the 40-byte header"):
        bs.FrameHeader.parse(bs.FrameHeader(frame_bytes=8).pack())


def test_custom_matrix_and_table_sets_are_sliced_out():
    stream = make_stream_header()
    matrix = bytes(range(128))
    t3, t5 = bytes([3]) * 120, bytes([5]) * 120
    body = bytearray()
    rh = bs.TileRowHeader(frame_number=0, row_index=0, tile_count=1, skip_bitmap=0)
    body += rh.pack() + bs.TileHeader(mode=nxvc.TileMode.INTRA).pack()
    payload = matrix + t3 + t5 + bytes(body)
    hdr = bs.FrameHeader(
        quant_matrix=255,
        tables_present=(1 << 3) | (1 << 5),
        frame_bytes=40 + len(payload),
    )
    frame = bs.parse_frame(stream.pack() + hdr.pack() + payload, stream.total_size, stream)
    assert frame.custom_matrix == matrix
    assert frame.table_deltas == {3: t3, 5: t5}
    assert frame.header.table_sets == [3, 5]


def test_pose_is_carried_verbatim_and_decodable():
    pose = bytes(range(26))
    hdr = bs.FrameHeader(pose=pose, frame_bytes=40)
    back = bs.FrameHeader.parse(hdr.pack())
    assert back.pose == pose
    decoded = back.decode_pose()
    assert len(decoded["quat"]) == 4
    assert len(decoded["angvel"]) == 3
    assert len(decoded["pos"]) == 3


# ------------------------------------------------------------ phase 1 advice


def test_phase1_reject_reasons():
    stream = make_stream_header()
    data = stream.pack() + make_frame(stream)
    parsed = bs.parse_stream(data)
    assert bs.phase1_reject_reason(parsed.header, parsed.frames[0]) is None

    inter = make_stream_header(
        tools=nxvc.Tool.INTRA_DC_PLANE | nxvc.Tool.INTER | nxvc.Tool.WARP
    )
    assert "outside the Phase 1 set" in bs.phase1_reject_reason(inter)

    stereo = make_stream_header(eyes=2)
    assert "eyes == 2" in bs.phase1_reject_reason(stereo)

    # A legal Phase 2 stream is refused on its tool mask, before any tile is
    # looked at -- an inter tile mode cannot occur without the INTER bit.
    inter_stream = make_stream_header(
        tools=nxvc.Tool.INTRA_DC_PLANE | nxvc.Tool.INTER
    )
    frame_bytes = make_frame(inter_stream, mode=nxvc.TileMode.STATIC_MV, ref_slots=1)
    f = bs.parse_frame(
        inter_stream.pack() + frame_bytes, inter_stream.total_size, inter_stream
    )
    assert "INTER" in bs.phase1_reject_reason(inter_stream, f)

    # The per-tile arm of the check still has to work, for a caller that walked
    # the bytes with validation off: the tools mask alone is then not enough to
    # know what the tiles say.
    assert "STATIC_MV" in bs.phase1_reject_reason(stream, f)


def test_iter_frames_matches_parse_stream():
    data = make_bitstream(frames=4)
    header = bs.parse_stream_header(data)
    assert len(list(bs.iter_frames(data, header))) == 4
