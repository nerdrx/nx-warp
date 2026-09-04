"""The PAPER.md 5.3 perceptual metrics: FovVideoVDP, fov-SSIM, pop-in, latency.

`pyfvvdp` is **mocked** throughout.  These tests must pass on a machine with no
PyTorch, no GPU and no reference implementation, so what is checked here is the
harness's half of the contract: the display model it builds, the arrays and the
fixation it hands over, the gaze segmentation, and the graceful failure when the
package is absent.  The metric itself is the authors' and is not re-tested.

The pop-in model has no such excuse: it is a port of `rc/src/tvm.cpp` and is
pinned against the numbers `rc/RESULTS-temporal.md` prints from the C++ side.
"""

from __future__ import annotations

import json
import math
import sys
import types

import numpy as np
import pytest

import foveated_metrics as fov
from nxq import fvvdp as nxfv
from nxq import latency, popin
from nxq.yuv import Format, Frame


# --- the display model ---------------------------------------------------


class TestHeadsetDisplay:
    def test_pico4_panel_ppd(self):
        """2160 px over 100 degrees is 15.8 ppd on axis, not 21.6."""
        d = nxfv.HEADSETS["pico4"]
        assert d.ppd_center(2160) == pytest.approx(15.81, abs=0.01)
        assert 2160 / 100.0 == pytest.approx(21.6)

    def test_ppd_agrees_with_foveated_metrics(self):
        """One acuity geometry for the whole harness, not two."""
        d = nxfv.HEADSETS["pico4"]
        for w in (256, 1024, 2160):
            assert d.ppd_center(w) == pytest.approx(fov.ppd_from_fov(w, 100.0))

    def test_ppd_follows_the_sequence_not_the_panel(self):
        d = nxfv.HEADSETS["pico4"]
        g = d.geometry_json(1024, 1024)
        assert g["panel_ppd_center"] == pytest.approx(15.81, abs=0.01)
        assert g["scored_ppd_center"] == pytest.approx(7.50, abs=0.01)
        assert g["scored_px_per_eye"] == [1024, 1024]

    def test_display_size_subtends_the_fov(self):
        d = nxfv.HEADSETS["pico4"]
        w, h = d.display_size_m(2160, 2160)
        subtended = 2.0 * math.degrees(math.atan(w / (2.0 * d.distance_m)))
        assert subtended == pytest.approx(d.fov_horizontal_deg)
        assert h == pytest.approx(w)  # square panel


# --- the mock ------------------------------------------------------------


class _FakeMetric:
    """Records every predict() call and returns a JOD derived from the input."""

    calls: list[dict] = []

    def __init__(self, **kwargs):
        self.kwargs = kwargs

    def predict(self, test, reference, dim_order, frames_per_second, fixation_point):
        _FakeMetric.calls.append({
            "test": test, "reference": reference, "dim_order": dim_order,
            "fps": frames_per_second, "fixation": fixation_point,
            "ctor": self.kwargs,
        })
        err = float(np.mean(np.abs(test.astype(float) - reference.astype(float))))
        return 10.0 - err / 25.0, {}


@pytest.fixture
def fake_pyfvvdp(monkeypatch):
    """Install a mock ``pyfvvdp`` (and a mock ``torch``) into sys.modules."""
    _FakeMetric.calls = []

    torch = types.ModuleType("torch")
    torch.device = lambda name: f"device:{name}"
    torch.set_num_threads = lambda n: None
    torch.cuda = types.SimpleNamespace(is_available=lambda: False)

    mod = types.ModuleType("pyfvvdp")
    mod.__version__ = "1.2.2-mock"
    mod.fvvdp = _FakeMetric

    def photo(peak, **kw):
        return {"peak": peak, **kw}

    class Geom:
        def __init__(self, resolution, distance_m=None, fov_horizontal=None):
            self.resolution = resolution
            self.distance_m = distance_m
            self.fov_horizontal = fov_horizontal
            f = (resolution[0] / 2.0) / math.tan(math.radians(fov_horizontal) / 2.0)
            self.ppd_centre = f * math.pi / 180.0

    mod.fvvdp_display_photo_eotf = photo
    mod.fvvdp_display_geometry = Geom
    monkeypatch.setitem(sys.modules, "torch", torch)
    monkeypatch.setitem(sys.modules, "pyfvvdp", mod)
    return mod


class TestAvailability:
    def test_missing_package_is_reported_not_guessed(self, monkeypatch):
        real_import = __builtins__["__import__"] if isinstance(__builtins__, dict) \
            else __builtins__.__import__

        def blocked(name, *a, **kw):
            if name in ("torch", "pyfvvdp"):
                raise ImportError(f"no module named {name}")
            return real_import(name, *a, **kw)

        monkeypatch.setitem(sys.modules, "torch", None)
        monkeypatch.setattr("builtins.__import__", blocked)
        ok, why = nxfv.available()
        assert not ok
        assert "torch" in why.lower() or "pyfvvdp" in why.lower()

    def test_present_package_is_available(self, fake_pyfvvdp):
        ok, why = nxfv.available()
        assert ok and why == ""


# --- the runner ----------------------------------------------------------


def _write_seq(path, fmt, frames, value_fn):
    with open(path, "wb") as fh:
        for i in range(frames):
            y = np.full((fmt.height, fmt.width), value_fn(i), np.uint8)
            cw, ch = fmt.chroma_size
            u = np.full((ch, cw), 128, np.uint8)
            fh.write(Frame(y, u, u).tobytes())


class TestFvvdpRunner:
    def _runner(self, tmp_path, layout="mono", **kw):
        sc = nxfv.FvvdpScoring(display=nxfv.HEADSETS["pico4"], layout=layout, **kw)
        return nxfv.FvvdpRunner(sc, fps=90.0)

    def test_hands_over_fhwc_uint8_at_the_sequence_fps(self, tmp_path, fake_pyfvvdp):
        fmt = Format(64, 64, "yuv444p")
        ref, dis = tmp_path / "r.yuv", tmp_path / "d.yuv"
        _write_seq(ref, fmt, 4, lambda i: 100)
        _write_seq(dis, fmt, 4, lambda i: 110)
        out = self._runner(tmp_path).score(str(ref), str(dis), fmt)
        call = _FakeMetric.calls[-1]
        assert call["dim_order"] == "FHWC"
        assert call["fps"] == 90.0
        assert call["test"].shape == (4, 64, 64, 3)
        assert call["test"].dtype == np.uint8
        assert out["views"].keys() == {"mono"}
        assert out["jod"] == out["views"]["mono"]["jod"]

    def test_stereo_is_scored_per_eye_and_averaged(self, tmp_path, fake_pyfvvdp):
        fmt = Format(128, 64, "yuv444p")
        ref, dis = tmp_path / "r.yuv", tmp_path / "d.yuv"
        _write_seq(ref, fmt, 2, lambda i: 100)
        _write_seq(dis, fmt, 2, lambda i: 108)
        out = self._runner(tmp_path, layout="sbs").score(str(ref), str(dis), fmt)
        assert set(out["views"]) == {"left", "right"}
        assert _FakeMetric.calls[-1]["test"].shape == (2, 64, 64, 3)
        assert out["jod"] == pytest.approx(
            np.mean([v["jod"] for v in out["views"].values()]))
        assert out["jod_min"] == min(v["jod"] for v in out["views"].values())

    def test_scored_ppd_is_the_view_width_not_the_panel(self, tmp_path, fake_pyfvvdp):
        fmt = Format(128, 64, "yuv444p")
        ref, dis = tmp_path / "r.yuv", tmp_path / "d.yuv"
        _write_seq(ref, fmt, 2, lambda i: 100)
        _write_seq(dis, fmt, 2, lambda i: 100)
        out = self._runner(tmp_path, layout="sbs").score(str(ref), str(dis), fmt)
        assert out["views"]["left"]["ppd_center"] == pytest.approx(
            fov.ppd_from_fov(64, 100.0))

    def test_centre_fixation_passes_none(self, tmp_path, fake_pyfvvdp):
        """pyfvvdp defaults to the image centre; we do not second-guess it."""
        fmt = Format(64, 64, "yuv444p")
        ref, dis = tmp_path / "r.yuv", tmp_path / "d.yuv"
        _write_seq(ref, fmt, 2, lambda i: 100)
        _write_seq(dis, fmt, 2, lambda i: 100)
        self._runner(tmp_path).score(str(ref), str(dis), fmt)
        assert _FakeMetric.calls[-1]["fixation"] is None

    def test_explicit_fixation_is_passed_through(self, tmp_path, fake_pyfvvdp):
        fmt = Format(64, 64, "yuv444p")
        ref, dis = tmp_path / "r.yuv", tmp_path / "d.yuv"
        _write_seq(ref, fmt, 2, lambda i: 100)
        _write_seq(dis, fmt, 2, lambda i: 100)
        self._runner(tmp_path, fixation=(12.0, 34.0)).score(str(ref), str(dis), fmt)
        assert list(_FakeMetric.calls[-1]["fixation"]) == [12.0, 34.0]

    def test_gaze_log_splits_the_clip_into_segments(self, tmp_path, fake_pyfvvdp):
        fmt = Format(64, 64, "yuv444p")
        ref, dis = tmp_path / "r.yuv", tmp_path / "d.yuv"
        _write_seq(ref, fmt, 6, lambda i: 100)
        _write_seq(dis, fmt, 6, lambda i: 100)
        gaze = tmp_path / "gaze.json"
        # ppd at 64 px over 100 deg is 0.47, so a 10 px jump is ~21 degrees.
        rows = [{"x": 32, "y": 32}] * 3 + [{"x": 42, "y": 32}] * 3
        gaze.write_text(json.dumps(rows))
        out = self._runner(tmp_path, gaze_log=str(gaze), gaze_tol_deg=2.0).score(
            str(ref), str(dis), fmt)
        assert out["views"]["mono"]["segments"] == 2
        assert [c["test"].shape[0] for c in _FakeMetric.calls[-2:]] == [3, 3]

    def test_missing_implementation_raises_with_the_install_line(self, monkeypatch):
        monkeypatch.setattr(nxfv, "available", lambda: (False, "pyfvvdp is not installed"))
        with pytest.raises(RuntimeError, match="not installed"):
            nxfv.FvvdpRunner(nxfv.FvvdpScoring(display=nxfv.HEADSETS["pico4"]), 90.0)


class TestGazeSegments:
    def test_constant_gaze_is_one_segment(self):
        g = [(10.0, 10.0)] * 8
        assert nxfv.gaze_segments(g, 8, ppd=10.0, tol_deg=1.0) == [(0, 8, (10.0, 10.0))]

    def test_segments_cover_every_frame_exactly_once(self):
        g = [(float(i * 5), 0.0) for i in range(20)]
        segs = nxfv.gaze_segments(g, 20, ppd=1.0, tol_deg=2.0)
        covered = [i for lo, hi, _ in segs for i in range(lo, hi)]
        assert covered == list(range(20))

    def test_no_gaze_is_one_unfixated_segment(self):
        assert nxfv.gaze_segments([], 5, ppd=10.0, tol_deg=1.0) == [(0, 5, None)]


# --- the Tursun-Didyk port ----------------------------------------------


class TestVisibilityModel:
    @pytest.mark.parametrize("f_s,e,want", [
        (0.25, 1.0, 4.86), (1.00, 30.0, 651.95), (4.00, 40.0, 45.13),
        (2.00, 10.0, 36.57), (3.00, 20.0, 80.93), (0.25, 40.0, 789.64),
    ])
    def test_matches_the_cpp_table(self, f_s, e, want):
        """rc/RESULTS-temporal.md section 1, sensitivity at f_t = 12 Hz."""
        assert float(popin.sensitivity(12.0, f_s, e)) == pytest.approx(want, abs=0.005)

    def test_de_lange_curve_is_band_pass(self):
        """Approximation 1: log-frequency argument, peak near 10 Hz, ~0 at 70."""
        f = np.array([0.0, 1.0, 10.0, 36.0, 72.0])
        s = np.asarray(popin.sensitivity_temporal(f))
        assert s[2] == max(s)
        # S_DL(0) = a0 = 3.2714; after the soft-plus that is 3.309.
        assert s[0] == pytest.approx(3.309, abs=0.01)
        assert s[4] < 0.5

    def test_sensitivity_rises_with_eccentricity(self):
        """The Ferry-Porter direction, and a theorem under the 8.2 clamps."""
        e = np.linspace(0.5, 60.0, 40)
        for f_s in (0.25, 1.0, 4.0):
            s = np.asarray(popin.sensitivity(12.0, f_s, e))
            assert np.all(np.diff(s) > 0)

    def test_white_noise_gives_R_one(self):
        rng = np.random.default_rng(7)
        plane = rng.integers(0, 256, (64, 64)).astype(np.uint8)
        st = popin.tile_stats(plane)
        assert float(st["freq_ratio"][0, 0]) == pytest.approx(1.0, abs=0.1)

    def test_spatial_freq_inversion(self):
        """R = 1 (white noise) is 1/4 cycle per sample per axis, doubled."""
        f = float(popin.spatial_freq_cpd(1.0, 20.0))
        assert f == pytest.approx(2.0 * 20.0 * math.asin(math.sqrt(0.5)) / (2 * math.pi))

    def test_visibility_is_monotone_in_step_and_k(self):
        args = dict(fps=90.0, f_s_cpd=1.0, ecc_deg=20.0, mean_luma=128.0)
        v = [float(popin.step_visibility(d, 3, **args)) for d in (1, 2, 4, 8)]
        assert v == sorted(v)
        vk = [float(popin.step_visibility(4.0, k, **args)) for k in (2, 3, 4, 6)]
        assert vk == sorted(vk)

    def test_k_of_one_is_exactly_zero(self):
        assert float(popin.step_visibility(50.0, 1, 90.0, 1.0, 20.0, 128.0)) == 0.0

    def test_detect_prob_spans_guess_to_certain(self):
        assert float(popin.detect_prob(0.0)) == pytest.approx(0.5)
        assert float(popin.detect_prob(1.0)) == pytest.approx(0.67, abs=0.01)
        assert float(popin.detect_prob(1e3)) == pytest.approx(1.0)

    def test_weber_floor_is_active_at_headset_luminance(self):
        """Approximation 5: every tile sits under the 50 nit de Vries-Rose floor."""
        p = popin.TvmParams()
        assert float(popin.luma_to_nits(255.0, p)) <= p.l_min_nits * 2.0
        c_bright = float(popin.weber_contrast(4.0, 250.0, p))
        c_dim = float(popin.weber_contrast(4.0, 60.0, p))
        assert c_bright > c_dim  # linear in dL while the floor holds


class TestTileStats:
    def test_matches_a_direct_loop(self):
        rng = np.random.default_rng(3)
        plane = rng.integers(0, 256, (128, 128)).astype(np.uint8)
        st = popin.tile_stats(plane, 64)
        block = plane[0:64, 0:64].astype(np.float64)
        assert st["mean"][0, 0] == pytest.approx(block.mean())
        n = 64
        acc = 0.0
        for y in range(n):
            for x in range(n):
                gx = 0.5 * (block[y, min(x + 1, n - 1)] - block[y, max(x - 1, 0)])
                gy = 0.5 * (block[min(y + 1, n - 1), x] - block[max(y - 1, 0), x])
                acc += gx * gx + gy * gy
        assert st["grad_energy"][0, 0] == pytest.approx(acc / (n * n))

    def test_partial_tiles_are_edge_replicated(self):
        plane = np.zeros((70, 70), np.uint8)
        st = popin.tile_stats(plane, 64)
        assert st["mean"].shape == (2, 2)

    def test_tile_mad(self):
        a = np.zeros((64, 128), np.uint8)
        b = np.zeros((64, 128), np.uint8)
        b[:, 64:] = 10
        mad = popin.tile_mad(a, b, 64)
        assert mad.shape == (1, 2)
        assert mad[0, 0] == 0.0 and mad[0, 1] == pytest.approx(10.0)


class TestSkipSchedule:
    def test_ladder_cadence_and_frame_zero(self):
        ecc = np.array([[1.0, 12.0, 40.0]])
        s = popin.SkipSchedule.ladder(6, 1, ecc, rings=popin.FLOETER_11223)
        assert s.flags.shape == (6, 1, 1, 3)
        assert list(s.flags[:, 0, 0, 0]) == [0, 0, 0, 0, 0, 0]   # fovea, k=1
        assert list(s.flags[:, 0, 0, 1]) == [0, 1, 0, 1, 0, 1]   # k=2
        assert list(s.flags[:, 0, 0, 2]) == [0, 1, 1, 0, 1, 1]   # k=3

    def test_round_trip_is_eye_major(self, tmp_path):
        ecc = np.array([[1.0, 40.0], [40.0, 40.0]])
        s = popin.SkipSchedule.ladder(4, 2, ecc)
        p = tmp_path / "m.skipmap"
        s.write(str(p))
        assert p.stat().st_size == 4 * 2 * 2 * 2
        back = popin.SkipSchedule.read(str(p), 2, 2, 2)
        assert np.array_equal(back.flags, s.flags)

    def test_read_rejects_a_wrong_geometry(self, tmp_path):
        p = tmp_path / "m.skipmap"
        p.write_bytes(b"\0" * 15)
        with pytest.raises(ValueError, match="whole number"):
            popin.SkipSchedule.read(str(p), 1, 2, 2)

    def test_stale_run_lengths(self):
        skip = np.array([[0], [1], [1], [0], [0]], dtype=np.uint8)[:, :, None]
        runs = popin.stale_run_lengths(skip)
        assert list(runs[:, 0, 0]) == [1, 1, 2, 3, 1]


class TestPopInMetric:
    def _frames(self, n, fn):
        u = np.full((64, 64), 128, np.uint8)
        return [Frame(fn(i), u, u) for i in range(n)]

    def test_a_perfect_codec_pops_not_at_all(self):
        rng = np.random.default_rng(11)
        planes = [rng.integers(0, 256, (64, 64)).astype(np.uint8) for _ in range(5)]
        ref = self._frames(5, lambda i: planes[i])
        dis = self._frames(5, lambda i: planes[i])
        sc = popin.PopInScoring(ppd_center=10.0, fps=90.0)
        out = popin.score_sequence(ref, dis, sc)
        assert out["popin_c_m"]["max"] == 0.0
        assert out["mode"] == "all-frames"

    def test_content_motion_cancels(self):
        """A codec that adds a constant offset moves with the content, so the
        excess frame-to-frame change is zero even though PSNR is not."""
        planes = [np.full((64, 64), 100 + 10 * i, np.uint8) for i in range(5)]
        ref = self._frames(5, lambda i: planes[i])
        dis = self._frames(5, lambda i: (planes[i].astype(int) + 5).astype(np.uint8))
        sc = popin.PopInScoring(ppd_center=10.0, fps=90.0)
        out = popin.score_sequence(ref, dis, sc)
        assert out["popin_c_m"]["max"] == pytest.approx(0.0, abs=1e-9)

    def test_a_held_then_refreshed_tile_pops(self):
        """The distorted tile freezes for two frames and then snaps back."""
        planes = [np.full((64, 64), 100 + 10 * i, np.uint8) for i in range(4)]
        held = [planes[0], planes[0], planes[0], planes[3]]
        ref = self._frames(4, lambda i: planes[i])
        dis = self._frames(4, lambda i: held[i])
        skip = np.array([[[[0]]], [[[1]]], [[[1]]], [[[0]]]], dtype=np.uint8)
        sc = popin.PopInScoring(ppd_center=10.0, fps=90.0)
        out = popin.score_sequence(ref, dis, sc, skip=skip)
        assert out["mode"] == "skip-map"
        # Exactly one refresh event: frame 3, after two skipped frames.
        assert out["popin_c_m"]["events"] == 1
        assert out["popin_c_m"]["max"] > 0.0
        assert out["mean_k"] == 3.0

    def test_skip_map_scores_only_refresh_frames(self):
        rng = np.random.default_rng(5)
        planes = [rng.integers(0, 256, (64, 64)).astype(np.uint8) for _ in range(6)]
        ref = self._frames(6, lambda i: planes[i])
        dis = self._frames(6, lambda i: planes[i])
        skip = np.zeros((6, 1, 1, 1), np.uint8)
        skip[[1, 2, 4]] = 1
        sc = popin.PopInScoring(ppd_center=10.0, fps=90.0)
        out = popin.score_sequence(ref, dis, sc, skip=skip)
        # coded frames after a stale run: 3 (after 1,2) and 5 (after 4)
        assert out["popin_c_m"]["events"] == 2

    def test_distribution_not_just_a_mean(self):
        out = popin._distribution(np.array([0.0, 1.0, 2.0, 3.0]), 1.0)
        assert out["events"] == 4
        assert out["median"] == pytest.approx(1.5)
        assert out["max"] == 3.0
        assert out["over_threshold_frac"] == pytest.approx(0.75)


# --- latency -------------------------------------------------------------


class TestLatency:
    def test_budget_totals_the_paper(self):
        """PAPER.md 4.2: 25 to 35 ms floor, motion to photons."""
        b = latency.LatencyBudget()
        assert 25.0 <= b.total_ms() <= 35.0
        assert latency.LatencyBudget.usb().total_ms() < b.total_ms()

    def test_never_claims_to_be_measured(self):
        out = latency.motion_to_photon(latency.LatencyBudget(),
                                       encode_ms_per_frame=68.0,
                                       decode_ms_per_frame=0.0, fps=90.0)
        assert out["measured"] is False
        assert out["reference_over_budget"] == pytest.approx(10.0)
        assert "photodiode" in out["what_would_measure_it"]

    def test_frame_period(self):
        out = latency.motion_to_photon(latency.LatencyBudget(), fps=90.0)
        assert out["frame_period_ms"] == pytest.approx(11.11, abs=0.01)
