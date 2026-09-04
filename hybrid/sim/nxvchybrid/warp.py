"""Pose warp and resolution resampling.

Two operations the hybrid predictor needs:

``warp``      target->source homography resampling, PAPER.md 2.2:
              H = K R_{N-1}^T R_N K^-1, clamp-to-edge, bilinear or Catmull-Rom
``upsample``  separable Catmull-Rom scaling of the decoded base to full
              resolution -- the spatial hypothesis of PAPER.md 1.7

Both are float here.  The shipping codec defines the warp in Q8.24 integer
arithmetic precisely so encoder and decoder cannot drift (2.2, "Determinism:
integer warp"); a float model is adequate for a rate-distortion study because
the simulator runs one implementation on both sides, and the paper already
states that the quantisation error of H lands in the residual.  What the float
model does *not* capture is the extra residual cost of that H quantisation,
which is small but non-zero: noted as a caveat in RESULTS.md.
"""

from __future__ import annotations

import numpy as np

# Catmull-Rom (a = -0.5) evaluated on a fractional offset t in [0,1).
def _catrom_weights(t: np.ndarray) -> tuple[np.ndarray, ...]:
    t2 = t * t
    t3 = t2 * t
    w0 = -0.5 * t3 + t2 - 0.5 * t
    w1 = 1.5 * t3 - 2.5 * t2 + 1.0
    w2 = -1.5 * t3 + 2.0 * t2 + 0.5 * t
    w3 = 0.5 * t3 - 0.5 * t2
    return w0, w1, w2, w3


def homography(K: np.ndarray, R_prev: np.ndarray, R_cur: np.ndarray) -> np.ndarray:
    """H mapping a pixel of frame N to its source position in frame N-1."""
    return K @ R_prev.T @ R_cur @ np.linalg.inv(K)


def scale_homography(H: np.ndarray, s: float) -> np.ndarray:
    """Same homography expressed in a coordinate system scaled by *s*."""
    S = np.diag([s, s, 1.0])
    return S @ H @ np.linalg.inv(S)


def _gather(img: np.ndarray, sx: np.ndarray, sy: np.ndarray, filt: str) -> np.ndarray:
    h, w = img.shape
    if filt == "bilinear":
        x0 = np.floor(sx).astype(np.int32)
        y0 = np.floor(sy).astype(np.int32)
        fx = (sx - x0).astype(np.float32)
        fy = (sy - y0).astype(np.float32)
        x0c = np.clip(x0, 0, w - 1)
        x1c = np.clip(x0 + 1, 0, w - 1)
        y0c = np.clip(y0, 0, h - 1)
        y1c = np.clip(y0 + 1, 0, h - 1)
        a = img[y0c, x0c]
        b = img[y0c, x1c]
        c = img[y1c, x0c]
        d = img[y1c, x1c]
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

    x0 = np.floor(sx).astype(np.int32)
    y0 = np.floor(sy).astype(np.int32)
    wx = _catrom_weights((sx - x0).astype(np.float32))
    wy = _catrom_weights((sy - y0).astype(np.float32))
    xs = [np.clip(x0 + k, 0, w - 1) for k in (-1, 0, 1, 2)]
    ys = [np.clip(y0 + k, 0, h - 1) for k in (-1, 0, 1, 2)]
    out = np.zeros(sx.shape, dtype=np.float32)
    for j in range(4):
        row = np.zeros(sx.shape, dtype=np.float32)
        for i in range(4):
            row += wx[i] * img[ys[j], xs[i]]
        out += wy[j] * row
    return out


def warp(img: np.ndarray, H: np.ndarray, filt: str = "catrom") -> np.ndarray:
    """Resample *img* (the previous output) through *H*, clamp to edge."""
    h, w = img.shape
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float64), np.arange(h, dtype=np.float64))
    num_x = H[0, 0] * gx + H[0, 1] * gy + H[0, 2]
    num_y = H[1, 0] * gx + H[1, 1] * gy + H[1, 2]
    den = H[2, 0] * gx + H[2, 1] * gy + H[2, 2]
    den = np.where(np.abs(den) < 1e-9, 1e-9, den)
    return _gather(img, num_x / den, num_y / den, filt)


def edge_pad(img: np.ndarray, r: int) -> np.ndarray:
    """Replicate-pad by *r* on every side.

    Padding once per frame turns every candidate motion vector of a search
    into a plain strided *view* (:func:`shifted_view`) instead of a two-pass
    fancy-index gather.  On a 1024^2 plane that is the difference between
    13 ms and roughly nothing per candidate, and the motion search evaluates
    tens of candidates per frame.
    """
    return np.pad(img, ((r, r), (r, r)), mode="edge")


def shifted_view(pad: np.ndarray, r: int, h: int, w: int, dx: int, dy: int) -> np.ndarray:
    """The (dx, dy) shift of the plane that :func:`edge_pad` padded by *r*."""
    return pad[r + dy : r + dy + h, r + dx : r + dx + w]


def shift(img: np.ndarray, dx: int, dy: int) -> np.ndarray:
    """Integer shift with clamp-to-edge, as a standalone array."""
    if dx == 0 and dy == 0:
        return img
    h, w = img.shape
    r = max(abs(dx), abs(dy))
    return shifted_view(edge_pad(img, r), r, h, w, dx, dy)


def _upsample_axis(img: np.ndarray, out_n: int, axis: int) -> np.ndarray:
    in_n = img.shape[axis]
    if in_n == out_n:
        return img
    scale = in_n / out_n
    pos = (np.arange(out_n, dtype=np.float64) + 0.5) * scale - 0.5
    i0 = np.floor(pos).astype(np.int32)
    wts = _catrom_weights((pos - i0).astype(np.float32))
    out = None
    for k, wk in zip((-1, 0, 1, 2), wts):
        idx = np.clip(i0 + k, 0, in_n - 1)
        part = np.take(img, idx, axis=axis) * (wk[:, None] if axis == 0 else wk[None, :])
        out = part if out is None else out + part
    return out  # type: ignore[return-value]


def upsample(img: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    """Separable Catmull-Rom resize (used for base -> full resolution)."""
    return _upsample_axis(_upsample_axis(img, out_h, 0), out_w, 1)
