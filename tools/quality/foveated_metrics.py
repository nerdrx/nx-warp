#!/usr/bin/env python3
"""Eccentricity-weighted PSNR and SSIM, per PAPER.md 5.1.2.

PSNR weighs every pixel equally, which PAPER.md 5.3 rightly calls the wrong
tool for a codec that spends 80 percent of its pixels in the periphery on
purpose.  These metrics are the cheap secondary the paper asks for: the same
PSNR and SSIM, but with each pixel weighted by how much detail the eye can
actually resolve at that pixel's eccentricity from the fixation point.

The acuity model is the paper's:

.. math::  ppd_{needed}(e) = \\frac{60}{1 + e/e_2}, \\quad e_2 = 2.3 \\deg

60 ppd is 30 cycles/degree, 20/20 acuity; ``e`` is eccentricity from gaze in
degrees.  The default weight is the normalised acuity ``1/(1 + e/e2)``.

Geometry
--------
Eccentricity is computed exactly, not as a scaled pixel distance.  A VR render
target is a rectilinear (tan) projection, so a pixel ``r`` pixels off the
optical axis sits at ``theta = atan(r/f)`` with ``f = ppd_center * 180/pi``
pixels per radian.  Eccentricity is then the true angle between the ray to the
pixel and the ray to the fixation point, which stays correct when the fixation
is itself off-axis.

A consequence worth stating, because it is easy to get backwards: in a tan
projection the *centre* has the **lowest** angular pixel density, and density
rises towards the edge as ``ppd_render(theta) = ppd_center / cos^2(theta)``
(the paper's formula, which :func:`ppd_render` reproduces and the tests check
against the exact pinhole derivative).  So for a 2160 px, 100 degree view,
``ppd_from_fov(2160, 100)`` returns about 15.8 ppd at the centre, while the
naive "2160/100 = 21.6 ppd" figure is the *average* over the width.  Pass
``mode="average"`` if you want that second number; they are not the same thing
and the difference matters when you set the ppd for these metrics.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nxq import metrics as _m  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402
from nxq.yuv import read_sequence  # noqa: E402

#: The cortical-magnification constant from PAPER.md 5.1.2 (Geisler & Perry 1998).
E2_DEG = 2.3
#: Foveal acuity in pixels per degree: 60 ppd = 30 cycles/deg = 20/20.
PPD_FOVEA = 60.0
#: The paper's s=1 fovea radius including the smooth-pursuit pad (5.1.4).
FOVEA_RADIUS_DEG = 8.0

WEIGHTINGS = ("acuity", "acuity2", "uniform")


def ppd_needed(e_deg: np.ndarray | float, e2: float = E2_DEG) -> np.ndarray | float:
    """Pixels per degree the eye can use at eccentricity *e_deg*."""
    return PPD_FOVEA / (1.0 + np.asarray(e_deg, dtype=np.float64) / e2)


def ppd_render(theta_deg: np.ndarray | float, ppd_center: float) -> np.ndarray | float:
    """Angular pixel density of a rectilinear target at off-axis angle *theta*."""
    c = np.cos(np.radians(np.asarray(theta_deg, dtype=np.float64)))
    return ppd_center / (c * c)


def ppd_from_fov(width_px: int, hfov_deg: float, mode: str = "center") -> float:
    """Pixels per degree for a rectilinear view.

    ``mode="center"`` (default) gives the exact on-axis density,
    ``ppd = f * pi/180`` with ``f = (width/2) / tan(hfov/2)``.
    ``mode="average"`` gives the naive ``width / hfov``.
    """
    if mode == "average":
        return width_px / hfov_deg
    if mode != "center":
        raise ValueError(f"unknown mode {mode!r}, want 'center' or 'average'")
    f = (width_px / 2.0) / math.tan(math.radians(hfov_deg) / 2.0)
    return f * math.pi / 180.0


def eccentricity_map(
    height: int,
    width: int,
    ppd_center: float,
    fixation: tuple[float, float] | None = None,
    center: tuple[float, float] | None = None,
) -> np.ndarray:
    """Eccentricity in degrees for every pixel of a rectilinear view.

    *fixation* and *center* are ``(x, y)`` in pixels; both default to the
    image centre.  *center* is the optical axis of the projection.
    """
    cx, cy = center if center is not None else ((width - 1) / 2.0, (height - 1) / 2.0)
    fx, fy = fixation if fixation is not None else (cx, cy)
    f = ppd_center * 180.0 / math.pi  # pixels per radian

    xs = np.arange(width, dtype=np.float64) - cx
    ys = np.arange(height, dtype=np.float64) - cy
    gx = np.broadcast_to(xs[None, :], (height, width))
    gy = np.broadcast_to(ys[:, None], (height, width))
    norm = np.sqrt(gx * gx + gy * gy + f * f)

    fdx, fdy = fx - cx, fy - cy
    fnorm = math.sqrt(fdx * fdx + fdy * fdy + f * f)
    dot = (gx * fdx + gy * fdy + f * f) / (norm * fnorm)
    return np.degrees(np.arccos(np.clip(dot, -1.0, 1.0)))


def acuity_weights(
    ecc_deg: np.ndarray, weighting: str = "acuity", e2: float = E2_DEG
) -> np.ndarray:
    """Per-pixel weights from an eccentricity map.

    ``acuity``   ``1/(1 + e/e2)`` -- the normalised acuity falloff (default)
    ``acuity2``  its square -- resolvable elements per unit *area*, which is
                 the right power when weighting pixels rather than lines
    ``uniform``  all ones, so the result reduces exactly to plain PSNR/SSIM
    """
    if weighting == "uniform":
        return np.ones_like(ecc_deg)
    w = 1.0 / (1.0 + np.asarray(ecc_deg, dtype=np.float64) / e2)
    if weighting == "acuity":
        return w
    if weighting == "acuity2":
        return w * w
    raise ValueError(f"unknown weighting {weighting!r}, want one of {WEIGHTINGS}")


# --- metrics -------------------------------------------------------------


def foveated_psnr(
    ref: np.ndarray, dis: np.ndarray, weights: np.ndarray, peak: float = 255.0
) -> float:
    """PSNR with a per-pixel weighted MSE."""
    if ref.shape != dis.shape:
        raise ValueError(f"shape mismatch {ref.shape} vs {dis.shape}")
    if weights.shape != ref.shape:
        raise ValueError(f"weight map {weights.shape} does not match image {ref.shape}")
    d = ref.astype(np.float64) - dis.astype(np.float64)
    tot = float(weights.sum())
    if tot <= 0:
        raise ValueError("weights sum to zero")
    wmse = float((weights * d * d).sum() / tot)
    return _m.psnr_from_mse(wmse, peak)


def foveated_ssim(ref: np.ndarray, dis: np.ndarray, weights: np.ndarray) -> float:
    """SSIM averaged with per-pixel weights.

    The SSIM map is computed with a 'valid' 11x11 window, so it is 10 pixels
    smaller in each dimension; the weight map is cropped to match.
    """
    smap = _m.ssim_map(ref, dis)
    k = _m.gaussian_kernel().size
    off = (k - 1) // 2
    w = weights[off : off + smap.shape[0], off : off + smap.shape[1]]
    if w.shape != smap.shape:
        raise ValueError(f"cropped weights {w.shape} do not match SSIM map {smap.shape}")
    tot = float(w.sum())
    if tot <= 0:
        raise ValueError("weights sum to zero")
    return float((w * smap).sum() / tot)


def region_psnr(
    ref: np.ndarray, dis: np.ndarray, ecc_deg: np.ndarray, radius_deg: float = FOVEA_RADIUS_DEG
) -> dict:
    """PSNR inside and outside the fovea disc.

    Phase 4's exit criterion is stated on "the fovea region", so it needs a
    hard region split as well as the smooth weighting.
    """
    d = ref.astype(np.float64) - dis.astype(np.float64)
    sq = d * d
    inside = ecc_deg <= radius_deg
    out: dict = {"fovea_radius_deg": radius_deg,
                 "fovea_pixel_fraction": float(inside.mean())}
    if inside.any():
        out["psnr_fovea"] = _m.psnr_from_mse(float(sq[inside].mean()))
    if (~inside).any():
        out["psnr_periphery"] = _m.psnr_from_mse(float(sq[~inside].mean()))
    return out


def foveated_frame_metrics(
    ref: np.ndarray,
    dis: np.ndarray,
    ppd_center: float,
    fixation: tuple[float, float] | None = None,
    weighting: str = "acuity",
    radius_deg: float = FOVEA_RADIUS_DEG,
    do_ssim: bool = True,
    ecc: np.ndarray | None = None,
) -> dict:
    """All foveated metrics for one luma plane pair.

    Pass a precomputed *ecc* map when the fixation does not move, which saves
    rebuilding it for every frame.
    """
    h, w = ref.shape
    if ecc is None:
        ecc = eccentricity_map(h, w, ppd_center, fixation)
    weights = acuity_weights(ecc, weighting)
    out = {
        "weighting": weighting,
        "ppd_center": ppd_center,
        "fixation": list(fixation) if fixation else [(w - 1) / 2.0, (h - 1) / 2.0],
        "max_eccentricity_deg": float(ecc.max()),
        "fov_psnr_y": foveated_psnr(ref, dis, weights),
        "psnr_y": _m.psnr_plane(ref, dis),
    }
    if do_ssim:
        out["fov_ssim_y"] = foveated_ssim(ref, dis, weights)
        out["ssim_y"] = _m.ssim(ref, dis)
    out.update(region_psnr(ref, dis, ecc, radius_deg))
    return out


# --- CLI -----------------------------------------------------------------


def _views(frame_y: np.ndarray, layout: str) -> list[tuple[str, np.ndarray]]:
    if layout == "sbs":
        half = frame_y.shape[1] // 2
        return [("left", frame_y[:, :half]), ("right", frame_y[:, half:])]
    return [("mono", frame_y)]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="reference sequence (.json sidecar or .yuv)")
    ap.add_argument("--dis", required=True, help="distorted sequence (raw .yuv, same geometry)")
    ap.add_argument("--w", type=int, help="width, if --ref is a raw .yuv")
    ap.add_argument("--h", type=int, help="height, if --ref is a raw .yuv")
    ap.add_argument("--pix", default=None, choices=("yuv444p", "yuv420p"))
    ap.add_argument("--layout", default=None, choices=("sbs", "mono"),
                    help="override the sidecar's stereo layout")
    ap.add_argument("--ppd", type=float, default=None,
                    help="pixels per degree at the optical centre of one view")
    ap.add_argument("--hfov", type=float, default=95.0,
                    help="horizontal FOV of one view, used when --ppd is not given")
    ap.add_argument("--fixation", default=None,
                    help="'x,y' in pixels within one view (default: view centre)")
    ap.add_argument("--gaze-log", default=None,
                    help="JSON list of per-frame {'x':..,'y':..} fixations within one view")
    ap.add_argument("--weighting", default="acuity", choices=WEIGHTINGS)
    ap.add_argument("--fovea-radius", type=float, default=FOVEA_RADIUS_DEG)
    ap.add_argument("--frames", type=int, default=None)
    ap.add_argument("--no-ssim", action="store_true")
    ap.add_argument("--out", default=None, help="write per-frame results as JSON")
    args = ap.parse_args(argv)

    seq = Sequence.open(args.ref, args.w, args.h, args.pix)
    layout = args.layout or seq.layout
    limit = min(seq.frames, args.frames) if args.frames else seq.frames

    view_w = seq.width // 2 if layout == "sbs" else seq.width
    ppd = args.ppd if args.ppd else ppd_from_fov(view_w, args.hfov)
    fixation = None
    if args.fixation:
        fx, fy = (float(v) for v in args.fixation.split(","))
        fixation = (fx, fy)
    gaze = None
    if args.gaze_log:
        with open(args.gaze_log) as fh:
            doc = json.load(fh)
        gaze = doc["frames"] if isinstance(doc, dict) else doc

    print(f"[fov] {seq.name}: {seq.width}x{seq.height} {seq.pix_fmt}, layout {layout}, "
          f"{limit} frames")
    print(f"[fov] view {view_w}px wide, hfov {args.hfov:g} deg -> "
          f"ppd_center {ppd:.2f} (average would be {ppd_from_fov(view_w, args.hfov, 'average'):.2f})")
    print(f"[fov] weighting '{args.weighting}', e2={E2_DEG} deg, "
          f"fovea radius {args.fovea_radius:g} deg")

    ecc_cache: dict = {}
    per_frame = []
    ref_it = read_sequence(seq.path, seq.fmt, limit)
    dis_it = read_sequence(args.dis, seq.fmt, limit)
    for i, (r, d) in enumerate(zip(ref_it, dis_it)):
        fix = fixation
        if gaze and i < len(gaze):
            g = gaze[i]
            fix = (float(g["x"]), float(g["y"]))
        row: dict = {"frame": i, "views": {}}
        for (vname, rv), (_, dv) in zip(_views(r.y, layout), _views(d.y, layout)):
            key = (vname, fix, rv.shape)
            if key not in ecc_cache:
                ecc_cache[key] = eccentricity_map(rv.shape[0], rv.shape[1], ppd, fix)
            row["views"][vname] = foveated_frame_metrics(
                rv, dv, ppd, fix, args.weighting, args.fovea_radius,
                do_ssim=not args.no_ssim, ecc=ecc_cache[key],
            )
        per_frame.append(row)

    if not per_frame:
        print("[fov] no frames compared")
        return 1

    print()
    for vname in per_frame[0]["views"]:
        keys = [k for k in per_frame[0]["views"][vname]
                if isinstance(per_frame[0]["views"][vname][k], float)
                and k not in ("ppd_center", "max_eccentricity_deg", "fovea_radius_deg")]
        print(f"  view {vname} (mean over {len(per_frame)} frames):")
        for k in keys:
            vals = [f["views"][vname][k] for f in per_frame
                    if np.isfinite(f["views"][vname].get(k, np.nan))]
            if vals:
                print(f"    {k:<24} {np.mean(vals):9.4f}")

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        with open(args.out, "w") as fh:
            json.dump({"sequence": seq.name, "ppd_center": ppd, "weighting": args.weighting,
                       "layout": layout, "frames": per_frame}, fh, indent=1)
            fh.write("\n")
        print(f"\n[fov] wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
