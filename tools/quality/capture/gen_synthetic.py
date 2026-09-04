#!/usr/bin/env python3
"""Generate synthetic VR-like stereo test sequences.

Writes, for each requested pixel format, a headerless planar YUV file, plus a
per-frame JSON pose log and a sequence sidecar.

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
"""

from __future__ import annotations

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from capture import synth  # noqa: E402
from nxq import yuv  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402

FULL_GUARD_PIXELS = 1024 * 1024  # per eye; above this, --full is required


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
) -> list[Sequence]:
    os.makedirs(outdir, exist_ok=True)
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))

    log(f"[synth] panorama {pano_w}x{pano_h} seed={seed} ...")
    t0 = time.time()
    pano = synth.make_panorama(pano_w, pano_h, seed=seed)
    log(f"[synth] panorama built in {time.time() - t0:.1f}s")

    poses = synth.make_poses(frames, motion=motion, fps=fps, seed=seed + 3)
    cam = synth.Camera(eye_w, eye_h, hfov_deg=hfov, vfov_deg=vfov)
    objs = synth.Objects(count=objects, seed=seed + 5) if objects > 0 else None
    dirs = synth._ray_grid(cam)

    out_w = eye_w * (2 if layout == "sbs" else 1)
    out_h = eye_h
    writers = {}
    seqs = []
    for pf in pix_fmts:
        fmt = yuv.Format(out_w, out_h, pf)
        path = os.path.join(outdir, f"{name}.{pf}.yuv")
        writers[pf] = (fmt, yuv.SequenceWriter(path, fmt), path)

    pose_path = os.path.join(outdir, f"{name}.poses.json")

    t0 = time.time()
    for i, pose in enumerate(poses):
        rgb = synth.render_stereo(pano, cam, pose, objs, layout=layout, dirs=dirs, hud=hud)
        f444 = yuv.rgb_to_yuv444(rgb)
        for pf, (fmt, w, _) in writers.items():
            w.write(yuv.to_format(f444, fmt))
        if not quiet and (i % 10 == 0 or i == len(poses) - 1):
            log(f"[synth]   frame {i + 1}/{len(poses)}  yaw={pose['yaw_deg']:7.2f} "
                f"av={pose['angular_velocity_deg_s']:6.1f} deg/s")
    dt = time.time() - t0
    log(f"[synth] rendered {frames} frames ({out_w}x{out_h}) in {dt:.1f}s "
        f"({dt / max(1, frames) * 1000:.0f} ms/frame)")

    yuv.write_pose_log(pose_path, poses)
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
            source=f"synthetic:{motion}:seed{seed}",
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
    ap.add_argument("--pano-width", type=int, default=0, help="0 = 8x the eye width")
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

    pano_w = args.pano_width or max(2048, args.eye_width * 8)
    pano_h = pano_w // 2

    motions = list(synth.MOTIONS) if args.motion == "all" else [args.motion]
    made = []
    for m in motions:
        name = args.name if len(motions) == 1 else f"{args.name}-{m}"
        seqs = build(
            args.out, name, args.frames, args.eye_width, args.eye_height, m, args.layout,
            pix_fmts, args.fps, args.seed, pano_w, pano_h, args.objects, not args.no_hud,
            args.hfov, args.vfov, args.quiet,
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
