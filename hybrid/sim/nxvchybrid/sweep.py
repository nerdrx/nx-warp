"""Sweep driver: material preparation, the job grid, and result aggregation.

Bit scaling.  The experiment targets the Pico 4 operating point of PAPER.md
2: two eyes at 2048x2048, 90 Hz, i.e. 8.39 Mpixel per frame and 755 Mpixel/s.
Running the simulator there would be wasteful, so it runs one square eye
buffer at ``--size`` (1024 by default) and every bitrate is converted by the
pixel ratio:

    bits_per_frame_test = Mbit * 1e6 / fps * (size^2 / (2 * 2048^2))

so "100 Mbit" in every table means "the bits per pixel that 100 Mbit/s buys
at 2 x 2048^2 x 90 Hz".  Quality numbers are the simulator's own resolution;
they are used for *comparison between configurations at a matched bit budget*,
which is the question, not as absolute predictions of headset PSNR.
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass

import numpy as np

from . import base as basemod, codec, hybrid as hyb
from .metrics import frame_psnr, ssim
from .panorama import Pose, pose_log, render_sequence
from .yuv import YuvSequence, write_sequence

DEVICE_PIXELS_PER_FRAME = 2 * 2048 * 2048
DEFAULT_TOTALS_MBIT = (50.0, 100.0, 150.0, 200.0)
DEFAULT_BASE_SCALES = (1.0, 0.75, 0.5)
DEFAULT_BASE_FRACS = (0.25, 0.40, 0.55, 0.70, 0.85)


def bits_per_frame(mbit: float, size: int, fps: float = 90.0) -> float:
    return mbit * 1e6 / fps * (size * size) / DEVICE_PIXELS_PER_FRAME


def test_bitrate_bps(mbit: float, size: int, fps: float = 90.0) -> float:
    return bits_per_frame(mbit, size, fps) * fps


def mbit_from_bits(bits: float, frames: int, size: int, fps: float = 90.0) -> float:
    """Inverse of :func:`bits_per_frame`, for reporting measured rates."""
    per_frame = bits / frames
    return per_frame * fps * DEVICE_PIXELS_PER_FRAME / (size * size) / 1e6


# --- material ------------------------------------------------------------


def prepare_material(outdir: str, size: int, frames: int, seed: int = 7, fov: float = 95.0,
                     pano_width: int = 4096, sprites: bool = True) -> tuple[str, str]:
    os.makedirs(outdir, exist_ok=True)
    yuv = os.path.join(outdir, f"vr-pano-{size}-{frames}f.yuv")
    posefile = os.path.join(outdir, f"vr-pano-{size}-{frames}f.poses.json")
    if os.path.exists(yuv) and os.path.exists(posefile):
        if os.path.getsize(yuv) == frames * (size * size * 3 // 2):
            return yuv, posefile
    poses = []
    with open(yuv, "wb") as fh:
        for f, p in render_sequence(size, frames, fov, pano_width, seed, sprites):
            fh.write(f.to_bytes())
            poses.append([p.yaw, p.pitch, p.roll])
    with open(posefile, "w") as fh:
        json.dump({"fov_deg": fov, "size": size, "poses": poses}, fh)
    return yuv, posefile


def load_poses(path: str) -> list[Pose]:
    with open(path) as fh:
        d = json.load(fh)
    return [Pose(*p) for p in d["poses"]]


# --- jobs ----------------------------------------------------------------


@dataclass
class Job:
    kind: str  # "hybrid" | "pure" | "hevc"
    total_mbit: float
    base_scale: float
    base_frac: float
    size: int
    frames: int
    fps: float
    seq: str
    poses: str
    workroot: str
    weights: tuple
    mv_radius: int

    @property
    def label(self) -> str:
        if self.kind == "hevc":
            return f"hevc-{self.total_mbit:g}"
        if self.kind == "pure":
            return f"pure-{self.total_mbit:g}"
        return f"hyb-s{self.base_scale:g}-f{self.base_frac:g}-{self.total_mbit:g}"


def _scaled_dims(size: int, scale: float) -> tuple[int, int]:
    d = int(round(size * scale))
    d -= d % 8  # keep the encoder and the 8x8 transform happy
    return d, d


def run_job(job: Job) -> dict:
    os.environ.setdefault("OMP_NUM_THREADS", "1")
    t0 = time.time()
    src = YuvSequence(job.seq, job.size, job.size)
    poses = load_poses(job.poses)
    work = os.path.join(job.workroot, job.label)
    os.makedirs(work, exist_ok=True)
    total_bps = test_bitrate_bps(job.total_mbit, job.size, job.fps)

    if job.kind == "hevc":
        r = basemod.encode_decode_base(
            job.seq, job.size, job.size, job.frames, job.size, job.size,
            total_bps, work, job.fps, tag="anchor",
        )
        rec = r.sequence()
        py, pw, ss = hyb.measure_sequence(src, rec)
        out = {
            "label": job.label, "mode": "hevc", "kind": "hevc",
            "total_mbit": job.total_mbit, "base_scale": 1.0, "base_frac": 1.0,
            "base_bits": r.bitstream_bits, "enh_bits": 0.0,
            "total_bits": r.bitstream_bits,
            "measured_mbit": mbit_from_bits(r.bitstream_bits, job.frames, job.size, job.fps),
            "psnr_y": py, "psnr_w": pw, "ssim_y": ss,
            "encoder": r.encoder, "frames": job.frames, "size": job.size,
            "seconds": time.time() - t0,
        }
        return out

    if job.kind == "pure":
        target = bits_per_frame(job.total_mbit, job.size, job.fps) * job.frames
        res = hyb.run_enhancement(
            src, None, poses, target, mode="pure", label=job.label,
            base_scale=0.0, base_bits=0, fps=job.fps,
            weights=job.weights, mv_radius=job.mv_radius,
        )
    else:
        bw, bh = _scaled_dims(job.size, job.base_scale)
        base_bps = total_bps * job.base_frac
        br = basemod.encode_decode_base(
            job.seq, job.size, job.size, job.frames, bw, bh, base_bps, work,
            job.fps, tag="base",
        )
        total_bits = bits_per_frame(job.total_mbit, job.size, job.fps) * job.frames
        enh_target = max(0.0, total_bits - br.bitstream_bits)
        res = hyb.run_enhancement(
            src, br.sequence(), poses, enh_target, mode="hybrid", label=job.label,
            base_scale=job.base_scale, base_bits=br.bitstream_bits, fps=job.fps,
            weights=job.weights, mv_radius=job.mv_radius,
        )

    d = res.to_json()
    d["kind"] = job.kind
    d["total_mbit"] = job.total_mbit
    d["base_frac"] = job.base_frac
    d["measured_mbit"] = mbit_from_bits(res.total_bits, job.frames, job.size, job.fps)
    d["base_mbit"] = mbit_from_bits(res.base_bits, job.frames, job.size, job.fps)
    d["size"] = job.size
    d["seconds"] = time.time() - t0
    return d


def build_jobs(
    seq: str, poses: str, size: int, frames: int, workroot: str,
    totals=DEFAULT_TOTALS_MBIT, scales=DEFAULT_BASE_SCALES, fracs=DEFAULT_BASE_FRACS,
    fps: float = 90.0, weights=codec.WEIGHTS_2BIT, mv_radius: int = 6,
) -> list[Job]:
    jobs: list[Job] = []
    common = dict(size=size, frames=frames, fps=fps, seq=seq, poses=poses,
                  workroot=workroot, weights=tuple(weights), mv_radius=mv_radius)
    for t in totals:
        jobs.append(Job("hevc", t, 1.0, 1.0, **common))
        jobs.append(Job("pure", t, 0.0, 0.0, **common))
        for s in scales:
            for fr in fracs:
                jobs.append(Job("hybrid", t, s, fr, **common))
    return jobs
