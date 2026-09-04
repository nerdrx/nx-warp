"""Report rendering: Markdown tables and both plot backends."""

from __future__ import annotations

import os
import xml.dom.minidom

import pytest

import report


@pytest.fixture
def results():
    def pts(rows):
        return [{"qp": qp, "bytes": 1000, "bitrate_mbps": r, "psnr_y": p,
                 "psnr_ycbcr": p + 0.4, "ssim_y": s, "frames": 4}
                for qp, r, p, s in rows]
    return {
        "schema": 1,
        "generated": "2026-09-04T00:00:00+02:00",
        "elapsed_s": 12.3,
        "sequence": {"name": "seq1", "path": "/x/seq1.yuv", "width": 128, "height": 64,
                     "pix_fmt": "yuv444p", "fps": 90.0, "frames": 4,
                     "source": "synthetic:mixed", "layout": "sbs", "pose_log": None},
        "machine": {"host": "h", "platform": "linux", "ffmpeg": "n9.0.1",
                    "encoders": ["libx264"], "vmaf": False},
        "codec_key": "nxv",
        "codecs": {
            "nxv": {"kind": "codec", "available": True, "rate_control": "qp",
                    "points": pts([(16, 300.0, 43.0, 0.99), (22, 200.0, 40.0, 0.98),
                                   (28, 140.0, 37.0, 0.97), (34, 100.0, 34.0, 0.95)])},
            "x264-intra": {"kind": "anchor", "available": True, "rate_control": "qp",
                           "points": pts([(16, 320.0, 43.5, 0.99), (22, 210.0, 40.4, 0.98),
                                          (28, 150.0, 37.5, 0.97), (34, 105.0, 34.4, 0.96)])},
        },
        "bd_rate": {"psnr_y": {"x264-intra": {"bd_rate_pct": 4.2, "bd_psnr_db": -0.4,
                                              "overlap_lo": 34.0, "overlap_hi": 43.0,
                                              "method": "cubic"}}},
        "phase1": {"anchor": "x264-intra", "codec": "nxv", "band_mbps": [100.0, 400.0],
                   "tolerance_db": 1.0, "metric": "psnr_y", "covered_mbps": [105.0, 300.0],
                   "worst_delta_db": -0.5, "mean_delta_db": -0.45, "best_delta_db": -0.4,
                   "worst_at_mbps": 150.0, "pass": True},
    }


class TestSeries:
    def test_codec_comes_first(self, results):
        s = report._series(results, "psnr_y")
        assert s[0]["name"] == "nxv" and s[0]["is_codec"]
        assert s[1]["name"] == "x264-intra"

    def test_points_are_sorted_by_rate(self, results):
        s = report._series(results, "psnr_y")
        assert s[0]["x"] == sorted(s[0]["x"])

    def test_missing_metric_yields_no_series(self, results):
        assert report._series(results, "vmaf") == []


class TestPlots:
    def test_hand_written_svg_is_well_formed(self, results, tmp_path):
        p = str(tmp_path / "a.svg")
        assert report.plot_svg(report._series(results, "psnr_y"), "psnr_y", "t", p)
        xml.dom.minidom.parse(p)
        text = open(p).read()
        assert "<svg" in text and "polyline" in text
        assert "nxv (codec)" in text

    def test_svg_escapes_titles(self, results, tmp_path):
        p = str(tmp_path / "b.svg")
        report.plot_svg(report._series(results, "psnr_y"), "psnr_y", "a & b <x>", p)
        body = open(p).read()
        assert "&amp;" in body and "<x>" not in body
        xml.dom.minidom.parse(p)

    def test_svg_handles_a_single_point(self, tmp_path):
        s = [{"name": "a", "is_codec": True, "x": [100.0], "y": [40.0], "labels": ["20"]}]
        p = str(tmp_path / "c.svg")
        assert report.plot_svg(s, "psnr_y", "t", p)
        xml.dom.minidom.parse(p)

    def test_svg_refuses_empty_series(self, tmp_path):
        assert not report.plot_svg([], "psnr_y", "t", str(tmp_path / "d.svg"))

    def test_matplotlib_backend_when_available(self, results, tmp_path):
        pytest.importorskip("matplotlib")
        p = str(tmp_path / "e.svg")
        assert report.plot_matplotlib(report._series(results, "psnr_y"), "psnr_y", "t", p)
        xml.dom.minidom.parse(p)

    def test_write_plot_falls_back_to_svg(self, results, tmp_path):
        p = str(tmp_path / "f.svg")
        assert report.write_plot(report._series(results, "psnr_y"), "psnr_y", "t", p, "svg") == "svg"

    def test_write_plot_reports_none_for_empty(self, tmp_path):
        assert report.write_plot([], "psnr_y", "t", str(tmp_path / "g.svg")) == "none"


class TestMarkdown:
    def test_build_writes_md_and_svg(self, results, tmp_path):
        import json
        rp = str(tmp_path / "res.json")
        with open(rp, "w") as fh:
            json.dump(results, fh)
        md = str(tmp_path / "out.md")
        report.build([rp], md, "psnr_y", "auto", "My report")
        text = open(md).read()
        assert "# My report" in text
        assert "## seq1" in text
        assert "**nxv** (codec under test)" in text
        assert "**x264-intra** (anchor)" in text
        assert "| 34 | 100.00 |" in text  # RD table row
        assert "+4.20" in text            # BD-rate
        assert "**PASS**" in text
        assert any(f.endswith(".svg") for f in os.listdir(tmp_path))

    def test_failing_gate_is_rendered_as_fail(self, results, tmp_path):
        import json
        results["phase1"]["pass"] = False
        results["phase1"]["worst_delta_db"] = -2.5
        rp = str(tmp_path / "res.json")
        with open(rp, "w") as fh:
            json.dump(results, fh)
        md = str(tmp_path / "out.md")
        report.build([rp], md, "psnr_y", "svg", "t")
        assert "**FAIL**" in open(md).read()

    def test_unevaluated_gate_is_explained(self, results, tmp_path):
        import json
        results["phase1"] = {"error": "band not covered"}
        rp = str(tmp_path / "res.json")
        with open(rp, "w") as fh:
            json.dump(results, fh)
        md = str(tmp_path / "out.md")
        report.build([rp], md, "psnr_y", "svg", "t")
        assert "Not evaluated" in open(md).read()

    def test_ssim_bd_column_is_not_called_bd_psnr(self, results, tmp_path):
        import json
        results["bd_rate"]["ssim_y"] = {"x264-intra": {
            "bd_rate_pct": 3.0, "bd_psnr_db": -0.01, "overlap_lo": 0.95,
            "overlap_hi": 0.99, "method": "cubic"}}
        rp = str(tmp_path / "res.json")
        with open(rp, "w") as fh:
            json.dump(results, fh)
        md = str(tmp_path / "out.md")
        report.build([rp], md, "psnr_y", "svg", "t")
        assert "BD-quality (SSIM (Y))" in open(md).read()

    def test_unavailable_anchor_is_noted(self, results, tmp_path):
        import json
        results["codecs"]["x265-p"] = {"kind": "anchor", "available": False,
                                       "reason": "libx265 not compiled in", "points": []}
        rp = str(tmp_path / "res.json")
        with open(rp, "w") as fh:
            json.dump(results, fh)
        md = str(tmp_path / "out.md")
        report.build([rp], md, "psnr_y", "svg", "t")
        assert "libx265 not compiled in" in open(md).read()


class TestFormatting:
    def test_none_and_infinity(self):
        assert report._fmt(None) == "-"
        assert report._fmt(float("inf")) == "lossless"
        assert report._fmt(1.23456, "{:.2f}") == "1.23"
