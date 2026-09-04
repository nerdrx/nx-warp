"""Residual coding model: 8x8 DCT, dead-zone quantiser, and a bit estimate.

This is *not* the codec.  It is the smallest model of PAPER.md 1.4-1.6 that
produces an honest rate-distortion curve, so that the hybrid question ("how
should bits split between an HEVC base and our enhancement") can be answered
before ``ref/`` exists.

Transform
    Orthonormal 8x8 DCT-II, applied to every 8x8 block of the residual plane.
    The paper's transform is an integer Loeffler factorisation; orthonormal
    float is its ideal, and the difference is well under the noise floor of a
    layer-split study.

Quantiser
    Dead-zone uniform, ``q = sign(c) * floor(|c| / step + 1 - dz)`` with
    ``dz = 1/3`` (the H.264/HEVC inter rounding offset), and the HEVC step
    ladder ``step(QP) = 2^((QP - 4) / 6)``, so QP 4 is step 1 and every +6 QP
    doubles the step.  Flat matrix: no perceptual weighting, because
    foveation (PAPER.md 5.1) is a separate axis and mixing it in here would
    confound the layer-split measurement.

Bit estimate (documented in full because everything downstream rests on it)
    The residual of a plane is coded as a significance map plus magnitudes,
    and the estimate is the entropy of exactly that under the context model
    PAPER.md 1.6 actually specifies: one probability per *frequency position*
    (the per-band context), shared by every block of the plane, static for
    the frame.  With ``p_k`` the measured fraction of blocks whose
    coefficient at position ``k`` is non-zero:

        bits = sum_k  [ -log2(p_k)      if q_k != 0
                        -log2(1 - p_k)  otherwise ]     # significance
             + sum_{q != 0} ( 1 + log2(1 + |q|) )       # sign + magnitude

    with the significance sum taken only over blocks whose coded-block flag is
    set; each block first pays the entropy of that flag.  A per-plane
    probability table of 65 bytes is added once.

    The magnitude term is the ``log2(1 + |q|)`` form the experiment brief
    specifies.  The significance term is measured from the data rather than
    assumed, so it is parameter-free, and it is what a static rANS table with
    per-band contexts achieves.

    An earlier version of this file charged ``log2(C(64, nz))`` for the
    positions -- the cost of naming ``nz`` significant coefficients if they
    were uniformly scattered.  They are not: they cluster at low frequencies,
    and the uniform assumption overcharged by roughly 25 bits per block, or
    about 600 kbit per frame at 1024^2 -- three times the entire 150 Mbit
    budget.  That estimator survives as :func:`plane_bits_uniform` and is
    reported in RESULTS.md as a pessimistic bound.

    Per tile, signalling is added: 2 bits mode, 2 bits blend weight, 2 bits
    QP delta, plus a signed Exp-Golomb motion vector when residual-motion
    search is enabled.  Per frame, 36 bytes of homography per eye.

    The *same* estimator is used for the enhancement layer and for the
    pure-codec anchor, so hybrid-vs-pure is insensitive to its calibration.
    Only the comparison against x265 depends on it, and that is stated
    wherever it appears.
"""

from __future__ import annotations

import math

import numpy as np

BLOCK = 8
DEADZONE = 1.0 / 3.0

# The paper's blend weights (1.7), in units of the *spatial* hypothesis.
WEIGHTS_3BIT = (0.0, 0.25, 0.5, 0.75, 1.0)
# The 2-bit subset the experiment brief asks for.  Chosen as the four that
# lose least in a full-set-vs-subset A/B (see RESULTS.md "weight alphabet").
WEIGHTS_2BIT = (0.0, 0.25, 0.5, 1.0)


def _dct_matrix(n: int = BLOCK) -> np.ndarray:
    k = np.arange(n)
    m = np.cos(np.pi * (2 * k[None, :] + 1) * k[:, None] / (2 * n))
    m *= math.sqrt(2.0 / n)
    m[0] *= 1.0 / math.sqrt(2.0)
    return m.astype(np.float32)


_D = _dct_matrix()
_DT = _D.T.copy()


def qstep(qp: float) -> float:
    return float(2.0 ** ((qp - 4.0) / 6.0))


def _to_blocks(plane: np.ndarray) -> np.ndarray:
    h, w = plane.shape
    return plane.reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK).transpose(0, 2, 1, 3)


def _from_blocks(b: np.ndarray, h: int, w: int) -> np.ndarray:
    return b.transpose(0, 2, 1, 3).reshape(h, w)


def fdct(plane: np.ndarray) -> np.ndarray:
    """Forward 8x8 DCT of a whole plane -> (by, bx, 8, 8) coefficients."""
    b = _to_blocks(plane.astype(np.float32))
    return _D @ b @ _DT


def idct(coeff: np.ndarray, h: int, w: int) -> np.ndarray:
    return _from_blocks(_DT @ coeff @ _D, h, w)


def quantise(coeff: np.ndarray, step: float) -> np.ndarray:
    # q = floor(|c|/step + f) with f = 1/3: a coefficient must reach 2/3 of a
    # step before it survives.  (f = 1 - 1/3 would be *more* sensitive than
    # round-to-nearest, i.e. an anti-dead-zone; that was a bug here once and it
    # roughly tripled the non-zero count at coarse QP.)
    a = np.abs(coeff) / step + DEADZONE
    q = np.floor(np.maximum(a, 0.0)).astype(np.int32)
    return np.sign(coeff).astype(np.int32) * q


def dequantise(q: np.ndarray, step: float) -> np.ndarray:
    return q.astype(np.float32) * step


# --- bit estimate --------------------------------------------------------

_LOG2 = 1.0 / math.log(2.0)
NCOEF = BLOCK * BLOCK
# Cost of shipping the 64 per-position probabilities plus the coded-block
# flag probability, 8 bits each, per plane.
PROB_TABLE_BITS = (NCOEF + 1) * 8

_BINOM64 = np.array(
    [math.log2(math.comb(NCOEF, k)) for k in range(NCOEF + 1)], dtype=np.float32
)


def significance_probs(q: np.ndarray) -> np.ndarray:
    """Per-frequency-position non-zero probability of a quantised plane."""
    flat = (q != 0).reshape(-1, NCOEF)
    n = max(1, flat.shape[0])
    p = flat.mean(axis=0).astype(np.float32)
    # never charge an infinite surprise for a symbol the table can represent
    return np.clip(p, 0.5 / n, 1.0 - 0.5 / n)


def block_bits(q: np.ndarray) -> np.ndarray:
    """Estimated bits for every 8x8 block of a quantised plane.

    Two levels, as in every real codec: a coded-block flag per block, then
    per-frequency significance *only inside blocks whose flag is set*.  Without
    the flag, an all-zero block still pays 64 "not significant" symbols, which
    at coarse QP is most of the bitstream -- roughly 70 kbit per 1024^2 frame
    of pure nothing.

    *q* has shape (by, bx, 8, 8).  Returns (by, bx) float32.  Excludes the
    per-plane probability tables, which :func:`plane_bits` adds once.
    """
    aq = np.abs(q)
    sig = aq > 0
    cbf = sig.any(axis=(2, 3))
    nblk = max(1, cbf.size)
    pc = float(np.clip(cbf.mean(), 0.5 / nblk, 1.0 - 0.5 / nblk))
    out = np.where(cbf, -math.log2(pc), -math.log2(1.0 - pc)).astype(np.float32)
    ncoded = int(cbf.sum())
    if ncoded == 0:
        return out
    p = significance_probs(q[cbf])
    c1 = (-np.log2(p)).reshape(BLOCK, BLOCK)
    c0 = (-np.log2(1.0 - p)).reshape(BLOCK, BLOCK)
    inner = np.where(sig, c1, c0).sum(axis=(2, 3))
    mag = ((np.log2(1.0 + aq.astype(np.float32)) + 1.0) * sig).sum(axis=(2, 3))
    return out + (inner + mag) * cbf


def plane_bits(q: np.ndarray) -> float:
    return float(block_bits(q).sum()) + PROB_TABLE_BITS


def plane_bits_uniform(q: np.ndarray) -> float:
    """The pessimistic estimator: uniform significance positions.

    ``1 + log2(C(64, nz)) + nz + sum log2(1 + |q|)`` per block.  Kept as a
    bound: it assumes the significant coefficients are scattered uniformly
    over the block, which costs about 25 bits per block more than they
    actually do.
    """
    aq = np.abs(q)
    nz = np.count_nonzero(aq, axis=(2, 3))
    mag = np.log2(1.0 + aq.astype(np.float32)).sum(axis=(2, 3))
    return float((1.0 + _BINOM64[nz] + nz.astype(np.float32) + mag).sum())


def empirical_bits(q: np.ndarray) -> float:
    """Order-0 entropy of the (zero-run, level) symbol stream, as a check.

    Levels are bucketed by magnitude class (0, 1, 2, 3-4, 5-8, 9-16, >16) and
    zero runs by run class, and the entropy of the joint alphabet is measured
    over the whole plane.  This is roughly what a context-free rANS coder with
    a well-fitted static table would achieve.
    """
    flat = q.reshape(-1, BLOCK * BLOCK)
    aq = np.abs(flat)
    lvl = np.digitize(aq, [1, 2, 3, 5, 9, 17])  # 0..6
    sign_bits = float(np.count_nonzero(aq))
    # run class: number of zeros since the previous non-zero, bucketed
    sym = lvl.reshape(-1)
    counts = np.bincount(sym, minlength=7).astype(np.float64)
    p = counts / counts.sum()
    p = p[p > 0]
    ent = float(-(p * np.log2(p)).sum()) * sym.size
    # residual magnitude refinement inside each class
    refine = float(np.log2(1.0 + aq[aq > 0]).sum()) * 0.5
    return ent + sign_bits + refine


def exp_golomb_bits(v: int) -> int:
    """Length of the signed Exp-Golomb code for *v* (k = 0)."""
    u = 2 * abs(int(v)) + (0 if v > 0 else 1) if v != 0 else 0
    return 2 * int(math.floor(math.log2(u + 1))) + 1


# --- tile classification -------------------------------------------------

CLASSES = ("flat", "texture", "edge", "text")


def classify_tiles(y: np.ndarray, tile: int) -> np.ndarray:
    """Classify every *tile*x*tile* luma tile as flat/texture/edge/text.

    Simple gradient statistics, chosen to be reproducible rather than clever:

    ``g``    mean absolute gradient magnitude
    ``p95``  95th percentile of the gradient magnitude
    ``bim``  fraction of pixels in the darkest or brightest decile of the tile
             (text on a plate is strongly bimodal; texture is not)

    flat     g < 4
    text     bim > 0.72 and p95 > 60
    edge     p95 > 4.5 * g   (energy concentrated in few pixels)
    texture  otherwise
    """
    h, w = y.shape
    ty, tx = h // tile, w // tile
    gx = np.abs(np.diff(y, axis=1, append=y[:, -1:]))
    gy = np.abs(np.diff(y, axis=0, append=y[-1:, :]))
    g = gx + gy
    gt = g.reshape(ty, tile, tx, tile).transpose(0, 2, 1, 3).reshape(ty, tx, -1)
    yt = y.reshape(ty, tile, tx, tile).transpose(0, 2, 1, 3).reshape(ty, tx, -1)
    gmean = gt.mean(axis=2)
    p95 = np.percentile(gt, 95, axis=2)
    lo = np.percentile(yt, 10, axis=2)[..., None]
    hi = np.percentile(yt, 90, axis=2)[..., None]
    bim = ((yt <= lo + 12) | (yt >= hi - 12)).mean(axis=2)
    out = np.full((ty, tx), 1, dtype=np.int8)  # texture
    out[p95 > 4.5 * np.maximum(gmean, 1e-3)] = 2  # edge
    out[(bim > 0.72) & (p95 > 60)] = 3  # text
    out[gmean < 4.0] = 0  # flat
    return out
