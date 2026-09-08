#!/usr/bin/env python3
"""Render a deterministic moving-camera stereo scene and encode it via FIFO.

The pixels are a pinhole-camera view of a scene (floor and rear wall).  Pose metadata is descriptive only; it is never passed to the
encoder, so this fixture exercises rendered camera motion without headset
pose or timewarp.
"""
import argparse
import errno
import hashlib
import json
import math
import os
from functools import lru_cache
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import time

import numpy as np


def pose(frame, fps):
    """Bounded, smooth camera motion in metres and degrees."""
    t = frame / fps
    return {
        "yaw_deg": 60.0 * math.sin(2.0 * math.pi * 0.80 * t),
        "pitch_deg": 25.0 * math.sin(2.0 * math.pi * 0.47 * t + 0.7),
        "position_m": [0.10 * math.sin(2.0 * math.pi * 0.37 * t + 0.4),
                       1.55 + 0.035 * math.sin(2.0 * math.pi * 0.91 * t),
                       0.12 * math.sin(2.0 * math.pi * 0.53 * t + 1.1)],
    }


def rotate_camera_rays(dx, dy, dz, yaw, pitch):
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    ry = cp * dy - sp * dz
    rz = sp * dy + cp * dz
    return cy * dx + sy * rz, ry, -sy * dx + cy * rz


def identity_center_ray_check():
    """Check the actual renderer transform against cardinal camera directions."""
    for yaw, pitch, expected in [(0, 0, (0, 0, 1)),
                                  (math.pi / 2, 0, (1, 0, 0)),
                                  (0, math.pi / 2, (0, -1, 0))]:
        if not np.allclose(rotate_camera_rays(0, 0, 1, yaw, pitch), expected, atol=1e-9):
            raise RuntimeError("camera basis check failed")


@lru_cache(maxsize=4)
def ray_grid(width, height):
    """Cache immutable normalized camera rays once per worker/geometry."""
    y, x = np.indices((height, width), dtype=np.float32)
    aspect = width / height
    xx = (2.0 * (x + 0.5) / width - 1.0) * math.tan(math.radians(95.0) / 2.0)
    yy = (1.0 - 2.0 * (y + 0.5) / height) * math.tan(math.radians(95.0) / 2.0) / aspect
    zz = np.ones_like(xx)
    norm = np.sqrt(xx * xx + yy * yy + zz * zz)
    rays = tuple(v / norm for v in (xx, yy, zz))
    for value in rays:
        value.setflags(write=False)
    return rays


def render_eye(width, height, eye, camera):
    """Return uint8 RGB for one eye using world-space ray intersections."""
    # 95 degree horizontal FOV, with a conventional camera +Z forward.
    dx, dy, dz = ray_grid(width, height)

    yaw = math.radians(camera["yaw_deg"])
    pitch = math.radians(camera["pitch_deg"])
    cy, sy = math.cos(yaw), math.sin(yaw)
    px, py, pz = rotate_camera_rays(dx, dy, dz, yaw, pitch)
    ox, oy, oz = camera["position_m"]
    # 64 mm inter-pupillary distance, offset in the camera's yaw-right axis.
    side = (-0.032 if eye == 0 else 0.032)
    ox = ox + side * cy
    oz = oz - side * sy

    rgb = np.empty((height, width, 3), dtype=np.float32)
    rgb[:] = (20.0, 28.0, 42.0)  # sky/ceiling fallback
    # Intersect y=0 floor and z=8 rear wall; select whichever is nearer.
    floor_t = np.divide(-oy, py, out=np.full_like(py, np.inf), where=py < -1e-5)
    wall_t = np.divide(8.0 - oz, pz, out=np.full_like(pz, np.inf), where=pz > 1e-5)
    use_floor = (floor_t > 0.0) & (floor_t < wall_t)
    use_wall = np.isfinite(wall_t) & (wall_t > 0.0) & ~use_floor
    hit_t = np.where(use_floor | use_wall, np.where(use_floor, floor_t, wall_t), 0.0)
    wx = ox + px * hit_t
    wy = oy + py * hit_t
    wz = oz + pz * hit_t
    # Keep texture math finite for rays that miss the bounded room.
    wx = np.nan_to_num(wx, nan=0.0, posinf=0.0, neginf=0.0)
    wy = np.nan_to_num(wy, nan=0.0, posinf=0.0, neginf=0.0)
    wz = np.nan_to_num(wz, nan=0.0, posinf=0.0, neginf=0.0)

    # Large crisp checker tiles plus thin world-space seams make motion clear.
    tile = (np.floor(wx / 0.48).astype(np.int32) ^ np.floor(wz / 0.48).astype(np.int32)) & 1
    floor_a = np.array([188, 194, 204], dtype=np.float32)
    floor_b = np.array([54, 66, 86], dtype=np.float32)
    floor_rgb = np.where(tile[..., None] == 0, floor_a, floor_b)
    seam = (np.mod(np.abs(wx), 0.48) < 0.018) | (np.mod(np.abs(wz), 0.48) < 0.018)
    floor_rgb[seam] = (232, 178, 54)
    wall_tile = (np.floor(wx / 0.72).astype(np.int32) + np.floor(wy / 0.72).astype(np.int32)) & 1
    wall_a = np.array([44, 116, 156], dtype=np.float32)
    wall_b = np.array([26, 48, 78], dtype=np.float32)
    wall_rgb = np.where(wall_tile[..., None] == 0, wall_a, wall_b)
    wall_seam = (np.mod(np.abs(wx), 0.72) < 0.022) | (np.mod(np.abs(wy), 0.72) < 0.022)
    wall_rgb[wall_seam] = (239, 104, 64)
    rgb[use_floor] = floor_rgb[use_floor]
    rgb[use_wall] = wall_rgb[use_wall]
    # A fixed bright sign on the rear wall provides a long-lived landmark.
    sign = use_wall & (np.abs(wx) < 0.9) & (wy > 1.2) & (wy < 2.2)
    rgb[sign] = (245, 220, 92)
    return np.clip(rgb, 0, 255).astype(np.uint8)


def render_yuv(width, height, camera):
    eyes = [render_eye(width // 2, height, eye, camera) for eye in range(2)]
    rgb = np.concatenate(eyes, axis=1)
    y = (0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]).astype(np.uint8)
    # Average each 2x2 RGB block before conversion for stable 4:2:0 chroma.
    rgb2 = rgb.reshape(height // 2, 2, width // 2, 2, 3).mean(axis=(1, 3))
    cb = np.clip(-0.168736 * rgb2[..., 0] - 0.331264 * rgb2[..., 1] + 0.5 * rgb2[..., 2] + 128, 0, 255).astype(np.uint8)
    cr = np.clip(0.5 * rgb2[..., 0] - 0.418688 * rgb2[..., 1] - 0.081312 * rgb2[..., 2] + 128, 0, 255).astype(np.uint8)
    return y.tobytes() + cb.tobytes() + cr.tobytes()


def render_frame(frame, width, height, fps):
    camera = pose(frame, fps)
    payload = render_yuv(width, height, camera)
    return frame, camera, payload, hashlib.sha256(payload).hexdigest()


def main():
    identity_center_ray_check()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--encoder", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--width", type=int, default=4352)
    ap.add_argument("--height", type=int, default=2176)
    ap.add_argument("--fps", type=float, default=240.0)
    ap.add_argument("--workers", type=int, default=1,
                    help="parallel CPU render workers (bounded to two frames each)")
    ap.add_argument("--qp", type=int, default=24)
    args = ap.parse_args()
    if args.frames < 1 or args.width < 2 or args.height < 2 or args.width % 128 or args.height % 64:
        ap.error("frames must be positive; width must be 128-aligned and height 64-aligned")
    if args.workers < 1:
        ap.error("--workers must be positive")
    if not math.isfinite(args.fps) or args.fps <= 0:
        ap.error("fps must be finite and positive")
    args.out = args.out.resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(args.encoder.resolve()), "--in", "FIFO", "--w", str(args.width), "--h", str(args.height),
           "--pix", "yuv420p", "--eyes", "2", "--frames", str(args.frames), "--qp", str(args.qp),
           "--inter", "--atlas", "--planar-prefer", "--entropy", "lite-fixed", "--out", str(args.out)]
    manifest = {"fixture": "camera-motion", "scope": "synthetic rendered camera motion, no headset pose/timewarp",
                "width": args.width, "height": args.height, "eyes": 2, "frames": args.frames, "fps": args.fps, "qp": args.qp, "workers": args.workers,
                "source_frames": [], "command": cmd}
    with tempfile.TemporaryDirectory(prefix="nx-camera-fixture-") as temp:
        fifo = Path(temp) / "source.yuv"
        os.mkfifo(fifo)
        actual_cmd = list(cmd); actual_cmd[actual_cmd.index("FIFO")] = str(fifo)
        env = dict(os.environ, NXVC_PLANAR_FORCE="1", NXVC_PLANAR_CONFIG="2,0")
        if os.environ.get("GPUflat") == "1":
            env["NXVC_ENC_PLANAR_GPU_FLAT"] = "1"
            manifest["gpu_flat_approximation"] = True
        log_path = args.out.with_suffix(".encode.log")
        with log_path.open("w") as log:
            proc = subprocess.Popen(actual_cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
            while True:
                if proc.poll() is not None:
                    raise RuntimeError("encoder exited before FIFO open; see " + str(log_path))
                try:
                    fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                    break
                except OSError as exc:
                    if exc.errno != errno.ENXIO:
                        raise
                    time.sleep(0.02)
            os.set_blocking(fd, True)
            with os.fdopen(fd, "wb") as stream, ProcessPoolExecutor(max_workers=args.workers) as pool:
                pending = {}
                next_submit = 0
                window = max(1, 2 * args.workers)
                while next_submit < min(args.frames, window):
                    pending[next_submit] = pool.submit(render_frame, next_submit, args.width,
                                                       args.height, args.fps)
                    next_submit += 1
                for frame in range(args.frames):
                    _, camera, payload, digest = pending.pop(frame).result()
                    stream.write(payload)
                    manifest["source_frames"].append({"frame": frame, "camera_pose": camera,
                                                       "sourcehash": digest})
                    if next_submit < args.frames:
                        pending[next_submit] = pool.submit(render_frame, next_submit, args.width,
                                                           args.height, args.fps)
                        next_submit += 1
            rc = proc.wait(timeout=300)
            if rc:
                raise RuntimeError("encoder failed with exit status %d; see %s" % (rc, log_path))
    manifest["distinct_source_frames"] = len({x["sourcehash"] for x in manifest["source_frames"]})
    manifest["streamhash"] = hashlib.sha256(args.out.read_bytes()).hexdigest()
    manifest["stream_bytes"] = args.out.stat().st_size
    manifest["encode_log"] = str(log_path)
    manifest["command"] = actual_cmd
    if manifest["distinct_source_frames"] != args.frames:
        raise RuntimeError("rendered source frame repetition")
    args.out.with_suffix(".manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(args.out, manifest["stream_bytes"], "bytes;", args.frames, "distinct rendered frames")


if __name__ == "__main__":
    main()
