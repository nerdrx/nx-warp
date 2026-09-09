#!/usr/bin/env python3
"""Recreate the 32-frame wide-ring check in a caller-owned scratch directory."""
import argparse, os, subprocess
from pathlib import Path
import numpy as np

W, H, N = 4352, 2176, 32

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path, required=True, help="directory containing nxvc-vkenc-api and nxv-dec")
    ap.add_argument("--work", type=Path, required=True, help="scratch output directory")
    args = ap.parse_args(); args.work.mkdir(parents=True, exist_ok=True)
    y, x = np.indices((H, W))
    base = ((x // 16 * 13 + y // 16 * 7) % 200 + 28).astype("uint8")
    c = np.full((H // 2, W // 2), 128, dtype="uint8")
    source = args.work / "large.yuv"
    with source.open("wb") as f:
        for t in range(N):
            a = base.copy(); a[700:1476, 800:1376] = np.roll(base[700:1476, 800:1376], t * 13, axis=1)
            a[128:640, 128 + t * 16:640 + t * 16] = 220
            f.write(a.tobytes() + c.tobytes() + c.tobytes())
    common = ["--in", str(source), "--w", str(W), "--h", str(H), "--eyes", "2", "--frames", str(N), "--qp", "40", "--inter", "--planar-gpu-centre", "--centre-quarter", "--centre-graduated"]
    for tag, wide in (("default", False), ("wide", True)):
        env = os.environ.copy()
        for key in ("NXVC_PLANAR_CADENCE", "NXVC_PLANAR_CADENCE_TRACE", "NXVC_PLANAR_WIDE_RING"):
            env.pop(key, None)
        if wide: env["NXVC_PLANAR_WIDE_RING"] = "1"
        subprocess.run([str(args.bin / "nxvc-vkenc-api"), *common, "--out", str(args.work / (tag + ".nxv"))], env=env, check=True)
        subprocess.run([str(args.bin / "nxv-dec"), "--in", str(args.work / (tag + ".nxv")), "--out", str(args.work / (tag + ".yuv")), "--pix", "yuv420p", "--quiet"], check=True)
    subprocess.run(["python3", str(Path(__file__).with_name("verify.py")), "--source", str(source), "--default", str(args.work / "default.yuv"), "--wide", str(args.work / "wide.yuv"), "--default-stream", str(args.work / "default.nxv"), "--wide-stream", str(args.work / "wide.nxv"), "--out", str(args.work)], check=True)

if __name__ == "__main__": main()
