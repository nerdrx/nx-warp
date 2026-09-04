"""Tests for the eccentricity-weighted metrics (PAPER.md 5.1.2)."""

from __future__ import annotations

import math

import numpy as np
import pytest

import foveated_metrics as fov
from nxq import metrics


class TestAcuityModel:
    def test_foveal_value(self):
        assert fov.ppd_needed(0.0) == pytest.approx(60.0)

    def test_paper_table_values(self):
        """PAPER.md 5.1.2 tabulates ppd_needed * 1.5 against eccentricity."""
        # at e = e2 = 2.3 deg the model halves
        assert fov.ppd_needed(2.3) == pytest.approx(30.0)
        # 1.5 * ppd_needed at 8 deg is about 20, the "fovea + pad" row
        assert 1.5 * fov.ppd_needed(8.0) == pytest.approx(20.0, abs=0.6)
        # and about 10 at 18 deg, the end of the mid ring
        assert 1.5 * fov.ppd_needed(18.0) == pytest.approx(10.0, abs=0.7)

    def test_monotone_decreasing(self):
        e = np.linspace(0, 60, 50)
        p = np.asarray(fov.ppd_needed(e))
        assert np.all(np.diff(p) < 0)


class TestProjectionGeometry:
    def test_ppd_render_matches_the_exact_pinhole_derivative(self):
        """ppd_render = ppd_center / cos^2(theta) must equal d(r)/d(theta)."""
        ppd_c = 20.0
        f = ppd_c * 180.0 / math.pi  # px per radian
        for theta_deg in (0.0, 10.0, 25.0, 45.0):
            th = math.radians(theta_deg)
            h = 1e-6
            # numeric derivative of r = f*tan(theta), converted to px per degree
            dr = (f * math.tan(th + h) - f * math.tan(th - h)) / (2 * h)
            numeric = dr * math.pi / 180.0
            assert fov.ppd_render(theta_deg, ppd_c) == pytest.approx(numeric, rel=1e-6)

    def test_paper_45_degree_claim(self):
        """PAPER.md: at 45 deg a tan-projected image spends 2x the centre density."""
        assert fov.ppd_render(45.0, 20.0) == pytest.approx(40.0)

    def test_ppd_from_fov_center_vs_average_differ(self):
        c = fov.ppd_from_fov(2160, 100.0, "center")
        a = fov.ppd_from_fov(2160, 100.0, "average")
        assert c == pytest.approx(15.83, abs=0.05)
        assert a == pytest.approx(21.6, abs=0.05)
        assert c < a  # the centre is the sparsest part of a tan projection

    def test_ppd_from_fov_bad_mode(self):
        with pytest.raises(ValueError, match="unknown mode"):
            fov.ppd_from_fov(100, 90.0, "middle")


class TestEccentricityMap:
    def test_zero_at_the_fixation(self):
        # arccos is ill-conditioned near 1, so the floor is ~1e-6 degrees
        # (a few nanoradians), not exactly zero.
        e = fov.eccentricity_map(64, 64, 20.0, fixation=(10.0, 20.0))
        assert e[20, 10] == pytest.approx(0.0, abs=1e-4)
        assert e.min() == pytest.approx(0.0, abs=1e-4)

    def test_centre_default(self):
        e = fov.eccentricity_map(65, 65, 20.0)
        assert e[32, 32] == pytest.approx(0.0, abs=1e-9)

    def test_increases_with_distance(self):
        e = fov.eccentricity_map(64, 64, 20.0)
        assert e[32, 63] > e[32, 48] > e[32, 34]

    def test_matches_atan_along_the_axis(self):
        ppd = 20.0
        f = ppd * 180.0 / math.pi
        h = w = 129
        e = fov.eccentricity_map(h, w, ppd)
        cx = (w - 1) / 2.0
        for x in (70, 90, 128):
            want = math.degrees(math.atan((x - cx) / f))
            assert e[64, x] == pytest.approx(want, abs=1e-9)

    def test_nonnegative_and_bounded(self):
        e = fov.eccentricity_map(48, 96, 12.0, fixation=(5.0, 5.0))
        assert e.min() >= 0.0
        assert e.max() < 180.0

    def test_shape(self):
        assert fov.eccentricity_map(37, 53, 20.0).shape == (37, 53)


class TestWeights:
    def test_uniform_is_all_ones(self):
        e = fov.eccentricity_map(32, 32, 20.0)
        assert np.all(fov.acuity_weights(e, "uniform") == 1.0)

    def test_acuity_peaks_at_the_fixation(self):
        e = fov.eccentricity_map(64, 64, 20.0, fixation=(16.0, 16.0))
        w = fov.acuity_weights(e, "acuity")
        assert w[16, 16] == pytest.approx(1.0)
        assert w.max() == pytest.approx(1.0)
        assert w[63, 63] < w[16, 16]

    def test_acuity2_is_the_square(self):
        e = fov.eccentricity_map(32, 32, 20.0)
        w1 = fov.acuity_weights(e, "acuity")
        w2 = fov.acuity_weights(e, "acuity2")
        assert w2 == pytest.approx(w1 * w1)

    def test_unknown_weighting(self):
        e = fov.eccentricity_map(8, 8, 20.0)
        with pytest.raises(ValueError, match="unknown weighting"):
            fov.acuity_weights(e, "cortical")


class TestFoveatedPSNR:
    @pytest.fixture
    def pair(self):
        rng = np.random.default_rng(7)
        ref = rng.integers(0, 256, (64, 64), dtype=np.uint8)
        return ref

    def test_uniform_weighting_reduces_to_plain_psnr(self, pair):
        rng = np.random.default_rng(8)
        dis = np.clip(pair.astype(int) + rng.normal(0, 6, pair.shape), 0, 255).astype(np.uint8)
        e = fov.eccentricity_map(64, 64, 20.0)
        w = fov.acuity_weights(e, "uniform")
        assert fov.foveated_psnr(pair, dis, w) == pytest.approx(metrics.psnr_plane(pair, dis))

    def test_peripheral_error_is_forgiven(self, pair):
        """The same error is scored better when it sits away from the gaze."""
        e = fov.eccentricity_map(64, 64, 20.0, fixation=(8.0, 8.0))
        w = fov.acuity_weights(e, "acuity")
        near = pair.copy()
        near[6:11, 6:11] = 0            # damage at the fixation
        far = pair.copy()
        far[56:61, 56:61] = 0           # the same size of damage far away
        # plain PSNR cannot tell these apart in any meaningful way
        assert fov.foveated_psnr(pair, far, w) > fov.foveated_psnr(pair, near, w)

    def test_identical_is_infinite(self, pair):
        e = fov.eccentricity_map(64, 64, 20.0)
        w = fov.acuity_weights(e, "acuity")
        assert fov.foveated_psnr(pair, pair, w) == float("inf")

    def test_rejects_mismatched_weights(self, pair):
        w = np.ones((8, 8))
        with pytest.raises(ValueError, match="weight map"):
            fov.foveated_psnr(pair, pair, w)

    def test_rejects_zero_weights(self, pair):
        with pytest.raises(ValueError, match="sum to zero"):
            fov.foveated_psnr(pair, pair.copy(), np.zeros_like(pair, dtype=float))


class TestFoveatedSSIM:
    def test_uniform_weighting_reduces_to_plain_ssim(self):
        rng = np.random.default_rng(3)
        ref = rng.integers(0, 256, (64, 64), dtype=np.uint8)
        dis = np.clip(ref.astype(int) + rng.normal(0, 6, ref.shape), 0, 255).astype(np.uint8)
        e = fov.eccentricity_map(64, 64, 20.0)
        w = fov.acuity_weights(e, "uniform")
        assert fov.foveated_ssim(ref, dis, w) == pytest.approx(metrics.ssim(ref, dis))

    def test_identical_is_one(self):
        rng = np.random.default_rng(4)
        ref = rng.integers(0, 256, (48, 48), dtype=np.uint8)
        e = fov.eccentricity_map(48, 48, 20.0)
        w = fov.acuity_weights(e, "acuity")
        assert fov.foveated_ssim(ref, ref, w) == pytest.approx(1.0, abs=1e-12)

    def test_peripheral_error_is_forgiven(self):
        rng = np.random.default_rng(5)
        ref = rng.integers(0, 256, (64, 64), dtype=np.uint8)
        e = fov.eccentricity_map(64, 64, 20.0, fixation=(12.0, 12.0))
        w = fov.acuity_weights(e, "acuity2")
        near, far = ref.copy(), ref.copy()
        near[10:15, 10:15] = 0
        far[54:59, 54:59] = 0
        assert fov.foveated_ssim(ref, far, w) > fov.foveated_ssim(ref, near, w)


class TestRegionSplit:
    def test_fraction_and_keys(self):
        rng = np.random.default_rng(6)
        ref = rng.integers(0, 256, (64, 64), dtype=np.uint8)
        dis = np.clip(ref.astype(int) + rng.normal(0, 5, ref.shape), 0, 255).astype(np.uint8)
        # 2 ppd over 64 px spans about 32 degrees, so an 8 degree fovea disc is
        # a genuine subset. (At 20 ppd the whole patch would be inside it.)
        e = fov.eccentricity_map(64, 64, 2.0)
        out = fov.region_psnr(ref, dis, e, radius_deg=8.0)
        assert 0.0 < out["fovea_pixel_fraction"] < 1.0
        assert "psnr_fovea" in out and "psnr_periphery" in out

    def test_a_huge_radius_swallows_the_image(self):
        ref = np.zeros((32, 32), np.uint8)
        e = fov.eccentricity_map(32, 32, 20.0)
        out = fov.region_psnr(ref, ref, e, radius_deg=180.0)
        assert out["fovea_pixel_fraction"] == pytest.approx(1.0)
        assert "psnr_periphery" not in out


class TestFrameMetrics:
    def test_full_dict(self):
        rng = np.random.default_rng(9)
        ref = rng.integers(0, 256, (64, 64), dtype=np.uint8)
        dis = np.clip(ref.astype(int) + rng.normal(0, 7, ref.shape), 0, 255).astype(np.uint8)
        out = fov.foveated_frame_metrics(ref, dis, 20.0)
        for k in ("fov_psnr_y", "psnr_y", "fov_ssim_y", "ssim_y", "psnr_fovea"):
            assert k in out
        assert out["weighting"] == "acuity"

    def test_precomputed_map_gives_the_same_answer(self):
        rng = np.random.default_rng(10)
        ref = rng.integers(0, 256, (64, 64), dtype=np.uint8)
        dis = np.clip(ref.astype(int) + rng.normal(0, 7, ref.shape), 0, 255).astype(np.uint8)
        e = fov.eccentricity_map(64, 64, 20.0, fixation=(20.0, 30.0))
        a = fov.foveated_frame_metrics(ref, dis, 20.0, (20.0, 30.0))
        b = fov.foveated_frame_metrics(ref, dis, 20.0, (20.0, 30.0), ecc=e)
        assert a["fov_psnr_y"] == pytest.approx(b["fov_psnr_y"])
