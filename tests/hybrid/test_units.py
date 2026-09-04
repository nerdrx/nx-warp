#!/usr/bin/env python3
"""Unit-level checks for the hybrid simulator that need no ffmpeg.

Exit codes: 0 pass, 1 fail, 77 skip (numpy absent).
"""

from __future__ import annotations

import math
import os
import sys

try:
    import numpy as np
except ImportError:
    print("SKIP: numpy is not available")
    sys.exit(77)

sys.path.insert(
    0,
    os.environ.get(
        "NXVCH_SIM_DIR",
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hybrid", "sim"),
    ),
)

from nxvchybrid import codec, warp as W  # noqa: E402
from nxvchybrid.metrics import psnr, ssim  # noqa: E402
from nxvchybrid.panorama import Pose, intrinsics, pose_log, render_frame, build_panorama  # noqa: E402
from nxvchybrid.sweep import bits_per_frame, mbit_from_bits  # noqa: E402

FAILED: list[str] = []


def check(cond: bool, msg: str) -> None:
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILED.append(msg)


def test_dct_roundtrip() -> None:
    rng = np.random.default_rng(0)
    plane = rng.random((64, 64), dtype=np.float32) * 255
    back = codec.idct(codec.fdct(plane), 64, 64)
    check(np.abs(plane - back).max() < 1e-2, "8x8 DCT round trip is lossless")
    # orthonormality: Parseval
    c = codec.fdct(plane - plane.mean())
    e_spatial = float(((plane - plane.mean()) ** 2).sum())
    check(abs(float((c**2).sum()) - e_spatial) / e_spatial < 1e-4,
          "DCT is orthonormal (Parseval holds)")


def test_quantiser() -> None:
    step = 10.0
    c = np.array([[[[0.0, 6.5, 6.8, -6.8, 25.0, 0, 0, 0]] * 8]], dtype=np.float32)
    q = codec.quantise(c, step)
    # dead zone: |c| must reach (1 - 1/3) * step = 6.667 before it survives
    check(q[0, 0, 0, 1] == 0 and q[0, 0, 0, 2] == 1,
          "dead-zone quantiser kills 6.5/10 and keeps 6.8/10")
    check(q[0, 0, 0, 3] == -1, "dead-zone quantiser is sign-symmetric")
    check(abs(codec.qstep(4) - 1.0) < 1e-9 and abs(codec.qstep(10) - 2.0) < 1e-9,
          "QP ladder: step(4) = 1 and +6 QP doubles the step")


def test_bit_model() -> None:
    zero = np.zeros((16, 16, 8, 8), dtype=np.int32)
    b0 = codec.plane_bits(zero)
    check(b0 < 1200, f"an all-zero plane costs almost nothing ({b0:.0f} bits)")
    rng = np.random.default_rng(1)
    dense = rng.integers(-8, 9, size=(16, 16, 8, 8)).astype(np.int32)
    b1 = codec.plane_bits(dense)
    check(b1 > 40 * b0, "a dense plane costs far more than an empty one")
    check(codec.plane_bits_uniform(dense) > 0, "the pessimistic estimator runs")
    # monotone in QP
    coeff = rng.normal(0, 40, size=(16, 16, 8, 8)).astype(np.float32)
    bits = [codec.plane_bits(codec.quantise(coeff, codec.qstep(qp)))
            for qp in (10, 20, 30, 40, 50)]
    check(all(bits[i] > bits[i + 1] for i in range(len(bits) - 1)),
          "modelled bits fall monotonically with QP")
    check(codec.exp_golomb_bits(0) == 1 and codec.exp_golomb_bits(1) == 3,
          "signed Exp-Golomb lengths")


def test_warp_identity_and_direction() -> None:
    K = intrinsics(128)
    R = Pose(0.0, 0.0, 0.0).matrix()
    rng = np.random.default_rng(2)
    img = (rng.random((128, 128), dtype=np.float32) * 255).astype(np.float32)
    H = W.homography(K, R, R)
    check(psnr(img, W.warp(img, H, "bilinear")) > 60,
          "warping by the identity homography is a no-op")

    # A pure yaw of a synthetic panorama must be predicted better by the warp
    # than by a plain copy, and better than by the reversed homography.
    pano = build_panorama(1024, 512, seed=3).astype(np.uint8)
    Kf = intrinsics(128)
    Kinv = np.linalg.inv(Kf)
    p0 = Pose(0.0, 0.02, 0.0)
    p1 = Pose(math.radians(3.0), 0.02, 0.0)
    f0 = render_frame(pano, p0, 128, Kinv, None, 0, 2)
    f1 = render_frame(pano, p1, 128, Kinv, None, 1, 2)
    Hf = W.homography(Kf, p0.matrix(), p1.matrix())
    Hr = W.homography(Kf, p1.matrix(), p0.matrix())
    good = psnr(f1.y, W.warp(f0.y, Hf, "catrom"))
    copy = psnr(f1.y, f0.y)
    rev = psnr(f1.y, W.warp(f0.y, Hr, "catrom"))
    check(good > copy + 3.0, f"pose warp beats a copy by >3 dB ({good:.1f} vs {copy:.1f})")
    check(good > rev + 3.0, f"the homography direction is right ({good:.1f} vs {rev:.1f})")


def test_upsample() -> None:
    rng = np.random.default_rng(4)
    small = (rng.random((32, 32), dtype=np.float32) * 255).astype(np.float32)
    up = W.upsample(small, 64, 64)
    check(up.shape == (64, 64), "upsample reaches the requested shape")
    check(W.upsample(small, 32, 32) is small, "upsample is a no-op at 1:1")
    flat = np.full((16, 16), 77.0, dtype=np.float32)
    check(np.abs(W.upsample(flat, 64, 64) - 77.0).max() < 1e-3,
          "Catmull-Rom upsample preserves a constant")


def test_classifier() -> None:
    flat = np.full((64, 64), 100.0, dtype=np.float32)
    rng = np.random.default_rng(5)
    noise = (rng.random((64, 64), dtype=np.float32) * 255).astype(np.float32)
    text = np.full((64, 64), 18.0, dtype=np.float32)
    text[::4, :] = 238.0
    img = np.zeros((64, 192), dtype=np.float32)
    img[:, 0:64] = flat
    img[:, 64:128] = noise
    img[:, 128:192] = text
    cls = codec.classify_tiles(img, 64)
    check(codec.CLASSES[cls[0, 0]] == "flat", "a constant tile classifies as flat")
    check(codec.CLASSES[cls[0, 1]] == "texture", "white noise classifies as texture")
    check(codec.CLASSES[cls[0, 2]] in ("text", "edge"),
          "a bimodal high-contrast tile classifies as text or edge")


def test_metrics() -> None:
    rng = np.random.default_rng(6)
    a = (rng.random((64, 64), dtype=np.float32) * 255).astype(np.float32)
    check(psnr(a, a) > 90, "PSNR of identical images is capped high")
    check(abs(ssim(a, a) - 1.0) < 1e-4, "SSIM of identical images is 1")
    b = np.clip(a + 10.0, 0, 255)
    check(ssim(a, b) < 1.0, "SSIM falls when the images differ")


def test_bit_scaling() -> None:
    # 100 Mbit at 2 x 2048^2 x 90 Hz, expressed at 1024^2
    bpf = bits_per_frame(100.0, 1024)
    check(abs(bpf - 100e6 / 90 * (1024 * 1024) / (2 * 2048 * 2048)) < 1e-6,
          "bits-per-frame scaling matches the device pixel ratio")
    check(abs(mbit_from_bits(bpf * 90, 90, 1024) - 100.0) < 1e-6,
          "bit scaling round-trips")


def test_pose_log() -> None:
    poses = pose_log(90)
    check(len(poses) == 90, "pose log has one entry per frame")
    speeds = [abs(poses[i].yaw - poses[i - 1].yaw) * 90 * 180 / math.pi
              for i in range(1, 90)]
    check(max(speeds) > 250, f"the sweep reaches the paper's 300 deg/s class ({max(speeds):.0f})")
    check(min(speeds) < 30, "the log also contains a near-static phase")
    for p in poses:
        R = p.matrix()
        check_ok = abs(float(np.linalg.det(R)) - 1.0) < 1e-9
        if not check_ok:
            break
    check(check_ok, "every pose matrix is a proper rotation")


def main() -> int:
    for fn in (test_dct_roundtrip, test_quantiser, test_bit_model,
               test_warp_identity_and_direction, test_upsample, test_classifier,
               test_metrics, test_bit_scaling, test_pose_log):
        print(fn.__name__)
        fn()
    if FAILED:
        print(f"\n{len(FAILED)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
