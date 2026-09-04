"""The hardware-class anchor plumbing: foveated QP maps, intra refresh, Vulkan.

Nothing here launches a real encoder.  ffmpeg is mocked at :func:`nxq.ffmpeg.probe`
and :func:`nxq.cpu.run`, so these run identically on a machine with no ffmpeg,
no Vulkan driver and no SVT-AV1.
"""

from __future__ import annotations

import numpy as np
import pytest

import compare
from nxq import cpu, ffmpeg, qpmap
from nxq.yuv import Format


# --- the foveated quantization map ---------------------------------------


class TestFoveaMapParsing:
    def test_default_is_the_documented_map(self):
        m = qpmap.FoveaMap.default()
        assert (m.center_delta, m.mid_delta, m.periphery_delta) == (-6.0, 0.0, 6.0)

    def test_none_and_empty_give_the_default(self):
        assert qpmap.FoveaMap.parse(None) == qpmap.FoveaMap.default()
        assert qpmap.FoveaMap.parse("") == qpmap.FoveaMap.default()

    def test_every_key_is_settable(self):
        m = qpmap.FoveaMap.parse("center=0.3:mid=0.6:dc=-9:dm=1:dp=12")
        assert (m.center_frac, m.mid_frac) == (0.3, 0.6)
        assert (m.center_delta, m.mid_delta, m.periphery_delta) == (-9.0, 1.0, 12.0)

    def test_british_spelling_and_commas(self):
        assert qpmap.FoveaMap.parse("centre=0.3,dp=3").center_frac == 0.3

    def test_unknown_key_is_an_error_not_a_silent_typo(self):
        with pytest.raises(ValueError, match="unknown foveation map key"):
            qpmap.FoveaMap.parse("centre_frac=0.3")

    def test_non_numeric_value_is_an_error(self):
        with pytest.raises(ValueError, match="not a number"):
            qpmap.FoveaMap.parse("dc=lots")

    def test_missing_equals_is_an_error(self):
        with pytest.raises(ValueError, match="not key=value"):
            qpmap.FoveaMap.parse("dc")

    def test_centre_larger_than_mid_is_rejected(self):
        with pytest.raises(ValueError, match="must not be larger"):
            qpmap.FoveaMap.parse("center=0.8:mid=0.4")

    def test_out_of_range_fraction_is_rejected(self):
        with pytest.raises(ValueError, match="must be in"):
            qpmap.FoveaMap.parse("center=0")


class TestFoveaMapGeometry:
    def test_mono_gives_one_centre_one_mid_and_the_periphery(self):
        r = qpmap.FoveaMap.default().regions(1000, 500, "mono")
        assert len(r) == 3
        assert r[-1] == (0, 0, 1000, 500, 6.0)

    def test_sbs_gives_one_fovea_per_eye(self):
        r = qpmap.FoveaMap.default().regions(1000, 500, "sbs")
        centres = [x for x in r if x[4] == -6.0]
        assert len(centres) == 2
        # one in each half, neither straddling the seam
        assert centres[0][0] + centres[0][2] <= 500 <= centres[1][0]

    def test_most_specific_region_comes_first(self):
        # ffmpeg's ROI side data gives priority to the FIRST matching region,
        # so the centre must precede the mid box and the mid box the periphery.
        deltas = [d for *_, d in qpmap.FoveaMap.default().regions(1000, 500, "mono")]
        assert deltas == [-6.0, 0.0, 6.0]

    def test_centre_box_is_centred_on_the_view(self):
        x, y, w, h, _ = qpmap.FoveaMap(center_frac=0.5).regions(1000, 500, "mono")[0]
        assert (w, h) == (500, 250)
        assert (x + w / 2, y + h / 2) == (500.0, 250.0)

    def test_boxes_never_collapse_to_zero(self):
        x, y, w, h, _ = qpmap.FoveaMap(center_frac=0.001).regions(16, 16, "mono")[0]
        assert w >= 1 and h >= 1

    def test_addroi_chain_scales_deltas_against_the_qp_range(self):
        chain = qpmap.FoveaMap.default().addroi_chain(1000, 500, "mono")
        assert chain.count("addroi=") == 3
        assert "qoffset=-6/51" in chain
        assert "qoffset=6/51" in chain
        assert chain.split(",")[0].startswith("addroi=")
        assert chain.endswith("addroi=x=0:y=0:w=1000:h=500:qoffset=6/51")

    def test_json_names_the_extension_it_emulates(self):
        j = qpmap.FoveaMap.default().to_json()
        assert "VK_KHR_video_encode_quantization_map" in j["kind"]
        assert "CRF only" in j["mechanism"]


# --- anchor definitions --------------------------------------------------


class TestAnchorParams:
    @pytest.mark.parametrize("name", ["x264-p-refresh", "x265-p-refresh"])
    def test_refresh_anchors_ask_for_periodic_intra_refresh(self, name):
        p = ffmpeg.ANCHORS[name].params(24)
        assert "intra-refresh=1" in p
        # the sweep is the clip by default, not the 1000-frame "never" keyint
        assert "keyint=24:min-keyint=24" in p

    @pytest.mark.parametrize("name", ["x264-p", "x265-p", "x264-intra", "x265-intra"])
    def test_the_flat_anchors_are_untouched(self, name):
        assert "intra-refresh" not in ffmpeg.ANCHORS[name].params(24)

    def test_an_explicit_refresh_period_wins(self):
        import dataclasses
        a = dataclasses.replace(ffmpeg.ANCHORS["x265-p-refresh"], refresh_period=8)
        assert a.period(24) == 8
        assert "keyint=8:min-keyint=8" in a.params(24)

    def test_refresh_anchors_keep_the_low_latency_configuration(self):
        p = ffmpeg.ANCHORS["x265-p-refresh"].params(24)
        assert "bframes=0" in p and "ref=1" in p and "rc-lookahead=0" in p

    def test_every_anchor_declares_a_rate_control_mode(self):
        for a in ffmpeg.ANCHORS.values():
            assert a.rc_modes


class TestAnchorRateControl:
    def test_the_requested_mode_is_used_when_supported(self):
        assert ffmpeg.anchor_rc(ffmpeg.ANCHORS["x264-intra"], "crf") == "crf"

    def test_foveated_anchors_fall_back_to_crf(self):
        # x264/x265 disable adaptive quantization in constant-QP mode and the
        # per-block offsets ride on the AQ path, so a CQP foveated encode would
        # be silently identical to a flat one.
        assert ffmpeg.anchor_rc(ffmpeg.ANCHORS["x265-p-refresh"], "qp") == "crf"

    def test_vulkan_anchors_fall_back_to_qp(self):
        assert ffmpeg.anchor_rc(ffmpeg.ANCHORS["hevc-vulkan"], "crf") == "qp"


# --- availability --------------------------------------------------------


def _caps(*encoders, vmaf=False):
    c = ffmpeg.Caps(ffmpeg="/usr/bin/ffmpeg", version="test")
    c.encoders = set(encoders)
    if vmaf:
        c.filters = {"libvmaf"}
    return c


class TestAnchorAvailability:
    def test_no_ffmpeg_is_reported_not_raised(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: ffmpeg.Caps())
        ok, why = ffmpeg.anchor_available(ffmpeg.ANCHORS["hevc-vulkan"])
        assert not ok and "not installed" in why

    def test_missing_encoder_is_reported(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("libx264"))
        ok, why = ffmpeg.anchor_available(ffmpeg.ANCHORS["av1-svt-p"])
        assert not ok and "libsvtav1" in why

    def test_444_is_refused_by_the_vulkan_anchors_and_says_why(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("hevc_vulkan"))
        monkeypatch.setattr(ffmpeg, "vulkan_ready", lambda *a, **k: (True, ""))
        ok, why = ffmpeg.anchor_available(
            ffmpeg.ANCHORS["hevc-vulkan"], Format(64, 64, "yuv444p")
        )
        assert not ok
        assert "yuv444p" in why and "4:4:4" in why

    def test_420_is_accepted_by_the_vulkan_anchors(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("hevc_vulkan"))
        monkeypatch.setattr(ffmpeg, "vulkan_ready", lambda *a, **k: (True, ""))
        ok, _ = ffmpeg.anchor_available(
            ffmpeg.ANCHORS["hevc-vulkan"], Format(64, 64, "yuv420p")
        )
        assert ok

    def test_444_is_refused_by_svtav1(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("libsvtav1"))
        ok, why = ffmpeg.anchor_available(
            ffmpeg.ANCHORS["av1-svt-p"], Format(64, 64, "yuv444p")
        )
        assert not ok and "yuv420p only" in why

    def test_the_software_anchors_take_any_pixel_format(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("libx265"))
        for fmt in ("yuv444p", "yuv420p"):
            ok, _ = ffmpeg.anchor_available(
                ffmpeg.ANCHORS["x265-p-refresh"], Format(64, 64, fmt)
            )
            assert ok


class TestVulkanReady:
    def setup_method(self):
        ffmpeg.vulkan_ready.cache_clear()

    def teardown_method(self):
        ffmpeg.vulkan_ready.cache_clear()

    def test_encoder_not_compiled_in(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("libx264"))
        ok, why = ffmpeg.vulkan_ready("hevc_vulkan")
        assert not ok and "not compiled into this ffmpeg" in why

    def test_driver_without_encode_support_is_detected_not_raised(self, monkeypatch):
        import subprocess
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("hevc_vulkan"))
        monkeypatch.setattr(cpu, "run", lambda *a, **k: subprocess.CompletedProcess(
            a[0] if a else [], 1, "",
            "[hevc_vulkan] No support for encoding H265\nConversion failed!"))
        ok, why = ffmpeg.vulkan_ready("hevc_vulkan")
        assert not ok
        assert "lacks video encode support" in why
        assert "No support for encoding H265" in why

    def test_a_working_driver_reports_ready(self, monkeypatch):
        import subprocess
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("hevc_vulkan"))
        monkeypatch.setattr(cpu, "run", lambda *a, **k: subprocess.CompletedProcess(
            a[0] if a else [], 0, "", ""))
        assert ffmpeg.vulkan_ready("hevc_vulkan") == (True, "")

    def test_a_probe_that_cannot_launch_is_reported(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("hevc_vulkan"))

        def boom(*a, **k):
            raise OSError("no such file")
        monkeypatch.setattr(cpu, "run", boom)
        ok, why = ffmpeg.vulkan_ready("hevc_vulkan")
        assert not ok and "could not run" in why

    def test_the_result_is_cached_per_encoder(self, monkeypatch):
        import subprocess
        calls = []
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("hevc_vulkan", "h264_vulkan"))

        def counting(*a, **k):
            calls.append(a[0])
            return subprocess.CompletedProcess(a[0], 0, "", "")
        monkeypatch.setattr(cpu, "run", counting)
        ffmpeg.vulkan_ready("hevc_vulkan")
        ffmpeg.vulkan_ready("hevc_vulkan")
        ffmpeg.vulkan_ready("h264_vulkan")
        assert len(calls) == 2


# --- command-line construction -------------------------------------------


@pytest.fixture
def captured(monkeypatch, tmp_path):
    """Capture the argv encode_anchor builds, without running ffmpeg."""
    seen: dict = {}

    def fake_run(args, what, want_cmd=False):
        seen["args"] = list(args)
        (tmp_path / "bs").write_bytes(b"x" * 123)
        return ["ffmpeg", *args] if want_cmd else ""

    monkeypatch.setattr(ffmpeg, "probe", lambda: _caps(
        "libx264", "libx265", "hevc_vulkan", "h264_vulkan", "libsvtav1"))
    monkeypatch.setattr(ffmpeg, "vulkan_ready", lambda *a, **k: (True, ""))
    monkeypatch.setattr(ffmpeg, "_run_ffmpeg", fake_run)
    return seen, str(tmp_path / "bs")


def _argstr(seen):
    return " ".join(seen["args"])


class TestEncodeCommandLines:
    def test_foveated_anchor_installs_the_addroi_chain(self, captured):
        seen, out = captured
        ffmpeg.encode_anchor(ffmpeg.ANCHORS["x265-p-refresh"], "src.yuv",
                             Format(1000, 500, "yuv444p"), out, crf=28, nframes=10)
        s = _argstr(seen)
        assert "addroi=" in s and "qoffset=-6/51" in s
        assert "intra-refresh=1" in s
        assert "-crf 28" in s

    def test_the_map_is_configurable_per_run(self, captured):
        seen, out = captured
        ffmpeg.encode_anchor(ffmpeg.ANCHORS["x265-p-refresh"], "src.yuv",
                             Format(1000, 500, "yuv444p"), out, crf=28, nframes=10,
                             fovea=qpmap.FoveaMap.parse("dc=-12:dp=9"))
        s = _argstr(seen)
        assert "qoffset=-12/51" in s and "qoffset=9/51" in s

    def test_sbs_layout_reaches_the_map(self, captured):
        seen, out = captured
        ffmpeg.encode_anchor(ffmpeg.ANCHORS["x264-p-refresh"], "src.yuv",
                             Format(1000, 500, "yuv444p"), out, crf=28, nframes=10,
                             layout="sbs")
        assert _argstr(seen).count("qoffset=-6/51") == 2

    def test_flat_anchors_get_no_filter_chain(self, captured):
        seen, out = captured
        ffmpeg.encode_anchor(ffmpeg.ANCHORS["x264-intra"], "src.yuv",
                             Format(64, 64, "yuv444p"), out, qp=20, nframes=10)
        assert "-vf" not in seen["args"]

    def test_vulkan_anchor_opens_a_device_and_uploads(self, captured):
        seen, out = captured
        ffmpeg.encode_anchor(ffmpeg.ANCHORS["hevc-vulkan"], "src.yuv",
                             Format(64, 64, "yuv420p"), out, qp=24, nframes=10)
        s = _argstr(seen)
        assert "-init_hw_device" in s and "vulkan=vk:" in s
        assert "format=nv12,hwupload" in s
        assert "-tune ull" in s and "-rc_mode cqp" in s
        assert "-c:v hevc_vulkan" in s

    def test_svtav1_anchor_is_low_delay_p(self, captured):
        seen, out = captured
        ffmpeg.encode_anchor(ffmpeg.ANCHORS["av1-svt-p"], "src.yuv",
                             Format(64, 64, "yuv420p"), out, crf=35, nframes=10)
        s = _argstr(seen)
        assert "pred-struct=1" in s and "lookahead=0" in s
        assert "-c:v libsvtav1" in s and "-crf 35" in s

    def test_giving_both_qp_and_crf_is_refused(self, captured):
        _, out = captured
        with pytest.raises(ffmpeg.FFmpegError, match="exactly one"):
            ffmpeg.encode_anchor(ffmpeg.ANCHORS["x264-p"], "src.yuv",
                                 Format(64, 64, "yuv444p"), out, qp=1, crf=1, nframes=10)

    def test_an_unavailable_anchor_raises_before_running_anything(self, monkeypatch):
        monkeypatch.setattr(ffmpeg, "probe", lambda: _caps("libx264"))
        with pytest.raises(ffmpeg.FFmpegError, match="unavailable"):
            ffmpeg.encode_anchor(ffmpeg.ANCHORS["av1-svt-p"], "src.yuv",
                                 Format(64, 64, "yuv420p"), "out", crf=30, nframes=4)

    def test_the_exact_command_line_comes_back(self, captured):
        seen, out = captured
        enc = ffmpeg.encode_anchor(ffmpeg.ANCHORS["x264-intra"], "src.yuv",
                                   Format(64, 64, "yuv444p"), out, qp=20, nframes=10)
        assert enc.size == 123
        assert enc.cmdline.startswith("ffmpeg ")
        assert "-qp 20" in enc.cmdline

    def test_cmdline_quotes_awkward_paths(self):
        e = ffmpeg.Encoded(1, ["ffmpeg", "-i", "a file.yuv"])
        assert "'a file.yuv'" in e.cmdline


class TestCodecCommandLine:
    def test_encode_argv_is_what_the_harness_records(self):
        from nxq.codec import CodecCLI
        cli = CodecCLI.from_args("nxv")
        argv = cli.encode_argv("in.yuv", Format(8, 8, "yuv420p"), 22, "out.nxv")
        assert argv[0] == "nxv-enc"
        assert "--qp" in argv and "22" in argv
        assert "--pix" in argv and "yuv420p" in argv


# --- eccentricity-weighted scoring ---------------------------------------


def _sc(**kw):
    # 4 ppd over a 128 px view puts the corners past 15 degrees, so the 8-degree
    # fovea disc is a real subset of the frame rather than the whole of it.
    return compare.FoveaScoring(ppd_center=kw.pop("ppd", 4.0), **kw)


def _plain_psnr(ref, dis):
    from nxq import metrics
    return metrics.psnr_plane(ref, dis)


class TestFoveatedScoring:
    def test_identical_frames_are_lossless(self):
        a = np.full((64, 64), 128, np.uint8)
        out = compare._fovea_frame(a, a, _sc(), {})
        assert out["fov_psnr_y"] == float("inf")

    def test_a_centre_error_costs_more_than_the_same_error_in_the_periphery(self):
        ref = np.full((128, 128), 128, np.uint8)
        centre = ref.copy()
        centre[56:72, 56:72] = 148
        edge = ref.copy()
        edge[0:16, 0:16] = 148
        c = compare._fovea_frame(ref, centre, _sc(), {})
        e = compare._fovea_frame(ref, edge, _sc(), {})
        # plain PSNR cannot tell them apart; the weighted one can
        assert _plain_psnr(ref, centre) == pytest.approx(_plain_psnr(ref, edge), abs=1e-9)
        assert c["fov_psnr_y"] < e["fov_psnr_y"]

    def test_the_hard_region_split_follows_the_error(self):
        ref = np.full((128, 128), 128, np.uint8)
        dis = ref.copy()
        dis[0:16, 0:16] = 148           # periphery only
        out = compare._fovea_frame(ref, dis, _sc(), {})
        assert out["psnr_fovea"] == float("inf")
        assert np.isfinite(out["psnr_periphery"])

    def test_uniform_weighting_reduces_to_plain_psnr(self):
        rng = np.random.default_rng(7)
        ref = rng.integers(0, 255, (64, 64), dtype=np.uint8)
        dis = rng.integers(0, 255, (64, 64), dtype=np.uint8)
        out = compare._fovea_frame(ref, dis, _sc(weighting="uniform"), {})
        assert out["fov_psnr_y"] == pytest.approx(_plain_psnr(ref, dis), abs=1e-9)

    def test_sbs_scores_both_eyes(self):
        ref = np.full((128, 256), 128, np.uint8)
        dis = ref.copy()
        dis[62:66, 190:194] = 200        # only the right eye's centre is damaged
        mono = compare._fovea_frame(ref, dis, _sc(), {})
        sbs = compare._fovea_frame(ref, dis, _sc(layout="sbs"), {})
        assert np.isfinite(sbs["fov_psnr_y"])
        # the damage sits at the right eye's centre, so weighting it per eye
        # makes it matter more than treating the pair as one wide image
        assert sbs["fov_psnr_y"] < mono["fov_psnr_y"]

    def test_the_eccentricity_map_is_cached_across_frames(self):
        cache: dict = {}
        a = np.full((32, 32), 100, np.uint8)
        compare._fovea_frame(a, a, _sc(), cache)
        n = len(cache)
        compare._fovea_frame(a, a, _sc(), cache)
        assert len(cache) == n == 1

    def test_scoring_config_is_recorded_for_the_results_json(self):
        j = _sc(layout="sbs").to_json()
        assert j["fixation"] == "view centre"
        assert j["ppd_center"] == 4.0
        assert j["e2_deg"] == pytest.approx(2.3)


class TestMeasureWiring:
    def test_foveated_metrics_appear_only_when_asked(self, tmp_path):
        from nxq.yuv import Format as F
        fmt = F(16, 16, "yuv420p")
        raw = tmp_path / "a.yuv"
        raw.write_bytes(bytes(fmt.frame_bytes * 2))
        plain = compare.measure(str(raw), str(raw), fmt, fps=90.0,
                                do_ssim=False, do_vmaf=False)
        assert "fov_psnr_y" not in plain
        fovd = compare.measure(str(raw), str(raw), fmt, fps=90.0,
                               do_ssim=False, do_vmaf=False,
                               fovea=compare.FoveaScoring(ppd_center=8.0))
        assert "fov_psnr_y" in fovd
        assert "fov_psnr_y" in fovd["per_frame"][0]
