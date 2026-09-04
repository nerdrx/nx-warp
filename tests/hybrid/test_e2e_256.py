#!/usr/bin/env python3
"""Tiny end-to-end run of the hybrid simulator at 256x256.

Renders the synthetic sequence, encodes an x265 base, runs the enhancement
loop, and checks that all three codec paths produce sane rate and quality.
Small enough to belong in ctest (about a minute), large enough that a broken
warp, a broken rate controller or a broken base round trip shows up.

Exit codes: 0 pass, 1 fail, 77 skip (numpy or ffmpeg/libx26x absent).
"""

from __future__ import annotations

import os
import sys

try:
    import numpy as np  # noqa: F401
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

from nxvchybrid import base as basemod, codec, sweep as S  # noqa: E402

SIZE = 256
FRAMES = 12
TOTAL_MBIT = 150.0

FAILED: list[str] = []


def check(cond: bool, msg: str) -> None:
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILED.append(msg)


def main() -> int:
    if not basemod.available_encoders():
        print("SKIP: ffmpeg with libx265 or libx264 is not available")
        return 77

    scratch = os.environ.get(
        "NXVCH_SCRATCH", os.path.join(os.path.dirname(os.path.abspath(__file__)), "_scratch")
    )
    seqdir = os.path.join(scratch, "seq")
    workroot = os.path.join(scratch, "runs", "ctest-e2e")
    os.makedirs(workroot, exist_ok=True)

    yuv, poses = S.prepare_material(seqdir, SIZE, FRAMES, pano_width=1024)
    check(os.path.getsize(yuv) == FRAMES * SIZE * SIZE * 3 // 2,
          "the rendered sequence has the expected size")

    results = {}
    for kind, scale, frac in (("hevc", 1.0, 1.0), ("pure", 0.0, 0.0),
                              ("hybrid", 0.5, 0.5), ("hybrid", 1.0, 0.5)):
        job = S.Job(kind, TOTAL_MBIT, scale, frac, SIZE, FRAMES, 90.0, yuv, poses,
                    workroot, codec.WEIGHTS_2BIT, 4)
        d = S.run_job(job)
        results[(kind, scale, frac)] = d
        print(f"{job.label:24s} {d['measured_mbit']:7.1f} Mbit  "
              f"PSNR-Y {d['psnr_y']:6.2f} dB  SSIM {d['ssim_y']:.4f}")
        check(15.0 < d["psnr_y"] < 70.0, f"{job.label}: PSNR-Y is in a sane range")
        check(0.0 < d["ssim_y"] <= 1.0, f"{job.label}: SSIM is in [0,1]")
        check(0.55 * TOTAL_MBIT < d["measured_mbit"] < 1.6 * TOTAL_MBIT,
              f"{job.label}: rate control lands near the target")

    hyb = results[("hybrid", 0.5, 0.5)]
    st = hyb["stats"]
    check(st["total_tiles"] == FRAMES * (SIZE // 64) ** 2,
          "every tile of every frame was visited")
    check(st["temporal_wins"] + st["base_wins"] > 0,
          "hypothesis statistics were collected")
    check(0.0 <= sum(st["weight_hist"].values()) <= st["total_tiles"],
          "the blend-weight histogram is consistent with the tile count")
    check(hyb["base_bits"] > 0, "the hybrid run actually coded an HEVC base layer")
    check(st["model_bits"] > 0 and st["empirical_bits"] > 0,
          "both bit estimators produced a number")

    # The enhancement layer must actually add something over its own base.
    check(hyb["psnr_y"] > 20.0, "the hybrid output is better than noise")

    if FAILED:
        print(f"\n{len(FAILED)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
