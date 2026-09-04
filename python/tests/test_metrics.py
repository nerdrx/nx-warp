"""Metrics: the identities that must hold whichever backend is live."""

from __future__ import annotations

import math

import numpy as np
import pytest

from nxvc import metrics


def _rng(seed=7):
    return np.random.default_rng(seed)


def test_backend_is_named():
    assert metrics.BACKEND in ("nxq", "numpy")


def test_psnr_of_identical_planes_is_infinite():
    a = _rng().integers(0, 256, (64, 64), dtype=np.uint8)
    assert metrics.mse(a, a) == 0.0
    assert math.isinf(metrics.psnr(a, a))


def test_psnr_matches_the_closed_form():
    a = np.zeros((32, 32), np.uint8)
    b = np.full((32, 32), 10, np.uint8)
    assert metrics.mse(a, b) == pytest.approx(100.0)
    assert metrics.psnr(a, b) == pytest.approx(10 * math.log10(255.0**2 / 100.0))


def test_psnr_is_symmetric_and_monotone_in_distortion():
    rng = _rng()
    a = rng.integers(0, 256, (48, 48), dtype=np.uint8)
    near = np.clip(a.astype(int) + rng.integers(-2, 3, a.shape), 0, 255).astype(np.uint8)
    far = np.clip(a.astype(int) + rng.integers(-20, 21, a.shape), 0, 255).astype(np.uint8)
    assert metrics.psnr(a, near) == pytest.approx(metrics.psnr(near, a))
    assert metrics.psnr(a, near) > metrics.psnr(a, far)


def test_weighted_psnr_is_formed_in_the_mse_domain():
    """(6Y + Cb + Cr) / 8 on MSEs -- the JVET convention, not a dB average."""
    rng = _rng(3)
    ref = [rng.integers(0, 256, (32, 32), dtype=np.uint8) for _ in range(3)]
    dis = [np.clip(p.astype(int) + 3, 0, 255).astype(np.uint8) for p in ref]
    out = metrics.psnr_planes(ref, dis)
    mses = [metrics.mse(r, d) for r, d in zip(ref, dis)]
    weighted = (6 * mses[0] + mses[1] + mses[2]) / 8.0
    assert out["psnr_ycbcr"] == pytest.approx(10 * math.log10(255.0**2 / weighted))
    # and it is NOT the mean of the per-plane dB values, in general
    assert set(out) == {"psnr_y", "psnr_u", "psnr_v", "psnr_ycbcr"}


def test_psnr_planes_rejects_a_plane_count_mismatch():
    a = np.zeros((8, 8), np.uint8)
    with pytest.raises(ValueError, match="plane count mismatch"):
        metrics.psnr_planes([a, a, a], [a, a])


def test_shape_mismatch_is_an_error():
    with pytest.raises(ValueError, match="shape mismatch"):
        metrics.mse(np.zeros((4, 4), np.uint8), np.zeros((4, 5), np.uint8))


def test_ssim_of_identical_planes_is_one():
    a = _rng().integers(0, 256, (64, 64), dtype=np.uint8)
    assert metrics.ssim(a, a) == pytest.approx(1.0, abs=1e-9)


def test_ssim_falls_with_noise():
    rng = _rng(11)
    a = rng.integers(0, 256, (96, 96), dtype=np.uint8)
    noisy = np.clip(a.astype(int) + rng.integers(-40, 41, a.shape), 0, 255).astype(np.uint8)
    s = metrics.ssim(a, noisy)
    assert 0.0 <= s < 0.99


def test_ssim_rejects_a_plane_smaller_than_the_window():
    tiny = np.zeros((8, 8), np.uint8)
    with pytest.raises(ValueError):
        metrics.ssim(tiny, tiny)


def test_ms_ssim_of_identical_planes_is_one():
    a = _rng(5).integers(0, 256, (256, 256), dtype=np.uint8)
    assert metrics.ms_ssim(a, a) == pytest.approx(1.0, abs=1e-6)


def test_compare_frames_reports_the_backend():
    a = _rng(2).integers(0, 256, (64, 64), dtype=np.uint8)
    out = metrics.compare_frames([a, a, a], [a, a, a])
    assert out["backend"] == metrics.BACKEND
    assert math.isinf(out["psnr_y"])
    assert out["ssim_y"] == pytest.approx(1.0, abs=1e-9)


@pytest.mark.skipif(metrics.BACKEND != "nxq", reason="the quality harness is not importable")
def test_delegation_agrees_with_the_local_numpy_implementation():
    """When both exist they must agree, or one of the two is wrong."""
    import nxq.metrics as nxq_metrics

    rng = _rng(13)
    a = rng.integers(0, 256, (128, 128), dtype=np.uint8)
    b = np.clip(a.astype(int) + rng.integers(-8, 9, a.shape), 0, 255).astype(np.uint8)

    assert metrics.psnr(a, b) == pytest.approx(nxq_metrics.psnr_plane(a, b), abs=1e-9)

    # Bypass the delegation and run this module's own fallback path.
    saved = metrics._nxq
    try:
        metrics._nxq = None
        local_ssim = metrics.ssim(a, b)
        local_psnr = metrics.psnr(a, b)
    finally:
        metrics._nxq = saved
    assert local_psnr == pytest.approx(nxq_metrics.psnr_plane(a, b), abs=1e-9)
    assert local_ssim == pytest.approx(nxq_metrics.ssim(a, b), abs=1e-6)
