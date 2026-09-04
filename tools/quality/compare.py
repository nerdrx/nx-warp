#!/usr/bin/env python3
"""Rate-distortion comparison of the NX Warp codec against x264/x265 anchors.

Runs every codec at a ladder of operating points over one raw YUV sequence,
measures rate and quality at each point, and reports BD-rate/BD-PSNR plus a
direct evaluation of the paper's Phase 1 exit criterion.

Anchors (PAPER.md 3.11 and 2.11 item 1):

``x264-intra``     ``--keyint 1 --tune zerolatency`` -- the Phase 1 gate anchor
``x264-p``         P-only, single reference, zerolatency
``x265-p``         P-only, single reference, zerolatency -- the Phase 2 anchor

The hardware-class baselines (docs/RESEARCH-INDUSTRY.md 2.2 and 1.6): Vulkan
now standardises per-block quantization maps and intra refresh, so a foveated,
IDR-free hardware HEVC streamer is buildable today and is the honest opponent.

``x264-p-refresh`` P-only, periodic intra refresh instead of IDRs, plus a
``x265-p-refresh``   foveated delta-QP map through ``addroi`` -- the software
                   emulation of ``VK_KHR_video_encode_intra_refresh`` plus
                   ``VK_KHR_video_encode_quantization_map`` (see nxq/qpmap.py)
``hevc-vulkan``    real Vulkan video encode through the local driver,
``h264-vulkan``      ultra-low-latency tuning, 4:2:0 8-bit only
``av1-svt-p``      SVT-AV1 low-delay P -- what Virtual Desktop ships

Metrics: PSNR (per plane and (6Y+Cb+Cr)/8 weighted), SSIM, MS-SSIM, all in
numpy, plus VMAF through ffmpeg's libvmaf filter when it is available, plus
eccentricity-weighted PSNR under ``--foveated-psnr`` so the foveated anchors
are compared on the metric they optimise.

Examples
--------
Prove the harness end to end with the mock codec::

    python3 compare.py --seq $NXQ_SCRATCH/seq/vr-mixed-512.yuv444p.json \\
        --codec-enc 'python3 dummy_codec.py enc' \\
        --codec-dec 'python3 dummy_codec.py dec' \\
        --qp 16,22,28,34 --anchor-qp 16,22,28,34 \\
        --out $NXQ_SCRATCH/results/dummy.json

The Phase 1 exit test, once nxv-enc exists::

    python3 compare.py --seq <capture>.yuv444p.json --codec-cmd nxv \\
        --anchors x264-intra --qp 12,18,24,30 --anchor-qp 12,18,24,30 \\
        --phase1-band 100,400 --out results/phase1.json

Results are written as JSON; render them with ``report.py``.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import json
import os
import platform
import shlex
import shutil
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import foveated_metrics as fov  # noqa: E402
from nxq import bdrate, cpu, ffmpeg, fvvdp, latency, metrics, popin, qpmap  # noqa: E402
from nxq.codec import CodecCLI, CodecError  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402
from nxq.yuv import Format, read_sequence, read_pose_log  # noqa: E402

DEFAULT_SCRATCH = "/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp"

#: The perceptual metrics of PAPER.md 5.3, selected with ``--metric``.
#: ``psnr`` and ``ssim`` are always computed; these are the ones that cost
#: enough to be opt-in.
PERCEPTUAL_METRICS = ("fvvdp", "fov-ssim", "popin")

#: Which per-point key each of them contributes to the RD tables.
METRIC_KEYS = {"fvvdp": "fvvdp_jod", "fov-ssim": "fov_ssim_y"}


# --- foveated scoring ----------------------------------------------------


@dataclasses.dataclass
class FoveaScoring:
    """How to compute eccentricity-weighted quality, per foveated_metrics.py.

    The fixation is the view centre, which is where the foveated anchors put
    their low-QP box, so a foveated anchor is scored on the metric it optimises.
    """

    ppd_center: float
    layout: str = "mono"
    weighting: str = "acuity"
    fovea_radius_deg: float = fov.FOVEA_RADIUS_DEG
    hfov_deg: float = 95.0

    def to_json(self) -> dict:
        return {
            "ppd_center": self.ppd_center, "layout": self.layout,
            "weighting": self.weighting, "fovea_radius_deg": self.fovea_radius_deg,
            "hfov_deg": self.hfov_deg, "fixation": "view centre",
            "e2_deg": fov.E2_DEG,
        }


def _fovea_frame(ref_y: np.ndarray, dis_y: np.ndarray, sc: FoveaScoring, cache: dict,
                 do_ssim: bool = False) -> dict:
    """Foveated metrics for one frame, averaged over the views.

    Averaging is on the MSE, not on the dB, because averaging decibels across
    views is not a mean square error of anything.  The eccentricity-weighted
    SSIM of PAPER.md 5.3's "secondary, cheap" metric is combined over the views
    as one weighted mean over both weight maps, which is what a single map over
    a two-eye image would have given.
    """
    acc: dict[str, list[float]] = {}
    ssim_num = ssim_den = 0.0
    for (_, rv), (_, dv) in zip(fov._views(ref_y, sc.layout), fov._views(dis_y, sc.layout)):
        key = (rv.shape, sc.weighting)
        if key not in cache:
            ecc = fov.eccentricity_map(rv.shape[0], rv.shape[1], sc.ppd_center)
            cache[key] = (ecc, fov.acuity_weights(ecc, sc.weighting))
        ecc, w = cache[key]
        d = rv.astype(np.float64) - dv.astype(np.float64)
        sq = d * d
        acc.setdefault("fov_psnr_y", []).append(float((w * sq).sum() / w.sum()))
        inside = ecc <= sc.fovea_radius_deg
        if inside.any():
            acc.setdefault("psnr_fovea", []).append(float(sq[inside].mean()))
        if (~inside).any():
            acc.setdefault("psnr_periphery", []).append(float(sq[~inside].mean()))
        if do_ssim:
            smap = metrics.ssim_map(rv, dv)
            off = (metrics.gaussian_kernel().size - 1) // 2
            wc = w[off : off + smap.shape[0], off : off + smap.shape[1]]
            ssim_num += float((wc * smap).sum())
            ssim_den += float(wc.sum())
    out = {k: metrics.psnr_from_mse(float(np.mean(v))) for k, v in acc.items()}
    if do_ssim and ssim_den > 0:
        out["fov_ssim_y"] = ssim_num / ssim_den
    return out


# --- measurement ---------------------------------------------------------


def bitrate_mbps(size_bytes: int, frames: int, fps: float) -> float:
    """Mean bitrate in Mbit/s for a coded sequence."""
    if frames <= 0:
        return 0.0
    return size_bytes * 8.0 / frames * fps / 1e6


def measure(
    ref_path: str,
    dis_path: str,
    fmt: Format,
    *,
    fps: float,
    do_ssim: bool = True,
    do_vmaf: bool = True,
    limit: int | None = None,
    fovea: FoveaScoring | None = None,
    fov_ssim: bool = False,
    fvvdp_runner: "fvvdp.FvvdpRunner | None" = None,
    popin_scoring: "popin.PopInScoring | None" = None,
    popin_skip: np.ndarray | None = None,
) -> dict:
    """Full-reference metrics between two raw YUV files.

    When *fovea* is given, each frame also gets the eccentricity-weighted PSNR
    of ``foveated_metrics.py`` (``fov_psnr_y``) plus the hard fovea/periphery
    split, so the RD curves can be re-read on the metric a foveated encoder is
    actually optimising, and with *fov_ssim* the eccentricity-weighted SSIM
    PAPER.md 5.3 names as the cheap secondary.

    *fvvdp_runner* adds the JOD score of the paper's primary metric, and
    *popin_scoring* the temporal pop-in distribution; both walk the files
    again, which is why they are opt-in.
    """
    per_frame = []
    ecc_cache: dict = {}
    ref_it = read_sequence(ref_path, fmt, limit)
    dis_it = read_sequence(dis_path, fmt, limit)
    for i, (r, d) in enumerate(zip(ref_it, dis_it)):
        m = metrics.frame_metrics(r, d, do_ssim=do_ssim, do_ms_ssim=do_ssim)
        if fovea is not None:
            m.update(_fovea_frame(r.y, d.y, fovea, ecc_cache, do_ssim=fov_ssim))
        m["frame"] = i
        per_frame.append(m)
    if not per_frame:
        raise RuntimeError(f"no frames compared between {ref_path} and {dis_path}")
    out = metrics.average_metrics([{k: v for k, v in f.items() if k != "frame"} for f in per_frame])
    out["per_frame"] = per_frame
    if do_vmaf:
        v = ffmpeg.vmaf(ref_path, dis_path, fmt, fps)
        if v is not None:
            out["vmaf"] = v
    if fvvdp_runner is not None:
        t0 = time.time()
        res = fvvdp_runner.score(ref_path, dis_path, fmt, limit=limit)
        res["wall_s"] = time.time() - t0
        out["fvvdp"] = res
        out["fvvdp_jod"] = res["jod"]
    if popin_scoring is not None:
        out["popin"] = popin.score_sequence(
            read_sequence(ref_path, fmt, limit), read_sequence(dis_path, fmt, limit),
            popin_scoring, skip=popin_skip,
        )
        for k in ("popin_p95", "popin_mean", "popin_visible_frac"):
            if k in out["popin"]:
                out[k] = out["popin"][k]
    return out


def _point_summary(p: dict) -> str:
    bits = f"{p['bitrate_mbps']:8.1f} Mbit/s"
    psnr = f"PSNR-Y {p['psnr_y']:6.2f} dB"
    extra = ""
    if "ssim_y" in p:
        extra += f"  SSIM {p['ssim_y']:.4f}"
    if "vmaf" in p:
        extra += f"  VMAF {p['vmaf']:5.1f}"
    if "fov_ssim_y" in p:
        extra += f"  fovSSIM {p['fov_ssim_y']:.4f}"
    if "fvvdp_jod" in p:
        extra += f"  JOD {p['fvvdp_jod']:6.3f}"
    if "popin_p95" in p:
        extra += f"  pop p95 {p['popin_p95']:6.3f}"
    return f"{bits}  {psnr}{extra}"


# --- codec runners -------------------------------------------------------


def run_anchor(
    anchor: ffmpeg.Anchor,
    seq: Sequence,
    points: list[int],
    rc: str,
    work: str,
    *,
    do_ssim: bool,
    do_vmaf: bool,
    frames: int,
    verbose: bool = True,
    fovea_map: qpmap.FoveaMap | None = None,
    fovea_scoring: FoveaScoring | None = None,
    extra: dict | None = None,
) -> dict:
    ok, why = ffmpeg.anchor_available(anchor, seq.fmt)
    if not ok:
        print(f"[compare] SKIP anchor {anchor.name}: {why}", flush=True)
        return {"kind": "anchor", "available": False, "reason": why, "points": []}

    # An anchor that cannot honour the run's rate-control mode falls back to
    # its own, and says so rather than pretending.
    eff_rc = ffmpeg.anchor_rc(anchor, rc)
    out = {"kind": "anchor", "available": True, "encoder": anchor.encoder,
           "backend": anchor.backend, "note": anchor.note,
           "rate_control": eff_rc, "requested_rate_control": rc,
           "intra_refresh": anchor.intra_refresh, "foveated": anchor.foveated,
           "points": []}
    if eff_rc != rc:
        out["rate_control_note"] = (
            f"the run asked for {rc}, but {anchor.name} supports "
            f"{'/'.join(anchor.rc_modes)}; the same numbers were used as {eff_rc}"
        )
        print(f"[compare]   note: {out['rate_control_note']}", flush=True)
    if anchor.intra_refresh:
        out["refresh_period_frames"] = anchor.period(frames)
    if anchor.foveated:
        m = fovea_map or qpmap.FoveaMap.default()
        out["fovea_map"] = m.to_json()
        out["fovea_map_regions"] = [
            {"x": x, "y": y, "w": w, "h": h, "qp_delta": d}
            for x, y, w, h, d in m.regions(seq.width, seq.height, seq.layout)
        ]

    for pt in points:
        tag = f"{anchor.name}-{eff_rc}{pt}"
        bs = os.path.join(work, f"{tag}.{anchor.raw_fmt}")
        rec = os.path.join(work, f"{tag}.yuv")
        t0 = time.time()
        try:
            kwargs = {"qp": pt} if eff_rc == "qp" else {"crf": pt}
            enc = ffmpeg.encode_anchor(
                anchor, seq.path, seq.fmt, bs, fps=seq.fps, nframes=frames,
                fovea=fovea_map, layout=seq.layout, **kwargs
            )
        except ffmpeg.FFmpegError as exc:
            print(f"[compare]   {tag}: FAILED -- {exc}", flush=True)
            continue
        enc_s = time.time() - t0
        t1 = time.time()
        try:
            ffmpeg.decode_bitstream(bs, seq.fmt, rec)
        except ffmpeg.FFmpegError as exc:
            print(f"[compare]   {tag}: DECODE FAILED -- {exc}", flush=True)
            continue
        dec_s = time.time() - t1
        m = measure(seq.path, rec, seq.fmt, fps=seq.fps, do_ssim=do_ssim,
                    do_vmaf=do_vmaf, limit=frames, fovea=fovea_scoring,
                    **(extra or {}))
        point = {eff_rc: pt, "bytes": enc.size,
                 "bitrate_mbps": bitrate_mbps(enc.size, frames, seq.fps),
                 "wall_s": enc_s + dec_s,
                 "encode_ms_per_frame": enc_s * 1000.0 / max(frames, 1),
                 "decode_ms_per_frame": dec_s * 1000.0 / max(frames, 1),
                 "cmd": enc.cmdline, **m}
        out["points"].append(point)
        if verbose:
            print(f"[compare]   {anchor.name} {eff_rc}={pt:<3} {_point_summary(point)}", flush=True)
        for f in (bs, rec):
            if os.path.exists(f):
                os.remove(f)
    if out["points"]:
        out["cmd"] = out["points"][0]["cmd"]
    return out


def run_codec(
    cli: CodecCLI,
    seq: Sequence,
    qps: list[int],
    work: str,
    *,
    do_ssim: bool,
    do_vmaf: bool,
    frames: int,
    verbose: bool = True,
    fovea_scoring: FoveaScoring | None = None,
    extra: dict | None = None,
) -> dict:
    ok, why = cli.available()
    if not ok:
        print(f"[compare] SKIP codec {cli.name}: {why}", flush=True)
        return {"kind": "codec", "available": False, "reason": why, "points": []}

    out = {"kind": "codec", "available": True, "enc": cli.enc, "dec": cli.dec,
           "rate_control": "qp", "points": []}
    # The codec CLIs take a whole file, not a frame count, so `--frames N` on a
    # longer sequence has to truncate the source: encoding all of it and then
    # dividing its bytes by N reports N/seq.frames of the real bitrate, which
    # is silent and wrong.  The anchors get their frame count through ffmpeg.
    src = seq.path
    if frames and frames < seq.frames:
        src = os.path.join(work, "src-%dframes.yuv" % frames)
        if not os.path.exists(src):
            with open(seq.path, "rb") as fi, open(src, "wb") as fo:
                fo.write(fi.read(seq.fmt.frame_bytes * frames))
    for qp in qps:
        tag = f"{cli.name}-qp{qp}"
        bs = os.path.join(work, f"{tag}.nxv")
        rec = os.path.join(work, f"{tag}.yuv")
        t0 = time.time()
        try:
            size = cli.encode(src, seq.fmt, qp, bs)
            enc_s = time.time() - t0
            t1 = time.time()
            cli.decode(bs, rec)
            dec_s = time.time() - t1
        except CodecError as exc:
            print(f"[compare]   {tag}: FAILED -- {exc}", flush=True)
            continue
        try:
            m = measure(src, rec, seq.fmt, fps=seq.fps, do_ssim=do_ssim,
                        do_vmaf=do_vmaf, limit=frames, fovea=fovea_scoring,
                        **(extra or {}))
        except (EOFError, ValueError) as exc:
            print(f"[compare]   {tag}: decoded output does not match the source geometry: {exc}",
                  flush=True)
            continue
        point = {"qp": qp, "bytes": size, "bitrate_mbps": bitrate_mbps(size, frames, seq.fps),
                 "wall_s": enc_s + dec_s,
                 "encode_ms_per_frame": enc_s * 1000.0 / max(frames, 1),
                 "decode_ms_per_frame": dec_s * 1000.0 / max(frames, 1),
                 "cmd": " ".join(shlex.quote(a) for a in
                                 cpu.wrap(cli.encode_argv(src, seq.fmt, qp, bs))),
                 **m}
        out["points"].append(point)
        if verbose:
            print(f"[compare]   {cli.name} qp={qp:<3} {_point_summary(point)}", flush=True)
        for f in (bs, rec):
            if os.path.exists(f):
                os.remove(f)
    return out


# --- analysis ------------------------------------------------------------


def curve(points: list[dict], metric: str) -> tuple[list[float], list[float]]:
    """(rates, distortions) for the points that have a finite value of *metric*."""
    r, d = [], []
    for p in points:
        v = p.get(metric)
        if v is None or not np.isfinite(v) or p["bitrate_mbps"] <= 0:
            continue
        r.append(p["bitrate_mbps"])
        d.append(float(v))
    return r, d


def bd_table(results: dict, codec_key: str, metric: str = "psnr_y") -> dict:
    """BD-rate/BD-PSNR of the codec against every available anchor."""
    out = {}
    cr, cd = curve(results["codecs"].get(codec_key, {}).get("points", []), metric)
    for name, entry in results["codecs"].items():
        if name == codec_key or entry.get("kind") != "anchor":
            continue
        ar, ad = curve(entry.get("points", []), metric)
        if len(ar) < 4 or len(cr) < 4:
            out[name] = {"error": f"need at least 4 points per curve "
                                  f"(anchor has {len(ar)}, codec has {len(cr)})"}
            continue
        out[name] = bdrate.bd_summary(ar, ad, cr, cd)
    return out


def phase1_gate(
    results: dict, codec_key: str, anchor_key: str = "x264-intra",
    band: tuple[float, float] = (100.0, 400.0), tolerance_db: float = 1.0,
    metric: str = "psnr_y",
) -> dict:
    """Evaluate the Phase 1 exit criterion directly.

    PAPER.md 3.11: "within 1.0 dB PSNR of x264 intra (--keyint 1, zerolatency)
    at 100 to 400 Mbit on VR captures".

    Both curves are interpolated in log-rate against PSNR over the part of the
    100-400 Mbit band that both actually cover, and the worst (most negative)
    PSNR difference in that band is compared against the tolerance.
    """
    res: dict = {
        "anchor": anchor_key, "codec": codec_key, "band_mbps": list(band),
        "tolerance_db": tolerance_db, "metric": metric,
    }
    anchor = results["codecs"].get(anchor_key, {})
    codec = results["codecs"].get(codec_key, {})
    ar, ad = curve(anchor.get("points", []), metric)
    cr, cd = curve(codec.get("points", []), metric)
    if len(ar) < 2 or len(cr) < 2:
        res["error"] = f"not enough points (anchor {len(ar)}, codec {len(cr)})"
        return res

    lo = max(min(ar), min(cr), band[0])
    hi = min(max(ar), max(cr), band[1])
    res["covered_mbps"] = [lo, hi]
    if hi <= lo:
        res["error"] = (
            f"the {band[0]:.0f}-{band[1]:.0f} Mbit band is not covered by both curves "
            f"(anchor spans {min(ar):.1f}-{max(ar):.1f}, codec spans {min(cr):.1f}-{max(cr):.1f} "
            "Mbit/s); choose QP points that land in the band"
        )
        return res

    def interp(rates, dists, xs):
        order = np.argsort(rates)
        return np.interp(xs, np.log10(np.asarray(rates)[order]), np.asarray(dists)[order])

    xs = np.linspace(np.log10(lo), np.log10(hi), 64)
    diff = interp(cr, cd, xs) - interp(ar, ad, xs)
    res["worst_delta_db"] = float(diff.min())
    res["mean_delta_db"] = float(diff.mean())
    res["best_delta_db"] = float(diff.max())
    res["worst_at_mbps"] = float(10 ** xs[int(np.argmin(diff))])
    res["pass"] = bool(diff.min() >= -tolerance_db)
    return res


def velocity_split(results: dict, poses: list[dict], pct: float = 20.0) -> dict:
    """Split per-frame metrics into high- and low-angular-velocity subsets.

    PAPER.md 2.11 item 1 asks for BD-rate "overall and on the 20 percent of
    frames with the highest angular velocity".  This computes the mean PSNR of
    each subset per operating point; feed the result to the same BD machinery
    once there are enough points.
    """
    av = np.array([p.get("angular_velocity_deg_s", 0.0) for p in poses], dtype=np.float64)
    if av.size == 0:
        return {"error": "pose log has no frames"}
    thresh = float(np.percentile(av, 100.0 - pct))
    high = set(np.where(av >= thresh)[0].tolist())
    out: dict = {
        "percentile": pct, "threshold_deg_s": thresh,
        "high_frames": len(high), "total_frames": int(av.size), "codecs": {},
    }
    for name, entry in results["codecs"].items():
        pts = []
        for p in entry.get("points", []):
            pf = p.get("per_frame") or []
            if not pf:
                continue
            hi = [f["psnr_y"] for f in pf if f["frame"] in high and np.isfinite(f["psnr_y"])]
            lo = [f["psnr_y"] for f in pf if f["frame"] not in high and np.isfinite(f["psnr_y"])]
            pts.append({
                "rate_control": p.get("qp", p.get("crf")),
                "bitrate_mbps": p["bitrate_mbps"],
                "psnr_y_high_velocity": float(np.mean(hi)) if hi else None,
                "psnr_y_low_velocity": float(np.mean(lo)) if lo else None,
            })
        if pts:
            out["codecs"][name] = pts
    return out


# --- main ----------------------------------------------------------------


def parse_points(s: str) -> list[int]:
    return [int(x) for x in s.replace(" ", "").split(",") if x]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_argument_group("input")
    src.add_argument("--seq", help="sequence .json sidecar (preferred)")
    src.add_argument("--in", dest="raw", help="raw .yuv file (then --w/--h/--pix are required)")
    src.add_argument("--w", type=int)
    src.add_argument("--h", type=int)
    src.add_argument("--pix", default=None, choices=("yuv444p", "yuv420p"))
    src.add_argument("--fps", type=float, default=None, help="playback rate used to turn bytes into Mbit/s")
    src.add_argument("--frames", type=int, default=None, help="only compare the first N frames")

    cod = ap.add_argument_group("codec under test")
    cod.add_argument("--codec-cmd", default=None,
                     help="prefix for the codec CLIs; '-enc'/'-dec' are appended (default: nxv)")
    cod.add_argument("--codec-enc", default=None, help="explicit encoder command line")
    cod.add_argument("--codec-dec", default=None, help="explicit decoder command line")
    cod.add_argument("--codec-name", default=None, help="label for the codec in the report")
    cod.add_argument("--qp", default="16,22,28,34", help="codec QP operating points")

    anc = ap.add_argument_group("anchors")
    anc.add_argument("--anchors", default="x264-intra,x265-p",
                     help=f"comma-separated, from {sorted(ffmpeg.ANCHORS)}")
    anc.add_argument("--anchor-qp", default=None, help="anchor QP points (constant-QP mode)")
    anc.add_argument("--anchor-crf", default=None, help="anchor CRF points instead of QP")
    anc.add_argument("--preset", default="medium", help="x264/x265 preset")
    anc.add_argument("--intra-refresh-period", type=int, default=0,
                     help="refresh sweep length in frames for the *-p-refresh anchors "
                          "(default 0: one sweep across the clip)")
    anc.add_argument("--fovea-map", default=None, metavar="SPEC",
                     help="foveated delta-QP map for the *-p-refresh anchors, as "
                          "'center=0.25:mid=0.55:dc=-6:dm=0:dp=6' (fractions of a view, "
                          "then QP deltas). Emulates VK_KHR_video_encode_quantization_map; "
                          "see nxq/qpmap.py")

    an = ap.add_argument_group("analysis")
    an.add_argument("--metric", default="", metavar="LIST",
                    help="perceptual metrics of PAPER.md 5.3 to add, comma-separated "
                         f"from {list(PERCEPTUAL_METRICS)}: 'fvvdp' is FovVideoVDP in a "
                         "headset display model (JOD per point, BD-rate on JOD), "
                         "'fov-ssim' the eccentricity-weighted SSIM, 'popin' the "
                         "temporal tile-refresh metric. PSNR and SSIM are always computed")
    an.add_argument("--foveated-psnr", action="store_true",
                    help="also report every RD curve as eccentricity-weighted PSNR "
                         "(foveated_metrics.py) with a centre fixation, so the foveated "
                         "anchors are compared on the metric they optimise")
    an.add_argument("--fov-hfov", type=float, default=95.0,
                    help="horizontal FOV of one view, for --foveated-psnr")
    an.add_argument("--fov-ppd", type=float, default=None,
                    help="pixels per degree at the view's optical centre (overrides --fov-hfov)")
    an.add_argument("--fov-weighting", default="acuity", choices=fov.WEIGHTINGS)
    an.add_argument("--fov-radius", type=float, default=fov.FOVEA_RADIUS_DEG,
                    help="fovea disc radius in degrees for the hard region split")
    an.add_argument("--phase1-anchor", default=ffmpeg.PHASE1_ANCHOR)
    an.add_argument("--phase1-band", default="100,400", help="Mbit/s band for the Phase 1 gate")
    an.add_argument("--phase1-tolerance", type=float, default=1.0, help="dB")
    an.add_argument("--velocity-pct", type=float, default=20.0,
                    help="top percent of frames by angular velocity for the Phase 2 split")
    an.add_argument("--no-ssim", action="store_true", help="skip SSIM/MS-SSIM (faster)")
    an.add_argument("--no-vmaf", action="store_true", help="skip VMAF even if libvmaf exists")

    vd = ap.add_argument_group("FovVideoVDP (--metric fvvdp)")
    fvvdp.add_arguments(vd)

    pi = ap.add_argument_group("pop-in (--metric popin)")
    pi.add_argument("--popin-skip-map", default=None, metavar="FILE",
                    help="the per-tile skip map that was handed to the encoder "
                         "(nxv-enc --skip-map); without it the metric scores every "
                         "frame and reports mode 'all-frames'")
    pi.add_argument("--popin-jnd", type=float, default=1.0,
                    help="JND threshold a pop must exceed to count as visible (default 1.0)")
    pi.add_argument("--write-skip-map", default=None, metavar="FILE",
                    help="write a temporal-ladder skip map for this sequence and exit; "
                         "feed it to nxv-enc --skip-map and back in via --popin-skip-map")
    pi.add_argument("--ladder", default="11223", choices=sorted(popin.LADDERS),
                    help="which refresh ladder --write-skip-map builds (default 11223, "
                         "Floeter et al.'s tolerated operating point)")

    lat = ap.add_argument_group("latency")
    lat.add_argument("--link", default="wifi6", choices=("wifi6", "usb"),
                     help="which PAPER.md 4.2 pipeline budget to report (default wifi6)")

    out = ap.add_argument_group("output")
    out.add_argument("--out", default=None, help="results JSON path")
    out.add_argument("--work", default=None, help="scratch directory for intermediate files")
    out.add_argument("--keep-work", action="store_true")
    out.add_argument("--probe", action="store_true", help="print machine capabilities and exit")

    args = ap.parse_args(argv)

    caps = ffmpeg.probe()
    if args.probe:
        print(caps.describe())
        print("anchors:")
        for name, a in sorted(ffmpeg.ANCHORS.items()):
            ok, why = ffmpeg.anchor_available(a)
            print(f"  {name:<15} {'available' if ok else 'UNAVAILABLE -- ' + why}")
        print(f"CPU discipline : {' '.join(cpu.prefix()) or 'DISABLED'} (ffmpeg -threads {cpu.threads()})")
        cli = CodecCLI.from_args(args.codec_cmd, args.codec_enc, args.codec_dec, args.codec_name)
        ok, why = cli.available()
        print(f"codec          : {'ready' if ok else 'unavailable -- ' + why}")
        print(f"                 enc: {' '.join(cli.enc)}\n                 dec: {' '.join(cli.dec)}")
        return 0

    if not args.seq and not args.raw:
        ap.error("give --seq (a sequence .json) or --in (a raw .yuv with --w/--h/--pix)")
    seq = Sequence.open(args.seq or args.raw, args.w, args.h, args.pix, args.fps)
    frames = min(seq.frames, args.frames) if args.frames else seq.frames
    if frames <= 0:
        ap.error("sequence has no frames")
    if frames < seq.frames:
        # --frames must limit the ENCODE as well as the metric window; otherwise
        # every bitrate is inflated by seq.frames / frames. Encode a truncated copy.
        trunc_dir = tempfile.mkdtemp(prefix="cmp-trunc-", dir=os.environ.get("NXQ_SCRATCH") or None)
        trunc_path = os.path.join(trunc_dir, f"{seq.name}.first{frames}.yuv")
        fb = seq.fmt.frame_bytes
        with open(seq.path, "rb") as src, open(trunc_path, "wb") as dst:
            remaining = fb * frames
            while remaining:
                chunk = src.read(min(remaining, 1 << 24))
                if not chunk:
                    break
                dst.write(chunk)
                remaining -= len(chunk)
        seq = dataclasses.replace(seq, path=trunc_path, frames=frames)
        print(f"[compare] --frames {frames}: encoding a truncated copy so bitrates are honest", flush=True)

    if not caps.available:
        print("[compare] WARNING: ffmpeg not found -- no anchors, no VMAF. "
              "Install ffmpeg with libx264/libx265 for a meaningful comparison.", flush=True)

    do_vmaf = (not args.no_vmaf) and caps.has_vmaf
    if not args.no_vmaf and not caps.has_vmaf:
        print("[compare] note: this ffmpeg has no libvmaf filter; VMAF will be omitted", flush=True)

    cli = CodecCLI.from_args(args.codec_cmd, args.codec_enc, args.codec_dec, args.codec_name)
    codec_key = cli.name

    if args.anchor_qp and args.anchor_crf:
        ap.error("give --anchor-qp or --anchor-crf, not both")
    if args.anchor_crf:
        anchor_rc, anchor_points = "crf", parse_points(args.anchor_crf)
    else:
        anchor_rc, anchor_points = "qp", parse_points(args.anchor_qp or args.qp)
    qps = parse_points(args.qp)

    try:
        fovea_map = qpmap.FoveaMap.parse(args.fovea_map)
    except ValueError as exc:
        ap.error(f"--fovea-map: {exc}")

    wanted = [m.strip() for m in args.metric.split(",") if m.strip()]
    for m in wanted:
        if m not in PERCEPTUAL_METRICS:
            ap.error(f"--metric {m!r}: choose from {list(PERCEPTUAL_METRICS)}")
    want_fvvdp = "fvvdp" in wanted
    want_fov_ssim = "fov-ssim" in wanted
    want_popin = "popin" in wanted

    view_w = seq.width // 2 if seq.layout == "sbs" else seq.width
    fovea_scoring = None
    if args.foveated_psnr or want_fov_ssim:
        ppd = args.fov_ppd or fov.ppd_from_fov(view_w, args.fov_hfov)
        fovea_scoring = FoveaScoring(
            ppd_center=ppd, layout=seq.layout, weighting=args.fov_weighting,
            fovea_radius_deg=args.fov_radius, hfov_deg=args.fov_hfov,
        )

    # The temporal-ladder skip map is a sequence property, not a codec one, so
    # it is written here and then given to every encoder run by hand.
    if args.write_skip_map:
        ppd = args.fov_ppd or fov.ppd_from_fov(view_w, args.fov_hfov)
        geo = popin.tile_geometry(seq.height, view_w, ppd, popin.TILE_SIZE)
        eyes = 2 if seq.layout == "sbs" else 1
        ecc = geo["ecc_deg"]
        sched = popin.SkipSchedule.ladder(frames, eyes, ecc,
                                          rings=popin.LADDERS[args.ladder])
        sched.write(args.write_skip_map)
        skipped = float((sched.flags != 0).mean())
        print(f"[compare] wrote {args.write_skip_map}: {frames} frames x "
              f"{ecc.shape[0]}x{ecc.shape[1]} tiles ({eyes} eye(s)), ladder "
              f"{args.ladder}, {100.0 * skipped:.1f}% of tile-frames forced to WARP_SKIP",
              flush=True)
        return 0

    fvvdp_runner = None
    if want_fvvdp:
        try:
            fvvdp_runner = fvvdp.FvvdpRunner(
                fvvdp.scoring_from_args(args, seq.width, seq.layout), seq.fps)
        except RuntimeError as exc:
            ap.error(f"--metric fvvdp: {exc}")
        print(f"[compare] fvvdp   : {fvvdp_runner.describe(view_w, seq.height)}", flush=True)

    popin_scoring = popin_skip = None
    if want_popin:
        ppd = args.fov_ppd or fov.ppd_from_fov(view_w, args.fov_hfov)
        popin_scoring = popin.PopInScoring(
            ppd_center=ppd, fps=seq.fps, layout=seq.layout,
            fovea_radius_deg=args.fov_radius, jnd_threshold=args.popin_jnd,
        )
        if args.popin_skip_map:
            eyes = 2 if seq.layout == "sbs" else 1
            ty = (seq.height + popin.TILE_SIZE - 1) // popin.TILE_SIZE
            tx = (view_w + popin.TILE_SIZE - 1) // popin.TILE_SIZE
            popin_skip = popin.SkipSchedule.read(args.popin_skip_map, eyes, ty, tx).flags
        print(f"[compare] pop-in  : ppd_center {ppd:.2f}, {popin.TILE_SIZE}px tiles, "
              f"JND threshold {args.popin_jnd:g}, "
              f"{'skip map ' + args.popin_skip_map if args.popin_skip_map else 'all frames'}",
              flush=True)

    measure_extra = {
        "fov_ssim": want_fov_ssim,
        "fvvdp_runner": fvvdp_runner,
        "popin_scoring": popin_scoring,
        "popin_skip": popin_skip,
    }

    work = args.work or os.path.join(DEFAULT_SCRATCH, "work")
    os.makedirs(work, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix="cmp-", dir=work)

    print(f"[compare] sequence : {seq.name}  {seq.width}x{seq.height} {seq.pix_fmt} "
          f"{frames} frames @ {seq.fps:g} fps", flush=True)
    print(f"[compare] source   : {seq.source}", flush=True)
    print(f"[compare] work dir : {workdir}", flush=True)
    print(f"[compare] {caps.describe()}".replace("\n", "\n[compare] "), flush=True)
    print(f"[compare] fovea map: {fovea_map.describe()}", flush=True)
    if fovea_scoring:
        print(f"[compare] foveated PSNR: ppd_center {fovea_scoring.ppd_center:.2f} "
              f"(hfov {args.fov_hfov:g} deg over a {seq.width // 2 if seq.layout == 'sbs' else seq.width} px view), "
              f"weighting '{fovea_scoring.weighting}', fovea radius "
              f"{fovea_scoring.fovea_radius_deg:g} deg, fixation at the view centre", flush=True)

    results: dict = {
        "schema": 1,
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "sequence": {
            "name": seq.name, "path": seq.path, "width": seq.width, "height": seq.height,
            "pix_fmt": seq.pix_fmt, "fps": seq.fps, "frames": frames, "source": seq.source,
            "layout": seq.layout, "pose_log": seq.pose_log,
        },
        "machine": {
            "host": platform.node(), "platform": platform.platform(),
            "ffmpeg": caps.version if caps.available else None,
            "encoders": [n for n in ffmpeg.ANCHOR_ENCODERS if n in caps.encoders],
            "vmaf": caps.has_vmaf, "cpu_prefix": cpu.prefix(), "ffmpeg_threads": cpu.threads(),
        },
        "codec_key": codec_key,
        "fovea_map": fovea_map.to_json(),
        "foveated_scoring": fovea_scoring.to_json() if fovea_scoring else None,
        "perceptual_metrics": wanted,
        "fvvdp_scoring": (
            dict(fvvdp_runner.scoring.to_json(view_w, seq.height),
                 pyfvvdp_version=fvvdp_runner.version, device=fvvdp_runner.device_name)
            if fvvdp_runner else None),
        "popin_scoring": (
            dict(popin_scoring.to_json(), skip_map=args.popin_skip_map)
            if popin_scoring else None),
        "codecs": {},
    }

    t_all = time.time()
    try:
        for name in [a.strip() for a in args.anchors.split(",") if a.strip()]:
            if name not in ffmpeg.ANCHORS:
                print(f"[compare] unknown anchor {name!r}, skipping "
                      f"(known: {sorted(ffmpeg.ANCHORS)})", flush=True)
                continue
            anchor = ffmpeg.ANCHORS[name]
            over = {}
            if args.preset != anchor.preset and anchor.backend == "sw":
                over["preset"] = args.preset
            if args.intra_refresh_period and anchor.intra_refresh:
                over["refresh_period"] = args.intra_refresh_period
            if over:
                anchor = dataclasses.replace(anchor, **over)
            print(f"[compare] anchor {name} ({anchor.encoder}, "
                  f"{ffmpeg.anchor_rc(anchor, anchor_rc)} {anchor_points}) ...", flush=True)
            results["codecs"][name] = run_anchor(
                anchor, seq, anchor_points, anchor_rc, workdir,
                do_ssim=not args.no_ssim, do_vmaf=do_vmaf, frames=frames,
                fovea_map=fovea_map, fovea_scoring=fovea_scoring, extra=measure_extra,
            )

        print(f"[compare] codec {codec_key} (qp {qps}) ...", flush=True)
        results["codecs"][codec_key] = run_codec(
            cli, seq, qps, workdir, do_ssim=not args.no_ssim, do_vmaf=do_vmaf, frames=frames,
            fovea_scoring=fovea_scoring, extra=measure_extra,
        )
    finally:
        if not args.keep_work:
            shutil.rmtree(workdir, ignore_errors=True)

    results["elapsed_s"] = time.time() - t_all

    # --- analysis
    results["bd_rate"] = {"psnr_y": bd_table(results, codec_key, "psnr_y")}
    if not args.no_ssim:
        results["bd_rate"]["ssim_y"] = bd_table(results, codec_key, "ssim_y")
    if fovea_scoring:
        for m in ("fov_psnr_y", "psnr_fovea", "psnr_periphery"):
            results["bd_rate"][m] = bd_table(results, codec_key, m)
    for name in wanted:
        key = METRIC_KEYS.get(name)
        if key:
            results["bd_rate"][key] = bd_table(results, codec_key, key)

    # PAPER.md 5.3: latency is a quality metric and is reported next to every
    # quality number, even when the harness can only model it.
    budget = latency.LatencyBudget.usb() if args.link == "usb" else latency.LatencyBudget()
    results["motion_to_photon"] = {}
    for name, entry in results["codecs"].items():
        pts = entry.get("points", [])
        enc = [p["encode_ms_per_frame"] for p in pts if "encode_ms_per_frame" in p]
        dec = [p["decode_ms_per_frame"] for p in pts if "decode_ms_per_frame" in p]
        results["motion_to_photon"][name] = latency.motion_to_photon(
            budget,
            encode_ms_per_frame=float(np.mean(enc)) if enc else None,
            decode_ms_per_frame=float(np.mean(dec)) if dec else None,
            fps=seq.fps,
        )

    band = tuple(float(x) for x in args.phase1_band.split(","))  # type: ignore[assignment]
    results["phase1"] = phase1_gate(
        results, codec_key, args.phase1_anchor, band, args.phase1_tolerance
    )

    if seq.pose_log and os.path.exists(seq.pose_log):
        try:
            poses = read_pose_log(seq.pose_log)[:frames]
            results["velocity_split"] = velocity_split(results, poses, args.velocity_pct)
        except (OSError, ValueError, KeyError) as exc:
            results["velocity_split"] = {"error": f"could not read pose log: {exc}"}

    out_path = args.out or os.path.join(DEFAULT_SCRATCH, "results", f"{seq.name}.json")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w") as fh:
        json.dump(results, fh, indent=1)
        fh.write("\n")

    print(f"\n[compare] elapsed {results['elapsed_s']:.1f}s -> {out_path}", flush=True)
    _print_summary(results)
    print(f"\n[compare] render it with:  python3 report.py --results {out_path}", flush=True)
    return 0


def _print_summary(results: dict) -> None:
    ck = results["codec_key"]
    print(f"\n  BD-rate of {ck} on PSNR-Y (negative is better):")
    for anchor, bd in results.get("bd_rate", {}).get("psnr_y", {}).items():
        if "error" in bd:
            print(f"    vs {anchor:<12} n/a ({bd['error']})")
            continue
        rate = f"{bd['bd_rate_pct']:+8.2f} %" if "bd_rate_pct" in bd else "     n/a"
        psnr = f"{bd['bd_psnr_db']:+6.3f} dB" if "bd_psnr_db" in bd else "   n/a"
        span = (f"(overlap {bd['overlap_lo']:.2f}-{bd['overlap_hi']:.2f} dB)"
                if "overlap_lo" in bd else "")
        print(f"    vs {anchor:<12} {rate}   BD-PSNR {psnr}   {span}")
        for key, label in (("bd_rate_error", "BD-rate"), ("bd_psnr_error", "BD-PSNR")):
            if key in bd:
                print(f"        {label} unavailable: {bd[key]}")
    for m, label in (("fov_psnr_y", "eccentricity-weighted PSNR-Y"),
                     ("psnr_fovea", "PSNR-Y inside the fovea disc"),
                     ("psnr_periphery", "PSNR-Y in the periphery"),
                     ("fov_ssim_y", "eccentricity-weighted SSIM (PAPER.md 5.3)"),
                     ("fvvdp_jod", "FovVideoVDP JOD (PAPER.md 5.3 primary)")):
        table = results.get("bd_rate", {}).get(m)
        if not table:
            continue
        unit = {"fvvdp_jod": "JOD", "fov_ssim_y": "SSIM"}.get(m, "dB")
        print(f"\n  BD-rate of {ck} on {label} (negative is better):")
        for anchor, bd in table.items():
            if "error" in bd:
                print(f"    vs {anchor:<15} n/a ({bd['error']})")
                continue
            rate = f"{bd['bd_rate_pct']:+8.2f} %" if "bd_rate_pct" in bd else "     n/a"
            dq = (f"{bd['bd_psnr_db']:+7.4f} {unit}" if "bd_psnr_db" in bd
                  else f"   n/a {unit}")
            print(f"    vs {anchor:<15} {rate}   BD-quality {dq}")

    popin_rows = [
        (name, p) for name, entry in results.get("codecs", {}).items()
        for p in entry.get("points", []) if "popin" in p
    ]
    if popin_rows:
        mode = popin_rows[0][1]["popin"]["mode"]
        print(f"\n  Tile pop-in, C_M in JND units, mode '{mode}' "
              "(PAPER.md 5.3, model docs/RATECONTROL.md 8.2):")
        print(f"    {'codec':<16}{'rate':>9}  {'mean':>7}{'p95':>8}{'max':>8}"
              f"{'>1 JND':>9}{'fovea p95':>11}")
        for name, p in popin_rows:
            d = p["popin"]["popin_c_m"]
            f = p["popin"]["popin_c_m_fovea"]
            if not d.get("events"):
                continue
            fovea = f"{f['p95']:11.3f}" if f.get("events") else f"{'no tiles':>11}"
            print(f"    {name:<16}{p['bitrate_mbps']:9.1f}  {d['mean']:7.3f}{d['p95']:8.3f}"
                  f"{d['max']:8.3f}{d['over_threshold_frac'] * 100:8.1f}%{fovea}")

    m2p = results.get("motion_to_photon", {})
    if m2p:
        any_entry = next(iter(m2p.values()))
        print(f"\n  Motion to photon (PAPER.md 5.3: reported alongside every quality "
              f"number).\n    Budget from PAPER.md 4.2, {any_entry['budget_ms']['link']}: "
              f"{any_entry['budget_total_ms']:.1f} ms; NOT measured -- "
              "no photodiode and no IMU here.")
        print(f"    {'codec':<16}{'enc ms/frame':>14}{'dec ms/frame':>14}"
              f"{'x the 6.8 ms budget':>22}")
        for name, e in m2p.items():
            if "reference_codec_ms_per_frame" not in e:
                continue
            print(f"    {name:<16}{e['reference_encode_ms_per_frame']:14.1f}"
                  f"{e['reference_decode_ms_per_frame']:14.1f}"
                  f"{e['reference_over_budget']:22.1f}")

    p1 = results.get("phase1", {})
    print("\n  Phase 1 gate (PAPER.md 3.11: within 1.0 dB of x264 intra, 100-400 Mbit):")
    if "error" in p1:
        print(f"    not evaluated: {p1['error']}")
    else:
        verdict = "PASS" if p1["pass"] else "FAIL"
        print(f"    {verdict}: worst {p1['worst_delta_db']:+.3f} dB at "
              f"{p1['worst_at_mbps']:.1f} Mbit/s, mean {p1['mean_delta_db']:+.3f} dB "
              f"over {p1['covered_mbps'][0]:.1f}-{p1['covered_mbps'][1]:.1f} Mbit/s")


if __name__ == "__main__":
    raise SystemExit(main())
