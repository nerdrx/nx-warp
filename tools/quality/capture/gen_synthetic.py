#!/usr/bin/env python3
"""Generate synthetic VR-like stereo test sequences.

Writes, for each requested pixel format, a headerless planar YUV file, plus a
per-frame JSON pose log and a sequence sidecar.

Version 2: band-limited by default
----------------------------------
Version 1 took **one bilinear tap** per output sample from a panorama at 2.1x
the eye's angular resolution.  Its frames therefore carried aliasing -- energy
above the eye's Nyquist -- and aliasing is not a geometric function of the
pose, so no warp of any precision can predict it.  `docs/WARP-AUDIT.md`
section 4 measured what that costs: holding the predictor and the pose pair
fixed and varying only the ground truth moves the ideal-warp ceiling by
**7.2 dB full-frame and 14.4 dB centre**, which is more than any predictor
change on offer is worth.  Every rate-distortion number measured on v1 material
was partly a measurement of the generator.

Version 2 renders band-limited, the way ``nxvc-warpsim`` does:

* a panorama at **16x the eye width** -- 4.2x its angular resolution -- carrying
  the *angular* content of the 4096-wide one, so the checkerboards and the zone
  plate stay the frequencies they were meant to be rather than becoming four
  times finer (``synth.make_panorama``'s ``feature_scale``);
* a **latitude-aware longitudinal prefilter**, because an equirectangular map's
  texels compress as 1/cos(lat) and a view pitched up reaches latitudes where
  the panorama is three times finer than anything the renderer samples it at;
* **4x4 box supersampling** of the render, objects and HUD included.

Every sequence is measured against its own **ideal-warp ceiling**: frame N-1
warped by the exact float homography of the pose pair, compared with frame N.
That is the best PSNR any predictor could score on the material, it is printed
at the end of a run and stored in the pose log, and every RD number on this
material should be read against it.

``--legacy`` restores version 1 exactly, bit for bit, so numbers measured
before this change stay reproducible.

Examples
--------
A small sequence for tests and quick harness runs (10 frames, 512x512 per eye,
side by side, so the coded picture is 1024x512)::

    python3 capture/gen_synthetic.py --out $NXQ_SCRATCH/seq --name vr-mixed-512 \\
        --frames 10 --eye-width 512 --eye-height 512 --motion mixed

The documented "full" mode, at the paper's per-eye resolution class::

    python3 capture/gen_synthetic.py --out $NXQ_SCRATCH/seq --name vr-mixed-2048 \\
        --frames 60 --eye-width 2048 --eye-height 2048 --motion mixed --full

Generate one sequence per motion profile at once with ``--motion all``.

Regenerate version 1 material -- byte-identical to what was published before
the band-limiting change::

    python3 capture/gen_synthetic.py --out $NXQ_SCRATCH/seq --name vr-mixed-1024 \\
        --frames 36 --eye-width 1024 --eye-height 1024 --motion mixed --full --legacy
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from capture import synth  # noqa: E402
from nxq import yuv  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402

FULL_GUARD_PIXELS = 1024 * 1024  # per eye; above this, --full is required

#: The projection conventions this generator renders with, written verbatim
#: into every `.poses.json` it produces.  See docs/WARP.md section 2.1 for the
#: schema and why it exists: the encoder derives its homography from this file,
#: and *every* field below is something it would otherwise have to assume.
#:
#: The FOV in particular used to be assumed.  `nxv-enc` defaulted to 95,95 and
#: the sidecar said nothing, so `gen_synthetic.py --hfov 110` produced a
#: sequence whose warp was silently wrong -- measured at 18.70 dB where the
#: correct FOV gives 31.01 dB on the same pair (docs/WARP-AUDIT.md section 5).
#: Nothing crashed and no test failed. Writing the FOV down is the fix.
POSE_CONVENTION = {
    # Bumped when any field below changes meaning. A consumer that does not
    # recognise the id must refuse the file rather than guess.
    "id": "nxv-openxr-1",
    "quaternion": "xyzw",           # component order of orientation_xyzw
    "handedness": "right",          # right-handed, active rotation
    "rotation": "camera_to_world",  # q rotates a camera-space vector into world
    "axes": "x_right_y_up_z_back",  # camera space: -Z is forward
    "image_origin": "top_left",     # row 0 is the TOP of the picture
    "pixel_centre": 0.5,            # sample (i, j) is sampled at (i+.5, j+.5)
    "fov_sign": "xrfovf",           # left and down negative, as XrFovf
    "pose_kind": "render",          # the pose the frame was rendered with,
                                    # not a predicted display pose
    "pairing": "n_minus_1_to_n",    # frame N is predicted from frame N-1
    "position_units": "m",
}


def write_pose_log(path, poses, hfov_deg, vfov_deg, fps, eye_w, eye_h,
                   render=None, ceiling=None) -> None:
    """Write the pose log with the conventions and the FOV it was rendered at.

    ``version`` 2 adds ``convention``, ``fov_deg`` and ``eye``; version 1 files
    have neither and a consumer must fall back to its own defaults, which is
    the hazard this version exists to close.

    ``render`` and ``ceiling`` are the v2 generator's additions: how the frames
    were band-limited, and the ideal-warp PSNR ceiling measured on them.  They
    are omitted entirely under ``--legacy`` so that a legacy run's pose log is
    byte-identical to what version 1 wrote.  Both are keyed so that
    `nxv-enc`'s deliberately tiny sidecar parser -- it scans for the first
    ``"id"`` and the first ``"fov_deg"`` and for every ``"orientation_xyzw"``
    -- cannot see them: neither block contains any of those keys, and both sit
    after ``convention`` in the file.
    """
    half_h = math.radians(hfov_deg) * 0.5
    half_v = math.radians(vfov_deg) * 0.5
    doc = {
        "version": 2,
        "convention": POSE_CONVENTION,
        # Symmetric here, but carried per-side because XrFovf is not, and
        # because a real headset capture will not be.
        "fov_deg": {"h": hfov_deg, "v": vfov_deg},
        "fov_rad": {
            "left": -half_h, "right": half_h,
            "up": half_v, "down": -half_v,
        },
        "eye": {"width": eye_w, "height": eye_h},
        "fps": fps,
    }
    if render is not None:
        doc["render"] = render
    if ceiling is not None:
        doc["ideal_warp_ceiling"] = ceiling
    doc["frames"] = poses
    os.makedirs(os.path.dirname(os.path.abspath(str(path))) or ".", exist_ok=True)
    with open(path, "w") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")


def measure_ceiling(prev_y, cur_y, pose_prev, pose_cur, cam, eyes: int) -> dict:
    """Ideal-warp PSNR for one frame pair: the material's own ceiling.

    ``prev_y`` and ``cur_y`` are the luma planes of the whole coded picture, so
    for an ``sbs`` sequence they hold both eyes side by side and each eye is
    warped and scored separately.  Full-frame includes the disocclusion strip on
    the leading edge, which the encoder answers with INTRA; the centre crop
    drops a 1/8 border, which is the number `warp/RESULTS.md` reports and the
    one that describes the predictor rather than the frame edge.
    """
    full, centre = [], []
    w = cam.width
    for e in range(eyes):
        p = prev_y[:, e * w : (e + 1) * w]
        c = cur_y[:, e * w : (e + 1) * w]
        pred = synth.ideal_warp(p, pose_prev, pose_cur, cam)
        full.append(synth.psnr(pred, c))
        centre.append(synth.psnr(synth.centre_crop(pred), synth.centre_crop(c)))
    return {"full_db": float(np.mean(full)), "centre_db": float(np.mean(centre))}


def summarise_ceiling(per_frame: list[dict], poses: list[dict]) -> dict:
    """Reduce the per-frame ceiling to the numbers a report quotes."""
    if not per_frame:
        return {}
    full = [p["full_db"] for p in per_frame]
    centre = [p["centre_db"] for p in per_frame]
    av = [p["angular_velocity_deg_s"] for p in poses[1:]]

    def stats(v):
        return {"mean_db": float(np.mean(v)), "min_db": float(np.min(v)),
                "max_db": float(np.max(v)), "first_db": float(v[0])}
    return {
        "what": "frame N-1 warped by the exact float homography of the pose "
                "pair, PSNR-Y against frame N. The best any predictor can do "
                "on this material; every RD number here should be read "
                "against it.",
        "frames": len(per_frame),
        "full": stats(full),
        "centre": stats(centre),
        "angular_velocity_deg_s": {"min": float(np.min(av)), "max": float(np.max(av))},
        "per_frame": [
            {"frame": i + 1, "full_db": round(full[i], 3), "centre_db": round(centre[i], 3),
             "angular_velocity_deg_s": round(av[i], 3)}
            for i in range(len(per_frame))
        ],
    }


def build(
    outdir: str,
    name: str,
    frames: int,
    eye_w: int,
    eye_h: int,
    motion: str,
    layout: str,
    pix_fmts: list[str],
    fps: float,
    seed: int,
    pano_w: int,
    pano_h: int,
    objects: int,
    hud: bool,
    hfov: float,
    vfov: float,
    quiet: bool = False,
    legacy: bool = False,
    supersample: int = 4,
    ceiling: bool = True,
) -> list[Sequence]:
    os.makedirs(outdir, exist_ok=True)
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))

    cam = synth.Camera(eye_w, eye_h, hfov_deg=hfov, vfov_deg=vfov)
    ss = 1 if legacy else supersample
    eye_ppd = synth.eye_ppd(cam)
    pano_ppd = synth.equirect_ppd(pano_w)
    # Size the pixel-defined features (checkerboard periods, the zone plate's
    # chirp, the stars) in *eye* pixels rather than panorama pixels.  Version 1
    # sized them in panorama pixels, so its finest checkerboard had a period of
    # 0.7 of an output pixel: not high-frequency detail, just aliasing with a
    # regular pattern.  One eye pixel is `pano_ppd / eye_ppd` panorama pixels,
    # so this puts the three checkerboards at 4, 8 and 16 output pixels per
    # period -- detail right down to the resolution limit, and none above it.
    feature_scale = 1.0 if legacy else pano_ppd / eye_ppd

    log(f"[synth] panorama {pano_w}x{pano_h} seed={seed} "
        f"feature_scale={feature_scale:.3g} ...")
    t0 = time.time()
    pano = synth.make_panorama(pano_w, pano_h, seed=seed, feature_scale=feature_scale)
    log(f"[synth] panorama built in {time.time() - t0:.1f}s "
        f"({pano_ppd:.1f} px/deg at the equator, eye {eye_ppd:.2f} px/deg on axis, "
        f"{pano_ppd / eye_ppd:.2f}x)")
    if not legacy:
        t0 = time.time()
        pano = synth.prefilter_equirect(pano, eye_ppd * ss)
        log(f"[synth] latitude prefilter to {eye_ppd * ss:.1f} px/deg "
            f"({ss}x{ss} supersampled) in {time.time() - t0:.1f}s")

    poses = synth.make_poses(frames, motion=motion, fps=fps, seed=seed + 3)
    objs = synth.Objects(count=objects, seed=seed + 5) if objects > 0 else None
    dirs = synth._ray_grid(cam if legacy else synth.Camera(eye_w * ss, eye_h * ss, hfov, vfov))

    out_w = eye_w * (2 if layout == "sbs" else 1)
    out_h = eye_h
    eyes = 2 if layout == "sbs" else 1
    writers = {}
    seqs = []
    for pf in pix_fmts:
        fmt = yuv.Format(out_w, out_h, pf)
        path = os.path.join(outdir, f"{name}.{pf}.yuv")
        writers[pf] = (fmt, yuv.SequenceWriter(path, fmt), path)

    pose_path = os.path.join(outdir, f"{name}.poses.json")

    t0 = time.time()
    prev_y = None
    per_frame = []
    for i, pose in enumerate(poses):
        if legacy:
            rgb = synth.render_stereo(pano, cam, pose, objs, layout=layout, dirs=dirs, hud=hud)
        else:
            rgb = synth.render_stereo_ss(pano, cam, pose, objs, layout=layout, ss=ss,
                                         dirs_hi=dirs, hud=hud)
        f444 = yuv.rgb_to_yuv444(rgb)
        for pf, (fmt, w, _) in writers.items():
            w.write(yuv.to_format(f444, fmt))
        if ceiling:
            if prev_y is not None:
                per_frame.append(measure_ceiling(prev_y, f444.y, poses[i - 1], pose, cam, eyes))
            prev_y = f444.y
        if not quiet and (i % 10 == 0 or i == len(poses) - 1):
            log(f"[synth]   frame {i + 1}/{len(poses)}  yaw={pose['yaw_deg']:7.2f} "
                f"av={pose['angular_velocity_deg_s']:6.1f} deg/s")
    dt = time.time() - t0
    log(f"[synth] rendered {frames} frames ({out_w}x{out_h}) in {dt:.1f}s "
        f"({dt / max(1, frames) * 1000:.0f} ms/frame)")

    render_block = None if legacy else {
        "generator": 2,
        "band_limited": True,
        "supersample": ss,
        "supersample_filter": "box",
        "panorama": {
            "width": pano_w, "height": pano_h,
            "px_per_deg_equator": round(pano_ppd, 4),
            "feature_scale": feature_scale,
            "prefilter": "latitude-aware longitudinal box to the render's "
                         "sample rate; equirectangular texels compress as "
                         "1/cos(lat) and the render does not",
        },
        "eye_px_per_deg_on_axis": round(eye_ppd, 4),
        "panorama_oversampling": round(pano_ppd / eye_ppd, 4),
        "render_sample_rate_px_per_deg": round(eye_ppd * ss, 4),
        "note": "docs/WARP-AUDIT.md section 4: version 1 point-sampled a 2.1x "
                "panorama once per output sample, so its frames carried "
                "pose-independent aliasing worth 7.2 dB full-frame / 14.4 dB "
                "centre on the ideal-warp ceiling. --legacy reproduces it.",
    }
    ceil_block = summarise_ceiling(per_frame, poses) if (ceiling and per_frame) else None
    if ceil_block:
        log(f"[synth] ideal-warp ceiling: {ceil_block['full']['mean_db']:.2f} dB full-frame, "
            f"{ceil_block['centre']['mean_db']:.2f} dB centre "
            f"(mean over {ceil_block['frames']} pairs; first pair "
            f"{ceil_block['full']['first_db']:.2f} / "
            f"{ceil_block['centre']['first_db']:.2f} dB)")
        log("[synth]   that is the best PSNR any predictor can reach on this "
            "material: read every RD number against it")
    write_pose_log(pose_path, poses, hfov, vfov, fps, eye_w, eye_h,
                   render=render_block, ceiling=None if legacy else ceil_block)
    for pf, (fmt, w, path) in writers.items():
        w.close()
        seq = Sequence(
            name=f"{name}.{pf}",
            path=path,
            width=out_w,
            height=out_h,
            pix_fmt=pf,
            fps=fps,
            frames=frames,
            pose_log=pose_path,
            source=(f"synthetic:{motion}:seed{seed}" if legacy
                    else f"synthetic:{motion}:seed{seed}:v2-bandlimited-ss{ss}"),
            layout=layout,
        )
        sidecar = os.path.join(outdir, f"{name}.{pf}.json")
        seq.save(sidecar)
        seqs.append(seq)
        log(f"[synth] wrote {path} ({os.path.getsize(path) / 1e6:.1f} MB) and {sidecar}")
    log(f"[synth] pose log: {pose_path}")
    return seqs


def write_preview(seq: Sequence, path: str, index: int = 0) -> bool:
    """Write a PNG of one frame for eyeballing. Needs Pillow; returns False if absent."""
    try:
        from PIL import Image
    except ImportError:
        return False
    f = yuv.read_frame(seq.path, seq.fmt, index)
    y = f.y.astype(np.float32)
    u = f.u.astype(np.float32)
    v = f.v.astype(np.float32)
    if u.shape != y.shape:
        u = np.repeat(np.repeat(u, 2, 0), 2, 1)[: y.shape[0], : y.shape[1]]
        v = np.repeat(np.repeat(v, 2, 0), 2, 1)[: y.shape[0], : y.shape[1]]
    yl = (y - 16.0) * (255.0 / 219.0)
    ul = (u - 128.0) * (255.0 / 224.0)
    vl = (v - 128.0) * (255.0 / 224.0)
    r = yl + 1.5748 * vl
    b = yl + 1.8556 * ul
    g = (yl - 0.2126 * r - 0.0722 * b) / 0.7152
    rgb = np.clip(np.stack([r, g, b], -1), 0, 255).astype(np.uint8)
    Image.fromarray(rgb).save(path)
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory (use nx-scratch, not the repo)")
    ap.add_argument("--name", default="vr", help="sequence base name")
    ap.add_argument("--frames", type=int, default=10)
    ap.add_argument("--eye-width", type=int, default=512)
    ap.add_argument("--eye-height", type=int, default=512)
    ap.add_argument("--motion", default="mixed", choices=(*synth.MOTIONS, "all"))
    ap.add_argument("--layout", default="sbs", choices=("sbs", "mono"))
    ap.add_argument("--pix", default="yuv444p,yuv420p", help="comma-separated pixel formats")
    ap.add_argument("--fps", type=float, default=90.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--pano-width", type=int, default=0,
                    help="0 = 16x the eye width (8x under --legacy)")
    ap.add_argument("--supersample", type=int, default=4, metavar="N",
                    help="NxN box supersampling of the render (default 4; minimum 2, "
                         "and the band-limiting argument needs 4)")
    ap.add_argument("--legacy", action="store_true",
                    help="reproduce the version 1 generator bit for bit: one bilinear tap "
                         "per output sample, panorama at 8x the eye width with absolute "
                         "pixel feature sizes, no prefilter, no ceiling measurement. For "
                         "regenerating material that published numbers were measured on")
    ap.add_argument("--no-ceiling", action="store_true",
                    help="skip the ideal-warp PSNR ceiling measurement")
    ap.add_argument("--ceiling", action="store_true",
                    help="measure and print the ceiling even under --legacy (it is never "
                         "written into a legacy pose log, which must stay byte-identical)")
    ap.add_argument("--objects", type=int, default=7, help="near-field moving objects (0 to disable)")
    ap.add_argument("--no-hud", action="store_true", help="omit the head-locked UI panel")
    ap.add_argument("--hfov", type=float, default=95.0)
    ap.add_argument("--vfov", type=float, default=95.0)
    ap.add_argument("--preview", action="store_true", help="also write a PNG of frame 0")
    ap.add_argument("--full", action="store_true",
                    help="allow large resolutions (over 1 Mpix per eye); these take minutes and gigabytes")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.eye_width * args.eye_height > FULL_GUARD_PIXELS and not args.full:
        ap.error(
            f"{args.eye_width}x{args.eye_height} per eye is over {FULL_GUARD_PIXELS / 1e6:.1f} Mpix. "
            "Pass --full to confirm, and make sure --out points at nx-scratch."
        )

    pix_fmts = [p.strip() for p in args.pix.split(",") if p.strip()]
    for p in pix_fmts:
        if p not in yuv.PIX_FMTS:
            ap.error(f"unsupported pix fmt {p!r}, want {yuv.PIX_FMTS}")

    if args.supersample < 1:
        ap.error("--supersample must be at least 1")
    if args.legacy and args.supersample != 4:
        ap.error("--legacy is version 1 exactly, which has no supersampling; "
                 "do not combine it with --supersample")
    scale = 8 if args.legacy else 16
    pano_w = args.pano_width or max(2048, args.eye_width * scale)
    pano_h = pano_w // 2
    want_ceiling = (args.ceiling if args.legacy else not args.no_ceiling)

    motions = list(synth.MOTIONS) if args.motion == "all" else [args.motion]
    made = []
    for m in motions:
        name = args.name if len(motions) == 1 else f"{args.name}-{m}"
        seqs = build(
            args.out, name, args.frames, args.eye_width, args.eye_height, m, args.layout,
            pix_fmts, args.fps, args.seed, pano_w, pano_h, args.objects, not args.no_hud,
            args.hfov, args.vfov, args.quiet,
            legacy=args.legacy, supersample=args.supersample, ceiling=want_ceiling,
        )
        made += seqs
        if args.preview and seqs:
            png = os.path.join(args.out, f"{name}.preview.png")
            if write_preview(seqs[0], png):
                print(f"[synth] preview: {png}")
            else:
                print("[synth] preview skipped (Pillow not installed)")

    print(f"[synth] done: {len(made)} sequence(s) in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
