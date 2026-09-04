"""Media import and WiVRn dump conversion."""

from __future__ import annotations

import os

import numpy as np
import pytest

from capture import import_media, wivrn_capture
from nxq import yuv


class TestNaturalSort:
    def test_numeric_order_not_lexical(self):
        files = ["f_10.png", "f_2.png", "f_1.png", "f_20.png"]
        assert sorted(files, key=import_media.natural_key) == [
            "f_1.png", "f_2.png", "f_10.png", "f_20.png"]

    def test_handles_paths(self):
        files = ["/a/b/frame100.png", "/a/b/frame9.png"]
        assert sorted(files, key=import_media.natural_key)[0].endswith("frame9.png")


class TestFindImages:
    def test_directory(self, tmp_path):
        for n in ("a.png", "b.jpg", "notes.txt"):
            (tmp_path / n).write_bytes(b"")
        found = import_media.find_images(str(tmp_path))
        assert len(found) == 2
        assert all(f.lower().endswith((".png", ".jpg")) for f in found)

    def test_glob(self, tmp_path):
        for n in ("a.png", "b.png", "c.jpg"):
            (tmp_path / n).write_bytes(b"")
        assert len(import_media.find_images(str(tmp_path / "*.png"))) == 2

    def test_no_match(self, tmp_path):
        assert import_media.find_images(str(tmp_path / "*.png")) == []


class TestImportImages:
    @pytest.fixture
    def pngs(self, tmp_path):
        Image = pytest.importorskip("PIL.Image")
        rng = np.random.default_rng(1)
        d = tmp_path / "src"
        d.mkdir()
        for i in range(3):
            a = rng.integers(0, 256, (16, 24, 3), dtype=np.uint8)
            Image.fromarray(a).save(d / f"f_{i}.png")
        return d

    def test_roundtrip_creates_sequence_and_sidecar(self, pngs, tmp_path):
        out = str(tmp_path / "seq")
        seqs = import_media.import_images(
            import_media.find_images(str(pngs)), out, "t", ["yuv444p"], 90.0, "mono", quiet=True
        )
        assert len(seqs) == 1
        s = seqs[0]
        assert (s.width, s.height, s.frames) == (24, 16, 3)
        assert os.path.exists(os.path.join(out, "t.yuv444p.json"))
        s.check()

    def test_both_pix_formats(self, pngs, tmp_path):
        out = str(tmp_path / "seq")
        seqs = import_media.import_images(
            import_media.find_images(str(pngs)), out, "t", ["yuv444p", "yuv420p"],
            90.0, "mono", quiet=True
        )
        assert {s.pix_fmt for s in seqs} == {"yuv444p", "yuv420p"}
        for s in seqs:
            s.check()

    def test_frame_limit(self, pngs, tmp_path):
        seqs = import_media.import_images(
            import_media.find_images(str(pngs)), str(tmp_path / "s"), "t", ["yuv444p"],
            90.0, "mono", limit=2, quiet=True
        )
        assert seqs[0].frames == 2

    def test_mismatched_sizes_are_rejected(self, pngs, tmp_path):
        Image = pytest.importorskip("PIL.Image")
        Image.fromarray(np.zeros((8, 8, 3), np.uint8)).save(pngs / "f_9.png")
        with pytest.raises(SystemExit, match="must share a size"):
            import_media.import_images(
                import_media.find_images(str(pngs)), str(tmp_path / "s"), "t",
                ["yuv444p"], 90.0, "mono", quiet=True
            )

    def test_empty_list(self, tmp_path):
        with pytest.raises(SystemExit, match="no images"):
            import_media.import_images([], str(tmp_path), "t", ["yuv444p"], 90.0, "mono")


class TestNV12:
    def test_frame_size(self):
        assert wivrn_capture.nv12_frame_bytes(64, 32) == 64 * 32 * 3 // 2

    def test_deinterleave(self):
        w, h = 4, 4
        y = np.arange(16, dtype=np.uint8).reshape(4, 4)
        cb = np.array([[1, 2], [3, 4]], np.uint8)
        cr = np.array([[5, 6], [7, 8]], np.uint8)
        inter = np.stack([cb, cr], axis=-1).reshape(-1)
        buf = y.tobytes() + inter.tobytes()
        f = wivrn_capture.nv12_to_frame(buf, w, h)
        assert np.array_equal(f.y, y)
        assert np.array_equal(f.u, cb)
        assert np.array_equal(f.v, cr)

    def test_chroma_upsample_replicates(self):
        f = yuv.Frame(np.zeros((4, 4), np.uint8),
                      np.array([[1, 2], [3, 4]], np.uint8),
                      np.array([[5, 6], [7, 8]], np.uint8))
        up = wivrn_capture.upsample_chroma(f)
        assert up.u.shape == (4, 4)
        assert up.u[0, 0] == 1 and up.u[0, 1] == 1 and up.u[1, 1] == 1
        assert up.u[2, 2] == 4

    def _write_dump(self, path, w, h, n, seed=3):
        rng = np.random.default_rng(seed)
        with open(path, "wb") as fh:
            for _ in range(n):
                fh.write(rng.integers(0, 256, wivrn_capture.nv12_frame_bytes(w, h),
                                      dtype=np.uint8).tobytes())

    def test_convert_dump_to_both_formats(self, tmp_path):
        src = str(tmp_path / "dump-0.yuv")
        self._write_dump(src, 32, 16, 4)
        seqs = wivrn_capture.convert_dump(src, 32, 16, str(tmp_path / "out"), "cap",
                                          ["yuv420p", "yuv444p"], 90.0, quiet=True)
        assert len(seqs) == 2
        for s in seqs:
            s.check()
            assert s.frames == 4
            assert s.source.startswith("wivrn:")

    def test_convert_respects_frame_limit(self, tmp_path):
        src = str(tmp_path / "d.yuv")
        self._write_dump(src, 32, 16, 5)
        seqs = wivrn_capture.convert_dump(src, 32, 16, str(tmp_path / "o"), "c",
                                          ["yuv420p"], 90.0, limit=2, quiet=True)
        assert seqs[0].frames == 2

    def test_convert_rejects_a_dump_with_no_whole_frames(self, tmp_path):
        src = tmp_path / "d.yuv"
        src.write_bytes(b"\x00" * 10)
        with pytest.raises(SystemExit, match="no complete"):
            wivrn_capture.convert_dump(str(src), 32, 16, str(tmp_path / "o"), "c",
                                       ["yuv420p"], 90.0, quiet=True)

    def test_wrong_geometry_still_converts_whole_frames(self, tmp_path, capsys):
        """A size mismatch warns rather than silently producing garbage counts."""
        src = str(tmp_path / "d.yuv")
        self._write_dump(src, 32, 16, 3)
        with open(src, "ab") as fh:
            fh.write(b"\x00" * 17)
        wivrn_capture.convert_dump(src, 32, 16, str(tmp_path / "o"), "c",
                                   ["yuv420p"], 90.0)
        assert "not a whole number" in capsys.readouterr().out


class TestPoseJoin:
    def _csvs(self, tmp_path):
        timings = tmp_path / "timings.csv"
        timings.write_text(
            "event,frame,time,stream\n"
            '"encode_begin",100,1000000,0\n'
            '"encode_end",100,1005000,0\n'
            '"encode_begin",101,12111000,0\n'
            '"encode_begin",102,23222000,0\n'
        )
        head = tmp_path / "head.csv"
        head.write_text(
            "in,production_timestamp,timestamp,now,position_0,position_1,position_2,"
            "orientation_0,orientation_1,orientation_2,orientation_3\n"
            "1,0,900000,0,0,0,0,0,0,0,1\n"
            "1,0,12000000,0,0.1,0,0,0,0.0872,0,0.9962\n"
            "1,0,23000000,0,0.2,0,0,0,0.1736,0,0.9848\n"
        )
        return str(timings), str(head)

    def test_join_produces_one_entry_per_frame(self, tmp_path):
        t, h = self._csvs(tmp_path)
        out = str(tmp_path / "p.json")
        n = wivrn_capture.join_poses(t, h, out, quiet=True)
        assert n == 3
        poses = yuv.read_pose_log(out)
        assert [p["source_frame_index"] for p in poses] == [100, 101, 102]
        assert poses[0]["time_s"] == 0.0
        assert poses[1]["time_s"] == pytest.approx(0.011111, abs=1e-5)

    def test_join_picks_the_nearest_pose_sample(self, tmp_path):
        t, h = self._csvs(tmp_path)
        out = str(tmp_path / "p.json")
        wivrn_capture.join_poses(t, h, out, quiet=True)
        poses = yuv.read_pose_log(out)
        assert poses[0]["orientation_xyzw"] == [0, 0, 0, 1]
        assert poses[1]["position_xyz"][0] == pytest.approx(0.1)

    def test_join_computes_angular_velocity(self, tmp_path):
        t, h = self._csvs(tmp_path)
        out = str(tmp_path / "p.json")
        wivrn_capture.join_poses(t, h, out, quiet=True)
        poses = yuv.read_pose_log(out)
        assert poses[0]["angular_velocity_deg_s"] == 0.0
        # 10 degrees of yaw in 11.1 ms is about 900 deg/s
        assert poses[1]["angular_velocity_deg_s"] == pytest.approx(900.0, rel=0.05)

    def test_unknown_event_lists_what_is_available(self, tmp_path):
        t, h = self._csvs(tmp_path)
        with pytest.raises(SystemExit, match="encode_end"):
            wivrn_capture.join_poses(t, h, str(tmp_path / "p.json"), event="nope", quiet=True)

    def test_missing_columns_are_explained(self, tmp_path):
        bad = tmp_path / "t.csv"
        bad.write_text("a,b,c\n1,2,3\n")
        head = tmp_path / "h.csv"
        head.write_text("timestamp,orientation_0,orientation_1,orientation_2,orientation_3\n"
                        "1,0,0,0,1\n")
        with pytest.raises(SystemExit, match="expected 'event'"):
            wivrn_capture.join_poses(str(bad), str(head), str(tmp_path / "p.json"), quiet=True)

    def test_stream_filter(self, tmp_path):
        timings = tmp_path / "t.csv"
        timings.write_text(
            "event,frame,time,stream\n"
            '"encode_begin",1,1000,0\n'
            '"encode_begin",1,1000,1\n'
            '"encode_begin",2,2000,1\n'
        )
        head = tmp_path / "h.csv"
        head.write_text("timestamp,orientation_0,orientation_1,orientation_2,orientation_3\n"
                        "1000,0,0,0,1\n2000,0,0,0,1\n")
        out = str(tmp_path / "p.json")
        assert wivrn_capture.join_poses(str(timings), str(head), out, stream=1, quiet=True) == 2


class TestColumnLookup:
    def test_exact_then_fuzzy(self):
        h = ["in", "production_timestamp", "timestamp", "position_0"]
        assert wivrn_capture._find_col(h, "timestamp") == 2
        assert wivrn_capture._find_col(h, "nothing", "position_0") == 3
        assert wivrn_capture._find_col(h, "posit") == 3
        assert wivrn_capture._find_col(h, "absent") is None

    def test_case_insensitive(self):
        assert wivrn_capture._find_col(["Frame", "Time"], "frame") == 0


class TestPlan:
    def test_plan_mentions_the_key_facilities(self):
        for token in ("WIVRN_DUMP_VIDEO", "WIVRN_DUMP_TIMINGS", "WIVRN_DUMP_HEAD",
                      '"encoder": "raw"', "foveation_strength"):
            assert token in wivrn_capture.PLAN

    def test_plan_runs(self, capsys):
        assert wivrn_capture.main(["plan"]) == 0
        assert "WIVRN_DUMP_VIDEO" in capsys.readouterr().out
