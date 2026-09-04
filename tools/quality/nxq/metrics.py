"""Full-reference quality metrics, implemented in numpy only.

Nothing here needs scipy: the only spatial operation is a separable Gaussian
filter, done as a sum of shifted slices, which keeps peak memory at a few
copies of the image even at 2048x2048.

Provided:

* :func:`psnr_plane`, :func:`psnr_frame`  -- per-plane and weighted PSNR
* :func:`ssim`                            -- Wang et al. 2004, 11x11 Gaussian, sigma 1.5
* :func:`ms_ssim`                         -- Wang et al. 2003, 5 scales, standard weights

The SSIM constants and the MS-SSIM scale weights are the canonical ones so the
numbers are comparable with published results and with ffmpeg's ``ssim`` filter
(ffmpeg uses an 8x8 uniform window, so it will differ by a few thousandths;
:func:`ssim` here is the Gaussian-window reference form).
"""

from __future__ import annotations

import numpy as np

from .yuv import Frame, YCBCR_WEIGHTS

MAX8 = 255.0

# SSIM stabilisers for 8-bit data (K1=0.01, K2=0.03, L=255).
_C1 = (0.01 * MAX8) ** 2
_C2 = (0.03 * MAX8) ** 2

# Wang et al. 2003 "Multiscale structural similarity" scale weights.
MS_SSIM_WEIGHTS = (0.0448, 0.2856, 0.3001, 0.2363, 0.1333)


# --- separable filtering -------------------------------------------------


def gaussian_kernel(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(x**2) / (2.0 * sigma**2))
    return k / k.sum()


def _conv1d_valid(img: np.ndarray, k: np.ndarray, axis: int) -> np.ndarray:
    """1-D 'valid' correlation along *axis*, as a sum of shifted slices."""
    n = k.size
    length = img.shape[axis]
    if length < n:
        raise ValueError(f"image axis {axis} of length {length} is smaller than the {n}-tap window")
    out = None
    for t in range(n):
        sl = [slice(None)] * img.ndim
        sl[axis] = slice(t, length - n + 1 + t)
        part = img[tuple(sl)] * k[t]
        out = part if out is None else out + part
    return out  # type: ignore[return-value]


def filter2_valid(img: np.ndarray, k1d: np.ndarray) -> np.ndarray:
    """Separable 2-D 'valid' filtering with the 1-D kernel *k1d*."""
    return _conv1d_valid(_conv1d_valid(img, k1d, 0), k1d, 1)


def _downsample2(img: np.ndarray) -> np.ndarray:
    """2x2 box average, dropping an odd last row/column (MS-SSIM convention)."""
    h, w = img.shape
    q = img[: h - h % 2, : w - w % 2]
    return 0.25 * (q[0::2, 0::2] + q[0::2, 1::2] + q[1::2, 0::2] + q[1::2, 1::2])


# --- PSNR ----------------------------------------------------------------


def mse_plane(a: np.ndarray, b: np.ndarray) -> float:
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch {a.shape} vs {b.shape}")
    d = a.astype(np.float64) - b.astype(np.float64)
    return float(np.mean(d * d))


def psnr_from_mse(mse: float, peak: float = MAX8) -> float:
    """PSNR in dB. A zero MSE returns ``inf``; callers cap it for reporting."""
    if mse <= 0.0:
        return float("inf")
    return float(10.0 * np.log10((peak * peak) / mse))


def psnr_plane(a: np.ndarray, b: np.ndarray, peak: float = MAX8) -> float:
    return psnr_from_mse(mse_plane(a, b), peak)


def psnr_frame(ref: Frame, dis: Frame) -> dict[str, float]:
    """Per-plane PSNR plus the (6Y+Cb+Cr)/8 weighted YCbCr figure.

    The weighted number is formed in the MSE domain (the JVET convention),
    not by averaging dB values.
    """
    mses = [mse_plane(r, d) for r, d in zip(ref.planes, dis.planes)]
    wmse = sum(w * m for w, m in zip(YCBCR_WEIGHTS, mses))
    return {
        "psnr_y": psnr_from_mse(mses[0]),
        "psnr_u": psnr_from_mse(mses[1]),
        "psnr_v": psnr_from_mse(mses[2]),
        "psnr_ycbcr": psnr_from_mse(wmse),
        "mse_y": mses[0],
    }


# --- SSIM ----------------------------------------------------------------


def ssim_map(a: np.ndarray, b: np.ndarray, k1d: np.ndarray | None = None) -> np.ndarray:
    """The local SSIM index map (valid region only)."""
    if k1d is None:
        k1d = gaussian_kernel()
    x = a.astype(np.float64)
    y = b.astype(np.float64)
    mu_x = filter2_valid(x, k1d)
    mu_y = filter2_valid(y, k1d)
    mu_xx, mu_yy, mu_xy = mu_x * mu_x, mu_y * mu_y, mu_x * mu_y
    sig_xx = filter2_valid(x * x, k1d) - mu_xx
    sig_yy = filter2_valid(y * y, k1d) - mu_yy
    sig_xy = filter2_valid(x * y, k1d) - mu_xy
    num = (2.0 * mu_xy + _C1) * (2.0 * sig_xy + _C2)
    den = (mu_xx + mu_yy + _C1) * (sig_xx + sig_yy + _C2)
    return num / den


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Mean SSIM over the valid region."""
    return float(np.mean(ssim_map(a, b)))


def _ssim_components(a: np.ndarray, b: np.ndarray, k1d: np.ndarray) -> tuple[float, float]:
    """(mean SSIM, mean contrast-structure term) for one scale."""
    x = a.astype(np.float64)
    y = b.astype(np.float64)
    mu_x = filter2_valid(x, k1d)
    mu_y = filter2_valid(y, k1d)
    mu_xx, mu_yy, mu_xy = mu_x * mu_x, mu_y * mu_y, mu_x * mu_y
    sig_xx = filter2_valid(x * x, k1d) - mu_xx
    sig_yy = filter2_valid(y * y, k1d) - mu_yy
    sig_xy = filter2_valid(x * y, k1d) - mu_xy
    lum = (2.0 * mu_xy + _C1) / (mu_xx + mu_yy + _C1)
    cs = (2.0 * sig_xy + _C2) / (sig_xx + sig_yy + _C2)
    return float(np.mean(lum * cs)), float(np.mean(cs))


def ms_ssim(a: np.ndarray, b: np.ndarray, weights=MS_SSIM_WEIGHTS) -> float:
    """Multi-scale SSIM.

    Falls back to fewer scales when the image is too small for the full five
    (each scale needs at least 11 pixels per side), renormalising the weights,
    so a 512x512 test clip and a 64x64 unit-test patch both work.
    """
    k1d = gaussian_kernel()
    x = a.astype(np.float64)
    y = b.astype(np.float64)
    cs_vals: list[float] = []
    ssim_last = None
    used = 0
    for level in range(len(weights)):
        if min(x.shape) < k1d.size:
            break
        s, cs = _ssim_components(x, y, k1d)
        ssim_last = s
        used = level + 1
        if level < len(weights) - 1:
            cs_vals.append(cs)
            x = _downsample2(x)
            y = _downsample2(y)
    if ssim_last is None:
        raise ValueError("image too small for MS-SSIM (needs at least 11x11)")
    # The loop appends a cs term for every level it completes, including the
    # last one when the *next* level is what turns out to be too small. The
    # product is cs for every scale but the coarsest, times the full SSIM of
    # the coarsest, so drop that trailing cs before renormalising.
    cs_vals = cs_vals[: used - 1]
    w = np.asarray(weights[:used], dtype=np.float64)
    w = w / w.sum()
    # Clamp: the cs terms can go slightly negative on pathological input, and a
    # fractional power of a negative number is undefined.
    terms = np.clip(np.asarray(cs_vals + [ssim_last], dtype=np.float64), 1e-12, None)
    return float(np.prod(terms**w))


# --- frame-level convenience --------------------------------------------


def frame_metrics(ref: Frame, dis: Frame, *, do_ssim: bool = True, do_ms_ssim: bool = True) -> dict:
    """PSNR (always) plus luma SSIM / MS-SSIM for one frame pair."""
    out = psnr_frame(ref, dis)
    if do_ssim:
        out["ssim_y"] = ssim(ref.y, dis.y)
    if do_ms_ssim:
        try:
            out["ms_ssim_y"] = ms_ssim(ref.y, dis.y)
        except ValueError:
            pass
    return out


def average_metrics(per_frame: list[dict]) -> dict:
    """Average a list of per-frame metric dicts.

    PSNR is averaged in the dB domain (the convention used for "average PSNR"
    in codec comparisons); infinities from lossless frames are excluded from
    the mean and reported separately as ``lossless_frames``.
    """
    if not per_frame:
        return {}
    keys = per_frame[0].keys()
    out: dict[str, float] = {}
    lossless = 0
    for k in keys:
        vals = [f[k] for f in per_frame if k in f]
        finite = [v for v in vals if np.isfinite(v)]
        if k.startswith("psnr") and len(finite) != len(vals):
            lossless = max(lossless, len(vals) - len(finite))
        out[k] = float(np.mean(finite)) if finite else float("inf")
    out["frames"] = len(per_frame)
    if lossless:
        out["lossless_frames"] = lossless
    return out
