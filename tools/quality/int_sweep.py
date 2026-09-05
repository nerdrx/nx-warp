#!/usr/bin/env python3
"""Rate/PSNR of one nxv-enc configuration on the ADR-0028 clip.

A sweep harness for the integer mode decision: it runs nxv-enc, decodes with
nxv-dec, and reports bytes per frame and luma PSNR against the source.  It
exists because the integer decision has knobs whose defaults have to be set by
measurement, and doing that through a shell loop kept getting the quoting
wrong.
"""
import argparse
import os
import subprocess
import sys

import numpy as np

SC = "/run/media/nerdrx/Lex/claude/nx-scratch/gpuencinter"
BIN = "/run/media/nerdrx/Lex/claude/nx-warp-wt/gpuencinter/build-gi/bin"
NICE = ["chrt", "-i", "0", "taskset", "-c", "16-31", "nice", "-n", "19"]


def run(tag, extra, w=1088, h=1088, frames=16, qp=30, seq="turn1088"):
    src = f"{SC}/seq/{seq}.yuv420p.yuv"
    nxv = f"{SC}/sweep_{tag}.nxv"
    yuv = f"{SC}/sweep_{tag}.yuv"
    enc = NICE + [
        f"{BIN}/nxv-enc", "--in", src, "--w", str(w), "--h", str(h),
        "--eyes", "1", "--pix", "yuv420p", "--frames", str(frames),
        "--qp", str(qp), "--no-rdo", "--intra-dir", "off", "--threads", "4",
        "--out", nxv,
    ] + extra
    r = subprocess.run(enc, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{tag}: ENCODE FAILED\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
        return None
    d = subprocess.run(
        [f"{BIN}/nxv-dec", "--in", nxv, "--out", yuv, "--pix", "yuv420p",
         "--quiet"], capture_output=True, text=True)
    if d.returncode != 0:
        print(f"{tag}: DECODE FAILED\n{d.stderr[-2000:]}")
        return None

    fs = w * h * 3 // 2
    a = np.fromfile(src, dtype=np.uint8)
    b = np.fromfile(yuv, dtype=np.uint8)
    n = min(len(a), len(b)) // fs
    ps = []
    for i in range(n):
        x = a[i * fs:i * fs + w * h].astype(np.float64)
        y = b[i * fs:i * fs + w * h].astype(np.float64)
        mse = np.mean((x - y) ** 2)
        ps.append(10 * np.log10(255.0 * 255.0 / mse) if mse > 0 else 99.0)
    size = os.path.getsize(nxv)
    return dict(tag=tag, bpf=size / n, mbps=size * 8 * 90 / n / 1e6,
                psnr=float(np.mean(ps)), pmin=float(np.min(ps)), n=n)


def show(r):
    if r:
        print(f"{r['tag']:34s} {r['bpf']:8.0f} B/frame  {r['mbps']:5.1f} Mbit/s"
              f"  PSNR-Y {r['psnr']:6.2f} dB (min {r['pmin']:6.2f})")
        sys.stdout.flush()


POSES = ["--poses", f"{SC}/seq/turn1088.poses.json"]
INTER = ["--inter", "on"] + POSES + [
    "--intra-period", "180", "--preset", "fast", "--me-effort", "1",
    "--quad-mv", "off", "--near-skip", "off"]

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="sweep")
    args = ap.parse_args()

    if args.mode == "base":
        show(run("intra-only", ["--inter", "off"]))
        show(run("inter, reference RD decision", INTER))
    else:
        show(run("intra-only", ["--inter", "off"]))
        show(run("reference RD decision", INTER))
        for lam in (45, 90, 180, 360):
            for mad in (12, 24, 48):
                show(run(f"int lam={lam} mad={mad}",
                         INTER + ["--int-decision", "on",
                                  "--int-lambda", str(lam),
                                  "--int-intra-mad", str(mad)]))
