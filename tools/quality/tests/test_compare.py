"""Analysis helpers in compare.py, plus one end-to-end run through the mock codec."""

from __future__ import annotations

import json
import os
import sys

import pytest

import compare
from nxq import ffmpeg

DUMMY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dummy_codec.py")


def _results(codec_points, anchor_points, codec="nxv", anchor="x264-intra"):
    """Build a minimal results dict of the shape compare.py produces."""
    def pts(items):
        return [{"qp": qp, "bytes": 1, "bitrate_mbps": r, "psnr_y": p, "frames": 1}
                for qp, r, p in items]
    return {
        "codec_key": codec,
        "codecs": {
            codec: {"kind": "codec", "available": True, "points": pts(codec_points)},
            anchor: {"kind": "anchor", "available": True, "points": pts(anchor_points)},
        },
    }


class TestBitrate:
    def test_known_value(self):
        # 1 MB over 10 frames at 90 fps
        assert compare.bitrate_mbps(1_000_000, 10, 90.0) == pytest.approx(72.0)

    def test_zero_frames(self):
        assert compare.bitrate_mbps(100, 0, 90.0) == 0.0


class TestCurve:
    def test_drops_non_finite_and_zero_rate(self):
        pts = [
            {"bitrate_mbps": 10.0, "psnr_y": 30.0},
            {"bitrate_mbps": 20.0, "psnr_y": float("inf")},
            {"bitrate_mbps": 0.0, "psnr_y": 40.0},
            {"bitrate_mbps": 30.0},
        ]
        r, d = compare.curve(pts, "psnr_y")
        assert r == [10.0] and d == [30.0]


class TestPhase1Gate:
    def test_identical_curves_pass_with_zero_delta(self):
        pts = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        res = compare.phase1_gate(_results(pts, pts), "nxv")
        assert res["pass"] is True
        assert res["worst_delta_db"] == pytest.approx(0.0, abs=1e-9)

    def test_half_a_dB_behind_passes(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        codec = [(qp, r, p - 0.5) for qp, r, p in anchor]
        res = compare.phase1_gate(_results(codec, anchor), "nxv")
        assert res["pass"] is True
        assert res["worst_delta_db"] == pytest.approx(-0.5, abs=1e-6)

    def test_two_dB_behind_fails(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        codec = [(qp, r, p - 2.0) for qp, r, p in anchor]
        res = compare.phase1_gate(_results(codec, anchor), "nxv")
        assert res["pass"] is False
        assert res["worst_delta_db"] == pytest.approx(-2.0, abs=1e-6)

    def test_exactly_at_the_tolerance_passes(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        codec = [(qp, r, p - 1.0) for qp, r, p in anchor]
        res = compare.phase1_gate(_results(codec, anchor), "nxv")
        assert res["pass"] is True

    def test_being_better_than_the_anchor_passes(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        codec = [(qp, r, p + 3.0) for qp, r, p in anchor]
        res = compare.phase1_gate(_results(codec, anchor), "nxv")
        assert res["pass"] is True
        assert res["worst_delta_db"] > 0

    def test_band_is_clipped_to_100_400(self):
        anchor = [(16, 800.0, 48.0), (22, 300.0, 42.0), (28, 150.0, 38.0), (34, 50.0, 32.0)]
        res = compare.phase1_gate(_results(anchor, anchor), "nxv")
        assert res["covered_mbps"][0] == pytest.approx(100.0)
        assert res["covered_mbps"][1] == pytest.approx(400.0)

    def test_curves_outside_the_band_explain_themselves(self):
        low = [(16, 40.0, 44.0), (22, 30.0, 41.0), (28, 20.0, 38.0), (34, 10.0, 35.0)]
        res = compare.phase1_gate(_results(low, low), "nxv")
        assert "error" in res
        assert "not covered" in res["error"]
        assert res.get("pass") is None

    def test_missing_codec_is_reported(self):
        r = _results([], [(16, 380.0, 44.0)] and
                     [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)])
        res = compare.phase1_gate(r, "nxv")
        assert "error" in res

    def test_custom_tolerance(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        codec = [(qp, r, p - 2.0) for qp, r, p in anchor]
        assert compare.phase1_gate(_results(codec, anchor), "nxv",
                                   tolerance_db=3.0)["pass"] is True


class TestBDTable:
    def test_reports_per_anchor(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0), (28, 160.0, 38.0), (34, 110.0, 35.0)]
        codec = [(qp, r * 0.8, p) for qp, r, p in anchor]
        out = compare.bd_table(_results(codec, anchor), "nxv")
        assert out["x264-intra"]["bd_rate_pct"] == pytest.approx(-20.0, abs=1e-6)

    def test_too_few_points_is_explained(self):
        anchor = [(16, 380.0, 44.0), (22, 260.0, 41.0)]
        out = compare.bd_table(_results(anchor, anchor), "nxv")
        assert "error" in out["x264-intra"]
        assert "at least 4" in out["x264-intra"]["error"]


class TestVelocitySplit:
    def _res_with_frames(self):
        per_frame = [{"frame": i, "psnr_y": 40.0 - (2.0 if i >= 8 else 0.0)} for i in range(10)]
        return {
            "codec_key": "nxv",
            "codecs": {"nxv": {"kind": "codec", "points": [
                {"qp": 20, "bitrate_mbps": 120.0, "per_frame": per_frame}]}},
        }

    def test_splits_at_the_percentile(self):
        poses = [{"angular_velocity_deg_s": float(i * 10)} for i in range(10)]
        out = compare.velocity_split(self._res_with_frames(), poses, 20.0)
        assert out["high_frames"] == 2
        assert out["total_frames"] == 10
        p = out["codecs"]["nxv"][0]
        assert p["psnr_y_high_velocity"] == pytest.approx(38.0)
        assert p["psnr_y_low_velocity"] == pytest.approx(40.0)

    def test_empty_pose_log(self):
        assert "error" in compare.velocity_split(self._res_with_frames(), [], 20.0)


class TestParsePoints:
    def test_parses_and_tolerates_spaces(self):
        assert compare.parse_points("16, 22,28 ,34") == [16, 22, 28, 34]

    def test_empty(self):
        assert compare.parse_points("") == []


class TestAnchorConfig:
    def test_x264_intra_sets_keyint_1(self):
        a = ffmpeg.ANCHORS["x264-intra"]
        p = a.params(10)
        assert "keyint=1" in p and "min-keyint=1" in p
        assert "bframes=0" in p

    def test_p_only_anchors_use_one_reference_and_no_b_frames(self):
        for name in ("x264-p", "x265-p"):
            p = ffmpeg.ANCHORS[name].params(10)
            assert "bframes=0" in p
            assert "ref=1" in p
            assert "keyint=1:" not in p  # one IDR at the start, not every frame

    def test_param_flag_matches_encoder(self):
        assert ffmpeg.ANCHORS["x264-intra"].param_flag == "-x264-params"
        assert ffmpeg.ANCHORS["x265-p"].param_flag == "-x265-params"

    def test_phase_anchors_exist(self):
        assert ffmpeg.PHASE1_ANCHOR in ffmpeg.ANCHORS
        assert ffmpeg.PHASE2_ANCHOR in ffmpeg.ANCHORS

    def test_unavailable_encoder_is_reported_not_raised(self, monkeypatch):
        caps = ffmpeg.Caps(ffmpeg="/usr/bin/ffmpeg", version="test", encoders=set())
        monkeypatch.setattr(ffmpeg, "probe", lambda: caps)
        ok, why = ffmpeg.anchor_available(ffmpeg.ANCHORS["x264-intra"])
        assert not ok and "libx264" in why

    def test_no_ffmpeg_is_reported_not_raised(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: ffmpeg.Caps())
        ok, why = ffmpeg.anchor_available(ffmpeg.ANCHORS["x264-intra"])
        assert not ok and "not installed" in why


@pytest.mark.slow
class TestEndToEnd:
    def test_full_run_with_the_dummy_codec(self, tmp_path):
        """Generate, encode with anchors and the mock, measure, analyse, report."""
        caps = ffmpeg.probe()
        if not caps.available or "libx264" not in caps.encoders:
            pytest.skip("needs ffmpeg with libx264")

        from capture.gen_synthetic import build
        seqs = build(
            str(tmp_path / "seq"), "t", frames=4, eye_w=64, eye_h=64, motion="mixed",
            layout="sbs", pix_fmts=["yuv444p"], fps=90.0, seed=1, pano_w=512, pano_h=256,
            objects=3, hud=True, hfov=95.0, vfov=95.0, quiet=True,
        )
        sidecar = str(tmp_path / "seq" / "t.yuv444p.json")
        assert os.path.exists(sidecar)
        assert seqs[0].frames == 4

        out = str(tmp_path / "res.json")
        rc = compare.main([
            "--seq", sidecar,
            "--codec-enc", f"{sys.executable} {DUMMY} enc",
            "--codec-dec", f"{sys.executable} {DUMMY} dec",
            "--codec-name", "mock",
            "--qp", "10,16,22,28",
            "--anchor-qp", "10,16,22,28",
            "--anchors", "x264-intra",
            "--no-vmaf",
            "--work", str(tmp_path / "work"),
            "--out", out,
        ])
        assert rc == 0
        with open(out) as fh:
            res = json.load(fh)

        assert res["codec_key"] == "mock"
        assert len(res["codecs"]["mock"]["points"]) == 4
        assert len(res["codecs"]["x264-intra"]["points"]) == 4
        # the mock quantiser must produce a monotone RD curve
        pts = sorted(res["codecs"]["mock"]["points"], key=lambda p: p["qp"])
        assert [p["bitrate_mbps"] for p in pts] == sorted(
            (p["bitrate_mbps"] for p in pts), reverse=True)
        assert [p["psnr_y"] for p in pts] == sorted((p["psnr_y"] for p in pts), reverse=True)
        assert "bd_rate" in res and "phase1" in res
        assert "velocity_split" in res

        # and the report renders from it
        import report
        md = str(tmp_path / "r.md")
        report.build([out], md, "psnr_y", "auto", "test")
        text = open(md).read()
        assert "Phase 1 exit criterion" in text
        assert "BD-rate" in text
        assert any(f.endswith(".svg") for f in os.listdir(tmp_path))
