"""End-to-end encode/decode through the C library.

Every test here is skipped -- with the reason and the exact build command --
when the shared library is not present.  Nothing in this file may be turned
into an assertion about *pixel* correctness beyond what the reference codec
itself guarantees: the reference codec is the specification, so the checks are
lossless bit-exactness, monotonicity, and agreement between the C decoder and
the pure-Python parser.
"""

from __future__ import annotations

import numpy as np
import pytest

import nxvc
from nxvc import bitstream as bs
from nxvc import metrics

from conftest import requires_library

pytestmark = requires_library


W, H = 128, 128


def synth_planes(chroma444=True, seed=1):
    """A picture with real entropy: gradient, texture and a hard edge."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:H, 0:W]
    y = ((xx * 2 + yy) % 256).astype(np.int32)
    y += (rng.integers(0, 24, (H, W)) - 12)
    y[H // 4 : H // 2, W // 4 : W // 2] = 235  # a flat bright block
    y = np.clip(y, 0, 255).astype(np.uint8)
    if chroma444:
        cw, ch = W, H
    else:
        cw, ch = W // 2, H // 2
    cyy, cxx = np.mgrid[0:ch, 0:cw]
    u = np.clip(96 + (cxx % 64), 0, 255).astype(np.uint8)
    v = np.clip(160 - (cyy % 48), 0, 255).astype(np.uint8)
    return [y, u, v]


def encode_one(planes, pix="yuv444p", **kwargs):
    with nxvc.Encoder(W, H, pix=pix, **kwargs) as enc:
        header = enc.stream_header()
        frame = enc.encode(planes)
        tiles = enc.tiles()
    return header, frame, tiles


# ---------------------------------------------------------------- smoke tests


def test_library_reports_its_path():
    assert nxvc.NXVC_LIBRARY_PATH
    assert nxvc.NXVC_LOAD_ERROR is None


def test_tile_layout_matches_the_pure_python_geometry():
    layout = nxvc.tile_layout(2048, 1024)
    assert (layout.tiles_x, layout.tiles_y) == (32, 16)
    assert layout.tile_count == 512
    assert layout.tile_size == nxvc.NXVC_TILE_SIZE
    hdr = bs.StreamHeader(width=2048, height=1024)
    assert (hdr.tiles_x, hdr.tiles_y, hdr.tile_count) == (32, 16, 512)


def test_status_string_comes_from_the_library():
    assert nxvc.status_string(nxvc.Status.ERR_TRUNCATED)


# -------------------------------------------------------------- encode/decode


@pytest.mark.parametrize("pix", ["yuv444p", "yuv420p"])
def test_round_trip_is_close_to_the_source(pix):
    planes = synth_planes(pix == "yuv444p")
    header, frame, tiles = encode_one(planes, pix=pix, base_qp=20)
    assert len(header) >= 64
    assert len(frame) > 0
    assert len(tiles) == 4  # 128x128 -> 2x2 tiles of 64x64

    with nxvc.Decoder() as dec:
        info, consumed = dec.parse_stream_header(header + frame)
        assert consumed == len(header)
        assert (info.width, info.height) == (W, H)
        assert info.pix_fmt == pix
        out, used = dec.decode(frame)
        assert used == len(frame)

    assert [p.shape for p in out] == [p.shape for p in planes]
    y_psnr = metrics.psnr(planes[0], out[0])
    assert y_psnr > 30.0, f"luma PSNR {y_psnr:.2f} dB at QP 20 is implausibly low"


def test_lossless_is_bit_exact():
    planes = synth_planes(True)
    header, frame, _ = encode_one(planes, pix="yuv444p", lossless=1)
    with nxvc.Decoder() as dec:
        dec.parse_stream_header(header + frame)
        out, _ = dec.decode(frame)
    for i, (a, b) in enumerate(zip(planes, out)):
        assert np.array_equal(a, b), f"plane {i} is not bit exact in lossless mode"


def test_quality_falls_as_qp_rises():
    planes = synth_planes(True)
    sizes, psnrs = [], []
    for qp in (14, 26, 38):
        header, frame, _ = encode_one(planes, base_qp=qp)
        with nxvc.Decoder() as dec:
            dec.parse_stream_header(header + frame)
            out, _ = dec.decode(frame)
        sizes.append(len(frame))
        psnrs.append(metrics.psnr(planes[0], out[0]))
    assert sizes[0] > sizes[1] > sizes[2], f"rate is not monotone in QP: {sizes}"
    assert psnrs[0] > psnrs[1] > psnrs[2], f"quality is not monotone in QP: {psnrs}"


def test_multi_frame_stream_walks_back():
    planes = [synth_planes(True, seed=s) for s in (1, 2, 3)]
    with nxvc.Encoder(W, H, pix="yuv444p", base_qp=24) as enc:
        data = bytearray(enc.stream_header())
        for p in planes:
            data += enc.encode(p)
    with nxvc.Decoder() as dec:
        decoded = list(dec.frames(bytes(data)))
    assert len(decoded) == 3
    for src, out in zip(planes, decoded):
        assert metrics.psnr(src[0], out[0]) > 28.0


def test_pose_survives_the_round_trip():
    pose = bytes(range(26))
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p") as enc:
        header = enc.stream_header()
        enc.set_pose(pose)
        frame = enc.encode(planes)
    with nxvc.Decoder() as dec:
        dec.parse_stream_header(header + frame)
        info, _ = dec.scan(frame)
    assert info.pose == pose
    assert bs.FrameHeader.parse(frame).pose == pose


def test_set_pose_rejects_a_wrong_length():
    with nxvc.Encoder(W, H) as enc:
        with pytest.raises(ValueError, match="26 bytes"):
            enc.set_pose(b"\x00" * 25)


def test_tlv_survives_into_the_stream_header():
    with nxvc.Encoder(W, H) as enc:
        enc.add_tlv(0x8123, b"hello")
        header = enc.stream_header()
        with pytest.raises(RuntimeError, match="before stream_header"):
            enc.add_tlv(0x8124, b"late")
    parsed = bs.parse_stream_header(header)
    assert any(t.type == 0x8123 and t.payload == b"hello" for t in parsed.tlvs)
    with nxvc.Decoder() as dec:
        info, consumed = dec.parse_stream_header(header)
    assert info.ext_tlv_count == 1
    assert consumed == len(header) == parsed.total_size


# ------------------------------------------------------------ per-tile maps


def test_qp_map_changes_the_per_tile_qp():
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p", base_qp=30) as enc:
        assert enc.layout.shape == (2, 2)
        qp_map = np.array([[10, 10], [50, 50]], dtype=np.uint8)
        enc.stream_header()
        enc.encode(planes, qp_map=qp_map)
        qps = enc.tile_map("qp")
    assert qps.shape == (2, 2)
    assert qps[0, 0] < qps[1, 0], f"qp_map had no effect: {qps}"
    assert qps[0].tolist() == [10, 10]
    assert qps[1].tolist() == [50, 50]


def test_qp_map_accepts_a_flat_array():
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p", base_qp=30) as enc:
        enc.stream_header()
        enc.encode(planes, qp_map=np.array([12, 12, 12, 12], dtype=np.uint8))
        assert [t.qp for t in enc.tiles()] == [12] * 4


def test_res_map_changes_the_per_tile_res_level():
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p", base_qp=24) as enc:
        header = enc.stream_header()
        res_map = np.array([[0, 1], [2, 0]], dtype=np.uint8)
        frame = enc.encode(planes, res_map=res_map)
        levels = enc.tile_map("res_level")
    assert levels.tolist() == [[0, 1], [2, 0]]
    # and the parser sees the same thing in the bytes.  The frame has to be
    # read against the encoder's OWN stream header: the tools mask decides how
    # big a transmitted probability table set is (120 bytes, or 160 under
    # CTX_V2), so a hand-built header with an empty mask would walk into the
    # tile rows at the wrong offset.
    stream = bs.parse_stream_header(header)
    assert stream.width == W and stream.chroma444
    parsed = bs.parse_frame(frame, 0, stream, validate=False)
    assert [t.header.res_level for t in parsed.tiles] == [0, 1, 2, 0]


def test_map_shape_is_checked():
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p") as enc:
        enc.stream_header()
        with pytest.raises(ValueError, match="qp_map"):
            enc.encode(planes, qp_map=np.zeros((3, 3), np.uint8))


def test_plane_shape_is_checked():
    with nxvc.Encoder(W, H, pix="yuv444p") as enc:
        enc.stream_header()
        with pytest.raises(ValueError, match="plane 0 must have shape"):
            enc.encode([np.zeros((8, 8), np.uint8)] * 3)
        with pytest.raises(TypeError, match="must be uint8"):
            enc.encode([np.zeros((H, W), np.uint16)] * 3)


# --------------------------------------- C decoder vs pure-Python parser


def test_the_python_parser_agrees_with_the_c_decoder():
    """The two independent readers of the same bytes must not disagree."""
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p", base_qp=26) as enc:
        data = bytearray(enc.stream_header())
        for _ in range(2):
            data += enc.encode(planes)
        c_tiles = enc.tiles()
    data = bytes(data)

    parsed = bs.parse_stream(data)
    assert parsed.header.width == W and parsed.header.height == H
    assert parsed.header.chroma444
    assert parsed.size == len(data)
    assert len(parsed.frames) == 2

    with nxvc.Decoder() as dec:
        info, consumed = dec.parse_stream_header(data)
        assert consumed == parsed.header.total_size
        assert info.tools == parsed.header.tools
        pos = consumed
        for frame in parsed.frames:
            fi, used = dec.scan(data[pos:])
            assert used == frame.header.frame_bytes
            assert fi.frame_number == frame.header.frame_number
            assert fi.base_qp == frame.header.base_qp
            # scan_frame is headers-only and deliberately leaves tile_count
            # at 0; only a full decode walks the tile structures.
            assert fi.tile_count == 0
            pos += used

    # A full decode does fill tile_count and the tile records.
    with nxvc.Decoder() as dec:
        planes = list(dec.frames(data))
        assert len(planes) == 2
        assert dec.frame_info().tile_count == len(parsed.frames[-1].tiles)
        assert len(dec.tiles()) == len(parsed.frames[-1].tiles)

    last = parsed.frames[-1]
    assert [t.header.payload_len for t in last.tiles] == [t.payload_len for t in c_tiles]
    assert [t.header.mode for t in last.tiles] == [t.mode for t in c_tiles]
    assert [
        t.header.resolved_qp(last.header.base_qp) for t in last.tiles
    ] == [t.qp for t in c_tiles]
    assert bs.phase1_reject_reason(parsed.header, last) is None


def test_truncated_frame_is_refused_not_misparsed():
    planes = synth_planes(True)
    header, frame, _ = encode_one(planes)
    with nxvc.Decoder() as dec:
        dec.parse_stream_header(header)
        with pytest.raises(nxvc.NxvcError) as exc:
            dec.decode(frame[: len(frame) // 2])
    assert exc.value.status in (nxvc.Status.ERR_TRUNCATED, nxvc.Status.ERR_BITSTREAM)


def test_garbage_stream_header_is_refused():
    with nxvc.Decoder() as dec:
        with pytest.raises(nxvc.NxvcError):
            dec.parse_stream_header(b"\x00" * 64)


# ------------------------------------------------------------------- YCoCg-R


def test_ycocgr_is_exactly_reversible():
    rng = np.random.default_rng(4)
    r, g, b = (rng.integers(0, 256, (16, 16), dtype=np.uint8) for _ in range(3))
    y, co, cg = nxvc.ycocgr_forward(r, g, b)
    r2, g2, b2 = nxvc.ycocgr_inverse(y, co, cg)
    assert np.array_equal(r, r2) and np.array_equal(g, g2) and np.array_equal(b, b2)


def test_ycocgr_matches_the_normative_lifting():
    """SYNTAX.md 5.1, computed independently in numpy with arithmetic shifts."""
    rng = np.random.default_rng(9)
    r, g, b = (rng.integers(0, 256, (8, 8), dtype=np.uint8) for _ in range(3))
    ri, gi, bi = (x.astype(np.int32) for x in (r, g, b))
    co = ri - bi
    t = bi + (co >> 1)
    cg = gi - t
    y = t + (cg >> 1)
    ly, lco, lcg = nxvc.ycocgr_forward(r, g, b)
    assert np.array_equal(ly.astype(np.int32), y)
    assert np.array_equal(lco.astype(np.int32), co + 256)
    assert np.array_equal(lcg.astype(np.int32), cg + 256)


# ------------------------------------------------------------------ raw files


def test_planar_yuv_round_trip(tmp_path):
    planes = synth_planes(False)
    path = tmp_path / "seq.yuv"
    assert nxvc.write_planar_yuv(path, [planes, planes]) == 2
    back = nxvc.read_planar_yuv(path, W, H, "yuv420p")
    assert len(back) == 2
    for a, b in zip(planes, back[0]):
        assert np.array_equal(a, b)


def test_short_frame_in_a_raw_file_is_an_error(tmp_path):
    path = tmp_path / "short.yuv"
    path.write_bytes(b"\x00" * 100)
    with pytest.raises(ValueError, match="short frame"):
        nxvc.read_planar_yuv(path, W, H, "yuv420p")


# ------------------------------------------------------------- colour space


def test_color_space_reaches_the_stream_header():
    """SYNTAX.md 2.2: descriptive, and RGB iff YCoCg-R."""
    with nxvc.Encoder(
        W, H, pix="yuv420p", color_space=nxvc.ColorSpace.YCBCR_709_FULL
    ) as enc:
        header = enc.stream_header()
    parsed = bs.parse_stream_header(header)
    assert parsed.color_space == nxvc.ColorSpace.YCBCR_709_FULL
    assert parsed.color_space_name == "YCbCr BT.709 full"
    assert parsed.color_transform == 0
    with nxvc.Decoder() as dec:
        info, _ = dec.parse_stream_header(header)
    assert info.color_space == nxvc.ColorSpace.YCBCR_709_FULL


def test_ycocgr_stream_declares_rgb():
    planes = synth_planes(True)
    with nxvc.Encoder(
        W, H, pix="yuv444p", color_transform=1, color_space=nxvc.ColorSpace.RGB,
        base_qp=18,
    ) as enc:
        header = enc.stream_header()
        frame = enc.encode(planes)
    parsed = bs.parse_stream_header(header)  # validates the RGB/YCoCg-R tie
    assert parsed.color_space == nxvc.ColorSpace.RGB
    assert parsed.color_transform == 1
    assert parsed.tools & nxvc.Tool.YCOCGR
    with nxvc.Decoder() as dec:
        dec.parse_stream_header(header + frame)
        out, _ = dec.decode(frame)
    # RGB in, RGB out: the transform is inside the codec.
    assert metrics.psnr(planes[0], out[0]) > 25.0


# -------------------------------------------------------------- bit accounting


def test_encode_stats_account_for_the_whole_frame():
    planes = synth_planes(True)
    with nxvc.Encoder(W, H, pix="yuv444p", base_qp=24, collect_stats=1) as enc:
        enc.stream_header()
        frame = enc.encode(planes)
        st = enc.stats()
    assert st.bytes_total == len(frame)
    parts = (
        st.bytes_frame_header
        + st.bytes_tables
        + st.bytes_row_headers
        + st.bytes_tile_headers
        + st.bytes_payload
    )
    assert parts == st.bytes_total, f"bit accounting does not add up: {st}"
    assert st.tiles == 4
    assert sum(st.tiles_res) == st.tiles
    assert st.overhead_bytes == st.bytes_total - st.bytes_payload
    assert st.lanes_total > 0
