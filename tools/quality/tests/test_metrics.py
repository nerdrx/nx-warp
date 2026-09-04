"""PSNR / SSIM / MS-SSIM tests."""

from __future__ import annotations

import numpy as np
import pytest

from nxq import metrics
from nxq.yuv import Format, Frame


@pytest.fixture
def rng():
    return np.random.default_rng(1234)


@pytest.fixture
def img(rng):
    return rng.integers(0, 256, (64, 64), dtype=np.uint8)


class TestFilter:
    def test_gaussian_kernel_normalised_and_symmetric(self):
        k = metrics.gaussian_kernel(11, 1.5)
        assert k.size == 11
        assert k.sum() == pytest.approx(1.0)
        assert k == pytest.approx(k[::-1])
        assert np.argmax(k) == 5

    def test_separable_filter_matches_a_naive_reference(self, rng):
        a = rng.random((23, 19))
        k = metrics.gaussian_kernel(5, 1.0)
        got = metrics.filter2_valid(a, k)
        k2 = np.outer(k, k)
        want = np.empty((23 - 4, 19 - 4))
        for i in range(want.shape[0]):
            for j in range(want.shape[1]):
                want[i, j] = (a[i : i + 5, j : j + 5] * k2).sum()
        assert got == pytest.approx(want)

    def test_valid_shape(self, img):
        out = metrics.filter2_valid(img.astype(float), metrics.gaussian_kernel(11))
        assert out.shape == (64 - 10, 64 - 10)

    def test_rejects_window_larger_than_image(self):
        with pytest.raises(ValueError, match="smaller than"):
            metrics.filter2_valid(np.zeros((5, 5)), metrics.gaussian_kernel(11))


class TestPSNR:
    def test_identical_is_infinite(self, img):
        assert metrics.psnr_plane(img, img) == float("inf")

    def test_known_value(self):
        """A uniform error of 1 LSB over the whole plane gives MSE 1."""
        a = np.full((16, 16), 100, np.uint8)
        b = np.full((16, 16), 101, np.uint8)
        assert metrics.mse_plane(a, b) == pytest.approx(1.0)
        assert metrics.psnr_plane(a, b) == pytest.approx(10 * np.log10(255.0**2))
        assert metrics.psnr_plane(a, b) == pytest.approx(48.1308, abs=1e-4)

    def test_worst_case(self):
        a = np.zeros((8, 8), np.uint8)
        b = np.full((8, 8), 255, np.uint8)
        assert metrics.psnr_plane(a, b) == pytest.approx(0.0)

    def test_symmetric(self, rng, img):
        other = rng.integers(0, 256, img.shape, dtype=np.uint8)
        assert metrics.psnr_plane(img, other) == pytest.approx(metrics.psnr_plane(other, img))

    def test_more_noise_is_lower_psnr(self, rng, img):
        a = np.clip(img.astype(int) + rng.normal(0, 2, img.shape), 0, 255).astype(np.uint8)
        b = np.clip(img.astype(int) + rng.normal(0, 8, img.shape), 0, 255).astype(np.uint8)
        assert metrics.psnr_plane(img, a) > metrics.psnr_plane(img, b)

    def test_shape_mismatch(self, img):
        with pytest.raises(ValueError, match="shape mismatch"):
            metrics.mse_plane(img, img[:32])

    def test_weighted_ycbcr_equals_plane_psnr_when_all_planes_share_the_error(self):
        """With the same MSE in all three planes, the 6:1:1 weights sum to 1."""
        fmt = Format(16, 16, "yuv444p")
        ref = Frame.gray(fmt, 100)
        dis = Frame(
            np.full((16, 16), 102, np.uint8),
            np.full((16, 16), 130, np.uint8),
            np.full((16, 16), 130, np.uint8),
        )
        ref = Frame(np.full((16, 16), 100, np.uint8),
                    np.full((16, 16), 128, np.uint8),
                    np.full((16, 16), 128, np.uint8))
        out = metrics.psnr_frame(ref, dis)
        assert out["psnr_y"] == pytest.approx(out["psnr_u"])
        assert out["psnr_ycbcr"] == pytest.approx(out["psnr_y"])

    def test_weighted_ycbcr_favours_luma(self):
        """Luma error hurts the weighted figure six times as much as one chroma plane."""
        z = np.full((16, 16), 128, np.uint8)
        ref = Frame(z.copy(), z.copy(), z.copy())
        luma_err = Frame(np.full((16, 16), 138, np.uint8), z.copy(), z.copy())
        chroma_err = Frame(z.copy(), np.full((16, 16), 138, np.uint8), z.copy())
        assert (metrics.psnr_frame(ref, luma_err)["psnr_ycbcr"]
                < metrics.psnr_frame(ref, chroma_err)["psnr_ycbcr"])


class TestSSIM:
    def test_identical_is_one(self, img):
        assert metrics.ssim(img, img) == pytest.approx(1.0, abs=1e-12)

    def test_symmetric(self, rng, img):
        other = rng.integers(0, 256, img.shape, dtype=np.uint8)
        assert metrics.ssim(img, other) == pytest.approx(metrics.ssim(other, img))

    def test_bounded(self, rng, img):
        other = rng.integers(0, 256, img.shape, dtype=np.uint8)
        assert -1.0 <= metrics.ssim(img, other) <= 1.0

    def test_more_noise_is_lower_ssim(self, rng, img):
        a = np.clip(img.astype(int) + rng.normal(0, 3, img.shape), 0, 255).astype(np.uint8)
        b = np.clip(img.astype(int) + rng.normal(0, 15, img.shape), 0, 255).astype(np.uint8)
        assert metrics.ssim(img, a) > metrics.ssim(img, b)

    def test_flat_images_of_equal_value(self):
        a = np.full((32, 32), 77, np.uint8)
        assert metrics.ssim(a, a) == pytest.approx(1.0)

    def test_map_shape(self, img):
        assert metrics.ssim_map(img, img).shape == (54, 54)


class TestMSSSIM:
    def test_identical_is_one(self, rng):
        a = rng.integers(0, 256, (256, 256), dtype=np.uint8)
        assert metrics.ms_ssim(a, a) == pytest.approx(1.0, abs=1e-9)

    def test_more_noise_is_lower(self, rng):
        a = rng.integers(0, 256, (256, 256), dtype=np.uint8)
        n1 = np.clip(a.astype(int) + rng.normal(0, 3, a.shape), 0, 255).astype(np.uint8)
        n2 = np.clip(a.astype(int) + rng.normal(0, 15, a.shape), 0, 255).astype(np.uint8)
        assert metrics.ms_ssim(a, n1) > metrics.ms_ssim(a, n2)

    def test_small_images_fall_back_to_fewer_scales(self, rng):
        """A 32x32 patch cannot carry five scales; it must still return a number."""
        a = rng.integers(0, 256, (32, 32), dtype=np.uint8)
        v = metrics.ms_ssim(a, a)
        assert v == pytest.approx(1.0, abs=1e-9)

    def test_too_small_raises(self, rng):
        a = rng.integers(0, 256, (8, 8), dtype=np.uint8)
        with pytest.raises(ValueError, match="too small"):
            metrics.ms_ssim(a, a)


class TestAggregation:
    def test_frame_metrics_keys(self, rng):
        fmt = Format(32, 32, "yuv444p")
        ref = Frame(*(rng.integers(0, 256, (32, 32), dtype=np.uint8) for _ in range(3)))
        dis = Frame(*(p.copy() for p in ref.planes))
        dis.y[0, 0] = (int(dis.y[0, 0]) + 40) % 256
        out = metrics.frame_metrics(ref, dis)
        assert {"psnr_y", "psnr_u", "psnr_v", "psnr_ycbcr", "ssim_y", "ms_ssim_y"} <= set(out)

    def test_average_excludes_infinities_and_counts_them(self):
        per = [{"psnr_y": float("inf")}, {"psnr_y": 30.0}, {"psnr_y": 40.0}]
        out = metrics.average_metrics(per)
        assert out["psnr_y"] == pytest.approx(35.0)
        assert out["frames"] == 3
        assert out["lossless_frames"] == 1

    def test_average_of_empty(self):
        assert metrics.average_metrics([]) == {}

    def test_all_lossless_stays_infinite(self):
        out = metrics.average_metrics([{"psnr_y": float("inf")}] * 3)
        assert out["psnr_y"] == float("inf")
