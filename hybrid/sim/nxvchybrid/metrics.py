"""PSNR and SSIM in numpy only (no scipy dependency)."""

from __future__ import annotations

import numpy as np

from .yuv import Frame, YCBCR_WEIGHTS

MAX8 = 255.0
_C1 = (0.01 * MAX8) ** 2
_C2 = (0.03 * MAX8) ** 2


def mse(a: np.ndarray, b: np.ndarray) -> float:
    d = a.astype(np.float64) - b.astype(np.float64)
    return float(np.mean(d * d))


def psnr_from_mse(m: float) -> float:
    if m <= 1e-12:
        return 99.0
    return float(10.0 * np.log10(MAX8 * MAX8 / m))


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    return psnr_from_mse(mse(a, b))


def frame_psnr(src: Frame, rec: Frame) -> tuple[float, float]:
    """(Y PSNR, weighted (6Y+Cb+Cr)/8 PSNR) for one frame."""
    py = psnr(src.y, rec.y)
    pc = [psnr(s, r) for s, r in zip(src.planes(), rec.planes())]
    wp = sum(w * v for w, v in zip(YCBCR_WEIGHTS, pc))
    return py, wp


def _gauss(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(x * x) / (2.0 * sigma * sigma))
    return (k / k.sum()).astype(np.float32)


_K = _gauss()


def _filt(img: np.ndarray, k: np.ndarray) -> np.ndarray:
    n = k.size
    out = None
    for axis in (0, 1):
        length = img.shape[axis]
        acc = None
        for t in range(n):
            sl = [slice(None)] * 2
            sl[axis] = slice(t, length - n + 1 + t)
            part = img[tuple(sl)] * k[t]
            acc = part if acc is None else acc + part
        img = acc  # type: ignore[assignment]
        out = img
    return out  # type: ignore[return-value]


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Wang et al. 2004 SSIM, 11x11 Gaussian window, mean over the map."""
    a = a.astype(np.float32)
    b = b.astype(np.float32)
    mu_a = _filt(a, _K)
    mu_b = _filt(b, _K)
    aa = _filt(a * a, _K) - mu_a * mu_a
    bb = _filt(b * b, _K) - mu_b * mu_b
    ab = _filt(a * b, _K) - mu_a * mu_b
    num = (2 * mu_a * mu_b + _C1) * (2 * ab + _C2)
    den = (mu_a**2 + mu_b**2 + _C1) * (aa + bb + _C2)
    return float(np.mean(num / den))
