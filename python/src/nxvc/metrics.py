"""PSNR / SSIM / MS-SSIM for nxvc planes.

The quality harness in ``tools/quality`` (`nxq.metrics`) is the reference
implementation of these metrics for the project, and its numbers are the ones
that appear in the Phase gates.  This module **delegates to it whenever it can
be imported**, so a number computed here and a number computed by
`compare.py` can never drift apart.  When the harness is not importable -- a
user who pip-installed only these bindings -- an equivalent numpy
implementation is used.

:data:`BACKEND` says which one is live: ``"nxq"`` or ``"numpy"``.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

__all__ = [
    "BACKEND",
    "MAX8",
    "mse",
    "psnr",
    "psnr_planes",
    "ssim",
    "ms_ssim",
    "compare_frames",
    "nxq_path",
]

MAX8 = 255.0

#: MS-SSIM scale weights, Wang et al. 2003.
MS_SSIM_WEIGHTS = (0.0448, 0.2856, 0.3001, 0.2363, 0.1333)


def nxq_path() -> Path | None:
    """Locate ``tools/quality`` in an nx-warp checkout, if there is one nearby."""
    starts = [Path(__file__).resolve(), (Path.cwd() / "x").resolve()]
    for start in starts:
        for parent in start.parents:
            cand = parent / "tools" / "quality"
            if (cand / "nxq" / "metrics.py").is_file():
                return cand
    return None


def _import_nxq():
    try:
        import nxq.metrics as m  # type: ignore

        return m
    except Exception:
        pass
    path = nxq_path()
    if path is None:
        return None
    sys.path.insert(0, str(path))
    try:
        import nxq.metrics as m  # type: ignore

        return m
    except Exception:
        try:
            sys.path.remove(str(path))
        except ValueError:
            pass
        return None


_nxq = _import_nxq()

#: ``"nxq"`` when the quality harness is driving, ``"numpy"`` for the fallback.
BACKEND = "nxq" if _nxq is not None else "numpy"


# ----------------------------------------------------------------- fallbacks


def _gaussian_kernel(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(x**2) / (2.0 * sigma**2))
    return k / k.sum()


def _filter_valid(img: np.ndarray, k1d: np.ndarray) -> np.ndarray:
    """Separable 'valid' convolution built from shifted slices.

    Sum-of-slices rather than a sliding-window view, so peak memory stays at a
    few copies of the image even at 2048x2048 -- the same choice
    ``nxq.metrics`` makes, and the reason neither needs scipy.
    """
    n = k1d.size
    h, w = img.shape
    tmp = np.zeros((h, w - n + 1), dtype=np.float64)
    for i, c in enumerate(k1d):
        tmp += c * img[:, i : i + w - n + 1]
    out = np.zeros((h - n + 1, tmp.shape[1]), dtype=np.float64)
    for i, c in enumerate(k1d):
        out += c * tmp[i : i + h - n + 1, :]
    return out


def _downsample2(img: np.ndarray) -> np.ndarray:
    h, w = img.shape[0] - img.shape[0] % 2, img.shape[1] - img.shape[1] % 2
    a = img[:h, :w].astype(np.float64)
    return 0.25 * (a[0::2, 0::2] + a[0::2, 1::2] + a[1::2, 0::2] + a[1::2, 1::2])


def _ssim_stats(a: np.ndarray, b: np.ndarray, k1d: np.ndarray, peak: float):
    c1 = (0.01 * peak) ** 2
    c2 = (0.03 * peak) ** 2
    af = a.astype(np.float64)
    bf = b.astype(np.float64)
    mu_a = _filter_valid(af, k1d)
    mu_b = _filter_valid(bf, k1d)
    mu_aa, mu_bb, mu_ab = mu_a * mu_a, mu_b * mu_b, mu_a * mu_b
    sa = _filter_valid(af * af, k1d) - mu_aa
    sb = _filter_valid(bf * bf, k1d) - mu_bb
    sab = _filter_valid(af * bf, k1d) - mu_ab
    lum = (2 * mu_ab + c1) / (mu_aa + mu_bb + c1)
    cs = (2 * sab + c2) / (sa + sb + c2)
    return lum, cs


# -------------------------------------------------------------------- public


def mse(a: np.ndarray, b: np.ndarray) -> float:
    """Mean squared error between two planes."""
    if _nxq is not None:
        return float(_nxq.mse_plane(np.asarray(a), np.asarray(b)))
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    d = a - b
    return float(np.mean(d * d))


def psnr(a: np.ndarray, b: np.ndarray, peak: float = MAX8) -> float:
    """PSNR of one plane in dB.  ``inf`` for identical planes."""
    if _nxq is not None:
        return float(_nxq.psnr_plane(np.asarray(a), np.asarray(b), peak))
    m = mse(a, b)
    if m <= 0.0:
        return float("inf")
    return float(10.0 * np.log10(peak * peak / m))


def psnr_planes(ref, dis, peak: float = MAX8) -> dict[str, float]:
    """Per-plane PSNR plus the JVET-weighted ``(6Y + Cb + Cr) / 8`` figure.

    The weighted number is formed in the **MSE domain**, which is the JVET
    convention and what ``nxq.metrics`` does; averaging dB values instead
    would give a different -- and wrong -- number.
    """
    ref = list(ref)
    dis = list(dis)
    if len(ref) != len(dis):
        raise ValueError(f"plane count mismatch: {len(ref)} vs {len(dis)}")
    names = ["y", "u", "v", "a"]
    out: dict[str, float] = {}
    mses: list[float] = []
    for i, (r, d) in enumerate(zip(ref, dis)):
        m = mse(r, d)
        mses.append(m)
        out[f"psnr_{names[i]}"] = (
            float("inf") if m <= 0 else float(10.0 * np.log10(peak * peak / m))
        )
    if len(mses) >= 3:
        weighted = (6.0 * mses[0] + mses[1] + mses[2]) / 8.0
        out["psnr_ycbcr"] = (
            float("inf") if weighted <= 0 else float(10.0 * np.log10(peak * peak / weighted))
        )
    return out


def ssim(a: np.ndarray, b: np.ndarray, peak: float = MAX8) -> float:
    """SSIM, Wang et al. 2004: Gaussian 11x11, sigma 1.5, on one plane."""
    if _nxq is not None and peak == MAX8:
        return float(_nxq.ssim(np.asarray(a), np.asarray(b)))
    a = np.asarray(a)
    b = np.asarray(b)
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    k = _gaussian_kernel()
    if min(a.shape) < k.size:
        raise ValueError(f"plane {a.shape} is smaller than the 11x11 SSIM window")
    lum, cs = _ssim_stats(a, b, k, peak)
    return float(np.mean(lum * cs))


def ms_ssim(a: np.ndarray, b: np.ndarray, peak: float = MAX8) -> float:
    """MS-SSIM, Wang et al. 2003: 5 scales, the standard weights."""
    if _nxq is not None and peak == MAX8:
        return float(_nxq.ms_ssim(np.asarray(a), np.asarray(b)))
    a = np.asarray(a).astype(np.float64)
    b = np.asarray(b).astype(np.float64)
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    k = _gaussian_kernel()
    value = 1.0
    for scale, w in enumerate(MS_SSIM_WEIGHTS):
        if min(a.shape) < k.size:
            raise ValueError(
                f"plane too small for {len(MS_SSIM_WEIGHTS)} MS-SSIM scales: "
                f"needs at least {k.size * 2 ** (len(MS_SSIM_WEIGHTS) - 1)} samples per edge"
            )
        lum, cs = _ssim_stats(a, b, k, peak)
        cs_mean = float(np.mean(cs))
        if scale == len(MS_SSIM_WEIGHTS) - 1:
            value *= max(float(np.mean(lum * cs)), 0.0) ** w
        else:
            value *= max(cs_mean, 0.0) ** w
            a = _downsample2(a)
            b = _downsample2(b)
    return float(value)


def compare_frames(ref, dis, *, do_ssim: bool = True, do_ms_ssim: bool = False) -> dict:
    """PSNR for every plane, plus SSIM / MS-SSIM on luma.

    >>> import numpy as np
    >>> a = np.zeros((64, 64), np.uint8); b = a.copy()
    >>> compare_frames([a, a, a], [b, b, b], do_ssim=False)["psnr_y"]
    inf
    """
    ref = list(ref)
    dis = list(dis)
    out = psnr_planes(ref, dis)
    if do_ssim:
        out["ssim_y"] = ssim(ref[0], dis[0])
    if do_ms_ssim:
        out["ms_ssim_y"] = ms_ssim(ref[0], dis[0])
    out["backend"] = BACKEND
    return out
