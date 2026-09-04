"""Raw YUV geometry, I/O, colour conversion and sequence descriptors."""

from __future__ import annotations

import os

import numpy as np
import pytest

from nxq import yuv
from nxq.sequence import Sequence


class TestFormat:
    def test_444_geometry(self):
        f = yuv.Format(64, 32, "yuv444p")
        assert f.chroma_size == (64, 32)
        assert f.frame_bytes == 64 * 32 * 3
        assert f.plane_shapes == ((32, 64), (32, 64), (32, 64))

    def test_420_geometry(self):
        f = yuv.Format(64, 32, "yuv420p")
        assert f.chroma_size == (32, 16)
        assert f.frame_bytes == 64 * 32 + 2 * (32 * 16)
        assert f.frame_bytes == 64 * 32 * 3 // 2

    def test_rejects_unknown_pix_fmt(self):
        with pytest.raises(ValueError, match="unsupported pix_fmt"):
            yuv.Format(16, 16, "nv12")

    def test_rejects_odd_dimensions_for_420(self):
        with pytest.raises(ValueError, match="even width and height"):
            yuv.Format(15, 16, "yuv420p")

    def test_rejects_nonpositive(self):
        with pytest.raises(ValueError, match="positive"):
            yuv.Format(0, 16)

    def test_frame_count(self, tmp_path):
        f = yuv.Format(8, 8, "yuv444p")
        p = tmp_path / "a.yuv"
        p.write_bytes(b"\x00" * (f.frame_bytes * 5))
        assert f.frame_count(p) == 5

    def test_frame_count_rejects_partial_frames(self, tmp_path):
        f = yuv.Format(8, 8, "yuv444p")
        p = tmp_path / "a.yuv"
        p.write_bytes(b"\x00" * (f.frame_bytes * 2 + 7))
        with pytest.raises(ValueError, match="whole number"):
            f.frame_count(p)


class TestIO:
    @pytest.fixture
    def frames(self):
        rng = np.random.default_rng(2)
        fmt = yuv.Format(16, 8, "yuv444p")
        out = []
        for _ in range(4):
            out.append(yuv.Frame(*(rng.integers(0, 256, (8, 16), dtype=np.uint8)
                                   for _ in range(3))))
        return fmt, out

    def test_roundtrip(self, tmp_path, frames):
        fmt, fs = frames
        p = str(tmp_path / "s.yuv")
        assert yuv.write_sequence(p, fmt, fs) == 4
        assert fmt.frame_count(p) == 4
        back = list(yuv.read_sequence(p, fmt))
        assert len(back) == 4
        for a, b in zip(fs, back):
            for pa, pb in zip(a.planes, b.planes):
                assert np.array_equal(pa, pb)

    def test_read_frame_by_index(self, tmp_path, frames):
        fmt, fs = frames
        p = str(tmp_path / "s.yuv")
        yuv.write_sequence(p, fmt, fs)
        assert np.array_equal(yuv.read_frame(p, fmt, 2).y, fs[2].y)

    def test_read_frame_past_end(self, tmp_path, frames):
        fmt, fs = frames
        p = str(tmp_path / "s.yuv")
        yuv.write_sequence(p, fmt, fs)
        with pytest.raises(EOFError):
            yuv.read_frame(p, fmt, 9)

    def test_limit(self, tmp_path, frames):
        fmt, fs = frames
        p = str(tmp_path / "s.yuv")
        yuv.write_sequence(p, fmt, fs)
        assert len(list(yuv.read_sequence(p, fmt, limit=2))) == 2

    def test_truncated_file_raises(self, tmp_path, frames):
        fmt, fs = frames
        p = tmp_path / "s.yuv"
        yuv.write_sequence(str(p), fmt, fs)
        data = p.read_bytes()
        p.write_bytes(data[: len(data) - 10])
        with pytest.raises(EOFError, match="truncated"):
            list(yuv.read_sequence(str(p), fmt))

    def test_writer_rejects_wrong_plane_shape(self, tmp_path):
        fmt = yuv.Format(16, 8, "yuv420p")
        bad = yuv.Frame(np.zeros((8, 16), np.uint8), np.zeros((8, 16), np.uint8),
                        np.zeros((8, 16), np.uint8))
        with pytest.raises(ValueError, match="plane shape"):
            with yuv.SequenceWriter(str(tmp_path / "x.yuv"), fmt) as w:
                w.write(bad)


class TestColour:
    def test_grey_maps_to_neutral_chroma(self):
        rgb = np.full((4, 4, 3), 128, np.uint8)
        f = yuv.rgb_to_yuv444(rgb)
        assert np.all(f.u == 128)
        assert np.all(f.v == 128)

    def test_black_and_white_hit_the_limited_range_ends(self):
        black = yuv.rgb_to_yuv444(np.zeros((2, 2, 3), np.uint8))
        white = yuv.rgb_to_yuv444(np.full((2, 2, 3), 255, np.uint8))
        assert np.all(black.y == 16)
        assert np.all(white.y == 235)

    def test_red_pushes_v_up(self):
        red = yuv.rgb_to_yuv444(np.array([[[255, 0, 0]]], np.uint8))
        assert red.v[0, 0] > 200
        assert red.u[0, 0] < 128

    def test_blue_pushes_u_up(self):
        blue = yuv.rgb_to_yuv444(np.array([[[0, 0, 255]]], np.uint8))
        assert blue.u[0, 0] > 200

    def test_chroma_downsample_shape_and_average(self):
        f = yuv.Frame(
            np.zeros((4, 4), np.uint8),
            np.array([[0, 100, 0, 0], [100, 100, 0, 0],
                      [0, 0, 0, 0], [0, 0, 0, 0]], np.uint8),
            np.zeros((4, 4), np.uint8),
        )
        d = yuv.downsample_chroma(f)
        assert d.u.shape == (2, 2)
        assert d.y.shape == (4, 4)
        assert d.u[0, 0] == 75  # (0+100+100+100)/4

    def test_to_format_is_a_noop_for_444(self):
        f = yuv.Frame(*(np.zeros((4, 4), np.uint8) for _ in range(3)))
        out = yuv.to_format(f, yuv.Format(4, 4, "yuv444p"))
        assert out.u.shape == (4, 4)

    def test_to_format_downsamples_for_420(self):
        f = yuv.Frame(*(np.zeros((4, 4), np.uint8) for _ in range(3)))
        out = yuv.to_format(f, yuv.Format(4, 4, "yuv420p"))
        assert out.u.shape == (2, 2)


class TestPoseLog:
    def test_roundtrip(self, tmp_path):
        poses = [{"frame": i, "yaw_deg": float(i)} for i in range(3)]
        p = str(tmp_path / "p.json")
        yuv.write_pose_log(p, poses)
        assert yuv.read_pose_log(p) == poses


class TestSequence:
    def test_save_load_roundtrip_with_relative_paths(self, tmp_path):
        fmt = yuv.Format(8, 8, "yuv444p")
        data = tmp_path / "v.yuv"
        yuv.write_sequence(str(data), fmt, [yuv.Frame.gray(fmt)] * 3)
        pose = tmp_path / "v.poses.json"
        yuv.write_pose_log(str(pose), [{"frame": 0}])
        s = Sequence("v", str(data), 8, 8, "yuv444p", 90.0, 3, str(pose), "test")
        side = str(tmp_path / "v.json")
        s.save(side)
        raw = (tmp_path / "v.json").read_text()
        assert '"v.yuv"' in raw  # stored relative, so the directory can move
        back = Sequence.load(side)
        assert back.width == 8 and back.frames == 3
        assert os.path.exists(back.path)

    def test_check_detects_size_mismatch(self, tmp_path):
        fmt = yuv.Format(8, 8, "yuv444p")
        data = tmp_path / "v.yuv"
        yuv.write_sequence(str(data), fmt, [yuv.Frame.gray(fmt)] * 2)
        s = Sequence("v", str(data), 8, 8, "yuv444p", 90.0, 5)
        with pytest.raises(ValueError, match="sidecar says 5 frames"):
            s.check()

    def test_open_raw_requires_geometry(self, tmp_path):
        p = tmp_path / "v.yuv"
        p.write_bytes(b"\x00" * 64)
        with pytest.raises(ValueError, match="pass --w and --h"):
            Sequence.open(str(p))

    def test_open_raw_with_geometry(self, tmp_path):
        fmt = yuv.Format(8, 8, "yuv444p")
        p = tmp_path / "v.yuv"
        yuv.write_sequence(str(p), fmt, [yuv.Frame.gray(fmt)] * 2)
        s = Sequence.open(str(p), 8, 8, "yuv444p")
        assert s.frames == 2

    def test_missing_file(self, tmp_path):
        s = Sequence("v", str(tmp_path / "nope.yuv"), 8, 8)
        with pytest.raises(FileNotFoundError):
            s.check()
