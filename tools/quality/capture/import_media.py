#!/usr/bin/env python3
"""Import test material from PNG/JPEG sequences or from video files.

Two sources:

**Image sequences** -- a directory or a glob of PNG/JPEG frames, sorted
naturally, converted to planar YUV with the harness's own BT.709 limited-range
conversion (Pillow only decodes the files).  If Pillow is missing, ffmpeg is
used instead.

**Video files** -- anything ffmpeg can decode, converted straight to raw planar
YUV by ffmpeg.

Both write the same sidecar as the synthetic generator, so ``compare.py`` and
``foveated_metrics.py`` take them without extra flags.

Examples
--------
::

    python3 capture/import_media.py images --in ~/shots/'*.png' \\
        --out $NXQ_SCRATCH/seq --name vrchat-stills --pix yuv444p,yuv420p

    python3 capture/import_media.py video --in capture.mp4 \\
        --out $NXQ_SCRATCH/seq --name alyx-60s --frames 300 --fps 90
"""

from __future__ import annotations

import argparse
import glob as globmod
import json
import os
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nxq import cpu, yuv  # noqa: E402
from nxq.ffmpeg import FFmpegError, probe  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".ppm", ".webp")


def natural_key(path: str):
    """Sort frame_2.png before frame_10.png."""
    base = os.path.basename(path)
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", base)]


def find_images(spec: str) -> list[str]:
    if os.path.isdir(spec):
        files = [os.path.join(spec, f) for f in os.listdir(spec)
                 if f.lower().endswith(IMAGE_EXTS)]
    else:
        files = [f for f in globmod.glob(spec) if f.lower().endswith(IMAGE_EXTS)]
    return sorted(files, key=natural_key)


def load_rgb(path: str) -> np.ndarray:
    """Decode one image to an (H, W, 3) uint8 RGB array."""
    try:
        from PIL import Image
    except ImportError as exc:  # pragma: no cover - depends on the machine
        raise RuntimeError(
            "Pillow is not installed, so image sequences cannot be decoded here. "
            "Either install Pillow, or import the frames through ffmpeg with the "
            "'video' subcommand (ffmpeg reads numbered image sequences too, "
            "e.g. --in 'frame_%04d.png')."
        ) from exc
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


def import_images(
    files: list[str], outdir: str, name: str, pix_fmts: list[str], fps: float,
    layout: str, limit: int | None = None, quiet: bool = False,
) -> list[Sequence]:
    if not files:
        raise SystemExit("no images matched")
    if limit:
        files = files[:limit]
    first = load_rgb(files[0])
    h, w = first.shape[:2]
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    log(f"[import] {len(files)} images, {w}x{h}, from {os.path.dirname(files[0]) or '.'}")

    os.makedirs(outdir, exist_ok=True)
    writers = {}
    for pf in pix_fmts:
        fmt = yuv.Format(w, h, pf)
        path = os.path.join(outdir, f"{name}.{pf}.yuv")
        writers[pf] = (fmt, yuv.SequenceWriter(path, fmt), path)

    for i, f in enumerate(files):
        rgb = first if i == 0 else load_rgb(f)
        if rgb.shape[:2] != (h, w):
            raise SystemExit(
                f"{f} is {rgb.shape[1]}x{rgb.shape[0]} but the first frame is {w}x{h}; "
                "all frames in a sequence must share a size"
            )
        f444 = yuv.rgb_to_yuv444(rgb)
        for pf, (fmt, wr, _) in writers.items():
            wr.write(yuv.to_format(f444, fmt))
        if not quiet and (i % 25 == 0 or i == len(files) - 1):
            log(f"[import]   {i + 1}/{len(files)}")

    seqs = []
    for pf, (fmt, wr, path) in writers.items():
        wr.close()
        seq = Sequence(f"{name}.{pf}", path, w, h, pf, fps, len(files), None,
                       f"images:{len(files)}", layout)
        seq.save(os.path.join(outdir, f"{name}.{pf}.json"))
        seqs.append(seq)
        log(f"[import] wrote {path}")
    return seqs


def probe_video(path: str) -> tuple[int, int, float, int]:
    """(width, height, fps, nb_frames) via ffprobe."""
    import shutil
    exe = shutil.which("ffprobe")
    if not exe:
        raise FFmpegError("ffprobe not found; it ships with ffmpeg")
    p = cpu.run([exe, "-v", "error", "-select_streams", "v:0", "-show_streams",
                 "-of", "json", path], check=False)
    if p.returncode != 0:
        raise FFmpegError(f"ffprobe failed on {path}: {p.stderr.strip()}")
    st = json.loads(p.stdout)["streams"][0]
    num, den = (st.get("avg_frame_rate") or "0/1").split("/")
    fps = float(num) / float(den) if float(den) else 0.0
    try:
        n = int(st.get("nb_frames", 0))
    except (TypeError, ValueError):
        n = 0
    return int(st["width"]), int(st["height"]), fps, n


def import_video(
    src: str, outdir: str, name: str, pix_fmts: list[str], fps: float | None,
    layout: str, limit: int | None, start: float, size: str | None, quiet: bool = False,
) -> list[Sequence]:
    caps = probe()
    if not caps.available:
        raise SystemExit("ffmpeg is required to import video")
    w, h, src_fps, n = probe_video(src)
    if size:
        w, h = (int(v) for v in size.lower().split("x"))
    out_fps = fps or src_fps or 90.0
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    log(f"[import] {src}: {w}x{h}, source {src_fps:g} fps, {n or 'unknown'} frames "
        f"-> tagging as {out_fps:g} fps")

    os.makedirs(outdir, exist_ok=True)
    seqs = []
    for pf in pix_fmts:
        fmt = yuv.Format(w, h, pf)
        path = os.path.join(outdir, f"{name}.{pf}.yuv")
        args = [caps.ffmpeg, "-hide_banner", "-nostdin", "-y", "-threads", str(cpu.threads())]
        if start:
            args += ["-ss", str(start)]
        args += ["-i", src]
        if limit:
            args += ["-frames:v", str(limit)]
        if size:
            args += ["-s", f"{w}x{h}"]
        args += ["-pix_fmt", pf, "-f", "rawvideo", path]
        p = cpu.run(args, check=False)
        if p.returncode != 0:
            tail = "\n".join((p.stderr or "").strip().splitlines()[-12:])
            raise SystemExit(f"ffmpeg failed importing {src}:\n{tail}")
        frames = fmt.frame_count(path)
        seq = Sequence(f"{name}.{pf}", path, w, h, pf, out_fps, frames, None,
                       f"video:{os.path.basename(src)}", layout)
        seq.save(os.path.join(outdir, f"{name}.{pf}.json"))
        seqs.append(seq)
        log(f"[import] wrote {path} ({frames} frames)")
    return seqs


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--out", required=True, help="output directory (use nx-scratch)")
    common.add_argument("--name", required=True, help="sequence base name")
    common.add_argument("--pix", default="yuv444p", help="comma-separated pixel formats")
    common.add_argument("--fps", type=float, default=None,
                        help="frame rate to tag the sequence with (default 90 for images)")
    common.add_argument("--layout", default="mono", choices=("sbs", "mono"),
                        help="stereo layout of the source frames")
    common.add_argument("--frames", type=int, default=None, help="import at most N frames")
    common.add_argument("--quiet", action="store_true")

    i = sub.add_parser("images", parents=[common], help="import a PNG/JPEG sequence")
    i.add_argument("--in", dest="inp", required=True,
                   help="a directory, or a glob such as 'shots/*.png'")

    v = sub.add_parser("video", parents=[common], help="import a video file via ffmpeg")
    v.add_argument("--in", dest="inp", required=True)
    v.add_argument("--start", type=float, default=0.0, help="seek this many seconds in")
    v.add_argument("--size", default=None, help="rescale to WxH")

    args = ap.parse_args(argv)
    pix_fmts = [p.strip() for p in args.pix.split(",") if p.strip()]
    for p in pix_fmts:
        if p not in yuv.PIX_FMTS:
            ap.error(f"unsupported pix fmt {p!r}, want {yuv.PIX_FMTS}")

    if args.mode == "images":
        files = find_images(args.inp)
        if not files:
            ap.error(f"no images matched {args.inp!r} "
                     f"(looked for {', '.join(IMAGE_EXTS)}); quote the glob so the "
                     "shell does not expand it")
        seqs = import_images(files, args.out, args.name, pix_fmts, args.fps or 90.0,
                             args.layout, args.frames, args.quiet)
    else:
        seqs = import_video(args.inp, args.out, args.name, pix_fmts, args.fps, args.layout,
                            args.frames, args.start, args.size, args.quiet)

    print(f"[import] done: {len(seqs)} sequence(s) in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
