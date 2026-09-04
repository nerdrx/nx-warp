#!/usr/bin/env python3
"""BD-rate of one encoder configuration against another, on one frame.

A fast stand-in for ``compare.py`` when the question is "is this tool worth
anything", not "where does the codec stand against x264".  It encodes the same
frame at four QPs with each of two configurations, measures luma PSNR against
the source, and reports the BD-rate of the second against the first -- so a
tool's own value, on our own rate, without the anchor's scale in the way.

    bd-probe.py --bin build-ref/bin --seq $NXQ_SCRATCH/seq/vr-mixed-1024-v2 \
                --pix yuv444p --a "--cfl off" --b "--cfl on"

Two binary directories may be given (``--bin-b``) to compare two builds, which
is how the encoder-only sweeps in ref/RESULTS-detail-b.md were run.  Every
process goes through the same CPU-discipline prefix the harness uses.
"""
from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import tempfile

import numpy as np

PREFIX = os.environ.get("NXQ_CPU_PREFIX", "chrt -i 0 taskset -c %s nice -n 19"
                        % os.environ.get("NXQ_CPUS", "24-27")).split()


def run(cmd: list[str]) -> None:
    subprocess.run(PREFIX + cmd, check=True, stdout=subprocess.DEVNULL)


def bd_rate(r1, p1, r2, p2) -> float:
    """Bjontegaard rate difference, cubic fit over the overlapping PSNR range."""
    l1, l2 = np.log10(r1), np.log10(r2)
    c1, c2 = np.polyfit(p1, l1, 3), np.polyfit(p2, l2, 3)
    lo, hi = max(min(p1), min(p2)), min(max(p1), max(p2))
    i1 = np.polyval(np.polyint(c1), hi) - np.polyval(np.polyint(c1), lo)
    i2 = np.polyval(np.polyint(c2), hi) - np.polyval(np.polyint(c2), lo)
    return (10 ** ((i2 - i1) / (hi - lo)) - 1) * 100


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="directory holding nxv-enc/nxv-dec")
    ap.add_argument("--bin-b", help="a second build for configuration B")
    ap.add_argument("--seq", required=True, help="sequence path without .<pix>.yuv")
    ap.add_argument("--pix", default="yuv444p", choices=("yuv444p", "yuv420p"))
    ap.add_argument("--w", type=int, default=2048)
    ap.add_argument("--h", type=int, default=1024)
    ap.add_argument("--qp", default="20,24,28,32")
    ap.add_argument("--a", default="", help="extra nxv-enc flags, configuration A")
    ap.add_argument("--b", default="", help="extra nxv-enc flags, configuration B")
    args = ap.parse_args()

    ysz = args.w * args.h
    csz = ysz if args.pix == "yuv444p" else ysz // 4
    nbytes = ysz + 2 * csz
    with open(f"{args.seq}.{args.pix}.yuv", "rb") as f:
        src = np.frombuffer(f.read(nbytes), dtype=np.uint8).astype(np.float64)[:ysz]

    qps = [int(q) for q in args.qp.split(",")]
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "src.yuv")
        with open(f"{args.seq}.{args.pix}.yuv", "rb") as f, open(raw, "wb") as g:
            g.write(f.read(nbytes))

        def measure(bindir: str, flags: str, qp: int) -> tuple[float, float]:
            nxv = os.path.join(tmp, "p.nxv")
            out = os.path.join(tmp, "p.yuv")
            run([f"{bindir}/nxv-enc", "--in", raw, "--w", str(args.w), "--h",
                 str(args.h), "--pix", args.pix, "--qp", str(qp), *shlex.split(flags),
                 "--out", nxv, "--quiet"])
            run([f"{bindir}/nxv-dec", "--in", nxv, "--out", out, "--quiet"])
            with open(out, "rb") as f:
                rec = np.frombuffer(f.read(nbytes), dtype=np.uint8).astype(np.float64)[:ysz]
            mse = ((src - rec) ** 2).mean()
            psnr = 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)
            return os.path.getsize(nxv) * 8.0, psnr

        binb = args.bin_b or args.bin
        a = [measure(args.bin, args.a, q) for q in qps]
        b = [measure(binb, args.b, q) for q in qps]

    for q, (ra, pa), (rb, pb) in zip(qps, a, b):
        print("  qp%-3d A %8.0f B %.3f dB   B %8.0f B %.3f dB   (%+.2f %% bits, "
              "%+.3f dB)" % (q, ra / 8, pa, rb / 8, pb, 100 * (rb / ra - 1), pb - pa))
    print("  BD-rate of B against A on PSNR-Y: %+.2f %%  (negative is better)"
          % bd_rate([x[0] for x in a], [x[1] for x in a],
                    [x[0] for x in b], [x[1] for x in b]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
