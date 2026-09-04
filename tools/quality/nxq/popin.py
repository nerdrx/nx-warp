"""The temporal pop-in metric, and the visibility model it is weighted by.

PAPER.md 5.3 asks for

    a dedicated "pop-in" metric: per-tile JOD delta between consecutive frames
    in the fovea ring after a scale change, thresholded; tracked as a
    distribution, not a mean.

and docs/RATECONTROL.md 8 builds the temporal ladder — tiles coded one frame in
`k`, emitted as `WARP_SKIP` in between — on the Tursun and Didyk peripheral
temporal-change visibility model (ACM TOG 41(6) 2022, arXiv 2205.00108).  This
module is both halves of that: the model, and a measurement that uses it.

What is measured
----------------

For every tile of every frame pair the metric forms

    step_dis = mean |dis[t] - dis[t-1]|   over the tile's luma
    step_ref = mean |ref[t] - ref[t-1]|   over the same pixels
    pop      = max(0, step_dis - step_ref)

`pop` is the frame-to-frame change the **codec** introduced on top of the
change the content itself has.  A tile whose residual has been withheld for
`k-1` frames and is then sent produces its accumulated correction in one frame,
and that step is exactly what `pop` sees.  Content motion, which the reference
has too, cancels.

`pop` is then priced by the visibility model at the temporal frequency the
refresh cadence excites (`f_t = fps/k` and its first harmonics), at the tile's
eccentricity, spatial frequency and mean luminance, giving `C_M` in the paper's
JND units — 1.0 is one just-noticeable difference.  The result is reported as a
**distribution over refresh events**: count, mean, median, p95, max, and the
fraction above one JND.  A mean alone would hide exactly the tail 5.3 is
worried about.

Where the refresh events come from
----------------------------------

Best: a **skip map**, the same ``tile_count`` bytes per frame that
``nxv-enc --skip-map`` consumes (docs/RATECONTROL.md 8.7).  Then the stale run
length `k` before each coded frame is known exactly and only genuine refresh
frames are scored.  :class:`SkipSchedule` both writes those files (so a
temporal ladder can be driven through the encoder) and reads them back.

Without one the metric falls back to scoring **every** frame with `k = 1`,
which measures the codec's frame-to-frame instability rather than a scheduled
pop-in.  The result carries ``mode: "all-frames"`` so the two are never
confused.

The model is a numpy port of ``rc/src/tvm.cpp``
-----------------------------------------------

Every fitted constant is Tursun and Didyk's Table 2; every clamp and reduction
is the harness's own and is listed in docs/RATECONTROL.md 8.2, which is where
they are argued.  ``tests/test_popin.py`` pins this port against the numbers
``rc/RESULTS-temporal.md`` reports from the C++ implementation, so the two
cannot drift apart silently.
"""

from __future__ import annotations

import dataclasses
import math
import os

import numpy as np

TILE_SIZE = 64  #: NXVC_TILE_SIZE, include/nxvc/nxvc.h


# --- the Tursun-Didyk model, ported from rc/src/tvm.cpp -------------------


@dataclasses.dataclass(frozen=True)
class TvmParams:
    """``nxrc::tvm::ModelParams``, field for field and value for value."""

    # Tursun and Didyk, Table 2.
    a: tuple[float, float, float, float] = (3.2714, 0.3830, 0.7669, -0.2555)
    b1: float = 1.0051
    b2: float = 0.1830
    b3: float = 0.9517
    b4: float = 0.0173
    b51: float = -0.1375
    b52: float = 0.3753
    b53: float = 2.3855
    b6: float = 0.0
    b7: float = 0.0
    b8: float = 0.0
    r: float = 1.9932
    p_g: float = 0.5
    p_l: float = 0.0
    beta0: float = 1.7934
    beta1: float = 1.5
    l_min_nits: float = 50.0
    # Our reduction; docs/RATECONTROL.md 8.2 approximations 1, 2 and 5.
    fs_max_cpd: float = 4.0
    ecc_min_deg: float = 0.5
    ecc_max_deg: float = 60.0
    sens_floor: float = 0.02
    display_peak_nits: float = 100.0
    display_gamma: float = 2.2
    harmonics: int = 3


def _ft_argument(f_t_hz, p: TvmParams):
    """The De Lange polynomial's argument: ``ln(1 + f_t)``, not raw Hz.

    Approximation 1 of docs/RATECONTROL.md 8.2, and the largest judgement in
    the model.  In raw Hz the published cubic crosses zero at 4.5 Hz, so
    nothing above 5 Hz would ever be visible, which is neither a De Lange curve
    nor what the paper's figures show.
    """
    return np.log1p(np.maximum(np.asarray(f_t_hz, dtype=np.float64), 0.0))


def sensitivity_temporal(f_t_hz, p: TvmParams = TvmParams()):
    """``S_SP = ln(1 + exp(S_DL(x)))``, the soft-plussed De Lange polynomial."""
    x = _ft_argument(f_t_hz, p)
    s = p.a[0] + x * (p.a[1] + x * (p.a[2] + x * p.a[3]))
    return np.maximum(s, 0.0) + np.log1p(np.exp(-np.abs(s)))


def sensitivity_scale(f_s_cpd, e_deg, p: TvmParams = TvmParams()):
    """``T(f_s, e) = b1 - b2 f_s^b3 + b4 e^q(f_s)``, clamped into the fit."""
    fs = np.clip(np.asarray(f_s_cpd, dtype=np.float64), 0.0, p.fs_max_cpd)
    e = np.clip(np.asarray(e_deg, dtype=np.float64), p.ecc_min_deg, p.ecc_max_deg)
    q = p.b51 * fs * fs + p.b52 * fs + p.b53
    t = p.b1 - p.b2 * np.power(fs, p.b3) + p.b4 * np.power(e, q)
    return np.maximum(t, p.sens_floor)


def sensitivity(f_t_hz, f_s_cpd, e_deg, p: TvmParams = TvmParams()):
    """``S = T(f_s, e) * S_SP(U)``, in units of 1/Weber-contrast."""
    fs = np.clip(np.asarray(f_s_cpd, dtype=np.float64), 0.0, p.fs_max_cpd)
    e = np.clip(np.asarray(e_deg, dtype=np.float64), p.ecc_min_deg, p.ecc_max_deg)
    u = np.asarray(f_t_hz, dtype=np.float64) - p.b6 + p.b7 * fs + p.b8 * e
    return sensitivity_scale(fs, e, p) * sensitivity_temporal(u, p)


def detect_prob(c_m, p: TvmParams = TvmParams()):
    """Weibull psychometric function (approximation 3 of RATECONTROL.md 8.2)."""
    c = np.maximum(np.asarray(c_m, dtype=np.float64), 0.0)
    w = 1.0 - np.exp(-np.power(c / max(p.beta0, 1e-6), p.beta1))
    return p.p_g + (1.0 - p.p_g) * (1.0 - p.p_l) * w


def visibility(c_m, p: TvmParams = TvmParams()):
    """Detection probability rescaled to "excess over chance" in [0, 1]."""
    d = 1.0 - p.p_g
    if d <= 0.0:
        return np.zeros_like(np.asarray(c_m, dtype=np.float64))
    return np.clip((detect_prob(c_m, p) - p.p_g) / d, 0.0, 1.0)


def luma_to_nits(code, p: TvmParams = TvmParams()):
    c = np.clip(np.asarray(code, dtype=np.float64), 0.0, 255.0) / 255.0
    return p.display_peak_nits * np.power(c, p.display_gamma)


def spatial_freq_cpd(freq_ratio_r, ppd_render, p: TvmParams = TvmParams()):
    """Representative ``f_h + f_v`` in cycles/degree from the classifier's R.

    ``R = E[sin^2(2 pi f_x)] + E[sin^2(2 pi f_y)]`` in cycles/sample, exactly
    1.0 for white noise; inverting it isotropically gives one frequency per
    axis and the model wants their sum.
    """
    rr = np.clip(np.asarray(freq_ratio_r, dtype=np.float64), 0.0, 2.0)
    f = np.arcsin(np.sqrt(rr * 0.5)) / (2.0 * math.pi)
    return 2.0 * f * np.maximum(np.asarray(ppd_render, dtype=np.float64), 1.0)


def weber_contrast(d_code, mean_luma, p: TvmParams = TvmParams()):
    """A luma-code step at a mean level, as Weber contrast with the 50 nit floor.

    The display's gamma curve is differentiated at the tile's mean rather than
    differenced across the step, so a dark tile's small code delta does not
    become a huge nit delta through a badly conditioned subtraction.  This is
    ``tile_visibility``'s contrast block in rc/src/tvm.cpp.
    """
    mean = np.asarray(mean_luma, dtype=np.float64)
    mean_n = luma_to_nits(mean, p)
    c01 = np.clip(mean, 1.0, 255.0) / 255.0
    d_l = (p.display_peak_nits * p.display_gamma * np.power(c01, p.display_gamma - 1.0)
           * (np.asarray(d_code, dtype=np.float64) / 255.0))
    return d_l / np.maximum(mean_n, p.l_min_nits)


def step_visibility(d_code, k, fps, f_s_cpd, ecc_deg, mean_luma,
                    p: TvmParams = TvmParams()):
    """``C_M`` in JND units for a luma step of *d_code* delivered one frame in *k*.

    The error signal a withheld-then-refreshed tile presents is a hold-and-reset
    ramp of period ``k/fps`` whose harmonic amplitudes fall as ``1/(pi m)``;
    the first ``p.harmonics`` of them are pooled with the paper's Minkowski
    exponent.  ``k <= 1`` is "coded every frame" and scores zero.

    This is the core that both users share:
    :func:`predicted_tile_visibility` feeds it a *modelled* accumulated
    residual, and the pop-in metric feeds it the step it actually *measured*.
    """
    k_arr = np.asarray(k, dtype=np.float64)
    c = weber_contrast(d_code, mean_luma, p)
    f0 = np.asarray(fps, dtype=np.float64) / np.maximum(k_arr, 1.0)
    acc = np.zeros(np.broadcast(c, f0, np.asarray(f_s_cpd), np.asarray(ecc_deg)).shape)
    for m in range(1, max(p.harmonics, 1) + 1):
        amp = c / (math.pi * m)
        c_jnd = sensitivity(f0 * m, f_s_cpd, ecc_deg, p) * amp
        acc = acc + np.power(np.maximum(c_jnd, 0.0), p.r)
    out = np.power(acc, 1.0 / p.r)
    return np.where(k_arr <= 1.0, 0.0, out)


def predicted_tile_visibility(residual_mad, k, fps, freq_ratio_r, ppd_render,
                              ecc_deg, mean_luma, p: TvmParams = TvmParams(),
                              drift_exponent: float = 1.0):
    """``nxrc::tvm::tile_visibility``: what the *scheduler* predicts.

    The residual the tile would have coded accumulates over the `k` frames it
    is left to the warp (approximation 5, linear = constant velocity).
    """
    d_code = np.asarray(residual_mad, dtype=np.float64) * np.power(
        np.asarray(k, dtype=np.float64), drift_exponent)
    f_s = spatial_freq_cpd(freq_ratio_r, ppd_render, p)
    return step_visibility(d_code, k, fps, f_s, ecc_deg, mean_luma, p)


# --- tile statistics, matching rc/src/classify.cpp -----------------------


def _tile_view(plane: np.ndarray, tile: int) -> np.ndarray:
    """(ty, tx, tile, tile), padding the right/bottom edge by replication.

    The codec's tile grid covers ``ceil(w/64) x ceil(h/64)`` tiles, so a
    sequence whose size is not a multiple of 64 has partial tiles; replicating
    the edge keeps every tile the same shape without inventing gradients.
    """
    h, w = plane.shape
    ty, tx = (h + tile - 1) // tile, (w + tile - 1) // tile
    pad = np.pad(plane.astype(np.float64), ((0, ty * tile - h), (0, tx * tile - w)),
                 mode="edge")
    return pad.reshape(ty, tile, tx, tile).transpose(0, 2, 1, 3)


def tile_stats(plane: np.ndarray, tile: int = TILE_SIZE) -> dict:
    """Per-tile mean, variance, gradient energy and frequency ratio R.

    The same five statistics ``nxrc::compute_one_tile_stats`` and
    ``nxrc::tile_frequency_ratio`` produce, with the same central-difference
    operator and the same edge clamping, so a tile the encoder calls flat is
    flat here too.
    """
    t = _tile_view(plane, tile)
    mean = t.mean(axis=(2, 3))
    var = np.maximum((t * t).mean(axis=(2, 3)) - mean * mean, 0.0)
    xp = np.concatenate([t[:, :, :, 1:], t[:, :, :, -1:]], axis=3)
    xm = np.concatenate([t[:, :, :, :1], t[:, :, :, :-1]], axis=3)
    yp = np.concatenate([t[:, :, 1:, :], t[:, :, -1:, :]], axis=2)
    ym = np.concatenate([t[:, :, :1, :], t[:, :, :-1, :]], axis=2)
    gx = 0.5 * (xp - xm)
    gy = 0.5 * (yp - ym)
    grad = ((gx * gx) + (gy * gy)).mean(axis=(2, 3))
    # tile_frequency_ratio() takes log_var = log2(var + 1) and undoes it, so
    # the round trip is the variance itself with the same 1e-3 floor.
    freq_ratio = grad / np.maximum(var, 1e-3)
    return {"mean": mean, "var": var, "grad_energy": grad, "freq_ratio": freq_ratio}


def tile_mad(a: np.ndarray, b: np.ndarray, tile: int = TILE_SIZE) -> np.ndarray:
    """Mean absolute difference per tile between two luma planes."""
    d = np.abs(_tile_view(a, tile) - _tile_view(b, tile))
    return d.mean(axis=(2, 3))


# --- skip maps and the temporal ladder schedule --------------------------


@dataclasses.dataclass
class SkipSchedule:
    """A per-frame, per-tile ``force_warp_skip`` map.

    The on-disk form is exactly what ``nxv-enc --skip-map`` reads:
    ``tile_count`` bytes per frame, 1 = force ``WARP_SKIP``, in the linear tile
    index of the transport (Annex D D-3).  For a stereo stream that index is
    **eye-major**: all of eye 0's ``tiles_y x tiles_x`` raster, then all of
    eye 1's (``nxvc_tile_layout_get_ex``: ``tile_count = eyes * tiles_x *
    tiles_y`` with ``tiles_x`` the per-eye column count).  The in-memory shape
    keeps that order explicit rather than folding the eyes into the columns,
    which would interleave them and silently address the wrong tiles.
    """

    flags: np.ndarray  # (frames, eyes, tiles_y, tiles_x) uint8

    @property
    def frames(self) -> int:
        return int(self.flags.shape[0])

    def write(self, path: str) -> str:
        os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
        with open(path, "wb") as fh:
            for f in self.flags:
                fh.write(np.ascontiguousarray(f, dtype=np.uint8).tobytes())
        return path

    @classmethod
    def read(cls, path: str, eyes: int, tiles_y: int, tiles_x: int) -> "SkipSchedule":
        n = eyes * tiles_y * tiles_x
        buf = np.fromfile(path, dtype=np.uint8)
        if buf.size % n:
            raise ValueError(f"{path}: {buf.size} bytes is not a whole number of "
                             f"{n}-tile frames")
        return cls(buf.reshape(-1, eyes, tiles_y, tiles_x))

    @classmethod
    def ladder(cls, frames: int, eyes: int, ecc_deg: np.ndarray,
               rings=None) -> "SkipSchedule":
        """The temporal ladder of docs/RATECONTROL.md 8.3 as a fixed schedule.

        *ecc_deg* is one eye's ``(tiles_y, tiles_x)`` eccentricity map; both
        eyes get the same schedule, which is what a centre fixation implies.

        *rings* is a list of ``(max_eccentricity_deg, k)``; the default is
        Floeter et al.'s FRC 11223 operating point — full rate inside their
        inner three regions (eccentricity 9.05 deg), 1/2 out to 15.55 deg and
        1/3 beyond — which is the most aggressive configuration their
        participants tolerated (ETRA 2025, arXiv 2505.03682).

        A tile with divisor `k` is coded on frames where ``t % k == 0`` and
        skipped otherwise, with frame 0 always coded, so every tile has a
        defined reference before any of them is withheld.
        """
        if rings is None:
            rings = FLOETER_11223
        k = np.ones(ecc_deg.shape, dtype=np.int64)
        for lo_ecc, kk in rings:
            k = np.where(ecc_deg > lo_ecc, kk, k)
        k = np.broadcast_to(k, (eyes,) + ecc_deg.shape)
        t = np.arange(frames)[:, None, None, None]
        flags = ((t % k[None, ...]) != 0).astype(np.uint8)
        flags[0] = 0
        return cls(flags)


#: Floeter et al.'s FRC 11223 read as (eccentricity above which, refresh divisor).
#: Their region boundaries are diameters 6.3/9.1/18.1/31.1 deg, i.e.
#: eccentricities 3.15/4.55/9.05/15.55 (docs/RATECONTROL.md 8).
FLOETER_11223 = ((9.05, 2), (15.55, 3))
#: Their 12345, where the discomfort scores start to move.
FLOETER_12345 = ((3.15, 2), (4.55, 3), (9.05, 4), (15.55, 5))
LADDERS = {"11223": FLOETER_11223, "12345": FLOETER_12345,
           "none": ()}


def stale_run_lengths(skip: np.ndarray) -> np.ndarray:
    """For each (frame, tile), how many frames it had been skipped before this one.

    ``k`` in the model's sense is that run plus one: a tile skipped for two
    frames and coded on the third carries three frames of drift and is excited
    at ``fps/3``.  Coded frames that follow a coded frame get ``k = 1`` and
    score zero.
    """
    out = np.zeros(skip.shape, dtype=np.int64)
    run = np.zeros(skip.shape[1:], dtype=np.int64)
    for t in range(skip.shape[0]):
        out[t] = run + 1
        run = np.where(skip[t] != 0, run + 1, 0)
    return out


# --- the metric ----------------------------------------------------------


@dataclasses.dataclass
class PopInScoring:
    """Geometry and cadence the pop-in metric needs."""

    ppd_center: float
    fps: float
    layout: str = "mono"
    tile: int = TILE_SIZE
    fovea_radius_deg: float = 8.0
    params: TvmParams = dataclasses.field(default_factory=TvmParams)
    jnd_threshold: float = 1.0

    def to_json(self) -> dict:
        return {
            "ppd_center": self.ppd_center, "fps": self.fps, "layout": self.layout,
            "tile": self.tile, "fovea_radius_deg": self.fovea_radius_deg,
            "jnd_threshold": self.jnd_threshold,
            "model": "Tursun and Didyk 2022, reduced per docs/RATECONTROL.md 8.2",
            "params": dataclasses.asdict(self.params),
        }


def tile_geometry(view_h: int, view_w: int, ppd_center: float, tile: int) -> dict:
    """Per-tile eccentricity and rendered ppd for one view, centre fixation.

    Both come from ``foveated_metrics``: eccentricity is the exact angle in the
    tan projection, and ``ppd_render(theta) = ppd_center / cos^2(theta)`` is
    the angular density that the same projection gives at that angle, which is
    what ``nxfov::ppd_render`` hands the scheduler.
    """
    import foveated_metrics as fov

    ecc = fov.eccentricity_map(view_h, view_w, ppd_center)
    ecc_t = _tile_view(ecc, tile).mean(axis=(2, 3))
    ppd_t = np.asarray(fov.ppd_render(ecc_t, ppd_center), dtype=np.float64)
    return {"ecc_deg": ecc_t, "ppd_render": ppd_t}


def _distribution(values: np.ndarray, threshold: float) -> dict:
    if values.size == 0:
        return {"events": 0}
    return {
        "events": int(values.size),
        "mean": float(values.mean()),
        "median": float(np.median(values)),
        "p95": float(np.percentile(values, 95.0)),
        "max": float(values.max()),
        "over_threshold_frac": float((values >= threshold).mean()),
    }


def score_sequence(
    ref_frames, dis_frames, scoring: PopInScoring, *,
    skip: np.ndarray | None = None,
) -> dict:
    """The pop-in metric for one decoded sequence.

    *ref_frames* and *dis_frames* are iterables of :class:`nxq.yuv.Frame`.
    *skip*, when given, is ``(frames, eyes, tiles_y, tiles_x)`` of the flags
    that were handed to the encoder; the frames axis is matched by index.

    Returns the ``C_M`` distribution over all refresh events, the same split
    into the fovea disc and the periphery (5.3 asks about the fovea ring
    specifically), and the raw ``pop`` step distribution in luma codes.
    """
    p = scoring.params
    prev_ref = prev_dis = None
    geo_cache: dict = {}
    cm_all: list[np.ndarray] = []
    cm_fovea: list[np.ndarray] = []
    cm_periph: list[np.ndarray] = []
    pop_all: list[np.ndarray] = []
    k_all: list[np.ndarray] = []
    runs = stale_run_lengths(skip) if skip is not None else None

    for t, (rf, df) in enumerate(zip(ref_frames, dis_frames)):
        if prev_ref is None:
            prev_ref, prev_dis = rf.y, df.y
            continue
        halves = 2 if scoring.layout == "sbs" else 1
        w = rf.y.shape[1] // halves
        for v in range(halves):
            sl = slice(v * w, (v + 1) * w)
            key = (rf.y.shape[0], w)
            if key not in geo_cache:
                geo_cache[key] = tile_geometry(key[0], w, scoring.ppd_center, scoring.tile)
            geo = geo_cache[key]
            step_dis = tile_mad(prev_dis[:, sl], df.y[:, sl], scoring.tile)
            step_ref = tile_mad(prev_ref[:, sl], rf.y[:, sl], scoring.tile)
            pop = np.maximum(step_dis - step_ref, 0.0)
            st = tile_stats(rf.y[:, sl], scoring.tile)
            f_s = spatial_freq_cpd(st["freq_ratio"], geo["ppd_render"], p)
            if runs is not None:
                ti = min(t, runs.shape[0] - 1)
                eye = min(v, runs.shape[1] - 1)
                k = runs[ti, eye]
                coded = skip[min(t, skip.shape[0] - 1), eye] == 0
                sel = coded & (k > 1)
            else:
                # No schedule: every frame is scored, at k = 2.  A change that
                # happens on every frame has a fundamental of fps/2 -- one full
                # cycle takes two frames -- so k = 2 is the *fastest* temporal
                # frequency a frame sequence can carry, not a free parameter.
                k = np.full(pop.shape, 2, dtype=np.int64)
                sel = np.ones(pop.shape, dtype=bool)
            if not sel.any():
                continue
            c_m = step_visibility(pop, k, scoring.fps, f_s, geo["ecc_deg"],
                                  st["mean"], p)
            inside = geo["ecc_deg"] <= scoring.fovea_radius_deg
            cm_all.append(c_m[sel])
            cm_fovea.append(c_m[sel & inside])
            cm_periph.append(c_m[sel & ~inside])
            pop_all.append(pop[sel])
            k_all.append(np.asarray(k, dtype=np.float64)[sel])
        prev_ref, prev_dis = rf.y, df.y

    def cat(xs: list[np.ndarray]) -> np.ndarray:
        return np.concatenate(xs) if xs else np.empty(0)

    cm = cat(cm_all)
    out = {
        "mode": "skip-map" if skip is not None else "all-frames",
        "popin_c_m": _distribution(cm, scoring.jnd_threshold),
        "popin_c_m_fovea": _distribution(cat(cm_fovea), scoring.jnd_threshold),
        "popin_c_m_periphery": _distribution(cat(cm_periph), scoring.jnd_threshold),
        "pop_step_codes": _distribution(cat(pop_all), 1.0),
    }
    if cm.size:
        # One scalar for the RD table.  p95 rather than the mean, because 5.3
        # asks for the tail: a schedule is bad when its worst pops are seen,
        # not when its average pop is large.
        out["popin_p95"] = out["popin_c_m"]["p95"]
        out["popin_mean"] = out["popin_c_m"]["mean"]
        out["popin_visible_frac"] = float(
            (visibility(cm, scoring.params) >= 0.5).mean())
        out["mean_k"] = float(cat(k_all).mean())
    return out
