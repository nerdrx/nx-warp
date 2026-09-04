"""Command line for ``nxvc-hybridsim``.

Subcommands::

    material   render the synthetic panorama sequence and pose log
    sweep      run the full base-resolution x bit-split grid
    one        run a single configuration (debugging)
    report     turn a sweep's JSON into the tables of hybrid/RESULTS.md
    selftest   a tiny end-to-end run, used by tests/hybrid

All heavy children run under ``chrt -i 0 taskset -c 12-15 nice -n 19``; see
:mod:`nxvchybrid.cpu`.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import sys
import time

from . import base as basemod, codec, report as reportmod, sweep as sweepmod

DEFAULT_SCRATCH = "/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/hybrid"


def scratch() -> str:
    return os.environ.get("NXVCH_SCRATCH", DEFAULT_SCRATCH)


def _weights(name: str):
    return {"2bit": codec.WEIGHTS_2BIT, "3bit": codec.WEIGHTS_3BIT,
            "base-only": (1.0,), "temporal-only": (0.0,)}[name]


def cmd_material(a) -> int:
    yuv, poses = sweepmod.prepare_material(
        os.path.join(scratch(), "seq"), a.size, a.frames, a.seed, a.fov, a.pano_width,
        not a.no_sprites,
    )
    print(f"sequence: {yuv}\nposes:    {poses}")
    return 0


def cmd_one(a) -> int:
    yuv, poses = sweepmod.prepare_material(
        os.path.join(scratch(), "seq"), a.size, a.frames, a.seed, a.fov, a.pano_width, True
    )
    job = sweepmod.Job(
        a.kind, a.total_mbit, a.base_scale, a.base_frac, a.size, a.frames, a.fps,
        yuv, poses, os.path.join(scratch(), "runs"), tuple(_weights(a.weights)), a.mv_radius,
    )
    d = sweepmod.run_job(job)
    print(json.dumps({k: v for k, v in d.items() if k != "stats"}, indent=2))
    if a.out:
        with open(a.out, "w") as fh:
            json.dump(d, fh)
    return 0


def _run_one(job):
    try:
        return sweepmod.run_job(job)
    except Exception as exc:  # noqa: BLE001 - one bad point must not kill a sweep
        return {"label": job.label, "kind": job.kind, "error": repr(exc)}


def cmd_sweep(a) -> int:
    if not basemod.available_encoders():
        print("no libx265/libx264 in ffmpeg; cannot run the sweep", file=sys.stderr)
        return 77
    yuv, poses = sweepmod.prepare_material(
        os.path.join(scratch(), "seq"), a.size, a.frames, a.seed, a.fov, a.pano_width, True
    )
    totals = tuple(float(x) for x in a.totals.split(","))
    scales = tuple(float(x) for x in a.scales.split(","))
    fracs = tuple(float(x) for x in a.fracs.split(","))
    jobs = sweepmod.build_jobs(
        yuv, poses, a.size, a.frames, os.path.join(scratch(), "runs"),
        totals, scales, fracs, a.fps, _weights(a.weights), a.mv_radius,
    )
    print(f"{len(jobs)} jobs, {a.workers} workers, {a.size}^2 x {a.frames} frames")
    t0 = time.time()
    results = []
    if a.workers > 1:
        ctx = mp.get_context("fork")
        with ctx.Pool(a.workers) as pool:
            for i, d in enumerate(pool.imap_unordered(_run_one, jobs), 1):
                results.append(d)
                print(f"[{i}/{len(jobs)}] {d['label']}: "
                      + (d["error"] if "error" in d else
                         f"{d.get('measured_mbit', 0):.1f} Mbit  "
                         f"PSNR-Y {d.get('psnr_y', 0):.2f}  SSIM {d.get('ssim_y', 0):.4f}"),
                      flush=True)
    else:
        for i, j in enumerate(jobs, 1):
            d = _run_one(j)
            results.append(d)
            print(f"[{i}/{len(jobs)}] {d['label']}", flush=True)
    out = a.out or os.path.join(scratch(), "results", "sweep.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as fh:
        json.dump({
            "size": a.size, "frames": a.frames, "fps": a.fps,
            "weights": a.weights, "mv_radius": a.mv_radius,
            "encoder": sorted(basemod.available_encoders()),
            "seconds": time.time() - t0,
            "results": results,
        }, fh)
    print(f"wrote {out} in {time.time() - t0:.0f} s")
    return 0


def cmd_report(a) -> int:
    with open(a.sweep) as fh:
        d = json.load(fh)
    text = reportmod.render(d, extra=[json.load(open(p)) for p in a.extra or []])
    if a.out:
        with open(a.out, "w") as fh:
            fh.write(text)
        print(f"wrote {a.out}")
    else:
        print(text)
    return 0


def cmd_selftest(a) -> int:
    if not basemod.available_encoders():
        print("SKIP: ffmpeg with libx265/libx264 not available")
        return 77
    size, frames = a.size, a.frames
    yuv, poses = sweepmod.prepare_material(os.path.join(scratch(), "seq"), size, frames,
                                           pano_width=1024)
    work = os.path.join(scratch(), "runs", "selftest")
    ok = True
    for kind, scale, frac in (("hevc", 1.0, 1.0), ("pure", 0.0, 0.0), ("hybrid", 0.5, 0.5)):
        job = sweepmod.Job(kind, 150.0, scale, frac, size, frames, 90.0, yuv, poses,
                           work, codec.WEIGHTS_2BIT, 4)
        d = sweepmod.run_job(job)
        mb = d.get("measured_mbit", 0.0)
        print(f"{kind:7s} {mb:7.1f} Mbit  PSNR-Y {d['psnr_y']:6.2f} dB  SSIM {d['ssim_y']:.4f}")
        if not (10.0 < d["psnr_y"] < 70.0):
            print(f"FAIL: {kind} PSNR out of range", file=sys.stderr)
            ok = False
        if kind != "hevc" and not (0.5 * 150.0 < mb < 1.6 * 150.0):
            print(f"FAIL: {kind} rate control missed the target ({mb:.1f} Mbit)",
                  file=sys.stderr)
            ok = False
    return 0 if ok else 1


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="nxvc-hybridsim", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(q):
        q.add_argument("--size", type=int, default=1024)
        q.add_argument("--frames", type=int, default=90)
        q.add_argument("--seed", type=int, default=7)
        q.add_argument("--fov", type=float, default=95.0)
        q.add_argument("--pano-width", type=int, default=4096)

    m = sub.add_parser("material", help="render the synthetic sequence")
    common(m)
    m.add_argument("--no-sprites", action="store_true")
    m.set_defaults(fn=cmd_material)

    o = sub.add_parser("one", help="run a single configuration")
    common(o)
    o.add_argument("--kind", choices=("hybrid", "pure", "hevc"), default="hybrid")
    o.add_argument("--total-mbit", type=float, default=150.0)
    o.add_argument("--base-scale", type=float, default=0.5)
    o.add_argument("--base-frac", type=float, default=0.5)
    o.add_argument("--fps", type=float, default=90.0)
    o.add_argument("--weights", default="2bit", choices=("2bit", "3bit", "base-only",
                                                         "temporal-only"))
    o.add_argument("--mv-radius", type=int, default=6)
    o.add_argument("--out")
    o.set_defaults(fn=cmd_one)

    s = sub.add_parser("sweep", help="the full grid")
    common(s)
    s.add_argument("--totals", default="50,100,150,200")
    s.add_argument("--scales", default="1.0,0.75,0.5")
    s.add_argument("--fracs", default="0.25,0.40,0.55,0.70,0.85")
    s.add_argument("--fps", type=float, default=90.0)
    s.add_argument("--weights", default="2bit", choices=("2bit", "3bit", "base-only",
                                                         "temporal-only"))
    s.add_argument("--mv-radius", type=int, default=6)
    s.add_argument("--workers", type=int, default=4)
    s.add_argument("--out")
    s.set_defaults(fn=cmd_sweep)

    r = sub.add_parser("report", help="render RESULTS tables from a sweep JSON")
    r.add_argument("sweep")
    r.add_argument("--extra", nargs="*", help="additional sweep JSONs (A/B runs)")
    r.add_argument("--out")
    r.set_defaults(fn=cmd_report)

    t = sub.add_parser("selftest", help="tiny end-to-end run (ctest)")
    t.add_argument("--size", type=int, default=256)
    t.add_argument("--frames", type=int, default=8)
    t.set_defaults(fn=cmd_selftest)

    a = p.parse_args(argv)
    return a.fn(a)
