#!/usr/bin/env python3
"""Compression at equal PERCEIVED quality: flat QP vs the rate-control library.

`compare.py` sweeps QP and reports BD-rate on PSNR.  That is the wrong
instrument for `nxv-enc --rc`, which spends bits where the eye is and takes
them away where it is not: it is *designed* to lose PSNR-Y.  This driver
therefore does three things `compare.py` does not:

* it drives the encoder at a **target bit rate**, not a QP, because that is
  the only input the rate controller takes;
* it scores every operating point on the eccentricity-weighted metrics of
  `foveated_metrics.py` and, when `pyfvvdp` is installed, on FovVideoVDP
  (PAPER.md 5.3's primary objective metric); and
* it reports **bits at equal foveated quality**, by interpolating the flat-QP
  rate/quality curve at the foveated quality each rate-controlled point
  reached.  That number is the claim; the PSNR-Y drop next to it is the price.

Arms
----
``flat``         `nxv-enc --qp Q`, a QP sweep, the codec as it ships today
``rc-spatial``   `--rc --rc-temporal off`, foveation + the spatial ladder
``rc-full``      `--rc`, the above plus the per-tile refresh scheduler
``x265-p-refresh`` the foveated hardware-class opponent from `nxq.ffmpeg`:
                 P-only, periodic intra refresh, foveated delta-QP map, CRF

Rate scaling
------------
The target rates on the command line are stated for the reference geometry
2048x1024 at 90 Hz (one `vr-mixed-1024-v2` frame, both eyes).  A smaller clip
is driven at the same *bits per pixel* rather than the same Mbit/s, because
40 Mbit/s into a 512x256 clip is 3.4 bpp and measures nothing.  Every table
prints the scaled rate it actually used.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import foveated_metrics as fov  # noqa: E402
from nxq import cpu, ffmpeg, qpmap  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402
from nxq.yuv import read_sequence  # noqa: E402

#: The geometry the --rates numbers are stated for: one vr-mixed-1024-v2 frame.
REFERENCE_PIXEL_RATE = 2048 * 1024 * 90.0

#: The Pico 4's render FOV per eye, degrees (nxfov::pico4_eye: +/-40.6).
PICO4_FOV_DEG = 81.2


# ------------------------------------------------------------------ encode


def run(cmd: list[str], what: str) -> str:
    p = subprocess.run(cpu.wrap(cmd), capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"{what} failed ({p.returncode}):\n{p.stderr[-4000:]}")
    return p.stdout


def enc_common(seq: Sequence, src: str, out: str) -> list[str]:
    return [
        "--in", src, "--w", str(seq.width), "--h", str(seq.height),
        "--pix", seq.pix_fmt, "--out", out, "--quiet",
        "--eyes", "2" if seq.layout == "sbs" else "1",
    ]


def encode_nxv(args, seq: Sequence, src: str, poses: str | None, out: str,
               *, qp: int | None = None, bitrate: float | None = None,
               temporal: bool = True, foveation: bool = True,
               panel: int = 2160, act: float = 1.0,
               rc_map: str | None = None) -> dict:
    cmd = [args.enc, *enc_common(seq, src, out), "--inter", "on"]
    if poses:
        cmd += ["--poses", poses]
    if args.frames:
        cmd += ["--frames", str(args.frames)]
    if qp is not None:
        cmd += ["--qp", str(qp)]
    else:
        cmd += [
            "--rc", "--rc-bitrate", f"{bitrate:.6g}",
            "--rc-fps", f"{seq.fps:g}",
            "--rc-fov", "on" if foveation else "off",
            "--rc-temporal", "on" if temporal else "off",
            "--rc-panel", str(panel),
            "--rc-act", f"{act:g}",
            "--rc-fov-deg", f"{args.hfov:g},{args.hfov:g}",
        ]
        if args.gaze:
            cmd += ["--gaze", args.gaze]
        if rc_map:
            cmd += ["--rc-map", rc_map]
    t0 = time.perf_counter()
    run(cmd, "nxv-enc")
    enc_s = time.perf_counter() - t0
    return {"bytes": os.path.getsize(out), "encode_s": enc_s, "cmd": cmd}


def decode_nxv(args, nxv: str, out: str) -> float:
    t0 = time.perf_counter()
    run([args.dec, "--in", nxv, "--out", out], "nxv-dec")
    return time.perf_counter() - t0


# ------------------------------------------------------------------ metrics


def measure(seq: Sequence, ref_path: str, dis_path: str, *, hfov: float,
            frames: int | None, ppd: float, fvvdp_runner=None) -> dict:
    """Foveated PSNR/SSIM per eye, pooled in the MSE domain, plus FovVideoVDP."""
    layout = seq.layout
    limit = min(seq.frames, frames) if frames else seq.frames
    ecc_cache: dict = {}
    acc: dict[str, list[float]] = {}

    def add(key: str, value: float) -> None:
        acc.setdefault(key, []).append(value)

    for r, d in zip(read_sequence(seq.path, seq.fmt, limit),
                    read_sequence(dis_path, seq.fmt, limit)):
        for (vname, rv), (_, dv) in zip(fov._views(r.y, layout),
                                        fov._views(d.y, layout)):
            key = (vname, rv.shape)
            if key not in ecc_cache:
                ecc_cache[key] = fov.eccentricity_map(rv.shape[0], rv.shape[1], ppd)
            m = fov.foveated_frame_metrics(rv, dv, ppd, None, "acuity",
                                           fov.FOVEA_RADIUS_DEG, do_ssim=True,
                                           ecc=ecc_cache[key])
            for k in ("fov_psnr_y", "psnr_y", "psnr_fovea", "psnr_periphery"):
                if k in m and math.isfinite(m[k]):
                    add(k, m[k])
            for k in ("fov_ssim_y", "ssim_y"):
                add(k, m[k])
            # `acuity2` is the area-weighted power: resolvable elements per
            # unit area rather than per line, which is the right exponent when
            # the thing being weighted is a pixel.  Both are reported because
            # the two disagree by several dB on a foveated encode and picking
            # one after the fact would be choosing the answer.
            w2 = fov.acuity_weights(ecc_cache[key], "acuity2")
            add("fov2_psnr_y", fov.foveated_psnr(rv, dv, w2))
            add("fov2_ssim_y", fov.foveated_ssim(rv, dv, w2))

    def pool_db(key: str) -> float:
        # Decibels are pooled in the MSE domain, never averaged directly.
        vals = acc.get(key, [])
        if not vals:
            return float("nan")
        mse = np.mean([255.0 ** 2 / (10.0 ** (v / 10.0)) for v in vals])
        return float(10.0 * math.log10(255.0 ** 2 / mse))

    out = {k: pool_db(k) for k in
           ("fov_psnr_y", "fov2_psnr_y", "psnr_y", "psnr_fovea", "psnr_periphery")}
    for k in ("fov_ssim_y", "fov2_ssim_y", "ssim_y"):
        out[k] = float(np.mean(acc[k]))
    if fvvdp_runner is not None:
        try:
            out["fvvdp"] = fvvdp_runner.score(seq.path, dis_path, seq.fmt,
                                              limit=limit)["jod"]
        except Exception as exc:  # the metric is optional, the run is not
            out["fvvdp_error"] = str(exc)
    return out


def mbits(nbytes: int, nframes: int, fps: float) -> float:
    return nbytes * 8.0 * fps / max(1, nframes) / 1e6


# ------------------------------------------------------------- ring report


def ring_report(rc_map: str, edges=(8.0, 20.0, 35.0)) -> dict:
    """Coded-tile fraction, mean QP and mean res_level per eccentricity ring.

    Frame 0 is excluded: it is the intra frame, where every tile is coded by
    definition and no temporal decision has been made yet.
    """
    import csv

    names = ["fovea<8", "8-20", "20-35", ">35"]
    n = [0] * 4
    coded = [0] * 4
    qp = [0.0] * 4
    res = [0.0] * 4
    forced = [0] * 4
    with open(rc_map, newline="") as fh:
        for row in csv.DictReader(fh):
            if int(row["frame"]) == 0:
                continue
            e = float(row["ecc_deg"])
            b = 0 if e < edges[0] else 1 if e < edges[1] else 2 if e < edges[2] else 3
            n[b] += 1
            coded[b] += int(row["coded"])
            forced[b] += int(row["force_skip"])
            qp[b] += float(row["qp"])
            res[b] += float(row["res_level"])
    return {
        "rings": [
            {
                "ring": names[b], "tiles": n[b],
                "coded_fraction": coded[b] / n[b] if n[b] else float("nan"),
                "forced_skip_fraction": forced[b] / n[b] if n[b] else float("nan"),
                "mean_qp": qp[b] / n[b] if n[b] else float("nan"),
                "mean_res_level": res[b] / n[b] if n[b] else float("nan"),
            }
            for b in range(4)
        ]
    }


# ------------------------------------------------ equal-quality interpolation


def bits_at_quality(curve: list[tuple[float, float]], quality: float) -> float | None:
    """Mbit/s the reference curve needs to reach *quality*.

    *curve* is [(mbits, quality)], monotone in rate.  Log-rate linear
    interpolation, the same domain BD-rate integrates in.  Returns None when
    the quality is outside the curve, because extrapolating a rate/quality
    curve is how honest tables become dishonest ones.
    """
    pts = sorted(curve)
    xs = np.log10([p[0] for p in pts])
    ys = np.asarray([p[1] for p in pts], dtype=float)
    if not (ys.min() <= quality <= ys.max()):
        return None
    for i in range(len(ys) - 1):
        lo, hi = ys[i], ys[i + 1]
        if (lo - quality) * (hi - quality) <= 0 and lo != hi:
            t = (quality - lo) / (hi - lo)
            return float(10.0 ** (xs[i] + t * (xs[i + 1] - xs[i])))
    return None


# ------------------------------------------------------------------ driver


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seq", required=True)
    ap.add_argument("--enc", default="nxv-enc")
    ap.add_argument("--dec", default="nxv-dec")
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rates", default="20,40,80,150",
                    help="target Mbit/s for the 2048x1024@90 reference geometry")
    ap.add_argument("--qp", default="14,20,26,32,38,44",
                    help="flat-QP sweep for the reference curve")
    ap.add_argument("--frames", type=int, default=None)
    ap.add_argument("--hfov", type=float, default=PICO4_FOV_DEG,
                    help="render FOV of one eye; the foveation map and the "
                         "foveated metrics both use it (default: the Pico 4)")
    ap.add_argument("--panel", type=int, default=2160,
                    help="panel pixels per eye the foveation ladder decides for")
    ap.add_argument("--gaze", default=None, metavar="X,Y")
    ap.add_argument("--anchor", default="x265-p-refresh",
                    help="foveated standard-codec anchor, or '' to skip it")
    ap.add_argument("--anchor-crf", default="18,24,30,36,42")
    ap.add_argument("--no-fvvdp", action="store_true")
    ap.add_argument("--arms", default=None,
                    help="comma-separated subset of the rate-controlled arms "
                         "to run (default: all of them)")
    args = ap.parse_args(argv)

    os.makedirs(args.work, exist_ok=True)
    seq = Sequence.load(args.seq)
    src = seq.resolved_path()
    poses = seq.pose_log if seq.pose_log and os.path.exists(seq.pose_log) else None
    nframes = min(seq.frames, args.frames) if args.frames else seq.frames
    view_w = seq.width // 2 if seq.layout == "sbs" else seq.width
    ppd = fov.ppd_from_fov(view_w, args.hfov)

    pixel_rate = seq.width * seq.height * seq.fps
    scale = pixel_rate / REFERENCE_PIXEL_RATE
    rates = [float(r) for r in args.rates.split(",")]

    fv = None
    if not args.no_fvvdp:
        from nxq import fvvdp as fvmod
        ok, why = fvmod.available()
        if ok:
            sc = fvmod.FvvdpScoring(
                display=fvmod.dataclasses.replace(
                    fvmod.HEADSETS["pico4"], fov_horizontal_deg=args.hfov,
                    fps=seq.fps),
                layout=seq.layout)
            fv = fvmod.FvvdpRunner(sc, seq.fps)
            print(f"[fvvdp] {fv.describe(view_w, seq.height)}")
        else:
            print(f"[fvvdp] unavailable: {why}")

    doc = {
        "sequence": seq.name, "width": seq.width, "height": seq.height,
        "pix_fmt": seq.pix_fmt, "fps": seq.fps, "layout": seq.layout,
        "frames": nframes, "hfov_deg": args.hfov, "ppd_center": ppd,
        "panel_px_per_eye": args.panel,
        "reference_pixel_rate": REFERENCE_PIXEL_RATE,
        "rate_scale": scale, "gaze": args.gaze,
        "cpu": {"cpus": cpu.cpus(), "threads": cpu.threads()}, "points": [], "rings": {},
    }

    def score(tag: str, nxv: str, meta: dict, rate_label=None) -> dict:
        dec = os.path.join(args.work, f"{tag}.yuv")
        dec_s = decode_nxv(args, nxv, dec)
        m = measure(seq, src, dec, hfov=args.hfov, frames=args.frames, ppd=ppd,
                    fvvdp_runner=fv)
        row = {
            "arm": meta["arm"], "label": tag, "target_mbits": rate_label,
            "mbits": mbits(meta["bytes"], nframes, seq.fps),
            "bpp": meta["bytes"] * 8.0 / (seq.width * seq.height * nframes),
            "encode_s_per_frame": meta["encode_s"] / nframes,
            "decode_s_per_frame": dec_s / nframes,
            **{k: v for k, v in meta.items() if k in ("qp", "bytes")},
            **m,
        }
        doc["points"].append(row)
        print(f"  {tag:<28} {row['mbits']:8.2f} Mbit/s  "
              f"fovPSNR {row['fov_psnr_y']:6.3f}  PSNR-Y {row['psnr_y']:6.3f}  "
              f"fovSSIM {row['fov_ssim_y']:.4f}"
              + (f"  JOD {row['fvvdp']:.3f}" if "fvvdp" in row else ""))
        os.remove(dec)
        return row

    print(f"[seq] {seq.name} {seq.width}x{seq.height} {seq.pix_fmt} {seq.layout}, "
          f"{nframes} frames at {seq.fps:g} Hz")
    print(f"[geo] one eye {view_w}px over {args.hfov:g} deg -> {ppd:.2f} ppd on axis; "
          f"rate scale {scale:.4f} of the 2048x1024@90 reference")

    print("[flat] QP sweep")
    for q in (int(v) for v in args.qp.split(",")):
        nxv = os.path.join(args.work, f"flat-q{q}.nxv")
        meta = encode_nxv(args, seq, src, poses, nxv, qp=q)
        meta["arm"] = "flat"
        meta["qp"] = q
        score(f"flat-q{q}", nxv, meta)
        os.remove(nxv)

    # The four rate-controlled arms.  `panel` is the pixel density the
    # foveation ladder decides for, and it is the one setting on which the
    # measurement and the shipping configuration genuinely differ:
    #
    #   panel = args.panel (2160)  the Pico 4's own density.  This is what the
    #       codec would do on the headset.  On a 1024-px-per-eye clip it
    #       subsamples one ladder step further than the clip's own pixels can
    #       justify, so the metrics over-charge it; see ref/RESULTS-percept.md.
    #   panel = view_w             the ladder told the truth about THIS clip.
    #       Self-consistent, and the arm whose foveated metrics mean what they
    #       say.
    arms = [
        ("rc-nofov", dict(foveation=False, temporal=False, panel=args.panel),
         rates[len(rates) // 2:len(rates) // 2 + 1]),
        ("rc-spatial", dict(foveation=True, temporal=False, panel=args.panel), rates),
        ("rc-full", dict(foveation=True, temporal=True, panel=args.panel), rates),
        ("rc-matched", dict(foveation=True, temporal=True, panel=view_w), rates),
        # The same arm with the activity term switched off, which is the one
        # change that lets the eccentricity term decide the allocation.
        ("rc-matched-noact",
         dict(foveation=True, temporal=True, panel=view_w, act=0.0), rates),
    ]
    want = set(args.arms.split(",")) if args.arms else None
    for arm, kw, arm_rates in arms:
        if want is not None and arm not in want:
            continue
        print(f"[{arm}] target-rate sweep (panel {kw['panel']} px/eye)")
        for r in arm_rates:
            br = r * scale
            nxv = os.path.join(args.work, f"{arm}-{r:g}.nxv")
            rcmap = os.path.join(args.work, f"{arm}-{r:g}.csv")
            meta = encode_nxv(args, seq, src, poses, nxv, bitrate=br,
                              rc_map=rcmap, **kw)
            meta["arm"] = arm
            score(f"{arm}-{r:g}", nxv, meta, rate_label=r)
            doc["rings"][f"{arm}-{r:g}"] = ring_report(rcmap)
            os.remove(nxv)

    if args.anchor:
        print(f"[{args.anchor}] CRF sweep")
        anchor = ffmpeg.ANCHORS[args.anchor]
        ok, why = ffmpeg.anchor_available(anchor, seq.fmt)
        if not ok:
            print(f"  unavailable: {why}")
        else:
            for crf in (float(v) for v in args.anchor_crf.split(",")):
                bs = os.path.join(args.work, f"anchor-crf{crf:g}.bin")
                enc = ffmpeg.encode_anchor(
                    anchor, src, seq.fmt, bs, crf=crf, fps=seq.fps,
                    nframes=nframes, fovea=qpmap.FoveaMap.default(),
                    layout=seq.layout)
                dec = os.path.join(args.work, f"anchor-crf{crf:g}.yuv")
                ffmpeg.decode_bitstream(bs, seq.fmt, dec)
                m = measure(seq, src, dec, hfov=args.hfov, frames=args.frames,
                            ppd=ppd, fvvdp_runner=fv)
                row = {"arm": args.anchor, "label": f"{args.anchor}-crf{crf:g}",
                       "crf": crf, "bytes": os.path.getsize(bs),
                       "mbits": mbits(os.path.getsize(bs), nframes, seq.fps),
                       "bpp": os.path.getsize(bs) * 8.0 /
                              (seq.width * seq.height * nframes), **m}
                doc["points"].append(row)
                print(f"  {row['label']:<28} {row['mbits']:8.2f} Mbit/s  "
                      f"fovPSNR {row['fov_psnr_y']:6.3f}  "
                      f"PSNR-Y {row['psnr_y']:6.3f}"
                      + (f"  JOD {row['fvvdp']:.3f}" if "fvvdp" in row else ""))
                os.remove(bs)
                os.remove(dec)

    # --- bits at equal foveated quality, against the flat-QP curve.
    doc["equal_quality"] = {}
    for metric in ("fov_psnr_y", "fov2_psnr_y", "fov_ssim_y", "fvvdp"):
        base = [(p["mbits"], p[metric]) for p in doc["points"]
                if p["arm"] == "flat" and metric in p]
        if len(base) < 2:
            continue
        rows = []
        for p in doc["points"]:
            if p["arm"] == "flat" or metric not in p:
                continue
            need = bits_at_quality(base, p[metric])
            rows.append({
                "arm": p["arm"], "label": p["label"], "mbits": p["mbits"],
                "quality": p[metric],
                "flat_mbits_at_same_quality": need,
                "saving_pct": None if need is None
                              else 100.0 * (1.0 - p["mbits"] / need),
                "psnr_y": p.get("psnr_y"),
            })
        doc["equal_quality"][metric] = rows

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    print(f"\n[out] {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
