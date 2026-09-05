"""Command line for ``nxvc-hybridsim``.

Subcommands::

    material   render the synthetic panorama sequence and pose log
    sweep      run the full base-resolution x bit-split grid
    one        run a single configuration (debugging)
    report     turn a sweep's JSON into the tables of hybrid/RESULTS.md
    selftest   a tiny end-to-end run, used by tests/hybrid

    spatial    the *spatial* hybrid: HEVC periphery + a real-codec fovea inset
    spatial-report   its tables, for hybrid/RESULTS-SPATIAL.md

The ``sweep``/``one`` family measures the **layered** hybrid of ADR 0014 and
0022 on a synthetic panorama with a modelled codec.  ``spatial`` measures a
different arrangement -- disjoint in space, not layered in quality -- on the
``tools/quality`` v2 sequences with the **real** codec from ``build-ref``; see
:mod:`nxvchybrid.spatial`.

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
        anchors=not a.no_anchors,
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
            "note": a.note,
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


def cmd_spatial(a) -> int:
    from . import spatial as sp

    if not basemod.available_encoders():
        print("no libx265/libx264 in ffmpeg; cannot run the sweep", file=sys.stderr)
        return 77
    totals = tuple(float(x) for x in a.totals.split(","))
    insets = tuple(int(x) for x in a.insets.split(",")) if a.insets else ()
    fracs = tuple(float(x) for x in a.fracs.split(","))
    workroot = a.work or os.path.join(scratch(), "spatial")
    a.codec_dir = a.codec_dir or sp.DEFAULT_CODEC_DIR
    jobs = sp.build_jobs(a.seq, workroot, totals, insets, fracs,
                         feather=a.feather, codec_dir=a.codec_dir, hfov=a.hfov,
                         fovea_spec=a.fovea_map, anchors=not a.no_anchors)
    for spec in a.extra_point or []:
        jobs.append(_spatial_extra(spec, a, workroot))
    print(f"{len(jobs)} points, {a.workers} producer(s); "
          f"seq {os.path.basename(a.seq)}", flush=True)
    t0 = time.time()
    results: list[dict] = []
    if a.workers > 1:
        ctx = mp.get_context("fork")
        with ctx.Pool(a.workers) as pool:
            for i, d in enumerate(pool.imap_unordered(sp._produce_one, jobs), 1):
                results.append(d)
                print(f"[produce {i}/{len(jobs)}] {d['label']}: "
                      + (d["error"] if "error" in d else
                         f"{d.get('measured_mbit', 0):.1f} Mbit "
                         f"(base {d.get('base_mbit', 0):.1f} + inset "
                         f"{d.get('inset_mbit', 0):.1f}, qp {d.get('inset_qp', '-')})"),
                      flush=True)
    else:
        for i, j in enumerate(jobs, 1):
            d = sp._produce_one(j)
            results.append(d)
            print(f"[produce {i}/{len(jobs)}] {d['label']}", flush=True)

    def prog(n, total, r):
        jod = r.get("jod")
        print(f"[score {n}/{total}] {r['label']}: fov-PSNR {r['fov_psnr_y']:.2f} dB, "
              f"fovea {r['psnr_fovea']:.2f}, periphery {r['psnr_periphery']:.2f}"
              + (f", JOD {jod:.3f}" if jod is not None else ""), flush=True)

    meta = sp.score_all(results, a.hfov, fvvdp=not a.no_fvvdp,
                        device=a.fvvdp_device, keep=a.keep, progress=prog)
    out = a.out or os.path.join(scratch(), "results", "spatial.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as fh:
        json.dump({
            "seq": a.seq, "hfov": a.hfov, "feather": a.feather,
            "fovea_map": a.fovea_map, "codec_dir": a.codec_dir,
            "note": a.note, "seconds": time.time() - t0,
            "encoder": sorted(basemod.available_encoders()),
            "metrics": meta, "results": results,
        }, fh)
    print(f"wrote {out} in {time.time() - t0:.0f} s")
    return 0


def _spatial_extra(spec: str, a, workroot):
    """``kind=spatial:total=80:inset=768:frac=0.7:feather=0:hole=8:tag=x`` ."""
    from . import spatial as sp

    kw = dict(kind="spatial", total=80.0, inset=768, frac=0.7,
              feather=a.feather, hole=0, tag="")
    for part in spec.split(":"):
        if not part:
            continue
        k, _, v = part.partition("=")
        if k not in kw:
            raise SystemExit(f"unknown --extra-point key {k!r}; known: {sorted(kw)}")
        kw[k] = v
    return sp.SpatialJob(str(kw["kind"]), float(kw["total"]), int(kw["inset"]),
                         float(kw["frac"]), int(kw["feather"]), a.seq, workroot,
                         a.codec_dir, a.hfov, int(kw["hole"]), a.fovea_map,
                         str(kw["tag"]))


def cmd_spatial_report(a) -> int:
    from . import spatial_report

    with open(a.results) as fh:
        d = json.load(fh)
    text = spatial_report.render(d)
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
    s.add_argument("--no-anchors", action="store_true",
                   help="skip the HEVC and pure-codec anchors (for A/B variants)")
    s.add_argument("--note", default="", help="label carried into the report")
    s.add_argument("--out")
    s.set_defaults(fn=cmd_sweep)

    r = sub.add_parser("report", help="render RESULTS tables from a sweep JSON")
    r.add_argument("sweep")
    r.add_argument("--extra", nargs="*", help="additional sweep JSONs (A/B runs)")
    r.add_argument("--out")
    r.set_defaults(fn=cmd_report)

    sp = sub.add_parser("spatial", help="the spatial hybrid: HEVC periphery + "
                                        "a real-codec fovea inset")
    sp.add_argument("--seq", required=True,
                    help="a tools/quality sequence sidecar (.json), sbs stereo")
    sp.add_argument("--totals", default="40,80,150",
                    help="total budgets, Mbit at 2 x 2048^2 x 90 Hz")
    sp.add_argument("--insets", default="512,640,768,896",
                    help="inset side in pixels per eye; multiples of 64 that "
                         "leave a multiple-of-64 offset")
    sp.add_argument("--fracs", default="0.40,0.55,0.70,0.85",
                    help="periphery share of the total bitrate")
    sp.add_argument("--feather", type=int, default=32,
                    help="composite cross-fade width in pixels, inward from the "
                         "inset border (0 = a hard seam)")
    sp.add_argument("--hfov", type=float, default=95.0,
                    help="horizontal FOV of one eye of the sequence")
    sp.add_argument("--fovea-map", default=None,
                    help="delta-QP map for the x265-p-refresh anchor, "
                         "nxq/qpmap.py syntax")
    sp.add_argument("--codec-dir", default=None,
                    help="directory holding nxv-enc/nxv-dec (default: build-ref/bin)")
    sp.add_argument("--extra-point", action="append",
                    help="one off-grid point, e.g. "
                         "'total=80:inset=768:frac=0.7:feather=0:tag=nofeather'")
    sp.add_argument("--no-anchors", action="store_true")
    sp.add_argument("--no-fvvdp", action="store_true",
                    help="skip FovVideoVDP (leaves the PSNR-shaped columns)")
    sp.add_argument("--fvvdp-device", default=None)
    sp.add_argument("--keep", action="store_true",
                    help="keep the composited YUV of every point (113 MB each)")
    sp.add_argument("--workers", type=int, default=2)
    sp.add_argument("--work", default=None)
    sp.add_argument("--note", default="")
    sp.add_argument("--out")
    sp.set_defaults(fn=cmd_spatial)

    spr = sub.add_parser("spatial-report", help="tables from a spatial run JSON")
    spr.add_argument("results")
    spr.add_argument("--out")
    spr.set_defaults(fn=cmd_spatial_report)

    t = sub.add_parser("selftest", help="tiny end-to-end run (ctest)")
    t.add_argument("--size", type=int, default=256)
    t.add_argument("--frames", type=int, default=8)
    t.set_defaults(fn=cmd_selftest)

    a = p.parse_args(argv)
    return a.fn(a)
