#!/usr/bin/env python3
"""The warp-only chain decay measurement (PAPER.md 2.11 item 2), on the real codec.

> Test: PSNR of a 2 s warp-only chain under recorded head motion with bilinear
> and Catmull-Rom; if the Full profile filter does not hold above 35 dB for 30
> frames on textured content the per-tile refresh rate must rise and the bit
> budget in 2.4 is wrong.

A warp-only chain is produced by making the encoder choose `WARP_SKIP` for
every tile of every frame after the first: `--skip-thresh` raises the SAD gate
above anything real content produces, and `--intra-period` is set beyond the
clip so the rolling refresh never fires.  Frame 0 is a normal intra frame; from
then on nothing but the pose warp reaches the decoder, so the decoded picture
is exactly the reference resampled once per frame, which is the chain the paper
is asking about.

Version 1 is bilinear only (docs/SYNTAX.md 13.4), so only the bilinear half of
the paper's comparison is measurable here; the Catmull-Rom half needs tool bit
23, which a v1 decoder refuses.  `warp/RESULTS.md` has the filter comparison on
the predictor in isolation.

    ref/warp_chain.py --seq $NXQ_SCRATCH/seq/vr-mixed-1024.yuv444p.json \\
        --enc build-ref/bin/nxv-enc --dec build-ref/bin/nxv-dec --eyes 2
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "quality"))

from nxq import cpu  # noqa: E402


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    d = a.astype(np.float64) - b.astype(np.float64)
    mse = float(np.mean(d * d))
    return 1000.0 if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seq", required=True, help="sequence .json sidecar")
    ap.add_argument("--poses", default=None, help="pose log (default: from the sidecar name)")
    ap.add_argument("--enc", default="nxv-enc")
    ap.add_argument("--dec", default="nxv-dec")
    ap.add_argument("--eyes", type=int, default=2)
    ap.add_argument("--qp", type=int, default=8)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--work", default=None)
    ap.add_argument("--threshold-db", type=float, default=35.0)
    ap.add_argument("--hold-frames", type=int, default=30)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    with open(args.seq) as fh:
        side = json.load(fh)
    raw = side["path"] if os.path.isabs(side.get("path", "")) else os.path.join(
        os.path.dirname(os.path.abspath(args.seq)), os.path.basename(side["path"]))
    W, H = int(side["width"]), int(side["height"])
    pix = side["pix_fmt"]
    fps = float(side.get("fps", 90.0))
    poses = args.poses or os.path.join(
        os.path.dirname(os.path.abspath(args.seq)),
        os.path.basename(args.seq).split(".")[0] + ".poses.json")

    work = args.work or os.path.dirname(os.path.abspath(args.seq))
    bs = os.path.join(work, "warpchain.nxv")
    rec = os.path.join(work, "warpchain.yuv")

    enc = [args.enc, "--in", raw, "--w", str(W), "--h", str(H), "--pix", pix,
           "--qp", str(args.qp), "--eyes", str(args.eyes), "--inter", "on",
           "--poses", poses, "--intra-period", "1000000",
           "--skip-thresh", "100000", "--out", bs, "--quiet"]
    if args.frames:
        enc += ["--frames", str(args.frames)]
    p = cpu.run(enc, check=False)
    if p.returncode != 0:
        print(p.stderr or p.stdout)
        return 1
    p = cpu.run([args.dec, "--in", bs, "--out", rec, "--quiet"], check=False)
    if p.returncode != 0:
        print(p.stderr or p.stdout)
        return 1

    csz = (W * H) if pix == "yuv444p" else (W // 2) * (H // 2)
    fsz = W * H + 2 * csz
    n = min(os.path.getsize(raw), os.path.getsize(rec)) // fsz
    if args.frames:
        n = min(n, args.frames)

    with open(args.seq.rsplit(".json", 1)[0] + ".json") as fh:
        pass
    av = []
    if os.path.exists(poses):
        with open(poses) as fh:
            doc = json.load(fh)
        fr = doc["frames"] if isinstance(doc, dict) else doc
        av = [f.get("angular_velocity_deg_s", 0.0) for f in fr]

    rows = []
    with open(raw, "rb") as fa, open(rec, "rb") as fb:
        for i in range(n):
            a = np.frombuffer(fa.read(fsz)[: W * H], dtype=np.uint8)
            b = np.frombuffer(fb.read(fsz)[: W * H], dtype=np.uint8)
            rows.append({"frame": i, "psnr_y": psnr(a, b),
                         "angular_velocity_deg_s": av[i] if i < len(av) else None})

    size = os.path.getsize(bs)
    print(f"warp-only chain: {n} frames, {pix} {W}x{H}, eyes {args.eyes}, "
          f"qp {args.qp}")
    print(f"  bitstream {size} B  ({size * 8 / max(1, n) * fps / 1e6:.2f} Mbit/s "
          f"-- frame 0 is intra, the rest carry no residual at all)")
    print("  frame   PSNR-Y dB   angular velocity")
    for r in rows:
        avs = "" if r["angular_velocity_deg_s"] is None else \
            f"{r['angular_velocity_deg_s']:8.1f} deg/s"
        print(f"  {r['frame']:5d}   {r['psnr_y']:9.2f}   {avs}")

    chain = rows[1:]
    held = 0
    for r in chain:
        if r["psnr_y"] >= args.threshold_db:
            held += 1
        else:
            break
    verdict = "PASS" if held >= args.hold_frames else "FAIL"
    print(f"\n  PAPER.md 2.11 item 2: \"if the Full profile filter does not hold "
          f"above 35 dB\n  for 30 frames on textured content the per-tile refresh "
          f"rate must rise and\n  the bit budget in 2.4 is wrong\"")
    print(f"    held above {args.threshold_db:.0f} dB for {held} consecutive "
          f"warped frames (needs {args.hold_frames})  {verdict}")
    if chain:
        print(f"    first warped frame {chain[0]['psnr_y']:.2f} dB, "
              f"last {chain[-1]['psnr_y']:.2f} dB, "
              f"decay {chain[0]['psnr_y'] - chain[-1]['psnr_y']:+.2f} dB over "
              f"{len(chain)} frames")
    print("    filter: bilinear (version 1 is bilinear only, docs/SYNTAX.md 13.4)")

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"frames": rows, "held_frames": held, "verdict": verdict,
                       "threshold_db": args.threshold_db, "bytes": size,
                       "sequence": os.path.basename(args.seq)}, fh, indent=1)
    for f in (bs, rec):
        if os.path.exists(f):
            os.remove(f)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
