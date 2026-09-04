#!/usr/bin/env python3
"""Grab real VR test material from a WiVRn NX session.

WiVRn NX already has everything needed for a first capture, with **no source
changes**: it ships a ``raw`` (uncompressed) encoder and a ``WIVRN_DUMP_VIDEO``
facility that writes that encoder's output straight to a file.  Combined with
``WIVRN_DUMP_TIMINGS`` and ``WIVRN_DUMP_HEAD`` you get frames plus poses.

This script does three things:

``plan``      print the exact recipe (env vars and config keys) for a session
``convert``   turn a ``WIVRN_DUMP_VIDEO`` NV12 dump into a harness sequence
``poses``     join the timing and head-pose CSVs into a harness pose log

See ``tools/quality/README.md`` for the full write-up, including the caveats
about foveation and where a proper dump hook would go.

This script never writes to the WiVRn NX tree.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nxq import yuv  # noqa: E402
from nxq.sequence import Sequence  # noqa: E402

WIVRN_PATH = "/run/media/nerdrx/Lex/claude/wivrn-nx"

PLAN = r"""
Capturing raw frames and poses from WiVRn NX
============================================

Nothing in the WiVRn NX tree needs to change for a first capture. The server
already has an uncompressed "raw" encoder and a bitstream dump that writes
whatever the encoder produced -- for the raw encoder, that is the frame itself.

1. Configure the server to use the raw encoder, and to keep the image linear.
   In the WiVRn NX configuration (the GUI's encoder settings, or the JSON
   config), set:

       "encoder": "raw"          the uncompressed encoder (8 bit only)
       "foveation_strength": 0   no continuous remap
       "foveation_adaptive": false
       render_scale / video scale so the stream extent is at least the render
       extent, which makes the foveation LUT an identity pass

   Also turn off FSR1 and motion smoothing: motion smoothing synthesises
   frames, which are not real render targets and must not enter a codec test.

2. Run the server with the three dump variables:

       WIVRN_DUMP_VIDEO=/path/to/scratch/dump \
       WIVRN_DUMP_TIMINGS=/path/to/scratch/timings.csv \
       WIVRN_DUMP_HEAD=/path/to/scratch/head.csv \
       wivrn-server

   This produces dump-0.yuv and dump-1.yuv (one per eye stream), each a
   sequence of NV12 frames at the encode resolution, plus the two CSVs.

   WIVRN_DUMP=list makes the server print the device names it can dump, if
   WIVRN_DUMP_HEAD turns out not to be the right variable on your build.

3. Connect the client and run the content for as long as you want. Raw frames
   are enormous (about 3 MB per eye per frame at 1440x1440 NV12, so roughly
   17 GB per eye per minute at 90 Hz) and they also go over the network, so
   capture short takes and keep them on the scratch volume.

4. Convert what you captured:

       wivrn_capture.py convert --in dump-0.yuv --w W --h H \
           --out $NXQ_SCRATCH/seq --name vrchat-left
       wivrn_capture.py poses --timings timings.csv --head head.csv \
           --out $NXQ_SCRATCH/seq/vrchat-left.poses.json

Caveats
-------
* The dumped frame is **post-foveation and post-colour-conversion**: the same
  compute pass does the foveation resample and the RGB->YCbCr conversion, so
  there is no un-foveated NV12 anywhere in the server. Step 1 degenerates the
  resample to an identity, which is the closest you get without a code change.
* The frame is BT.709 **full range** with an sRGB transfer already applied.
* head.csv logs the pose *track*, not one row per frame, so `poses` joins it to
  the frame indices in timings.csv by timestamp.
* There is no headless or fake-driver mode: a real client must be connected.

For a pre-foveation linear RGBA tap, or a proper per-frame pose sidecar, see
the "Where a dump hook belongs" section of tools/quality/README.md.
"""


# --- NV12 conversion -----------------------------------------------------


def nv12_frame_bytes(w: int, h: int) -> int:
    return w * h * 3 // 2


def nv12_to_frame(buf: bytes, w: int, h: int) -> yuv.Frame:
    """Split one NV12 frame into planar Y, Cb, Cr (4:2:0)."""
    y = np.frombuffer(buf, np.uint8, count=w * h).reshape(h, w)
    cbcr = np.frombuffer(buf, np.uint8, count=w * h // 2, offset=w * h).reshape(h // 2, w // 2, 2)
    return yuv.Frame(y, np.ascontiguousarray(cbcr[..., 0]), np.ascontiguousarray(cbcr[..., 1]))


def upsample_chroma(frame: yuv.Frame) -> yuv.Frame:
    """4:2:0 -> 4:4:4 by pixel replication (no invention of detail)."""
    def up(p: np.ndarray) -> np.ndarray:
        return np.repeat(np.repeat(p, 2, axis=0), 2, axis=1)
    return yuv.Frame(frame.y, up(frame.u), up(frame.v))


def convert_dump(
    src: str, w: int, h: int, outdir: str, name: str, pix_fmts: list[str],
    fps: float, limit: int | None = None, quiet: bool = False,
) -> list[Sequence]:
    fb = nv12_frame_bytes(w, h)
    size = os.path.getsize(src)
    total = size // fb
    if size % fb:
        print(f"[wivrn] warning: {src} is {size} bytes, not a whole number of "
              f"{fb}-byte {w}x{h} NV12 frames ({total} whole frames, "
              f"{size % fb} bytes left over). Check --w/--h against the encode "
              "resolution the server logged.", flush=True)
    if total == 0:
        raise SystemExit(f"{src} holds no complete {w}x{h} NV12 frames")
    n = min(total, limit) if limit else total
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    log(f"[wivrn] {src}: {total} NV12 frames at {w}x{h}, converting {n}")

    os.makedirs(outdir, exist_ok=True)
    writers = {}
    for pf in pix_fmts:
        fmt = yuv.Format(w, h, pf)
        path = os.path.join(outdir, f"{name}.{pf}.yuv")
        writers[pf] = (fmt, yuv.SequenceWriter(path, fmt), path)

    with open(src, "rb") as fh:
        for i in range(n):
            f420 = nv12_to_frame(fh.read(fb), w, h)
            f444 = None
            for pf, (fmt, wr, _) in writers.items():
                if pf == "yuv420p":
                    wr.write(f420)
                else:
                    if f444 is None:
                        f444 = upsample_chroma(f420)
                    wr.write(f444)
            if not quiet and (i % 50 == 0 or i == n - 1):
                log(f"[wivrn]   {i + 1}/{n}")

    seqs = []
    for pf, (fmt, wr, path) in writers.items():
        wr.close()
        seq = Sequence(f"{name}.{pf}", path, w, h, pf, fps, n, None,
                       f"wivrn:{os.path.basename(src)}", "mono")
        seq.save(os.path.join(outdir, f"{name}.{pf}.json"))
        seqs.append(seq)
        log(f"[wivrn] wrote {path}")
    return seqs


# --- pose joining --------------------------------------------------------


def _read_csv(path: str) -> tuple[list[str], list[list[str]]]:
    with open(path, newline="") as fh:
        rows = list(csv.reader(fh))
    if not rows:
        raise SystemExit(f"{path} is empty")
    return [c.strip().strip('"') for c in rows[0]], rows[1:]


def _find_col(header: list[str], *candidates: str) -> int | None:
    """Locate a column by any of several names, case-insensitively."""
    low = [c.lower() for c in header]
    for cand in candidates:
        c = cand.lower()
        if c in low:
            return low.index(c)
    for cand in candidates:
        c = cand.lower()
        for i, name in enumerate(low):
            if c in name:
                return i
    return None


def _quat_angle(a, b) -> float:
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return math.degrees(2.0 * math.acos(min(1.0, max(-1.0, dot))))


def join_poses(timings: str, head: str, out: str, event: str = "encode_begin",
               stream: int | None = None, quiet: bool = False) -> int:
    """Join the frame-indexed timing CSV to the head-pose track CSV.

    ``timings.csv`` is frame-indexed but has no pose; ``head.csv`` has poses but
    is a track sampled at its own rate.  For every frame in timings, the head
    row nearest in time is taken.
    """
    log = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    th, trows = _read_csv(timings)
    hh, hrows = _read_csv(head)

    c_event = _find_col(th, "event")
    c_frame = _find_col(th, "frame")
    c_time = _find_col(th, "time")
    c_stream = _find_col(th, "stream")
    if None in (c_event, c_frame, c_time):
        raise SystemExit(
            f"{timings}: expected 'event', 'frame' and 'time' columns, found {th}. "
            "The dump format may have changed; pass the right file or update this script."
        )

    c_ts = _find_col(hh, "timestamp", "now", "production_timestamp")
    c_ox = _find_col(hh, "orientation_0", "orientation[0]", "orientation.x", "qx")
    c_px = _find_col(hh, "position_0", "position[0]", "position.x", "x")
    if c_ts is None or c_ox is None:
        raise SystemExit(
            f"{head}: could not find a timestamp and an orientation column. "
            f"Columns present: {hh}. Update the column names in this script."
        )

    track = []
    for r in hrows:
        try:
            t = float(r[c_ts])
            q = [float(r[c_ox + i]) for i in range(4)]
            p = [float(r[c_px + i]) for i in range(3)] if c_px is not None else [0.0, 0.0, 0.0]
        except (ValueError, IndexError):
            continue
        track.append((t, q, p))
    if not track:
        raise SystemExit(f"{head}: no parseable pose rows")
    track.sort(key=lambda x: x[0])
    times = np.array([t for t, _, _ in track])
    log(f"[wivrn] {len(track)} pose samples, {times.min():.0f}..{times.max():.0f}")

    frames: dict[int, float] = {}
    for r in trows:
        try:
            ev = r[c_event].strip().strip('"')
            if ev != event:
                continue
            if stream is not None and c_stream is not None and int(r[c_stream]) != stream:
                continue
            frames[int(r[c_frame])] = float(r[c_time])
        except (ValueError, IndexError):
            continue
    if not frames:
        raise SystemExit(
            f"{timings}: no rows with event {event!r}. "
            f"Events present: {sorted({r[c_event].strip(chr(34)) for r in trows if len(r) > c_event})}"
        )
    log(f"[wivrn] {len(frames)} frames with event {event!r}")

    poses = []
    prev_q = None
    t0 = None
    for out_i, (fidx, ftime) in enumerate(sorted(frames.items())):
        j = int(np.abs(times - ftime).argmin())
        t, q, p = track[j]
        if t0 is None:
            t0 = ftime
        dt_s = (ftime - t0) / 1e9  # timings are nanoseconds
        av = 0.0
        if prev_q is not None and out_i > 0 and poses:
            dt = dt_s - poses[-1]["time_s"]
            if dt > 0:
                av = _quat_angle(prev_q, q) / dt
        poses.append({
            "frame": out_i,
            "source_frame_index": fidx,
            "time_s": dt_s,
            "position_xyz": p,
            "orientation_xyzw": q,
            "angular_velocity_deg_s": av,
            "pose_time_delta_ns": float(t - ftime),
        })
        prev_q = q

    yuv.write_pose_log(out, poses)
    worst = max(abs(p["pose_time_delta_ns"]) for p in poses) / 1e6
    log(f"[wivrn] wrote {out} ({len(poses)} frames); worst pose/frame time gap {worst:.2f} ms")
    if worst > 11.0:
        log("[wivrn] warning: the nearest pose sample is more than a frame away for at "
            "least one frame; the join may be unreliable")
    return len(poses)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    sub.add_parser("plan", help="print the capture recipe")

    c = sub.add_parser("convert", help="NV12 dump -> harness sequence")
    c.add_argument("--in", dest="inp", required=True, help="dump-N.yuv from WIVRN_DUMP_VIDEO")
    c.add_argument("--w", type=int, required=True, help="encode width the server logged")
    c.add_argument("--h", type=int, required=True, help="encode height")
    c.add_argument("--out", required=True, help="output directory (use nx-scratch)")
    c.add_argument("--name", required=True)
    c.add_argument("--pix", default="yuv420p", help="comma-separated output formats")
    c.add_argument("--fps", type=float, default=90.0)
    c.add_argument("--frames", type=int, default=None)
    c.add_argument("--quiet", action="store_true")

    p = sub.add_parser("poses", help="timings.csv + head.csv -> harness pose log")
    p.add_argument("--timings", required=True)
    p.add_argument("--head", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--event", default="encode_begin",
                   help="timing event that marks a frame (default encode_begin)")
    p.add_argument("--stream", type=int, default=None, help="restrict to one video stream")
    p.add_argument("--quiet", action="store_true")

    args = ap.parse_args(argv)
    if args.mode == "plan":
        print(PLAN.strip())
        print(f"\nWiVRn NX checkout expected at: {WIVRN_PATH}")
        return 0
    if args.mode == "convert":
        pix = [x.strip() for x in args.pix.split(",") if x.strip()]
        for x in pix:
            if x not in yuv.PIX_FMTS:
                ap.error(f"unsupported pix fmt {x!r}")
        convert_dump(args.inp, args.w, args.h, args.out, args.name, pix,
                     args.fps, args.frames, args.quiet)
        return 0
    join_poses(args.timings, args.head, args.out, args.event, args.stream, args.quiet)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
