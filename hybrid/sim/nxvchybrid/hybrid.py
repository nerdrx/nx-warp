"""The enhancement layer: two hypotheses, a per-tile blend, a coded residual.

This is the heart of the experiment.  Per PAPER.md 1.7 and 2.9, every
enhancement tile has two predictor hypotheses:

``S`` spatial: the upsampled reconstruction of the layer below -- here the
      decoded HEVC base of frame N, Catmull-Rom upsampled to full resolution.
      Drift-free, needs no motion, but carries only the base's detail.
``T`` temporal: ``warp(Out(N-1))``, the pose-warped previous *final* output,
      plus an optional per-tile integer MV correction (PAPER.md 2.3).  Carries
      full-resolution detail, but drifts and fails on screen-space movers.

The tile predictor is ``P = w*S + (1-w)*T`` with ``w`` from a small alphabet
signalled in the tile header, chosen by least residual energy.  The residual
``src - P`` is coded with the 8x8 DCT / dead-zone quantiser / bit model of
:mod:`nxvchybrid.codec`, and ``Out(N) = clip(P + dequant(residual))`` becomes
the next frame's temporal hypothesis.

The same loop with the spatial hypothesis removed is the *pure-codec anchor*:
INTRA or pose-warped INTER only, no base layer.  Running the anchor through
identical machinery is what makes the hybrid-vs-pure comparison meaningful
even though the bit model is a model.
"""

from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, field, asdict

import numpy as np

from . import codec, warp as warpmod
from .metrics import frame_psnr, psnr, ssim
from .panorama import intrinsics
from .yuv import Frame, YuvSequence

TILE = 64  # luma tile, PAPER.md 6.2

MODE_INTRA = 0
MODE_BLEND = 1

# Signalling per tile, in bits: mode(2) + weight(2) + qp_delta(2).
TILE_SIGNAL_BITS = 6
INTRA_DC_BITS = 8
FRAME_HEADER_BITS = 36 * 8  # the Q8.24 homography, PAPER.md 2.2


def tile_sse(a: np.ndarray, b: np.ndarray, tile: int) -> np.ndarray:
    d = (a - b).astype(np.float32)
    d *= d
    h, w = d.shape
    return d.reshape(h // tile, tile, w // tile, tile).sum(axis=(1, 3))


def tile_mean(a: np.ndarray, tile: int) -> np.ndarray:
    h, w = a.shape
    return a.reshape(h // tile, tile, w // tile, tile).mean(axis=(1, 3))


def expand(t: np.ndarray, tile: int) -> np.ndarray:
    """(ty,tx) per-tile values -> per-pixel array."""
    return np.repeat(np.repeat(t, tile, axis=0), tile, axis=1)


@dataclass
class RunStats:
    # per-frame series
    bits: list[float] = field(default_factory=list)
    psnr_y: list[float] = field(default_factory=list)
    psnr_w: list[float] = field(default_factory=list)
    ssim_y: list[float] = field(default_factory=list)
    qp: list[float] = field(default_factory=list)
    # hypothesis statistics, accumulated over inter frames
    weight_hist: dict[str, int] = field(default_factory=dict)
    # tiles where the pure-temporal hypothesis beats the pure-base hypothesis
    temporal_wins: int = 0
    base_wins: int = 0
    # ... broken down by tile class
    class_temporal_wins: dict[str, int] = field(default_factory=dict)
    class_total: dict[str, int] = field(default_factory=dict)
    class_weight_sum: dict[str, float] = field(default_factory=dict)
    # blend gain: SSE of the blended predictor over SSE of the best single one
    blend_sse: float = 0.0
    best_single_sse: float = 0.0
    intra_tiles: int = 0
    skipped_tiles: int = 0
    total_tiles: int = 0
    empirical_bits: float = 0.0
    model_bits: float = 0.0


@dataclass
class RunResult:
    label: str
    mode: str  # "hybrid" | "pure" | "hevc"
    base_scale: float
    base_bits: int
    enh_bits: float
    frames: int
    fps: float
    width: int
    height: int
    psnr_y: float
    psnr_w: float
    ssim_y: float
    stats: RunStats

    @property
    def total_bits(self) -> float:
        return self.base_bits + self.enh_bits

    def bitrate_bps(self) -> float:
        return self.total_bits * self.fps / self.frames

    def to_json(self) -> dict:
        d = asdict(self)
        d["total_bits"] = self.total_bits
        d["bitrate_bps"] = self.bitrate_bps()
        return d


def _mv_candidates(radius: int) -> list[tuple[int, int]]:
    """Coarse (step 2) then fine (step 1) full-pel candidates."""
    coarse = [(dx, dy) for dy in range(-radius, radius + 1, 2) for dx in range(-radius, radius + 1, 2)]
    fine = [(dx, dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    seen: list[tuple[int, int]] = []
    for c in coarse + fine:
        if c not in seen:
            seen.append(c)
    return seen


def _search_mv(src: np.ndarray, temporal: np.ndarray, tile: int, radius: int):
    """Per-tile integer MV minimising SSE against *temporal*.

    Returns (mvx, mvy) per tile and the MV-corrected temporal plane.
    """
    ty, tx = src.shape[0] // tile, src.shape[1] // tile
    best = np.full((ty, tx), np.inf, dtype=np.float32)
    bx = np.zeros((ty, tx), dtype=np.int16)
    by = np.zeros((ty, tx), dtype=np.int16)
    for dx, dy in _mv_candidates(radius):
        cand = warpmod.shift(temporal, dx, dy)
        s = tile_sse(src, cand, tile)
        better = s < best
        best = np.where(better, s, best)
        bx = np.where(better, dx, bx).astype(np.int16)
        by = np.where(better, dy, by).astype(np.int16)
    out = np.empty_like(temporal)
    for dx, dy in set(zip(bx.reshape(-1).tolist(), by.reshape(-1).tolist())):
        m = expand((bx == dx) & (by == dy), tile)
        np.copyto(out, warpmod.shift(temporal, dx, dy), where=m)
    return bx, by, out


def _quantise_planes(coeffs, qp: float):
    step = codec.qstep(qp)
    return [codec.quantise(c, step) for c in coeffs], step


def _bits_for(qs) -> float:
    return sum(codec.plane_bits(q) for q in qs)


def _choose_qp(coeffs, target_bits: float, qp_lo: float = 4.0, qp_hi: float = 51.0, iters: int = 9):
    """Bisect the frame QP so the modelled residual bits meet *target_bits*."""
    lo, hi = qp_lo, qp_hi
    qs, step = _quantise_planes(coeffs, hi)
    if _bits_for(qs) > target_bits:
        return hi, qs, _bits_for(qs)
    qs_lo, _ = _quantise_planes(coeffs, lo)
    if _bits_for(qs_lo) <= target_bits:
        return lo, qs_lo, _bits_for(qs_lo)
    best = (hi, qs, _bits_for(qs))
    for _ in range(iters):
        mid = 0.5 * (lo + hi)
        qs, _ = _quantise_planes(coeffs, mid)
        b = _bits_for(qs)
        if b > target_bits:
            lo = mid
        else:
            hi = mid
            best = (mid, qs, b)
    return best


def _greedy_tile_skip(coeffs, qs, target_bits: float, tile: int, shapes):
    """Zero whole tiles, cheapest distortion first, until the budget fits.

    Only reached when even QP 51 overshoots -- the bottom of the paper's
    degradation ladder (4.6.1: blur, never block).  Returns the number of
    tiles zeroed.
    """
    # per-tile bits and per-tile coded energy, luma plane only for ranking
    q0 = qs[0]
    bb = codec.block_bits(q0)  # (by, bx)
    tb = tile // codec.BLOCK
    by, bx = bb.shape
    tile_bits = bb.reshape(by // tb, tb, bx // tb, tb).sum(axis=(1, 3))
    step_energy = (
        (np.abs(q0).astype(np.float32).sum(axis=(2, 3)))
        .reshape(by // tb, tb, bx // tb, tb)
        .sum(axis=(1, 3))
    )
    benefit = step_energy / np.maximum(tile_bits, 1.0)
    order = np.argsort(benefit, axis=None)
    total = _bits_for(qs)
    drop = np.zeros(tile_bits.shape, dtype=bool)
    n = 0
    for idx in order:
        if total <= target_bits:
            break
        iy, ix = np.unravel_index(idx, tile_bits.shape)
        if tile_bits[iy, ix] <= 0:
            continue
        drop[iy, ix] = True
        total -= float(tile_bits[iy, ix])
        n += 1
    if n:
        for pi, q in enumerate(qs):
            t = tile if pi == 0 else tile // 2
            tbp = t // codec.BLOCK
            m = np.repeat(np.repeat(drop, tbp, axis=0), tbp, axis=1)
            q[m] = 0
    return n


def run_enhancement(
    src: YuvSequence,
    base: "YuvSequence | None",
    poses,
    target_enh_bits: float,
    *,
    mode: str = "hybrid",
    label: str = "",
    base_scale: float = 1.0,
    base_bits: int = 0,
    fps: float = 90.0,
    fov_deg: float = 95.0,
    weights=codec.WEIGHTS_2BIT,
    mv_radius: int = 6,
    warp_filter: str = "catrom",
    collect_stats: bool = True,
) -> RunResult:
    """Run the closed-loop enhancement (or pure) codec over the sequence."""
    n = len(src)
    w = src.w
    h = src.h
    K = intrinsics(w, fov_deg)
    K_c = intrinsics(w // 2, fov_deg)

    have_base = base is not None and mode == "hybrid"
    st = RunStats()
    for c in codec.CLASSES:
        st.class_temporal_wins[c] = 0
        st.class_total[c] = 0
        st.class_weight_sum[c] = 0.0
    for wt in weights:
        st.weight_hist[f"{wt:g}"] = 0

    prev_out: Frame | None = None
    cum_bits = 0.0
    cum_target = 0.0
    per_frame_target = target_enh_bits / n

    for i in range(n):
        f = src[i]
        planes = f.planes()

        # --- hypotheses -------------------------------------------------
        if have_base:
            b = base[i]  # type: ignore[index]
            S = [
                warpmod.upsample(b.y, h, w),
                warpmod.upsample(b.cb, h // 2, w // 2),
                warpmod.upsample(b.cr, h // 2, w // 2),
            ]
        else:
            S = None

        if prev_out is not None and i > 0:
            H = warpmod.homography(K, poses[i - 1].matrix(), poses[i].matrix())
            H_c = warpmod.homography(K_c, poses[i - 1].matrix(), poses[i].matrix())
            T = [
                warpmod.warp(prev_out.y, H, warp_filter),
                warpmod.warp(prev_out.cb, H_c, warp_filter),
                warpmod.warp(prev_out.cr, H_c, warp_filter),
            ]
            if mv_radius > 0:
                mvx, mvy, T[0] = _search_mv(f.y, T[0], TILE, mv_radius)
                cx = np.rint(mvx / 2).astype(int)
                cy = np.rint(mvy / 2).astype(int)
                outc = [np.empty_like(T[1]), np.empty_like(T[2])]
                for dx, dy in set(zip(cx.reshape(-1).tolist(), cy.reshape(-1).tolist())):
                    m = expand((cx == dx) & (cy == dy), TILE // 2)
                    for pi in (0, 1):
                        np.copyto(outc[pi], warpmod.shift(T[pi + 1], dx, dy), where=m)
                T[1], T[2] = outc
            else:
                mvx = mvy = np.zeros((h // TILE, w // TILE), dtype=np.int16)
        else:
            T = None
            mvx = mvy = np.zeros((h // TILE, w // TILE), dtype=np.int16)

        ty, tx = h // TILE, w // TILE
        ntiles = ty * tx

        # --- per-tile predictor choice (least residual energy) ----------
        def combined_sse(pred) -> np.ndarray:
            s = tile_sse(planes[0], pred[0], TILE)
            s = s + tile_sse(planes[1], pred[1], TILE // 2)
            s = s + tile_sse(planes[2], pred[2], TILE // 2)
            return s

        cands: list[tuple[str, float, list[np.ndarray]]] = []
        intra_dc = [tile_mean(p, TILE if k == 0 else TILE // 2) for k, p in enumerate(planes)]
        intra_pred = [expand(intra_dc[k], TILE if k == 0 else TILE // 2) for k in range(3)]
        cands.append(("intra", -1.0, intra_pred))

        if S is not None and T is not None:
            for wt in weights:
                if wt == 0.0:
                    pred = T
                elif wt == 1.0:
                    pred = S
                else:
                    pred = [wt * S[k] + (1.0 - wt) * T[k] for k in range(3)]
                cands.append(("blend", wt, pred))
        elif S is not None:
            cands.append(("blend", 1.0, S))
        elif T is not None:
            cands.append(("blend", 0.0, T))

        sses = np.stack([combined_sse(p) for _, _, p in cands], axis=0)
        pick = np.argmin(sses, axis=0)

        pred_planes = [np.empty_like(p) for p in planes]
        for ci, (kind, wt, pred) in enumerate(cands):
            m = pick == ci
            if not m.any():
                continue
            for k in range(3):
                t = TILE if k == 0 else TILE // 2
                np.copyto(pred_planes[k], pred[k], where=expand(m, t))

        # --- statistics on which hypothesis won -------------------------
        if collect_stats and S is not None and T is not None:
            cls = codec.classify_tiles(planes[0], TILE)
            sse_T = combined_sse(T)
            sse_S = combined_sse(S)
            twin = sse_T < sse_S
            st.temporal_wins += int(twin.sum())
            st.base_wins += int((~twin).sum())
            best_single = np.minimum(sse_T, sse_S)
            blend_only = sses[1:].min(axis=0) if len(cands) > 1 else best_single
            st.best_single_sse += float(best_single.sum())
            st.blend_sse += float(blend_only.sum())
            wsel = np.array([c[1] for c in cands], dtype=np.float32)[pick]
            for ci, c in enumerate(codec.CLASSES):
                m = cls == ci
                st.class_total[c] += int(m.sum())
                st.class_temporal_wins[c] += int((twin & m).sum())
                sel = wsel[m]
                st.class_weight_sum[c] += float(sel[sel >= 0].sum())
            for ci, (kind, wt, _p) in enumerate(cands):
                if kind == "blend":
                    st.weight_hist[f"{wt:g}"] = st.weight_hist.get(f"{wt:g}", 0) + int(
                        (pick == ci).sum()
                    )
        n_intra = int((pick == 0).sum())
        st.intra_tiles += n_intra
        st.total_tiles += ntiles

        # --- residual, rate control, reconstruction ---------------------
        resid = [planes[k] - pred_planes[k] for k in range(3)]
        coeffs = [codec.fdct(r) for r in resid]

        signal_bits = FRAME_HEADER_BITS + ntiles * TILE_SIGNAL_BITS + n_intra * 3 * INTRA_DC_BITS
        if mv_radius > 0 and T is not None:
            signal_bits += float(
                sum(codec.exp_golomb_bits(int(v)) for v in mvx.reshape(-1))
                + sum(codec.exp_golomb_bits(int(v)) for v in mvy.reshape(-1))
            )

        cum_target += per_frame_target
        budget = max(0.0, cum_target - cum_bits) * 0.5 + per_frame_target * 0.5
        resid_target = max(64.0, budget - signal_bits)

        qp, qs, mbits = _choose_qp(coeffs, resid_target)
        if mbits > resid_target * 1.02:
            st.skipped_tiles += _greedy_tile_skip(coeffs, qs, resid_target, TILE, None)
            mbits = _bits_for(qs)
        step = codec.qstep(qp)

        frame_bits = mbits + signal_bits
        cum_bits += frame_bits
        if collect_stats:
            st.model_bits += mbits
            st.empirical_bits += sum(codec.empirical_bits(q) for q in qs)

        rec = []
        for k in range(3):
            hh, ww = planes[k].shape
            r = codec.idct(codec.dequantise(qs[k], step), hh, ww)
            rec.append(np.clip(np.rint(pred_planes[k] + r), 0.0, 255.0).astype(np.float32))
        out = Frame(rec[0], rec[1], rec[2])

        py, pw_ = frame_psnr(f, out)
        st.bits.append(frame_bits)
        st.psnr_y.append(py)
        st.psnr_w.append(pw_)
        st.ssim_y.append(ssim(f.y, out.y))
        st.qp.append(qp)
        prev_out = out

    enh_bits = float(sum(st.bits))
    return RunResult(
        label=label,
        mode=mode,
        base_scale=base_scale,
        base_bits=base_bits,
        enh_bits=enh_bits,
        frames=n,
        fps=fps,
        width=w,
        height=h,
        psnr_y=float(np.mean(st.psnr_y)),
        psnr_w=float(np.mean(st.psnr_w)),
        ssim_y=float(np.mean(st.ssim_y)),
        stats=st,
    )


def measure_sequence(src: YuvSequence, rec: YuvSequence) -> tuple[float, float, float]:
    """Mean Y-PSNR, weighted PSNR and Y-SSIM between two sequences."""
    pys, pws, ss = [], [], []
    for i in range(len(src)):
        a = src[i]
        b = rec[i]
        py, pw_ = frame_psnr(a, b)
        pys.append(py)
        pws.append(pw_)
        ss.append(ssim(a.y, b.y))
    return float(np.mean(pys)), float(np.mean(pws)), float(np.mean(ss))
