#!/usr/bin/env python3
"""Write a ``WIVRN_RAW_DUMP`` directory without a headset.

`ingest_wivrn.py` is the only thing standing between a recorded session and
every gate number in the project, and it is written against a format nobody can
produce on demand: reproducing a real dump needs a headset, a client, a running
compositor and a few gigabytes.  So this writes one -- **byte for byte in
`raw_dump.cpp`'s format** -- out of material the harness already has.

Two uses, and both matter:

* the end-to-end test (`test_ingest_wivrn.py`) generates a dump whose contents
  it knows exactly, ingests it, and asserts the round trip is lossless in the
  pixels and correct in the poses;
* a person about to record a real session can rehearse the whole pipeline --
  ingest, manifest, gates -- in a minute, and find out that a path is wrong
  *before* spending ten seconds of head rotation and 15 GB of disk on it.

The format is `raw_dump.cpp` and nothing else::

    stream<i>.yuv         appended NV12 (or P010) frames, no container
    stream<i>.jsonl       one object per frame: frame, stream, display_time_ns,
                          width, height, alpha, pose[2], fov[2], foveation[2]
    stream<i>-info.json   stream, eye, width, height, bit_depth, pixel_format,
                          chroma, planes, frame_bytes, note

Usage::

    # from a v2 synthetic sequence (an sbs .json sidecar): splits it back apart
    python3 tools/quality/capture/fake_raw_dump.py from-sequence \\
        --seq $NXQ_SCRATCH/seq/vr-mixed-256-v2.yuv420p.json --out /tmp/dump

    # from nothing: a smooth analytic world under a pure yaw rotation, which is
    # the pair the convention test measures
    python3 tools/quality/capture/fake_raw_dump.py rotation \\
        --out /tmp/dump --eye 256 --frames 2 --yaw-step 2.0
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nxq import yuv  # noqa: E402


# ------------------------------------------------------------- the raw format


def planar_to_nv12(f: yuv.Frame) -> bytes:
    """Planar 4:2:0 -> NV12: Y, then Cb and Cr interleaved."""
    cbcr = np.empty((f.u.shape[0], f.u.shape[1], 2), dtype=np.uint8)
    cbcr[..., 0] = f.u
    cbcr[..., 1] = f.v
    return f.y.tobytes() + cbcr.tobytes()


def planar_to_p010(f: yuv.Frame) -> bytes:
    """Planar 8-bit 4:2:0 -> P010: 10-bit values in the TOP 10 bits of u16 LE.

    ``v10 = v8 * 4`` is the exact inverse of the ingester's video-range
    down-convert, so an 8 -> 10 -> 8 round trip through here is lossless and a
    test can tell a broken shift apart from a lossy one.
    """
    def up(p):
        return ((p.astype(np.uint16) * 4) << 6).astype("<u2")
    cbcr = np.empty((f.u.shape[0], f.u.shape[1], 2), dtype="<u2")
    cbcr[..., 0] = up(f.u)
    cbcr[..., 1] = up(f.v)
    return up(f.y).tobytes() + cbcr.tobytes()


def quat_yaw(deg: float) -> list[float]:
    """Rotation about +Y (up) as (x, y, z, w) -- OpenXR's order and handedness."""
    h = math.radians(deg) * 0.5
    return [0.0, math.sin(h), 0.0, math.cos(h)]


def info_json(stream: int, w: int, h: int, depth: int) -> dict:
    """Exactly the object `raw_dump.cpp`'s constructor writes."""
    return {
        "stream": stream,
        "eye": "left" if stream == 0 else "right",
        "width": w,
        "height": h,
        "bit_depth": depth,
        "pixel_format": "p010le" if depth == 10 else "nv12",
        "chroma": "4:2:0",
        "planes": [
            {"name": "Y", "width": w, "height": h, "components": 1},
            {"name": "CbCr", "width": w // 2, "height": h // 2, "components": 2},
        ],
        "frame_bytes": w * h * 3 // 2 * (2 if depth == 10 else 1),
        "note": "encoder input, foveated; see the foveation runs in the .jsonl "
                "to map back to render space",
    }


def jsonl_record(stream: int, frame: int, w: int, h: int, t_ns: int,
                 quat, pos, fov_tan, foveation) -> dict:
    """Exactly the object `raw_dump.cpp`'s write() emits, key for key."""
    return {
        "frame": frame,
        "stream": stream,
        "display_time_ns": t_ns,
        "width": w,
        "height": h,
        "alpha": False,
        "pose": [{"orientation": list(quat), "position": list(pos)} for _ in range(2)],
        "fov": [{"tan_left": fov_tan[0], "tan_right": fov_tan[1],
                 "tan_up": fov_tan[2], "tan_down": fov_tan[3]} for _ in range(2)],
        "foveation": [{"x": list(foveation[0]), "y": list(foveation[1])}
                      for _ in range(2)],
    }


class DumpWriter:
    """Append frames and their pose records, the way the tap does."""

    def __init__(self, out: str, w: int, h: int, depth: int, streams: int = 2):
        os.makedirs(out, exist_ok=True)
        self.out, self.w, self.h, self.depth = out, w, h, depth
        self.streams = streams
        self.n = 0
        for s in range(streams):
            with open(os.path.join(out, f"stream{s}-info.json"), "w") as fh:
                json.dump(info_json(s, w, h, depth), fh)
                fh.write("\n")
        self._yuv = [open(os.path.join(out, f"stream{s}.yuv"), "wb") for s in range(streams)]
        self._jsonl = [open(os.path.join(out, f"stream{s}.jsonl"), "w") for s in range(streams)]

    def write(self, eyes: list[yuv.Frame], quat, pos, fov_tan, foveation,
              t_ns: int | None = None, frame: int | None = None) -> None:
        frame = self.n if frame is None else frame
        t_ns = (frame * 11_111_111) if t_ns is None else t_ns
        pack = planar_to_p010 if self.depth == 10 else planar_to_nv12
        for s in range(self.streams):
            self._yuv[s].write(pack(eyes[min(s, len(eyes) - 1)]))
            rec = jsonl_record(s, frame, self.w, self.h, t_ns, quat, pos,
                               fov_tan, foveation)
            self._jsonl[s].write(json.dumps(rec) + "\n")
        self.n += 1

    def close(self) -> None:
        for fh in self._yuv + self._jsonl:
            fh.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ------------------------------------------------- an analytic band-limited world


def world_sample(d: np.ndarray) -> np.ndarray:
    """A smooth scalar field on the sphere: a short sum of low-frequency waves.

    The same field as ``tests/ref/test_warp_convention.cpp``, and for the same
    reason: it is band-limited *by construction*, so a geometrically correct
    warp reproduces it to within the predictor's own filter error and a
    convention error has nowhere to hide.  On the generator's panorama even the
    ideal float warp only reaches 24 dB, and a test written there would pass
    with the rotation applied backwards (`docs/WARP-AUDIT.md`).
    """
    n = np.array([[0.8020, 0.2673, 0.5345], [-0.3244, 0.8111, 0.4867],
                  [0.4558, -0.5698, 0.6838], [-0.6547, -0.3273, 0.6813],
                  [0.1690, 0.9296, -0.3273], [0.5774, -0.5774, -0.5774]])
    w = np.array([3.1, 4.7, 6.3, 8.1, 5.2, 2.4])
    a = np.array([34.0, 22.0, 15.0, 9.0, 18.0, 26.0])
    p = np.array([0.31, 1.72, 2.55, 0.94, 3.61, 1.18])
    v = np.full(d.shape[:-1], 128.0)
    for k in range(6):
        v += a[k] * np.sin(w[k] * (d @ n[k]) + p[k])
    return np.clip(v, 0.0, 255.0)


def rot_y(deg: float) -> np.ndarray:
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])


def render_view(eye: int, fov_deg: float, yaw_deg: float) -> yuv.Frame:
    """One view of the analytic world.

    Camera space is OpenXR's: x right, y up, -z forward, sample centres at
    ``+0.5``, row 0 at the top.  ``R`` is camera-to-world, so the world-space
    ray of a pixel is ``R @ d_camera`` -- which is precisely the convention the
    pose log claims and `derive_homography()` implements.
    """
    t = math.tan(math.radians(fov_deg) * 0.5)
    i = (np.arange(eye) + 0.5) / eye * 2.0 - 1.0
    x = i * t
    y = -i * t                      # row 0 is the top: +y is up, rows run down
    gx, gy = np.meshgrid(x, y)
    d = np.stack([gx, gy, -np.ones_like(gx)], axis=-1)
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    d = d @ rot_y(yaw_deg).T        # (R @ d^T)^T
    luma = np.rint(world_sample(d)).astype(np.uint8)
    flat = np.full((eye // 2, eye // 2), 128, dtype=np.uint8)
    return yuv.Frame(luma, flat, flat.copy())


# -------------------------------------------------------------------- commands


def cmd_from_sequence(args) -> int:
    """Split an sbs harness sequence back into two eye streams."""
    with open(args.seq) as fh:
        side = json.load(fh)
    base = os.path.dirname(os.path.abspath(args.seq))
    raw = side["path"] if os.path.isabs(side["path"]) else os.path.join(base, side["path"])
    W, H, pix = int(side["width"]), int(side["height"]), side["pix_fmt"]
    if pix != "yuv420p":
        print(f"{args.seq}: the tap only ever writes 4:2:0; pass a yuv420p sidecar",
              file=sys.stderr)
        return 1
    eyes = 2 if side.get("layout") == "sbs" else 1
    w = W // eyes
    fmt = yuv.Format(W, H, pix)

    poses = None
    pl = side.get("pose_log")
    if pl:
        pl = pl if os.path.isabs(pl) else os.path.join(base, pl)
        if os.path.exists(pl):
            with open(pl) as fh:
                poses = json.load(fh)

    fov = poses["fov_deg"] if poses else {"h": 95.0, "v": 95.0}
    th = math.tan(math.radians(fov["h"]) * 0.5)
    tv = math.tan(math.radians(fov["v"]) * 0.5)
    fov_tan = (-th, th, tv, -tv)
    fps = float(poses["fps"]) if poses else float(side.get("fps", 90.0))
    fovea = ([w], [H]) if args.foveation == "identity" else ([1, w - 2, 1], [1, H - 2, 1])

    n = fmt.frame_count(raw)
    if args.frames:
        n = min(n, args.frames)
    with DumpWriter(args.out, w, H, args.depth, streams=2) as dw:
        for i in range(n):
            f = yuv.read_frame(raw, fmt, i)
            per_eye = [yuv.Frame(f.y[:, e * w:(e + 1) * w],
                                 f.u[:, e * w // 2:(e + 1) * w // 2],
                                 f.v[:, e * w // 2:(e + 1) * w // 2])
                       for e in range(eyes)]
            fr = poses["frames"][i] if poses else None
            q = fr["orientation_xyzw"] if fr else [0.0, 0.0, 0.0, 1.0]
            p = fr.get("position_xyz", [0.0, 0.0, 0.0]) if fr else [0.0, 0.0, 0.0]
            dw.write(per_eye, q, p, fov_tan, fovea,
                     t_ns=int(round(i / fps * 1e9)))
    print(f"[fake] {args.out}: {n} frames, {w}x{H} per eye, "
          f"{'p010le' if args.depth == 10 else 'nv12'}")
    return 0


def cmd_rotation(args) -> int:
    """A pure-yaw pair (or chain) of the analytic world.  The convention case."""
    fov_tan = (-math.tan(math.radians(args.fov) * 0.5),
               math.tan(math.radians(args.fov) * 0.5),
               math.tan(math.radians(args.fov) * 0.5),
               -math.tan(math.radians(args.fov) * 0.5))
    fovea = ([args.eye], [args.eye])
    with DumpWriter(args.out, args.eye, args.eye, args.depth, streams=2) as dw:
        for i in range(args.frames):
            yaw = i * args.yaw_step
            f = render_view(args.eye, args.fov, yaw)
            dw.write([f, f], quat_yaw(yaw), [0.0, 0.0, 0.0], fov_tan, fovea,
                     t_ns=int(round(i / args.fps * 1e9)))
    print(f"[fake] {args.out}: {args.frames} frames, {args.eye}x{args.eye} per eye, "
          f"pure yaw {args.yaw_step} deg/frame, fov {args.fov} deg")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("from-sequence", help="split an sbs harness sequence into a dump")
    a.add_argument("--seq", required=True, help="a yuv420p sequence .json sidecar")
    a.add_argument("--out", required=True)
    a.add_argument("--depth", type=int, default=8, choices=(8, 10))
    a.add_argument("--frames", type=int, default=0)
    a.add_argument("--foveation", default="identity", choices=("identity", "on"),
                   help="'on' writes non-degenerate runs, which the ingester refuses")
    a.set_defaults(fn=cmd_from_sequence)

    b = sub.add_parser("rotation", help="a pure-rotation pair of a band-limited world")
    b.add_argument("--out", required=True)
    b.add_argument("--eye", type=int, default=256)
    b.add_argument("--frames", type=int, default=2)
    b.add_argument("--yaw-step", type=float, default=2.0, help="degrees per frame")
    b.add_argument("--fov", type=float, default=95.0)
    b.add_argument("--fps", type=float, default=90.0)
    b.add_argument("--depth", type=int, default=8, choices=(8, 10))
    b.set_defaults(fn=cmd_rotation)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
