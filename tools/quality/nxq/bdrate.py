"""Bjontegaard delta rate and delta PSNR.

Implements the original VCEG-M33 method (Bjontegaard 2001): fit a cubic
polynomial through the four (or N) rate-distortion points, integrate it
analytically over the overlapping interval, and take the difference of the
averages.

* :func:`bd_rate` fits ``log10(rate)`` as a function of distortion and returns
  the average bitrate difference in **percent** (negative means the test codec
  needs fewer bits for the same quality -- an improvement).
* :func:`bd_psnr` fits distortion as a function of ``log10(rate)`` and returns
  the average quality difference in **dB** (positive is an improvement).

Both accept any monotone distortion metric, not just PSNR; for SSIM-like
metrics use :func:`bd_rate` with the SSIM values as ``dist``.

A piecewise-cubic-Hermite (``method="pchip"``) variant is also provided.  It is
monotone and therefore immune to the overshoot the plain cubic fit can show on
five or more points, but the plain cubic is the default because it is what the
JCT-VC/JVET common test conditions report.
"""

from __future__ import annotations

import numpy as np

Method = str


def _prepare(rate, dist) -> tuple[np.ndarray, np.ndarray]:
    r = np.asarray(rate, dtype=np.float64)
    d = np.asarray(dist, dtype=np.float64)
    if r.shape != d.shape:
        raise ValueError("rate and dist must have the same length")
    if r.size < 4:
        raise ValueError("Bjontegaard needs at least 4 rate-distortion points")
    if np.any(r <= 0):
        raise ValueError("rates must be positive")
    if not np.all(np.isfinite(d)):
        raise ValueError("distortion values must be finite (a lossless point cannot be fitted)")
    order = np.argsort(d)
    return r[order], d[order]


def _poly_int(coeffs: np.ndarray, lo: float, hi: float) -> float:
    """Definite integral of a polynomial given in numpy (highest-first) order."""
    p = np.polyint(coeffs)
    return float(np.polyval(p, hi) - np.polyval(p, lo))


def _pchip_int(x: np.ndarray, y: np.ndarray, lo: float, hi: float) -> float:
    from scipy.interpolate import PchipInterpolator  # imported lazily: optional dependency

    return float(PchipInterpolator(x, y).integrate(lo, hi))


def _average_over_overlap(
    x1: np.ndarray, y1: np.ndarray, x2: np.ndarray, y2: np.ndarray, method: Method
) -> tuple[float, float, float]:
    """Mean of y1 and y2 over the overlapping x interval, plus the interval width."""
    lo = max(float(x1.min()), float(x2.min()))
    hi = min(float(x1.max()), float(x2.max()))
    if hi <= lo:
        raise ValueError(
            f"the two curves do not overlap (anchor spans [{x1.min():.4g}, {x1.max():.4g}], "
            f"test spans [{x2.min():.4g}, {x2.max():.4g}]); "
            "pick matched operating points closer together"
        )
    if method == "cubic":
        i1 = _poly_int(np.polyfit(x1, y1, 3), lo, hi)
        i2 = _poly_int(np.polyfit(x2, y2, 3), lo, hi)
    elif method == "pchip":
        i1 = _pchip_int(x1, y1, lo, hi)
        i2 = _pchip_int(x2, y2, lo, hi)
    else:
        raise ValueError(f"unknown method {method!r} (want 'cubic' or 'pchip')")
    span = hi - lo
    return i1 / span, i2 / span, span


def bd_rate(
    rate_anchor,
    dist_anchor,
    rate_test,
    dist_test,
    method: Method = "cubic",
) -> float:
    """Average bitrate difference of *test* against *anchor*, in percent.

    Negative is better (fewer bits at matched quality).
    """
    r1, d1 = _prepare(rate_anchor, dist_anchor)
    r2, d2 = _prepare(rate_test, dist_test)
    a1, a2, _ = _average_over_overlap(d1, np.log10(r1), d2, np.log10(r2), method)
    return float((10.0 ** (a2 - a1) - 1.0) * 100.0)


def bd_psnr(
    rate_anchor,
    dist_anchor,
    rate_test,
    dist_test,
    method: Method = "cubic",
) -> float:
    """Average quality difference of *test* against *anchor*, in dB.

    Positive is better (more dB at matched rate).
    """
    r1, d1 = _prepare(rate_anchor, dist_anchor)
    r2, d2 = _prepare(rate_test, dist_test)
    a1, a2, _ = _average_over_overlap(np.log10(r1), d1, np.log10(r2), d2, method)
    return float(a2 - a1)


def overlap_range(rate_anchor, dist_anchor, rate_test, dist_test) -> tuple[float, float]:
    """The distortion interval the BD-rate integral actually covers."""
    _, d1 = _prepare(rate_anchor, dist_anchor)
    _, d2 = _prepare(rate_test, dist_test)
    return (max(float(d1.min()), float(d2.min())), min(float(d1.max()), float(d2.max())))


def bd_summary(rate_anchor, dist_anchor, rate_test, dist_test, method: Method = "cubic") -> dict:
    """Both figures plus the overlap, or an ``error`` key if they cannot be computed."""
    try:
        lo, hi = overlap_range(rate_anchor, dist_anchor, rate_test, dist_test)
        return {
            "bd_rate_pct": bd_rate(rate_anchor, dist_anchor, rate_test, dist_test, method),
            "bd_psnr_db": bd_psnr(rate_anchor, dist_anchor, rate_test, dist_test, method),
            "overlap_lo": lo,
            "overlap_hi": hi,
            "method": method,
        }
    except ValueError as exc:
        return {"error": str(exc), "method": method}
